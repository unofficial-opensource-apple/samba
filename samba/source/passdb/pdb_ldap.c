/* 
   Unix SMB/CIFS mplementation.
   LDAP protocol helper functions for SAMBA
   Copyright (C) Jean Fran�ois Micouleau	1998
   Copyright (C) Gerald Carter			2001-2003
   Copyright (C) Shahms King			2001
   Copyright (C) Andrew Bartlett		2002-2003
   Copyright (C) Stefan (metze) Metzmacher	2002
    
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
   
*/

/* TODO:
*  persistent connections: if using NSS LDAP, many connections are made
*      however, using only one within Samba would be nice
*  
*  Clean up SSL stuff, compile on OpenLDAP 1.x, 2.x, and Netscape SDK
*
*  Other LDAP based login attributes: accountExpires, etc.
*  (should be the domain of Samba proper, but the sam_password/SAM_ACCOUNT
*  structures don't have fields for some of these attributes)
*
*  SSL is done, but can't get the certificate based authentication to work
*  against on my test platform (Linux 2.4, OpenLDAP 2.x)
*/

/* NOTE: this will NOT work against an Active Directory server
*  due to the fact that the two password fields cannot be retrieved
*  from a server; recommend using security = domain in this situation
*  and/or winbind
*/

#include "includes.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_PASSDB

#include <lber.h>
#include <ldap.h>

/*
 * Work around versions of the LDAP client libs that don't have the OIDs
 * defined, or have them defined under the old name.  
 * This functionality is really a factor of the server, not the client 
 *
 */

#if defined(LDAP_EXOP_X_MODIFY_PASSWD) && !defined(LDAP_EXOP_MODIFY_PASSWD)
#define LDAP_EXOP_MODIFY_PASSWD LDAP_EXOP_X_MODIFY_PASSWD
#elif !defined(LDAP_EXOP_MODIFY_PASSWD)
#define "1.3.6.1.4.1.4203.1.11.1"
#endif

#if defined(LDAP_EXOP_X_MODIFY_PASSWD_ID) && !defined(LDAP_EXOP_MODIFY_PASSWD_ID)
#define LDAP_TAG_EXOP_MODIFY_PASSWD_ID LDAP_EXOP_X_MODIFY_PASSWD_ID
#elif !defined(LDAP_EXOP_MODIFY_PASSWD_ID)
#define LDAP_TAG_EXOP_MODIFY_PASSWD_ID        ((ber_tag_t) 0x80U)
#endif

#if defined(LDAP_EXOP_X_MODIFY_PASSWD_NEW) && !defined(LDAP_EXOP_MODIFY_PASSWD_NEW)
#define LDAP_TAG_EXOP_MODIFY_PASSWD_NEW LDAP_EXOP_X_MODIFY_PASSWD_NEW
#elif !defined(LDAP_EXOP_MODIFY_PASSWD_NEW)
#define LDAP_TAG_EXOP_MODIFY_PASSWD_NEW       ((ber_tag_t) 0x82U)
#endif


#ifndef SAM_ACCOUNT
#define SAM_ACCOUNT struct sam_passwd
#endif

#include "smbldap.h"

struct ldapsam_privates {
	struct smbldap_state *smbldap_state;

	/* Former statics */
	LDAPMessage *result;
	LDAPMessage *entry;
	int index;
	
	const char *domain_name;
	DOM_SID domain_sid;
	
	/* configuration items */
	int schema_ver;
};

/**********************************************************************
 Free a LDAPMessage (one is stored on the SAM_ACCOUNT)
 **********************************************************************/
 
static void private_data_free_fn(void **result) 
{
	ldap_msgfree(*result);
	*result = NULL;
}

/**********************************************************************
 get the attribute name given a user schame version 
 **********************************************************************/
 
static const char* get_userattr_key2string( int schema_ver, int key )
{
	switch ( schema_ver )
	{
		case SCHEMAVER_SAMBAACCOUNT:
			return get_attr_key2string( attrib_map_v22, key );
			
		case SCHEMAVER_SAMBASAMACCOUNT:
			return get_attr_key2string( attrib_map_v30, key );
			
		default:
			DEBUG(0,("get_userattr_key2string: unknown schema version specified\n"));
			break;
	}
	return NULL;
}

/**********************************************************************
 return the list of attribute names given a user schema version 
 **********************************************************************/

static char** get_userattr_list( int schema_ver )
{
	switch ( schema_ver ) 
	{
		case SCHEMAVER_SAMBAACCOUNT:
			return get_attr_list( attrib_map_v22 );
			
		case SCHEMAVER_SAMBASAMACCOUNT:
			return get_attr_list( attrib_map_v30 );
		default:
			DEBUG(0,("get_userattr_list: unknown schema version specified!\n"));
			break;
	}
	
	return NULL;
}
/*******************************************************************
 generate the LDAP search filter for the objectclass based on the 
 version of the schema we are using 
 ******************************************************************/

static const char* get_objclass_filter( int schema_ver )
{
	static fstring objclass_filter;
	
	switch( schema_ver ) 
	{
		case SCHEMAVER_SAMBAACCOUNT:
			fstr_sprintf( objclass_filter, "(objectclass=%s)", LDAP_OBJ_SAMBAACCOUNT );
			break;
		case SCHEMAVER_SAMBASAMACCOUNT:
			fstr_sprintf( objclass_filter, "(objectclass=%s)", LDAP_OBJ_SAMBASAMACCOUNT );
			break;
		default:
			DEBUG(0,("pdb_ldapsam: get_objclass_filter(): Invalid schema version specified!\n"));
			break;
	}
	
	return objclass_filter;	
}

/*******************************************************************
 run the search by name.
******************************************************************/
static int ldapsam_search_suffix_by_name (struct ldapsam_privates *ldap_state, 
					  const char *user,
					  LDAPMessage ** result, char **attr)
{
	pstring filter;
	char *escape_user = escape_ldap_string_alloc(user);

	if (!escape_user) {
		return LDAP_NO_MEMORY;
	}

	/*
	 * in the filter expression, replace %u with the real name
	 * so in ldap filter, %u MUST exist :-)
	 */
	pstr_sprintf(filter, "(&%s%s)", lp_ldap_filter(), 
		get_objclass_filter(ldap_state->schema_ver));

	/* 
	 * have to use this here because $ is filtered out
	   * in pstring_sub
	 */
	

	all_string_sub(filter, "%u", escape_user, sizeof(pstring));
	SAFE_FREE(escape_user);

	return smbldap_search_suffix(ldap_state->smbldap_state, filter, attr, result);
}

/*******************************************************************
 run the search by rid.
******************************************************************/
static int ldapsam_search_suffix_by_rid (struct ldapsam_privates *ldap_state, 
					 uint32 rid, LDAPMessage ** result, 
					 char **attr)
{
	pstring filter;
	int rc;

	pstr_sprintf(filter, "(&(rid=%i)%s)", rid, 
		get_objclass_filter(ldap_state->schema_ver));
	
	rc = smbldap_search_suffix(ldap_state->smbldap_state, filter, attr, result);
	
	return rc;
}

/*******************************************************************
 run the search by SID.
******************************************************************/
static int ldapsam_search_suffix_by_sid (struct ldapsam_privates *ldap_state, 
					 const DOM_SID *sid, LDAPMessage ** result, 
					 char **attr)
{
	pstring filter;
	int rc;
	fstring sid_string;

	pstr_sprintf(filter, "(&(%s=%s)%s)", 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_SID),
		sid_to_string(sid_string, sid), 
		get_objclass_filter(ldap_state->schema_ver));
		
	rc = smbldap_search_suffix(ldap_state->smbldap_state, filter, attr, result);
	
	return rc;
}

/*******************************************************************
 Delete complete object or objectclass and attrs from
 object found in search_result depending on lp_ldap_delete_dn
******************************************************************/
static NTSTATUS ldapsam_delete_entry(struct ldapsam_privates *ldap_state,
				     LDAPMessage *result,
				     const char *objectclass,
				     char **attrs)
{
	int rc;
	LDAPMessage *entry;
	LDAPMod **mods = NULL;
	char *name, *dn;
	BerElement *ptr = NULL;

	rc = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);

	if (rc != 1) {
		DEBUG(0, ("Entry must exist exactly once!\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	dn    = ldap_get_dn(ldap_state->smbldap_state->ldap_struct, entry);

	if (lp_ldap_delete_dn()) {
		NTSTATUS ret = NT_STATUS_OK;
		rc = smbldap_delete(ldap_state->smbldap_state, dn);

		if (rc != LDAP_SUCCESS) {
			DEBUG(0, ("Could not delete object %s\n", dn));
			ret = NT_STATUS_UNSUCCESSFUL;
		}
		ldap_memfree(dn);
		return ret;
	}

	/* Ok, delete only the SAM attributes */
	
	for (name = ldap_first_attribute(ldap_state->smbldap_state->ldap_struct, entry, &ptr);
	     name != NULL;
	     name = ldap_next_attribute(ldap_state->smbldap_state->ldap_struct, entry, ptr)) 
	{
		char **attrib;

		/* We are only allowed to delete the attributes that
		   really exist. */

		for (attrib = attrs; *attrib != NULL; attrib++) 
		{
			if (StrCaseCmp(*attrib, name) == 0) {
				DEBUG(10, ("deleting attribute %s\n", name));
				smbldap_set_mod(&mods, LDAP_MOD_DELETE, name, NULL);
			}
		}

		ldap_memfree(name);
	}
	
	if (ptr != NULL) {
		ber_free(ptr, 0);
	}
	
	smbldap_set_mod(&mods, LDAP_MOD_DELETE, "objectClass", objectclass);

	rc = smbldap_modify(ldap_state->smbldap_state, dn, mods);
	ldap_mods_free(mods, True);

	if (rc != LDAP_SUCCESS) {
		char *ld_error = NULL;
		ldap_get_option(ldap_state->smbldap_state->ldap_struct, LDAP_OPT_ERROR_STRING,
				&ld_error);
		
		DEBUG(0, ("could not delete attributes for %s, error: %s (%s)\n",
			  dn, ldap_err2string(rc), ld_error?ld_error:"unknown"));
		SAFE_FREE(ld_error);
		ldap_memfree(dn);
		return NT_STATUS_UNSUCCESSFUL;
	}

	ldap_memfree(dn);
	return NT_STATUS_OK;
}
		  

/* New Interface is being implemented here */

#if 0	/* JERRY - not uesed anymore */

/**********************************************************************
Initialize SAM_ACCOUNT from an LDAP query (unix attributes only)
*********************************************************************/
static BOOL get_unix_attributes (struct ldapsam_privates *ldap_state, 
				SAM_ACCOUNT * sampass,
				LDAPMessage * entry,
				gid_t *gid)
{
	pstring  homedir;
	pstring  temp;
	char **ldap_values;
	char **values;

	if ((ldap_values = ldap_get_values (ldap_state->smbldap_state->ldap_struct, entry, "objectClass")) == NULL) {
		DEBUG (1, ("get_unix_attributes: no objectClass! \n"));
		return False;
	}

	for (values=ldap_values;*values;values++) {
		if (strcasecmp(*values, LDAP_OBJ_POSIXACCOUNT ) == 0) {
			break;
		}
	}
	
	if (!*values) { /*end of array, no posixAccount */
		DEBUG(10, ("user does not have %s attributes\n", LDAP_OBJ_POSIXACCOUNT));
		ldap_value_free(ldap_values);
		return False;
	}
	ldap_value_free(ldap_values);

	if ( !smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_UNIX_HOME), homedir) ) 
	{
		return False;
	}
	
	if ( !smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_GIDNUMBER), temp) )
	{
		return False;
	}
	
	*gid = (gid_t)atol(temp);

	pdb_set_unix_homedir(sampass, homedir, PDB_SET);
	
	DEBUG(10, ("user has %s attributes\n", LDAP_OBJ_POSIXACCOUNT));
	
	return True;
}

#endif

/**********************************************************************
Initialize SAM_ACCOUNT from an LDAP query
(Based on init_sam_from_buffer in pdb_tdb.c)
*********************************************************************/
static BOOL init_sam_from_ldap (struct ldapsam_privates *ldap_state, 
				SAM_ACCOUNT * sampass,
				LDAPMessage * entry)
{
	time_t  logon_time,
			logoff_time,
			kickoff_time,
			pass_last_set_time, 
			pass_can_change_time, 
			pass_must_change_time;
	pstring 	username, 
			domain,
			nt_username,
			fullname,
			homedir,
			dir_drive,
			logon_script,
			profile_path,
			acct_desc,
			munged_dial,
			workstations;
	uint32 		user_rid; 
	uint8 		smblmpwd[LM_HASH_LEN],
			smbntpwd[NT_HASH_LEN];
	uint16 		acct_ctrl = 0, 
			logon_divs;
	uint32 hours_len;
	uint8 		hours[MAX_HOURS_LEN];
	pstring temp;
	struct passwd	*pw = NULL;
	uid_t		uid = -1;
	gid_t		gid = -1;

	/*
	 * do a little initialization
	 */
	username[0] 	= '\0';
	domain[0] 	= '\0';
	nt_username[0] 	= '\0';
	fullname[0] 	= '\0';
	homedir[0] 	= '\0';
	dir_drive[0] 	= '\0';
	logon_script[0] = '\0';
	profile_path[0] = '\0';
	acct_desc[0] 	= '\0';
	munged_dial[0] 	= '\0';
	workstations[0] = '\0';
	 

	if (sampass == NULL || ldap_state == NULL || entry == NULL) {
		DEBUG(0, ("init_sam_from_ldap: NULL parameters found!\n"));
		return False;
	}

	if (ldap_state->smbldap_state->ldap_struct == NULL) {
		DEBUG(0, ("init_sam_from_ldap: ldap_state->smbldap_state->ldap_struct is NULL!\n"));
		return False;
	}
	
	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, "uid", username)) {
		DEBUG(1, ("No uid attribute found for this user!\n"));
		return False;
	}

	DEBUG(2, ("Entry found for user: %s\n", username));

	/* I'm not going to fail here, since there are checks 
	   higher up the cal stack to do this   --jerry */

	if ( (pw=Get_Pwnam(username)) != NULL ) {
		uid = pw->pw_uid;
		gid = pw->pw_gid;
	}

	pstrcpy(nt_username, username);

	pstrcpy(domain, ldap_state->domain_name);
	
	pdb_set_username(sampass, username, PDB_SET);

	pdb_set_domain(sampass, domain, PDB_DEFAULT);
	pdb_set_nt_username(sampass, nt_username, PDB_SET);

	/* deal with different attributes between the schema first */
	
	if ( ldap_state->schema_ver == SCHEMAVER_SAMBASAMACCOUNT ) 
	{
		if (smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_SID), temp)) 
		{
			pdb_set_user_sid_from_string(sampass, temp, PDB_SET);
		}
		
		if (smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PRIMARY_GROUP_SID), temp)) 
		{
			pdb_set_group_sid_from_string(sampass, temp, PDB_SET);			
		}
		else 
		{
			pdb_set_group_sid_from_rid(sampass, DOMAIN_GROUP_RID_USERS, PDB_DEFAULT);
		}


	} 
	else 
	{
		if (smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_RID), temp)) 
		{
			user_rid = (uint32)atol(temp);
			pdb_set_user_sid_from_rid(sampass, user_rid, PDB_SET);
		}
		
		if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PRIMARY_GROUP_RID), temp)) 
		{
			pdb_set_group_sid_from_rid(sampass, DOMAIN_GROUP_RID_USERS, PDB_DEFAULT);
		} else {
			uint32 group_rid;
			
			group_rid = (uint32)atol(temp);
			
			/* for some reason, we often have 0 as a primary group RID.
			   Make sure that we treat this just as a 'default' value */
			   
			if ( group_rid > 0 )
				pdb_set_group_sid_from_rid(sampass, group_rid, PDB_SET);
			else
				pdb_set_group_sid_from_rid(sampass, DOMAIN_GROUP_RID_USERS, PDB_DEFAULT);
		}
	}

	if (pdb_get_init_flags(sampass,PDB_USERSID) == PDB_DEFAULT) {
		DEBUG(1, ("no %s or %s attribute found for this user %s\n", 
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_SID),
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_RID),
			username));
		return False;
	}


#if 0	/* JERRY -- not used anymore */
	/* 
	 * If so configured, try and get the values from LDAP 
	 */

	if (lp_ldap_trust_ids() && (get_unix_attributes(ldap_state, sampass, entry, &gid))) 
	{	
		if (pdb_get_init_flags(sampass,PDB_GROUPSID) == PDB_DEFAULT) 
		{
			GROUP_MAP map;
			/* call the mapping code here */
			if(pdb_getgrgid(&map, gid)) {
				pdb_set_group_sid(sampass, &map.sid, PDB_SET);
			} 
			else {
				pdb_set_group_sid_from_rid(sampass, pdb_gid_to_group_rid(gid), PDB_SET);
			}
		}
	}
#endif

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PWD_LAST_SET), temp)) 
	{
		/* leave as default */
	} else {
		pass_last_set_time = (time_t) atol(temp);
		pdb_set_pass_last_set_time(sampass, pass_last_set_time, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LOGON_TIME), temp)) 
	{
		/* leave as default */
	} else {
		logon_time = (time_t) atol(temp);
		pdb_set_logon_time(sampass, logon_time, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LOGOFF_TIME), temp)) 
	{
		/* leave as default */
	} else {
		logoff_time = (time_t) atol(temp);
		pdb_set_logoff_time(sampass, logoff_time, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_KICKOFF_TIME), temp)) 
	{
		/* leave as default */
	} else {
		kickoff_time = (time_t) atol(temp);
		pdb_set_kickoff_time(sampass, kickoff_time, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PWD_CAN_CHANGE), temp)) 
	{
		/* leave as default */
	} else {
		pass_can_change_time = (time_t) atol(temp);
		pdb_set_pass_can_change_time(sampass, pass_can_change_time, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PWD_MUST_CHANGE), temp)) 
	{	
		/* leave as default */
	} else {
		pass_must_change_time = (time_t) atol(temp);
		pdb_set_pass_must_change_time(sampass, pass_must_change_time, PDB_SET);
	}

	/* recommend that 'gecos' and 'displayName' should refer to the same
	 * attribute OID.  userFullName depreciated, only used by Samba
	 * primary rules of LDAP: don't make a new attribute when one is already defined
	 * that fits your needs; using cn then displayName rather than 'userFullName'
	 */

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry,
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_DISPLAY_NAME), fullname)) 
	{
		if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_CN), fullname)) 
		{
			/* leave as default */
		} else {
			pdb_set_fullname(sampass, fullname, PDB_SET);
		}
	} else {
		pdb_set_fullname(sampass, fullname, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_HOME_DRIVE), dir_drive)) 
	{
		pdb_set_dir_drive(sampass, talloc_sub_specified(sampass->mem_ctx, 
								  lp_logon_drive(),
								  username, domain, 
								  uid, gid),
				  PDB_DEFAULT);
	} else {
		pdb_set_dir_drive(sampass, dir_drive, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry,
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_HOME_PATH), homedir)) 
	{
		pdb_set_homedir(sampass, talloc_sub_specified(sampass->mem_ctx, 
								  lp_logon_home(),
								  username, domain, 
								  uid, gid), 
				  PDB_DEFAULT);
	} else {
		pdb_set_homedir(sampass, homedir, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry,
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LOGON_SCRIPT), logon_script)) 
	{
		pdb_set_logon_script(sampass, talloc_sub_specified(sampass->mem_ctx, 
								     lp_logon_script(),
								     username, domain, 
								     uid, gid), 
				     PDB_DEFAULT);
	} else {
		pdb_set_logon_script(sampass, logon_script, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry,
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PROFILE_PATH), profile_path)) 
	{
		pdb_set_profile_path(sampass, talloc_sub_specified(sampass->mem_ctx, 
								     lp_logon_path(),
								     username, domain, 
								     uid, gid), 
				     PDB_DEFAULT);
	} else {
		pdb_set_profile_path(sampass, profile_path, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_DESC), acct_desc)) 
	{
		/* leave as default */
	} else {
		pdb_set_acct_desc(sampass, acct_desc, PDB_SET);
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_WKS), workstations)) 
	{
		/* leave as default */;
	} else {
		pdb_set_workstations(sampass, workstations, PDB_SET);
	}

	/* FIXME: hours stuff should be cleaner */
	
	logon_divs = 168;
	hours_len = 21;
	memset(hours, 0xff, hours_len);

	if (!smbldap_get_single_attribute (ldap_state->smbldap_state->ldap_struct, entry, 
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LMPW), temp)) 
	{
		/* leave as default */
	} else {
		pdb_gethexpwd(temp, smblmpwd);
		memset((char *)temp, '\0', strlen(temp)+1);
		if (!pdb_set_lanman_passwd(sampass, smblmpwd, PDB_SET))
			return False;
		ZERO_STRUCT(smblmpwd);
	}

	if (!smbldap_get_single_attribute (ldap_state->smbldap_state->ldap_struct, entry,
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_NTPW), temp)) 
	{
		/* leave as default */
	} else {
		pdb_gethexpwd(temp, smbntpwd);
		memset((char *)temp, '\0', strlen(temp)+1);
		if (!pdb_set_nt_passwd(sampass, smbntpwd, PDB_SET))
			return False;
		ZERO_STRUCT(smbntpwd);
	}

	if (!smbldap_get_single_attribute (ldap_state->smbldap_state->ldap_struct, entry,
		get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_ACB_INFO), temp)) 
	{
		acct_ctrl |= ACB_NORMAL;
	} else {
		acct_ctrl = pdb_decode_acct_ctrl(temp);

		if (acct_ctrl == 0)
			acct_ctrl |= ACB_NORMAL;

		pdb_set_acct_ctrl(sampass, acct_ctrl, PDB_SET);
	}

	pdb_set_hours_len(sampass, hours_len, PDB_SET);
	pdb_set_logon_divs(sampass, logon_divs, PDB_SET);

	pdb_set_munged_dial(sampass, munged_dial, PDB_SET);
	
	/* pdb_set_unknown_3(sampass, unknown3, PDB_SET); */
	/* pdb_set_unknown_5(sampass, unknown5, PDB_SET); */
	/* pdb_set_unknown_6(sampass, unknown6, PDB_SET); */

	pdb_set_hours(sampass, hours, PDB_SET);

	return True;
}

/**********************************************************************
Initialize SAM_ACCOUNT from an LDAP query
(Based on init_buffer_from_sam in pdb_tdb.c)
*********************************************************************/
static BOOL init_ldap_from_sam (struct ldapsam_privates *ldap_state, 
				LDAPMessage *existing,
				LDAPMod *** mods, SAM_ACCOUNT * sampass,
				BOOL (*need_update)(const SAM_ACCOUNT *,
						    enum pdb_elements))
{
	pstring temp;
	uint32 rid;

	if (mods == NULL || sampass == NULL) {
		DEBUG(0, ("init_ldap_from_sam: NULL parameters found!\n"));
		return False;
	}

	*mods = NULL;

	/* 
	 * took out adding "objectclass: sambaAccount"
	 * do this on a per-mod basis
	 */
	if (need_update(sampass, PDB_USERNAME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods, 
			      "uid", pdb_get_username(sampass));

	DEBUG(2, ("Setting entry for user: %s\n", pdb_get_username(sampass)));

	/* only update the RID if we actually need to */
	if (need_update(sampass, PDB_USERSID)) 
	{
		fstring sid_string;
		fstring dom_sid_string;
		const DOM_SID *user_sid = pdb_get_user_sid(sampass);
		
		switch ( ldap_state->schema_ver )
		{
			case SCHEMAVER_SAMBAACCOUNT:
				if (!sid_peek_check_rid(&ldap_state->domain_sid, user_sid, &rid)) {
					DEBUG(1, ("User's SID (%s) is not for this domain (%s), cannot add to LDAP!\n", 
						sid_to_string(sid_string, user_sid), 
						sid_to_string(dom_sid_string, &ldap_state->domain_sid)));
					return False;
				}
				slprintf(temp, sizeof(temp) - 1, "%i", rid);
				smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
					get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_RID), 
					temp);
				break;
				
			case SCHEMAVER_SAMBASAMACCOUNT:
				smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
					get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_SID), 
					sid_to_string(sid_string, user_sid));				      
				break;
				
			default:
				DEBUG(0,("init_ldap_from_sam: unknown schema version specified\n"));
				break;
		}		
	}

	/* we don't need to store the primary group RID - so leaving it
	   'free' to hang off the unix primary group makes life easier */

	if (need_update(sampass, PDB_GROUPSID)) 
	{
		fstring sid_string;
		fstring dom_sid_string;
		const DOM_SID *group_sid = pdb_get_group_sid(sampass);
		
		switch ( ldap_state->schema_ver )
		{
			case SCHEMAVER_SAMBAACCOUNT:
				if (!sid_peek_check_rid(&ldap_state->domain_sid, group_sid, &rid)) {
					DEBUG(1, ("User's Primary Group SID (%s) is not for this domain (%s), cannot add to LDAP!\n",
						sid_to_string(sid_string, group_sid),
						sid_to_string(dom_sid_string, &ldap_state->domain_sid)));
					return False;
				}

				slprintf(temp, sizeof(temp) - 1, "%i", rid);
				smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
					get_userattr_key2string(ldap_state->schema_ver, 
					LDAP_ATTR_PRIMARY_GROUP_RID), temp);
				break;
				
			case SCHEMAVER_SAMBASAMACCOUNT:
				smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
					get_userattr_key2string(ldap_state->schema_ver, 
					LDAP_ATTR_PRIMARY_GROUP_SID), sid_to_string(sid_string, group_sid));
				break;
				
			default:
				DEBUG(0,("init_ldap_from_sam: unknown schema version specified\n"));
				break;
		}
		
	}
	
	/* displayName, cn, and gecos should all be the same
	 *  most easily accomplished by giving them the same OID
	 *  gecos isn't set here b/c it should be handled by the 
	 *  add-user script
	 *  We change displayName only and fall back to cn if
	 *  it does not exist.
	 */

	if (need_update(sampass, PDB_FULLNAME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_DISPLAY_NAME), 
			pdb_get_fullname(sampass));

	if (need_update(sampass, PDB_ACCTDESC))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_DESC), 
			pdb_get_acct_desc(sampass));

	if (need_update(sampass, PDB_WORKSTATIONS))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_WKS), 
			pdb_get_workstations(sampass));

	if (need_update(sampass, PDB_SMBHOME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_HOME_PATH), 
			pdb_get_homedir(sampass));
			
	if (need_update(sampass, PDB_DRIVE))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_HOME_DRIVE), 
			pdb_get_dir_drive(sampass));

	if (need_update(sampass, PDB_LOGONSCRIPT))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LOGON_SCRIPT), 
			pdb_get_logon_script(sampass));

	if (need_update(sampass, PDB_PROFILE))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PROFILE_PATH), 
			pdb_get_profile_path(sampass));

	slprintf(temp, sizeof(temp) - 1, "%li", pdb_get_logon_time(sampass));
	if (need_update(sampass, PDB_LOGONTIME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LOGON_TIME), temp);

	slprintf(temp, sizeof(temp) - 1, "%li", pdb_get_logoff_time(sampass));
	if (need_update(sampass, PDB_LOGOFFTIME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LOGOFF_TIME), temp);

	slprintf (temp, sizeof (temp) - 1, "%li", pdb_get_kickoff_time(sampass));
	if (need_update(sampass, PDB_KICKOFFTIME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_KICKOFF_TIME), temp);

	slprintf (temp, sizeof (temp) - 1, "%li", pdb_get_pass_can_change_time(sampass));
	if (need_update(sampass, PDB_CANCHANGETIME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PWD_CAN_CHANGE), temp);

	slprintf (temp, sizeof (temp) - 1, "%li", pdb_get_pass_must_change_time(sampass));
	if (need_update(sampass, PDB_MUSTCHANGETIME))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PWD_MUST_CHANGE), temp);

	if ((pdb_get_acct_ctrl(sampass)&(ACB_WSTRUST|ACB_SVRTRUST|ACB_DOMTRUST))
		|| (lp_ldap_passwd_sync()!=LDAP_PASSWD_SYNC_ONLY)) 
	{

		pdb_sethexpwd(temp, pdb_get_lanman_passwd(sampass),
			       pdb_get_acct_ctrl(sampass));

		if (need_update(sampass, PDB_LMPASSWD))
			smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
				get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_LMPW), 
				temp);

		pdb_sethexpwd (temp, pdb_get_nt_passwd(sampass),
			       pdb_get_acct_ctrl(sampass));

		if (need_update(sampass, PDB_NTPASSWD))
			smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
				get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_NTPW), 
				temp);

		slprintf (temp, sizeof (temp) - 1, "%li", pdb_get_pass_last_set_time(sampass));
		if (need_update(sampass, PDB_PASSLASTSET))
			smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
				get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_PWD_LAST_SET), 
				temp);
	}

	/* FIXME: Hours stuff goes in LDAP  */

	if (need_update(sampass, PDB_ACCTCTRL))
		smbldap_make_mod(ldap_state->smbldap_state->ldap_struct, existing, mods,
			get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_ACB_INFO), 
			pdb_encode_acct_ctrl (pdb_get_acct_ctrl(sampass), NEW_PW_FORMAT_SPACE_PADDED_LEN));

	return True;
}



/**********************************************************************
Connect to LDAP server for password enumeration
*********************************************************************/
static NTSTATUS ldapsam_setsampwent(struct pdb_methods *my_methods, BOOL update)
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	int rc;
	pstring filter;
	char **attr_list;

	pstr_sprintf( filter, "(&%s%s)", lp_ldap_filter(), 
		get_objclass_filter(ldap_state->schema_ver));
	all_string_sub(filter, "%u", "*", sizeof(pstring));

	attr_list = get_userattr_list(ldap_state->schema_ver);
	rc = smbldap_search_suffix(ldap_state->smbldap_state, filter, 
				   attr_list, &ldap_state->result);
	free_attr_list( attr_list );

	if (rc != LDAP_SUCCESS) {
		DEBUG(0, ("LDAP search failed: %s\n", ldap_err2string(rc)));
		DEBUG(3, ("Query was: %s, %s\n", lp_ldap_suffix(), filter));
		ldap_msgfree(ldap_state->result);
		ldap_state->result = NULL;
		return NT_STATUS_UNSUCCESSFUL;
	}

	DEBUG(2, ("ldapsam_setsampwent: %d entries in the base!\n",
		ldap_count_entries(ldap_state->smbldap_state->ldap_struct,
		ldap_state->result)));

	ldap_state->entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct,
				 ldap_state->result);
	ldap_state->index = 0;

	return NT_STATUS_OK;
}

/**********************************************************************
End enumeration of the LDAP password list 
*********************************************************************/
static void ldapsam_endsampwent(struct pdb_methods *my_methods)
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	if (ldap_state->result) {
		ldap_msgfree(ldap_state->result);
		ldap_state->result = NULL;
	}
}

/**********************************************************************
Get the next entry in the LDAP password database 
*********************************************************************/
static NTSTATUS ldapsam_getsampwent(struct pdb_methods *my_methods, SAM_ACCOUNT *user)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	BOOL bret = False;

	while (!bret) {
		if (!ldap_state->entry)
			return ret;
		
		ldap_state->index++;
		bret = init_sam_from_ldap(ldap_state, user, ldap_state->entry);
		
		ldap_state->entry = ldap_next_entry(ldap_state->smbldap_state->ldap_struct,
					    ldap_state->entry);	
	}

	return NT_STATUS_OK;
}

/**********************************************************************
Get SAM_ACCOUNT entry from LDAP by username 
*********************************************************************/
static NTSTATUS ldapsam_getsampwnam(struct pdb_methods *my_methods, SAM_ACCOUNT *user, const char *sname)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	LDAPMessage *result;
	LDAPMessage *entry;
	int count;
	char ** attr_list;
	int rc;
	
	attr_list = get_userattr_list( ldap_state->schema_ver );
	rc = ldapsam_search_suffix_by_name(ldap_state, sname, &result, attr_list);
	free_attr_list( attr_list );

	if ( rc != LDAP_SUCCESS ) 
		return NT_STATUS_NO_SUCH_USER;
	
	count = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);
	
	if (count < 1) {
		DEBUG(4,
		      ("Unable to locate user [%s] count=%d\n", sname,
		       count));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_USER;
	} else if (count > 1) {
		DEBUG(1,
		      ("Duplicate entries for this user [%s] Failing. count=%d\n", sname,
		       count));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_USER;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	if (entry) {
		if (!init_sam_from_ldap(ldap_state, user, entry)) {
			DEBUG(1,("ldapsam_getsampwnam: init_sam_from_ldap failed for user '%s'!\n", sname));
			ldap_msgfree(result);
			return NT_STATUS_NO_SUCH_USER;
		}
		pdb_set_backend_private_data(user, result, 
					     private_data_free_fn, 
					     my_methods, PDB_CHANGED);
		ret = NT_STATUS_OK;
	} else {
		ldap_msgfree(result);
	}
	return ret;
}

static int ldapsam_get_ldap_user_by_sid(struct ldapsam_privates *ldap_state, 
				   const DOM_SID *sid, LDAPMessage **result) 
{
	int rc = -1;
	char ** attr_list;
	uint32 rid;

	switch ( ldap_state->schema_ver )
	{
		case SCHEMAVER_SAMBASAMACCOUNT:
			attr_list = get_userattr_list(ldap_state->schema_ver);
			rc = ldapsam_search_suffix_by_sid(ldap_state, sid, result, attr_list);
			free_attr_list( attr_list );

			if ( rc != LDAP_SUCCESS ) 
				return rc;
			break;
			
		case SCHEMAVER_SAMBAACCOUNT:
			if (!sid_peek_check_rid(&ldap_state->domain_sid, sid, &rid)) {
				return rc;
			}
		
			attr_list = get_userattr_list(ldap_state->schema_ver);
			rc = ldapsam_search_suffix_by_rid(ldap_state, rid, result, attr_list );
			free_attr_list( attr_list );

			if ( rc != LDAP_SUCCESS ) 
				return rc;
			break;
	}
	return rc;
}

/**********************************************************************
Get SAM_ACCOUNT entry from LDAP by SID
*********************************************************************/
static NTSTATUS ldapsam_getsampwsid(struct pdb_methods *my_methods, SAM_ACCOUNT * user, const DOM_SID *sid)
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	LDAPMessage *result;
	LDAPMessage *entry;
	int count;
	int rc;
	fstring sid_string;

	rc = ldapsam_get_ldap_user_by_sid(ldap_state, 
					  sid, &result); 
	if (rc != LDAP_SUCCESS)
		return NT_STATUS_NO_SUCH_USER;

	count = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);
	
	if (count < 1) 
	{
		DEBUG(4,
		      ("Unable to locate SID [%s] count=%d\n", sid_to_string(sid_string, sid),
		       count));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_USER;
	}  
	else if (count > 1) 
	{
		DEBUG(1,
		      ("More than one user with SID [%s]. Failing. count=%d\n", sid_to_string(sid_string, sid),
		       count));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_USER;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	if (!entry) 
	{
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_USER;
	}

	if (!init_sam_from_ldap(ldap_state, user, entry)) {
		DEBUG(1,("ldapsam_getsampwrid: init_sam_from_ldap failed!\n"));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_USER;
	}

	pdb_set_backend_private_data(user, result, 
				     private_data_free_fn, 
				     my_methods, PDB_CHANGED);
	return NT_STATUS_OK;
}	

/********************************************************************
Do the actual modification - also change a plaintext passord if 
it it set.
**********************************************************************/

static NTSTATUS ldapsam_modify_entry(struct pdb_methods *my_methods, 
				     SAM_ACCOUNT *newpwd, char *dn,
				     LDAPMod **mods, int ldap_op, 
				     BOOL (*need_update)(const SAM_ACCOUNT *,
							 enum pdb_elements))
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	int rc;
	
	if (!my_methods || !newpwd || !dn) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	
	if (!mods) {
		DEBUG(5,("mods is empty: nothing to modify\n"));
		/* may be password change below however */
	} else {
		switch(ldap_op)
		{
			case LDAP_MOD_ADD: 
				smbldap_set_mod(&mods, LDAP_MOD_ADD, 
						"objectclass", 
						LDAP_OBJ_ACCOUNT);
				rc = smbldap_add(ldap_state->smbldap_state, 
						 dn, mods);
				break;
			case LDAP_MOD_REPLACE: 
				rc = smbldap_modify(ldap_state->smbldap_state, 
						    dn ,mods);
				break;
			default: 	
				DEBUG(0,("Wrong LDAP operation type: %d!\n", 
					 ldap_op));
				return NT_STATUS_INVALID_PARAMETER;
		}
		
		if (rc!=LDAP_SUCCESS) {
			char *ld_error = NULL;
			ldap_get_option(ldap_state->smbldap_state->ldap_struct, LDAP_OPT_ERROR_STRING,
					&ld_error);
			DEBUG(1,
			      ("failed to %s user dn= %s with: %s\n\t%s\n",
			       ldap_op == LDAP_MOD_ADD ? "add" : "modify",
			       dn, ldap_err2string(rc),
			       ld_error?ld_error:"unknown"));
			SAFE_FREE(ld_error);
			return NT_STATUS_UNSUCCESSFUL;
		}  
	}
	
	if (!(pdb_get_acct_ctrl(newpwd)&(ACB_WSTRUST|ACB_SVRTRUST|ACB_DOMTRUST)) &&
		(lp_ldap_passwd_sync() != LDAP_PASSWD_SYNC_OFF) &&
		need_update(newpwd, PDB_PLAINTEXT_PW) &&
		(pdb_get_plaintext_passwd(newpwd)!=NULL)) {
		BerElement *ber;
		struct berval *bv;
		char *retoid;
		struct berval *retdata;
		char *utf8_password;
		char *utf8_dn;

		if (push_utf8_allocate(&utf8_password, pdb_get_plaintext_passwd(newpwd)) == (size_t)-1) {
			return NT_STATUS_NO_MEMORY;
		}

		if (push_utf8_allocate(&utf8_dn, dn) == (size_t)-1) {
			return NT_STATUS_NO_MEMORY;
		}

		if ((ber = ber_alloc_t(LBER_USE_DER))==NULL) {
			DEBUG(0,("ber_alloc_t returns NULL\n"));
			SAFE_FREE(utf8_password);
			return NT_STATUS_UNSUCCESSFUL;
		}

		ber_printf (ber, "{");
		ber_printf (ber, "ts", LDAP_TAG_EXOP_MODIFY_PASSWD_ID, utf8_dn);
	        ber_printf (ber, "ts", LDAP_TAG_EXOP_MODIFY_PASSWD_NEW, utf8_password);
	        ber_printf (ber, "N}");

	        if ((rc = ber_flatten (ber, &bv))<0) {
			DEBUG(0,("ber_flatten returns a value <0\n"));
			ber_free(ber,1);
			SAFE_FREE(utf8_dn);
			SAFE_FREE(utf8_password);
			return NT_STATUS_UNSUCCESSFUL;
		}
		
		SAFE_FREE(utf8_dn);
		SAFE_FREE(utf8_password);
		ber_free(ber, 1);

		if ((rc = smbldap_extended_operation(ldap_state->smbldap_state, 
						     LDAP_EXOP_MODIFY_PASSWD,
						     bv, NULL, NULL, &retoid, 
						     &retdata)) != LDAP_SUCCESS) {
			DEBUG(0,("LDAP Password could not be changed for user %s: %s\n",
				pdb_get_username(newpwd),ldap_err2string(rc)));
		} else {
			DEBUG(3,("LDAP Password changed for user %s\n",pdb_get_username(newpwd)));
#ifdef DEBUG_PASSWORD
			DEBUG(100,("LDAP Password changed to %s\n",pdb_get_plaintext_passwd(newpwd)));
#endif    
			ber_bvfree(retdata);
			ber_memfree(retoid);
		}
		ber_bvfree(bv);
	}
	return NT_STATUS_OK;
}

/**********************************************************************
Delete entry from LDAP for username 
*********************************************************************/
static NTSTATUS ldapsam_delete_sam_account(struct pdb_methods *my_methods, SAM_ACCOUNT * sam_acct)
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	const char *sname;
	int rc;
	LDAPMessage *result;
	NTSTATUS ret;
	char **attr_list;
	fstring objclass;

	if (!sam_acct) {
		DEBUG(0, ("sam_acct was NULL!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	sname = pdb_get_username(sam_acct);

	DEBUG (3, ("Deleting user %s from LDAP.\n", sname));

	attr_list= get_userattr_list( ldap_state->schema_ver );
	rc = ldapsam_search_suffix_by_name(ldap_state, sname, &result, attr_list);

	if (rc != LDAP_SUCCESS)  {
		free_attr_list( attr_list );
		return NT_STATUS_NO_SUCH_USER;
	}
	
	switch ( ldap_state->schema_ver )
	{
		case SCHEMAVER_SAMBASAMACCOUNT:
			fstrcpy( objclass, LDAP_OBJ_SAMBASAMACCOUNT );
			break;
			
		case SCHEMAVER_SAMBAACCOUNT:
			fstrcpy( objclass, LDAP_OBJ_SAMBAACCOUNT );
			break;
		default:
			fstrcpy( objclass, "UNKNOWN" );
			DEBUG(0,("ldapsam_delete_sam_account: Unknown schema version specified!\n"));
				break;
	}

	ret = ldapsam_delete_entry(ldap_state, result, objclass, attr_list );
	ldap_msgfree(result);
	free_attr_list( attr_list );

	return ret;
}

/**********************************************************************
  Helper function to determine for update_sam_account whether
  we need LDAP modification.
*********************************************************************/
static BOOL element_is_changed(const SAM_ACCOUNT *sampass,
			       enum pdb_elements element)
{
	return IS_SAM_CHANGED(sampass, element);
}

/**********************************************************************
Update SAM_ACCOUNT 
*********************************************************************/
static NTSTATUS ldapsam_update_sam_account(struct pdb_methods *my_methods, SAM_ACCOUNT * newpwd)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	int rc;
	char *dn;
	LDAPMessage *result;
	LDAPMessage *entry;
	LDAPMod **mods;
	char **attr_list;

	result = pdb_get_backend_private_data(newpwd, my_methods);
	if (!result) {
		attr_list = get_userattr_list(ldap_state->schema_ver);
		rc = ldapsam_search_suffix_by_name(ldap_state, pdb_get_username(newpwd), &result, attr_list );
		free_attr_list( attr_list );
		if (rc != LDAP_SUCCESS) {
			return NT_STATUS_UNSUCCESSFUL;
		}
		pdb_set_backend_private_data(newpwd, result, private_data_free_fn, my_methods, PDB_CHANGED);
	}

	if (ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result) == 0) {
		DEBUG(0, ("No user to modify!\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	dn = ldap_get_dn(ldap_state->smbldap_state->ldap_struct, entry);

	DEBUG(4, ("user %s to be modified has dn: %s\n", pdb_get_username(newpwd), dn));

	if (!init_ldap_from_sam(ldap_state, entry, &mods, newpwd,
				element_is_changed)) {
		DEBUG(0, ("ldapsam_update_sam_account: init_ldap_from_sam failed!\n"));
		ldap_memfree(dn);
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	if (mods == NULL) {
		DEBUG(4,("mods is empty: nothing to update for user: %s\n",
			 pdb_get_username(newpwd)));
		ldap_mods_free(mods, True);
		ldap_memfree(dn);
		return NT_STATUS_OK;
	}
	
	ret = ldapsam_modify_entry(my_methods,newpwd,dn,mods,LDAP_MOD_REPLACE, element_is_changed);
	ldap_mods_free(mods,True);
	ldap_memfree(dn);

	if (!NT_STATUS_IS_OK(ret)) {
		char *ld_error = NULL;
		ldap_get_option(ldap_state->smbldap_state->ldap_struct, LDAP_OPT_ERROR_STRING,
				&ld_error);
		DEBUG(0,("failed to modify user with uid = %s, error: %s (%s)\n",
			 pdb_get_username(newpwd), ld_error?ld_error:"(unknwon)", ldap_err2string(rc)));
		SAFE_FREE(ld_error);
		return ret;
	}

	DEBUG(2, ("successfully modified uid = %s in the LDAP database\n",
		  pdb_get_username(newpwd)));
	return NT_STATUS_OK;
}

/**********************************************************************
  Helper function to determine for update_sam_account whether
  we need LDAP modification.
 *********************************************************************/
static BOOL element_is_set_or_changed(const SAM_ACCOUNT *sampass,
				      enum pdb_elements element)
{
	return (IS_SAM_SET(sampass, element) ||
		IS_SAM_CHANGED(sampass, element));
}

/**********************************************************************
Add SAM_ACCOUNT to LDAP 
*********************************************************************/

static NTSTATUS ldapsam_add_sam_account(struct pdb_methods *my_methods, SAM_ACCOUNT * newpwd)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	int rc;
	LDAPMessage 	*result = NULL;
	LDAPMessage 	*entry  = NULL;
	pstring 	dn;
	LDAPMod 	**mods = NULL;
	int 		ldap_op;
	uint32		num_result;
	char 		**attr_list;
	char 		*escape_user;
	const char 	*username = pdb_get_username(newpwd);
	const DOM_SID 	*sid = pdb_get_user_sid(newpwd);
	pstring		filter;
	fstring         sid_string;

	if (!username || !*username) {
		DEBUG(0, ("Cannot add user without a username!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	/* free this list after the second search or in case we exit on failure */
	attr_list = get_userattr_list(ldap_state->schema_ver);

	rc = ldapsam_search_suffix_by_name (ldap_state, username, &result, attr_list);

	if (rc != LDAP_SUCCESS) {
		free_attr_list( attr_list );
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result) != 0) {
		DEBUG(0,("User '%s' already in the base, with samba attributes\n", 
			 username));
		ldap_msgfree(result);
		free_attr_list( attr_list );
		return NT_STATUS_UNSUCCESSFUL;
	}
	ldap_msgfree(result);
	result = NULL;

	if (element_is_set_or_changed(newpwd, PDB_USERSID)) {
		rc = ldapsam_get_ldap_user_by_sid(ldap_state, 
						  sid, &result); 
		if (rc == LDAP_SUCCESS) {
			if (ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result) != 0) {
				DEBUG(0,("SID '%s' already in the base, with samba attributes\n", 
					 sid_to_string(sid_string, sid)));
				free_attr_list( attr_list );
				return NT_STATUS_UNSUCCESSFUL;
			}
			ldap_msgfree(result);
		}
	}

	/* does the entry already exist but without a samba attributes?
	   we need to return the samba attributes here */
	   
	escape_user = escape_ldap_string_alloc( username );
	pstrcpy( filter, lp_ldap_filter() );
	all_string_sub( filter, "%u", escape_user, sizeof(filter) );
	SAFE_FREE( escape_user );

	rc = smbldap_search_suffix(ldap_state->smbldap_state, 
				   filter, attr_list, &result);
	if ( rc != LDAP_SUCCESS ) {
		free_attr_list( attr_list );
		return NT_STATUS_UNSUCCESSFUL;
	}

	num_result = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);
	
	if (num_result > 1) {
		DEBUG (0, ("More than one user with that uid exists: bailing out!\n"));
		free_attr_list( attr_list );
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	/* Check if we need to update an existing entry */
	if (num_result == 1) {
		char *tmp;
		
		DEBUG(3,("User exists without samba attributes: adding them\n"));
		ldap_op = LDAP_MOD_REPLACE;
		entry = ldap_first_entry (ldap_state->smbldap_state->ldap_struct, result);
		tmp = ldap_get_dn (ldap_state->smbldap_state->ldap_struct, entry);
		slprintf (dn, sizeof (dn) - 1, "%s", tmp);
		ldap_memfree (tmp);

	} else if (ldap_state->schema_ver == SCHEMAVER_SAMBASAMACCOUNT) {

		/* There might be a SID for this account already - say an idmap entry */

		pstr_sprintf(filter, "(&(%s=%s)(|(objectClass=%s)(objectClass=%s)))", 
			 get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_SID),
			 sid_to_string(sid_string, sid),
			 LDAP_OBJ_IDMAP_ENTRY,
			 LDAP_OBJ_SID_ENTRY);
		
		rc = smbldap_search_suffix(ldap_state->smbldap_state, 
					   filter, attr_list, &result);
			
		if ( rc != LDAP_SUCCESS ) {
			free_attr_list( attr_list );
			return NT_STATUS_UNSUCCESSFUL;
		}
		
		num_result = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);
		
		if (num_result > 1) {
			DEBUG (0, ("More than one user with that uid exists: bailing out!\n"));
			free_attr_list( attr_list );
			ldap_msgfree(result);
			return NT_STATUS_UNSUCCESSFUL;
		}
		
		/* Check if we need to update an existing entry */
		if (num_result == 1) {
			char *tmp;
			
			DEBUG(3,("User exists without samba attributes: adding them\n"));
			ldap_op = LDAP_MOD_REPLACE;
			entry = ldap_first_entry (ldap_state->smbldap_state->ldap_struct, result);
			tmp = ldap_get_dn (ldap_state->smbldap_state->ldap_struct, entry);
			slprintf (dn, sizeof (dn) - 1, "%s", tmp);
			ldap_memfree (tmp);
		}
	}
	
	free_attr_list( attr_list );

	if (num_result == 0) {
		/* Check if we need to add an entry */
		DEBUG(3,("Adding new user\n"));
		ldap_op = LDAP_MOD_ADD;
		if (username[strlen(username)-1] == '$') {
			slprintf (dn, sizeof (dn) - 1, "uid=%s,%s", username, lp_ldap_machine_suffix ());
		} else {
			slprintf (dn, sizeof (dn) - 1, "uid=%s,%s", username, lp_ldap_user_suffix ());
		}
	}

	if (!init_ldap_from_sam(ldap_state, entry, &mods, newpwd,
				element_is_set_or_changed)) {
		DEBUG(0, ("ldapsam_add_sam_account: init_ldap_from_sam failed!\n"));
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;		
	}
	
	ldap_msgfree(result);

	if (mods == NULL) {
		DEBUG(0,("mods is empty: nothing to add for user: %s\n",pdb_get_username(newpwd)));
		return NT_STATUS_UNSUCCESSFUL;
	}
	switch ( ldap_state->schema_ver )
	{
		case SCHEMAVER_SAMBAACCOUNT:
			smbldap_set_mod(&mods, LDAP_MOD_ADD, "objectclass", LDAP_OBJ_SAMBAACCOUNT);
			break;
		case SCHEMAVER_SAMBASAMACCOUNT:
			smbldap_set_mod(&mods, LDAP_MOD_ADD, "objectclass", LDAP_OBJ_SAMBASAMACCOUNT);
			break;
		default:
			DEBUG(0,("ldapsam_add_sam_account: invalid schema version specified\n"));
			break;
	}

	ret = ldapsam_modify_entry(my_methods,newpwd,dn,mods,ldap_op, element_is_set_or_changed);
	if (!NT_STATUS_IS_OK(ret)) {
		DEBUG(0,("failed to modify/add user with uid = %s (dn = %s)\n",
			 pdb_get_username(newpwd),dn));
		ldap_mods_free(mods, True);
		return ret;
	}

	DEBUG(2,("added: uid == %s in the LDAP database\n", pdb_get_username(newpwd)));
	ldap_mods_free(mods, True);
	
	return NT_STATUS_OK;
}

/**********************************************************************
 *********************************************************************/

static int ldapsam_search_one_group (struct ldapsam_privates *ldap_state,
				     const char *filter,
				     LDAPMessage ** result)
{
	int scope = LDAP_SCOPE_SUBTREE;
	int rc;
	char **attr_list;

	DEBUG(2, ("ldapsam_search_one_group: searching for:[%s]\n", filter));


	attr_list = get_attr_list(groupmap_attr_list);
	rc = smbldap_search(ldap_state->smbldap_state, 
			    lp_ldap_group_suffix (), scope,
			    filter, attr_list, 0, result);
	free_attr_list( attr_list );

	if (rc != LDAP_SUCCESS) {
		char *ld_error = NULL;
		ldap_get_option(ldap_state->smbldap_state->ldap_struct, LDAP_OPT_ERROR_STRING,
				&ld_error);
		DEBUG(0, ("ldapsam_search_one_group: "
			  "Problem during the LDAP search: LDAP error: %s (%s)",
			  ld_error?ld_error:"(unknown)", ldap_err2string(rc)));
		DEBUG(3, ("ldapsam_search_one_group: Query was: %s, %s\n",
			  lp_ldap_group_suffix(), filter));
		SAFE_FREE(ld_error);
	}

	return rc;
}

/**********************************************************************
 *********************************************************************/

static BOOL init_group_from_ldap(struct ldapsam_privates *ldap_state,
				 GROUP_MAP *map, LDAPMessage *entry)
{
	pstring temp;

	if (ldap_state == NULL || map == NULL || entry == NULL ||
	    ldap_state->smbldap_state->ldap_struct == NULL) 
	{
		DEBUG(0, ("init_group_from_ldap: NULL parameters found!\n"));
		return False;
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_GIDNUMBER), temp)) 
	{
		DEBUG(0, ("Mandatory attribute %s not found\n", 
			get_attr_key2string( groupmap_attr_list, LDAP_ATTR_GIDNUMBER)));
		return False;
	}
	DEBUG(2, ("Entry found for group: %s\n", temp));

	map->gid = (gid_t)atol(temp);

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_attr_key2string( groupmap_attr_list, LDAP_ATTR_GROUP_SID), temp)) 
	{
		DEBUG(0, ("Mandatory attribute %s not found\n",
			get_attr_key2string( groupmap_attr_list, LDAP_ATTR_GROUP_SID)));
		return False;
	}
	string_to_sid(&map->sid, temp);

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_attr_key2string( groupmap_attr_list, LDAP_ATTR_GROUP_TYPE), temp)) 
	{
		DEBUG(0, ("Mandatory attribute %s not found\n",
			get_attr_key2string( groupmap_attr_list, LDAP_ATTR_GROUP_TYPE)));
		return False;
	}
	map->sid_name_use = (enum SID_NAME_USE)atol(temp);

	if ((map->sid_name_use < SID_NAME_USER) ||
	    (map->sid_name_use > SID_NAME_UNKNOWN)) {
		DEBUG(0, ("Unknown Group type: %d\n", map->sid_name_use));
		return False;
	}

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_attr_key2string( groupmap_attr_list, LDAP_ATTR_DISPLAY_NAME), temp)) 
	{
		temp[0] = '\0';
		if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
			get_attr_key2string( groupmap_attr_list, LDAP_ATTR_CN), temp)) 
		{
			DEBUG(0, ("Attributes cn not found either "
				  "for gidNumber(%lu)\n",(unsigned long)map->gid));
			return False;
		}
	}
	fstrcpy(map->nt_name, temp);

	if (!smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
		get_attr_key2string( groupmap_attr_list, LDAP_ATTR_DESC), temp)) 
	{
		temp[0] = '\0';
	}
	fstrcpy(map->comment, temp);

	return True;
}

/**********************************************************************
 *********************************************************************/

static BOOL init_ldap_from_group(LDAP *ldap_struct,
				 LDAPMessage *existing,
				 LDAPMod ***mods,
				 const GROUP_MAP *map)
{
	pstring tmp;

	if (mods == NULL || map == NULL) {
		DEBUG(0, ("init_ldap_from_group: NULL parameters found!\n"));
		return False;
	}

	*mods = NULL;

	sid_to_string(tmp, &map->sid);
	smbldap_make_mod(ldap_struct, existing, mods, 
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_GROUP_SID), tmp);
	pstr_sprintf(tmp, "%i", map->sid_name_use);
	smbldap_make_mod(ldap_struct, existing, mods, 
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_GROUP_TYPE), tmp);

	smbldap_make_mod(ldap_struct, existing, mods, 
		get_attr_key2string( groupmap_attr_list, LDAP_ATTR_DISPLAY_NAME), map->nt_name);
	smbldap_make_mod(ldap_struct, existing, mods, 
		get_attr_key2string( groupmap_attr_list, LDAP_ATTR_DESC), map->comment);

	return True;
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_getgroup(struct pdb_methods *methods,
				 const char *filter,
				 GROUP_MAP *map)
{
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	LDAPMessage *result;
	LDAPMessage *entry;
	int count;

	if (ldapsam_search_one_group(ldap_state, filter, &result)
	    != LDAP_SUCCESS) {
		return NT_STATUS_NO_SUCH_GROUP;
	}

	count = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);

	if (count < 1) {
		DEBUG(4, ("Did not find group\n"));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_GROUP;
	}

	if (count > 1) {
		DEBUG(1, ("Duplicate entries for filter %s: count=%d\n",
			  filter, count));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_GROUP;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);

	if (!entry) {
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (!init_group_from_ldap(ldap_state, map, entry)) {
		DEBUG(1, ("init_group_from_ldap failed for group filter %s\n",
			  filter));
		ldap_msgfree(result);
		return NT_STATUS_NO_SUCH_GROUP;
	}

	ldap_msgfree(result);
	return NT_STATUS_OK;
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_getgrsid(struct pdb_methods *methods, GROUP_MAP *map,
				 DOM_SID sid)
{
	pstring filter;

	pstr_sprintf(filter, "(&(objectClass=%s)(%s=%s))",
		LDAP_OBJ_GROUPMAP, 
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_GROUP_SID),
		sid_string_static(&sid));

	return ldapsam_getgroup(methods, filter, map);
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_getgrgid(struct pdb_methods *methods, GROUP_MAP *map,
				 gid_t gid)
{
	pstring filter;

	pstr_sprintf(filter, "(&(objectClass=%s)(%s=%lu))",
		LDAP_OBJ_GROUPMAP,
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_GIDNUMBER),
		(unsigned long)gid);

	return ldapsam_getgroup(methods, filter, map);
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_getgrnam(struct pdb_methods *methods, GROUP_MAP *map,
				 const char *name)
{
	pstring filter;
	char *escape_name = escape_ldap_string_alloc(name);

	if (!escape_name) {
		return NT_STATUS_NO_MEMORY;
	}

	pstr_sprintf(filter, "(&(objectClass=%s)(|(%s=%s)(%s=%s)))",
		LDAP_OBJ_GROUPMAP,
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_DISPLAY_NAME), escape_name,
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_CN), escape_name);

	SAFE_FREE(escape_name);

	return ldapsam_getgroup(methods, filter, map);
}

/**********************************************************************
 *********************************************************************/

static int ldapsam_search_one_group_by_gid(struct ldapsam_privates *ldap_state,
					   gid_t gid,
					   LDAPMessage **result)
{
	pstring filter;

	pstr_sprintf(filter, "(&(objectClass=%s)(%s=%lu))", 
		LDAP_OBJ_POSIXGROUP,
		get_attr_key2string(groupmap_attr_list, LDAP_ATTR_GIDNUMBER),
		(unsigned long)gid);

	return ldapsam_search_one_group(ldap_state, filter, result);
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_add_group_mapping_entry(struct pdb_methods *methods,
						GROUP_MAP *map)
{
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	LDAPMessage *result = NULL;
	LDAPMod **mods = NULL;
	int count;

	char *tmp;
	pstring dn;
	LDAPMessage *entry;

	GROUP_MAP dummy;

	int rc;

	if (NT_STATUS_IS_OK(ldapsam_getgrgid(methods, &dummy,
					     map->gid))) {
		DEBUG(0, ("Group %ld already exists in LDAP\n", (unsigned long)map->gid));
		return NT_STATUS_UNSUCCESSFUL;
	}

	rc = ldapsam_search_one_group_by_gid(ldap_state, map->gid, &result);
	if (rc != LDAP_SUCCESS) {
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	count = ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result);

	if ( count == 0 ) {
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (count > 1) {
		DEBUG(2, ("Group %lu must exist exactly once in LDAP\n",
			  (unsigned long)map->gid));
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	tmp = ldap_get_dn(ldap_state->smbldap_state->ldap_struct, entry);
	pstrcpy(dn, tmp);
	ldap_memfree(tmp);

	if (!init_ldap_from_group(ldap_state->smbldap_state->ldap_struct,
				  result, &mods, map)) {
		DEBUG(0, ("init_ldap_from_group failed!\n"));
		ldap_mods_free(mods, True);
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	ldap_msgfree(result);

	if (mods == NULL) {
		DEBUG(0, ("mods is empty\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	smbldap_set_mod(&mods, LDAP_MOD_ADD, "objectClass", LDAP_OBJ_GROUPMAP );

	rc = smbldap_modify(ldap_state->smbldap_state, dn, mods);
	ldap_mods_free(mods, True);

	if (rc != LDAP_SUCCESS) {
		char *ld_error = NULL;
		ldap_get_option(ldap_state->smbldap_state->ldap_struct, LDAP_OPT_ERROR_STRING,
				&ld_error);
		DEBUG(0, ("failed to add group %lu error: %s (%s)\n", (unsigned long)map->gid, 
			  ld_error ? ld_error : "(unknown)", ldap_err2string(rc)));
		SAFE_FREE(ld_error);
		return NT_STATUS_UNSUCCESSFUL;
	}

	DEBUG(2, ("successfully modified group %lu in LDAP\n", (unsigned long)map->gid));
	return NT_STATUS_OK;
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_update_group_mapping_entry(struct pdb_methods *methods,
						   GROUP_MAP *map)
{
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	int rc;
	char *dn;
	LDAPMessage *result;
	LDAPMessage *entry;
	LDAPMod **mods;

	rc = ldapsam_search_one_group_by_gid(ldap_state, map->gid, &result);

	if (rc != LDAP_SUCCESS) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (ldap_count_entries(ldap_state->smbldap_state->ldap_struct, result) == 0) {
		DEBUG(0, ("No group to modify!\n"));
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	dn = ldap_get_dn(ldap_state->smbldap_state->ldap_struct, entry);

	if (!init_ldap_from_group(ldap_state->smbldap_state->ldap_struct,
				  result, &mods, map)) {
		DEBUG(0, ("init_ldap_from_group failed\n"));
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	ldap_msgfree(result);

	if (mods == NULL) {
		DEBUG(4, ("mods is empty: nothing to do\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	rc = smbldap_modify(ldap_state->smbldap_state, dn, mods);

	ldap_mods_free(mods, True);

	if (rc != LDAP_SUCCESS) {
		char *ld_error = NULL;
		ldap_get_option(ldap_state->smbldap_state->ldap_struct, LDAP_OPT_ERROR_STRING,
				&ld_error);
		DEBUG(0, ("failed to modify group %lu error: %s (%s)\n", (unsigned long)map->gid, 
			  ld_error ? ld_error : "(unknown)", ldap_err2string(rc)));
		SAFE_FREE(ld_error);
	}

	DEBUG(2, ("successfully modified group %lu in LDAP\n", (unsigned long)map->gid));
	return NT_STATUS_OK;
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_delete_group_mapping_entry(struct pdb_methods *methods,
						   DOM_SID sid)
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)methods->private_data;
	pstring sidstring, filter;
	LDAPMessage *result;
	int rc;
	NTSTATUS ret;
	char **attr_list;

	sid_to_string(sidstring, &sid);
	
	pstr_sprintf(filter, "(&(objectClass=%s)(%s=%s))", 
		LDAP_OBJ_GROUPMAP, LDAP_ATTRIBUTE_SID, sidstring);

	rc = ldapsam_search_one_group(ldap_state, filter, &result);

	if (rc != LDAP_SUCCESS) {
		return NT_STATUS_NO_SUCH_GROUP;
	}

	attr_list = get_attr_list( groupmap_attr_list_to_delete );
	ret = ldapsam_delete_entry(ldap_state, result, LDAP_OBJ_GROUPMAP, attr_list);
	free_attr_list ( attr_list );

	ldap_msgfree(result);

	return ret;
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_setsamgrent(struct pdb_methods *my_methods, BOOL update)
{
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	fstring filter;
	int rc;
	char **attr_list;

	pstr_sprintf( filter, "(objectclass=%s)", LDAP_OBJ_GROUPMAP);
	attr_list = get_attr_list( groupmap_attr_list );
	rc = smbldap_search(ldap_state->smbldap_state, lp_ldap_group_suffix(),
			    LDAP_SCOPE_SUBTREE, filter,
			    attr_list, 0, &ldap_state->result);
	free_attr_list( attr_list );

	if (rc != LDAP_SUCCESS) {
		DEBUG(0, ("LDAP search failed: %s\n", ldap_err2string(rc)));
		DEBUG(3, ("Query was: %s, %s\n", lp_ldap_group_suffix(), filter));
		ldap_msgfree(ldap_state->result);
		ldap_state->result = NULL;
		return NT_STATUS_UNSUCCESSFUL;
	}

	DEBUG(2, ("ldapsam_setsampwent: %d entries in the base!\n",
		  ldap_count_entries(ldap_state->smbldap_state->ldap_struct,
				     ldap_state->result)));

	ldap_state->entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, ldap_state->result);
	ldap_state->index = 0;

	return NT_STATUS_OK;
}

/**********************************************************************
 *********************************************************************/

static void ldapsam_endsamgrent(struct pdb_methods *my_methods)
{
	ldapsam_endsampwent(my_methods);
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_getsamgrent(struct pdb_methods *my_methods,
				    GROUP_MAP *map)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)my_methods->private_data;
	BOOL bret = False;

	while (!bret) {
		if (!ldap_state->entry)
			return ret;
		
		ldap_state->index++;
		bret = init_group_from_ldap(ldap_state, map, ldap_state->entry);
		
		ldap_state->entry = ldap_next_entry(ldap_state->smbldap_state->ldap_struct,
					    ldap_state->entry);	
	}

	return NT_STATUS_OK;
}

/**********************************************************************
 *********************************************************************/

static NTSTATUS ldapsam_enum_group_mapping(struct pdb_methods *methods,
					   enum SID_NAME_USE sid_name_use,
					   GROUP_MAP **rmap, int *num_entries,
					   BOOL unix_only)
{
	GROUP_MAP map;
	GROUP_MAP *mapt;
	int entries = 0;

	*num_entries = 0;
	*rmap = NULL;

	if (!NT_STATUS_IS_OK(ldapsam_setsamgrent(methods, False))) {
		DEBUG(0, ("Unable to open passdb\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	while (NT_STATUS_IS_OK(ldapsam_getsamgrent(methods, &map))) {
		if (sid_name_use != SID_NAME_UNKNOWN &&
		    sid_name_use != map.sid_name_use) {
			DEBUG(11,("enum_group_mapping: group %s is not of the requested type\n", map.nt_name));
			continue;
		}
		if (unix_only==ENUM_ONLY_MAPPED && map.gid==-1) {
			DEBUG(11,("enum_group_mapping: group %s is non mapped\n", map.nt_name));
			continue;
		}

		mapt=(GROUP_MAP *)Realloc((*rmap), (entries+1)*sizeof(GROUP_MAP));
		if (!mapt) {
			DEBUG(0,("enum_group_mapping: Unable to enlarge group map!\n"));
			SAFE_FREE(*rmap);
			return NT_STATUS_UNSUCCESSFUL;
		}
		else
			(*rmap) = mapt;

		mapt[entries] = map;

		entries += 1;

	}
	ldapsam_endsamgrent(methods);

	*num_entries = entries;

	return NT_STATUS_OK;
}

/**********************************************************************
 Housekeeping
 *********************************************************************/

static void free_private_data(void **vp) 
{
	struct ldapsam_privates **ldap_state = (struct ldapsam_privates **)vp;

	smbldap_free_struct(&(*ldap_state)->smbldap_state);

	*ldap_state = NULL;

	/* No need to free any further, as it is talloc()ed */
}

/**********************************************************************
 Intitalise the parts of the pdb_context that are common to all pdb_ldap modes
 *********************************************************************/

static NTSTATUS pdb_init_ldapsam_common(PDB_CONTEXT *pdb_context, PDB_METHODS **pdb_method, 
					const char *location)
{
	NTSTATUS nt_status;
	struct ldapsam_privates *ldap_state;

	if (!NT_STATUS_IS_OK(nt_status = make_pdb_methods(pdb_context->mem_ctx, pdb_method))) {
		return nt_status;
	}

	(*pdb_method)->name = "ldapsam";

	(*pdb_method)->setsampwent = ldapsam_setsampwent;
	(*pdb_method)->endsampwent = ldapsam_endsampwent;
	(*pdb_method)->getsampwent = ldapsam_getsampwent;
	(*pdb_method)->getsampwnam = ldapsam_getsampwnam;
	(*pdb_method)->getsampwsid = ldapsam_getsampwsid;
	(*pdb_method)->add_sam_account = ldapsam_add_sam_account;
	(*pdb_method)->update_sam_account = ldapsam_update_sam_account;
	(*pdb_method)->delete_sam_account = ldapsam_delete_sam_account;

	(*pdb_method)->getgrsid = ldapsam_getgrsid;
	(*pdb_method)->getgrgid = ldapsam_getgrgid;
	(*pdb_method)->getgrnam = ldapsam_getgrnam;
	(*pdb_method)->add_group_mapping_entry = ldapsam_add_group_mapping_entry;
	(*pdb_method)->update_group_mapping_entry = ldapsam_update_group_mapping_entry;
	(*pdb_method)->delete_group_mapping_entry = ldapsam_delete_group_mapping_entry;
	(*pdb_method)->enum_group_mapping = ldapsam_enum_group_mapping;

	/* TODO: Setup private data and free */

	ldap_state = talloc_zero(pdb_context->mem_ctx, sizeof(*ldap_state));
	if (!ldap_state) {
		DEBUG(0, ("talloc() failed for ldapsam private_data!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	if (!NT_STATUS_IS_OK(nt_status = 
			     smbldap_init(pdb_context->mem_ctx, location, 
					  &ldap_state->smbldap_state)));

	ldap_state->domain_name = talloc_strdup(pdb_context->mem_ctx, get_global_sam_name());
	if (!ldap_state->domain_name) {
		return NT_STATUS_NO_MEMORY;
	}

	(*pdb_method)->private_data = ldap_state;

	(*pdb_method)->free_private_data = free_private_data;

	return NT_STATUS_OK;
}

/**********************************************************************
 Initialise the 'compat' mode for pdb_ldap
 *********************************************************************/

static NTSTATUS pdb_init_ldapsam_compat(PDB_CONTEXT *pdb_context, PDB_METHODS **pdb_method, const char *location)
{
	NTSTATUS nt_status;
	struct ldapsam_privates *ldap_state;

#ifdef WITH_LDAP_SAMCONFIG
	if (!location) {
		int ldap_port = lp_ldap_port();
			
		/* remap default port if not using SSL (ie clear or TLS) */
		if ( (lp_ldap_ssl() != LDAP_SSL_ON) && (ldap_port == 636) ) {
			ldap_port = 389;
		}

		location = talloc_asprintf(pdb_context->mem_ctx, "%s://%s:%d", lp_ldap_ssl() == LDAP_SSL_ON ? "ldaps" : "ldap", lp_ldap_server(), ldap_port);
		if (!location) {
			return NT_STATUS_NO_MEMORY;
		}
	}
#endif

	if (!NT_STATUS_IS_OK(nt_status = pdb_init_ldapsam_common(pdb_context, pdb_method, location))) {
		return nt_status;
	}

	(*pdb_method)->name = "ldapsam_compat";

	ldap_state = (*pdb_method)->private_data;
	ldap_state->schema_ver = SCHEMAVER_SAMBAACCOUNT;

	sid_copy(&ldap_state->domain_sid, get_global_sam_sid());

	return NT_STATUS_OK;
}

/**********************************************************************
 Initialise the normal mode for pdb_ldap
 *********************************************************************/

static NTSTATUS pdb_init_ldapsam(PDB_CONTEXT *pdb_context, PDB_METHODS **pdb_method, const char *location)
{
	NTSTATUS nt_status;
	struct ldapsam_privates *ldap_state;
	uint32 alg_rid_base;
	pstring alg_rid_base_string;
	LDAPMessage *result = NULL;
	LDAPMessage *entry = NULL;
	DOM_SID ldap_domain_sid;
	DOM_SID secrets_domain_sid;
	pstring domain_sid_string;

	if (!NT_STATUS_IS_OK(nt_status = pdb_init_ldapsam_common(pdb_context, pdb_method, location))) {
		return nt_status;
	}

	(*pdb_method)->name = "ldapsam";

	ldap_state = (*pdb_method)->private_data;
	ldap_state->schema_ver = SCHEMAVER_SAMBASAMACCOUNT;

	/* Try to setup the Domain Name, Domain SID, algorithmic rid base */
	
	nt_status = smbldap_search_domain_info(ldap_state->smbldap_state, &result, 
		ldap_state->domain_name, True);
	
	if ( !NT_STATUS_IS_OK(nt_status) ) {
		DEBUG(2, ("WARNING: Could not get domain info, nor add one to the domain\n"));
		DEBUGADD(2, ("Continuing on regardless, will be unable to allocate new users/groups, "
			"and will risk BDCs having inconsistant SIDs\n"));
		sid_copy(&ldap_state->domain_sid, get_global_sam_sid());
		return NT_STATUS_OK;
	}

	/* Given that the above might fail, everything below this must be optional */
	
	entry = ldap_first_entry(ldap_state->smbldap_state->ldap_struct, result);
	if (!entry) {
		DEBUG(0, ("Could not get domain info entry\n"));
		ldap_msgfree(result);
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
				 get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_USER_SID), 
				 domain_sid_string)) 
	{
		BOOL found_sid;
		string_to_sid(&ldap_domain_sid, domain_sid_string);
		found_sid = secrets_fetch_domain_sid(ldap_state->domain_name, &secrets_domain_sid);
		if (!found_sid || !sid_equal(&secrets_domain_sid, &ldap_domain_sid)) {
			/* reset secrets.tdb sid */
			secrets_store_domain_sid(ldap_state->domain_name, &ldap_domain_sid);
		}
		sid_copy(&ldap_state->domain_sid, &ldap_domain_sid);
	}

	if (smbldap_get_single_attribute(ldap_state->smbldap_state->ldap_struct, entry, 
				 get_userattr_key2string(ldap_state->schema_ver, LDAP_ATTR_ALGORITHMIC_RID_BASE), 
				 alg_rid_base_string)) 
	{
		alg_rid_base = (uint32)atol(alg_rid_base_string);
		if (alg_rid_base != algorithmic_rid_base()) {
			DEBUG(0, ("The value of 'algorithmic RID base' has changed since the LDAP\n"
				  "database was initialised.  Aborting. \n"));
			ldap_msgfree(result);
			return NT_STATUS_UNSUCCESSFUL;
		}
	}
	ldap_msgfree(result);

	return NT_STATUS_OK;
}

NTSTATUS pdb_ldap_init(void)
{
	NTSTATUS nt_status;
	if (!NT_STATUS_IS_OK(nt_status = smb_register_passdb(PASSDB_INTERFACE_VERSION, "ldapsam", pdb_init_ldapsam)))
		return nt_status;

	if (!NT_STATUS_IS_OK(nt_status = smb_register_passdb(PASSDB_INTERFACE_VERSION, "ldapsam_compat", pdb_init_ldapsam_compat)))
		return nt_status;

	return NT_STATUS_OK;
}


