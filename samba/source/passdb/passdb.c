/* 
   Unix SMB/CIFS implementation.
   Password and authentication handling
   Copyright (C) Jeremy Allison 		1996-2001
   Copyright (C) Luke Kenneth Casson Leighton 	1996-1998
   Copyright (C) Gerald (Jerry) Carter		2000-2001
   Copyright (C) Andrew Bartlett		2001-2002
      
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

#include "includes.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_PASSDB

/*
 * This is set on startup - it defines the SID for this
 * machine, and therefore the SAM database for which it is
 * responsible.
 */

/************************************************************
 Fill the SAM_ACCOUNT with default values.
 ***********************************************************/

static void pdb_fill_default_sam(SAM_ACCOUNT *user)
{
	ZERO_STRUCT(user->private); /* Don't touch the talloc context */

	/* no initial methods */
	user->methods = NULL;

        /* Don't change these timestamp settings without a good reason.
           They are important for NT member server compatibility. */

	user->private.uid = user->private.gid	    = -1;

	user->private.logon_time            = (time_t)0;
	user->private.pass_last_set_time    = (time_t)0;
	user->private.pass_can_change_time  = (time_t)0;
	user->private.logoff_time           = 
	user->private.kickoff_time          = 
	user->private.pass_must_change_time = get_time_t_max();
	user->private.unknown_3 = 0x00ffffff; 	/* don't know */
	user->private.logon_divs = 168; 	/* hours per week */
	user->private.hours_len = 21; 		/* 21 times 8 bits = 168 */
	memset(user->private.hours, 0xff, user->private.hours_len); /* available at all hours */
	user->private.unknown_5 = 0x00000000; /* don't know */
	user->private.unknown_6 = 0x000004ec; /* don't know */

	/* Some parts of samba strlen their pdb_get...() returns, 
	   so this keeps the interface unchanged for now. */
	   
	user->private.username = "";
	user->private.domain = "";
	user->private.nt_username = "";
	user->private.full_name = "";
	user->private.home_dir = "";
	user->private.logon_script = "";
	user->private.profile_path = "";
	user->private.acct_desc = "";
	user->private.workstations = "";
	user->private.unknown_str = "";
	user->private.munged_dial = "";

	user->private.plaintext_pw = NULL;

}	

static void destroy_pdb_talloc(SAM_ACCOUNT **user) 
{
	if (*user) {
		data_blob_clear_free(&((*user)->private.lm_pw));
		data_blob_clear_free(&((*user)->private.nt_pw));

		if((*user)->private.plaintext_pw!=NULL)
			memset((*user)->private.plaintext_pw,'\0',strlen((*user)->private.plaintext_pw));
		talloc_destroy((*user)->mem_ctx);
		*user = NULL;
	}
}


/**********************************************************************
 Alloc memory and initialises a struct sam_passwd on supplied mem_ctx.
***********************************************************************/

NTSTATUS pdb_init_sam_talloc(TALLOC_CTX *mem_ctx, SAM_ACCOUNT **user)
{
	if (*user != NULL) {
		DEBUG(0,("pdb_init_sam_talloc: SAM_ACCOUNT was non NULL\n"));
#if 0
		smb_panic("non-NULL pointer passed to pdb_init_sam\n");
#endif
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (!mem_ctx) {
		DEBUG(0,("pdb_init_sam_talloc: mem_ctx was NULL!\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	*user=(SAM_ACCOUNT *)talloc(mem_ctx, sizeof(SAM_ACCOUNT));

	if (*user==NULL) {
		DEBUG(0,("pdb_init_sam_talloc: error while allocating memory\n"));
		return NT_STATUS_NO_MEMORY;
	}

	(*user)->mem_ctx = mem_ctx;

	(*user)->free_fn = NULL;

	pdb_fill_default_sam(*user);
	
	return NT_STATUS_OK;
}


/*************************************************************
 Alloc memory and initialises a struct sam_passwd.
 ************************************************************/

NTSTATUS pdb_init_sam(SAM_ACCOUNT **user)
{
	TALLOC_CTX *mem_ctx;
	NTSTATUS nt_status;
	
	mem_ctx = talloc_init("passdb internal SAM_ACCOUNT allocation");

	if (!mem_ctx) {
		DEBUG(0,("pdb_init_sam: error while doing talloc_init()\n"));
		return NT_STATUS_NO_MEMORY;
	}

	if (!NT_STATUS_IS_OK(nt_status = pdb_init_sam_talloc(mem_ctx, user))) {
		talloc_destroy(mem_ctx);
		return nt_status;
	}
	
	(*user)->free_fn = destroy_pdb_talloc;

	return NT_STATUS_OK;
}


/*************************************************************
 Initialises a struct sam_passwd with sane values.
 ************************************************************/

NTSTATUS pdb_fill_sam_pw(SAM_ACCOUNT *sam_account, const struct passwd *pwd)
{
	GROUP_MAP map;

	const char *guest_account = lp_guestaccount();
	if (!(guest_account && *guest_account)) {
		DEBUG(1, ("NULL guest account!?!?\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (!pwd) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	pdb_fill_default_sam(sam_account);

	pdb_set_username(sam_account, pwd->pw_name, PDB_SET);
	pdb_set_fullname(sam_account, pwd->pw_gecos, PDB_SET);

	pdb_set_unix_homedir(sam_account, pwd->pw_dir, PDB_SET);

	pdb_set_domain (sam_account, lp_workgroup(), PDB_DEFAULT);

	pdb_set_uid(sam_account, pwd->pw_uid, PDB_SET);
	pdb_set_gid(sam_account, pwd->pw_gid, PDB_SET);
	
	/* When we get a proper uid -> SID and SID -> uid allocation
	   mechinism, we should call it here.  
	   
	   We can't just set this to 0 or allow it only to be filled
	   in when added to the backend, becouse the user's SID 
	   may already be in security descriptors etc.
	   
	   -- abartlet 11-May-02
	*/


	/* Ensure this *must* be set right */
	if (strcmp(pwd->pw_name, guest_account) == 0) {
		if (!pdb_set_user_sid_from_rid(sam_account, DOMAIN_USER_RID_GUEST, PDB_DEFAULT)) {
			return NT_STATUS_UNSUCCESSFUL;
		}
		if (!pdb_set_group_sid_from_rid(sam_account, DOMAIN_GROUP_RID_GUESTS, PDB_DEFAULT)) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	} else {

		if (!pdb_set_user_sid_from_rid(sam_account, 
					       fallback_pdb_uid_to_user_rid(pwd->pw_uid), PDB_SET)) {
			DEBUG(0,("Can't set User SID from RID!\n"));
			return NT_STATUS_INVALID_PARAMETER;
		}
		
		/* call the mapping code here */
		if(pdb_getgrgid(&map, pwd->pw_gid, MAPPING_WITHOUT_PRIV)) {
			if (!pdb_set_group_sid(sam_account,&map.sid, PDB_SET)){
				DEBUG(0,("Can't set Group SID!\n"));
				return NT_STATUS_INVALID_PARAMETER;
			}
		} 
		else {
			if (!pdb_set_group_sid_from_rid(sam_account,pdb_gid_to_group_rid(pwd->pw_gid), PDB_SET)) {
				DEBUG(0,("Can't set Group SID\n"));
				return NT_STATUS_INVALID_PARAMETER;
			}
		}
	}

	/* check if this is a user account or a machine account */
	if (pwd->pw_name[strlen(pwd->pw_name)-1] != '$')
	{
		pdb_set_profile_path(sam_account, 
				     talloc_sub_specified((sam_account)->mem_ctx, 
							    lp_logon_path(), 
							    pwd->pw_name, global_myname(), 
							    pwd->pw_uid, pwd->pw_gid), 
				     PDB_DEFAULT);
		
		pdb_set_homedir(sam_account, 
				talloc_sub_specified((sam_account)->mem_ctx, 
						       lp_logon_home(),
						       pwd->pw_name, global_myname(), 
						       pwd->pw_uid, pwd->pw_gid),
				PDB_DEFAULT);
		
		pdb_set_dir_drive(sam_account, 
				  talloc_sub_specified((sam_account)->mem_ctx, 
							 lp_logon_drive(),
							 pwd->pw_name, global_myname(), 
							 pwd->pw_uid, pwd->pw_gid),
				  PDB_DEFAULT);
		
		pdb_set_logon_script(sam_account, 
				     talloc_sub_specified((sam_account)->mem_ctx, 
							    lp_logon_script(),
							    pwd->pw_name, global_myname(), 
							    pwd->pw_uid, pwd->pw_gid), 
				     PDB_DEFAULT);
		if (!pdb_set_acct_ctrl(sam_account, ACB_NORMAL, PDB_DEFAULT)) {
			DEBUG(1, ("Failed to set 'normal account' flags for user %s.\n", pwd->pw_name));
			return NT_STATUS_UNSUCCESSFUL;
		}
	} else {
		if (!pdb_set_acct_ctrl(sam_account, ACB_WSTRUST, PDB_DEFAULT)) {
			DEBUG(1, ("Failed to set 'trusted workstation account' flags for user %s.\n", pwd->pw_name));
			return NT_STATUS_UNSUCCESSFUL;
		}
	}
	return NT_STATUS_OK;
}


/*************************************************************
 Initialises a struct sam_passwd with sane values.
 ************************************************************/

NTSTATUS pdb_init_sam_pw(SAM_ACCOUNT **new_sam_acct, const struct passwd *pwd)
{
	NTSTATUS nt_status;

	if (!pwd) {
		new_sam_acct = NULL;
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (!NT_STATUS_IS_OK(nt_status = pdb_init_sam(new_sam_acct))) {
		new_sam_acct = NULL;
		return nt_status;
	}

	if (!NT_STATUS_IS_OK(nt_status = pdb_fill_sam_pw(*new_sam_acct, pwd))) {
		pdb_free_sam(new_sam_acct);
		new_sam_acct = NULL;
		return nt_status;
	}

	return NT_STATUS_OK;
}


/**
 * Free the contets of the SAM_ACCOUNT, but not the structure.
 *
 * Also wipes the LM and NT hashes and plaintext password from 
 * memory.
 *
 * @param user SAM_ACCOUNT to free members of.
 **/

static void pdb_free_sam_contents(SAM_ACCOUNT *user)
{

	/* Kill off sensitive data.  Free()ed by the
	   talloc mechinism */

	data_blob_clear_free(&(user->private.lm_pw));
	data_blob_clear_free(&(user->private.nt_pw));
	if (user->private.plaintext_pw!=NULL)
		memset(user->private.plaintext_pw,'\0',strlen(user->private.plaintext_pw));
}


/************************************************************
 Reset the SAM_ACCOUNT and free the NT/LM hashes.
 ***********************************************************/

NTSTATUS pdb_reset_sam(SAM_ACCOUNT *user)
{
	if (user == NULL) {
		DEBUG(0,("pdb_reset_sam: SAM_ACCOUNT was NULL\n"));
#if 0
		smb_panic("NULL pointer passed to pdb_free_sam\n");
#endif
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	pdb_free_sam_contents(user);

	pdb_fill_default_sam(user);

	return NT_STATUS_OK;
}


/************************************************************
 Free the SAM_ACCOUNT and the member pointers.
 ***********************************************************/

NTSTATUS pdb_free_sam(SAM_ACCOUNT **user)
{
	if (*user == NULL) {
		DEBUG(0,("pdb_free_sam: SAM_ACCOUNT was NULL\n"));
#if 0
		smb_panic("NULL pointer passed to pdb_free_sam\n");
#endif
		return NT_STATUS_UNSUCCESSFUL;
	}

	pdb_free_sam_contents(*user);
	
	if ((*user)->free_fn) {
		(*user)->free_fn(user);
	}

	return NT_STATUS_OK;	
}


/**********************************************************
 Encode the account control bits into a string.
 length = length of string to encode into (including terminating
 null). length *MUST BE MORE THAN 2* !
 **********************************************************/

char *pdb_encode_acct_ctrl(uint16 acct_ctrl, size_t length)
{
	static fstring acct_str;
	size_t i = 0;

	acct_str[i++] = '[';

	if (acct_ctrl & ACB_PWNOTREQ ) acct_str[i++] = 'N';
	if (acct_ctrl & ACB_DISABLED ) acct_str[i++] = 'D';
	if (acct_ctrl & ACB_HOMDIRREQ) acct_str[i++] = 'H';
	if (acct_ctrl & ACB_TEMPDUP  ) acct_str[i++] = 'T'; 
	if (acct_ctrl & ACB_NORMAL   ) acct_str[i++] = 'U';
	if (acct_ctrl & ACB_MNS      ) acct_str[i++] = 'M';
	if (acct_ctrl & ACB_WSTRUST  ) acct_str[i++] = 'W';
	if (acct_ctrl & ACB_SVRTRUST ) acct_str[i++] = 'S';
	if (acct_ctrl & ACB_AUTOLOCK ) acct_str[i++] = 'L';
	if (acct_ctrl & ACB_PWNOEXP  ) acct_str[i++] = 'X';
	if (acct_ctrl & ACB_DOMTRUST ) acct_str[i++] = 'I';

	for ( ; i < length - 2 ; i++ )
		acct_str[i] = ' ';

	i = length - 2;
	acct_str[i++] = ']';
	acct_str[i++] = '\0';

	return acct_str;
}     

/**********************************************************
 Decode the account control bits from a string.
 **********************************************************/

uint16 pdb_decode_acct_ctrl(const char *p)
{
	uint16 acct_ctrl = 0;
	BOOL finished = False;

	/*
	 * Check if the account type bits have been encoded after the
	 * NT password (in the form [NDHTUWSLXI]).
	 */

	if (*p != '[')
		return 0;

	for (p++; *p && !finished; p++) {
		switch (*p) {
			case 'N': { acct_ctrl |= ACB_PWNOTREQ ; break; /* 'N'o password. */ }
			case 'D': { acct_ctrl |= ACB_DISABLED ; break; /* 'D'isabled. */ }
			case 'H': { acct_ctrl |= ACB_HOMDIRREQ; break; /* 'H'omedir required. */ }
			case 'T': { acct_ctrl |= ACB_TEMPDUP  ; break; /* 'T'emp account. */ } 
			case 'U': { acct_ctrl |= ACB_NORMAL   ; break; /* 'U'ser account (normal). */ } 
			case 'M': { acct_ctrl |= ACB_MNS      ; break; /* 'M'NS logon user account. What is this ? */ } 
			case 'W': { acct_ctrl |= ACB_WSTRUST  ; break; /* 'W'orkstation account. */ } 
			case 'S': { acct_ctrl |= ACB_SVRTRUST ; break; /* 'S'erver account. */ } 
			case 'L': { acct_ctrl |= ACB_AUTOLOCK ; break; /* 'L'ocked account. */ } 
			case 'X': { acct_ctrl |= ACB_PWNOEXP  ; break; /* No 'X'piry on password */ } 
			case 'I': { acct_ctrl |= ACB_DOMTRUST ; break; /* 'I'nterdomain trust account. */ }
            case ' ': { break; }
			case ':':
			case '\n':
			case '\0': 
			case ']':
			default:  { finished = True; }
		}
	}

	return acct_ctrl;
}

/*************************************************************
 Routine to set 32 hex password characters from a 16 byte array.
**************************************************************/

void pdb_sethexpwd(char *p, const unsigned char *pwd, uint16 acct_ctrl)
{
	if (pwd != NULL) {
		int i;
		for (i = 0; i < 16; i++)
			slprintf(&p[i*2], 3, "%02X", pwd[i]);
	} else {
		if (acct_ctrl & ACB_PWNOTREQ)
			safe_strcpy(p, "NO PASSWORDXXXXXXXXXXXXXXXXXXXXX", 33);
		else
			safe_strcpy(p, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 33);
	}
}

/*************************************************************
 Routine to get the 32 hex characters and turn them
 into a 16 byte array.
**************************************************************/

BOOL pdb_gethexpwd(const char *p, unsigned char *pwd)
{
	int i;
	unsigned char   lonybble, hinybble;
	const char      *hexchars = "0123456789ABCDEF";
	char           *p1, *p2;
	
	if (!p)
		return (False);
	
	for (i = 0; i < 32; i += 2) {
		hinybble = toupper(p[i]);
		lonybble = toupper(p[i + 1]);

		p1 = strchr(hexchars, hinybble);
		p2 = strchr(hexchars, lonybble);

		if (!p1 || !p2)
			return (False);

		hinybble = PTR_DIFF(p1, hexchars);
		lonybble = PTR_DIFF(p2, hexchars);

		pwd[i / 2] = (hinybble << 4) | lonybble;
	}
	return (True);
}

/*******************************************************************
 Converts NT user RID to a UNIX uid.
 ********************************************************************/

static int algorithmic_rid_base(void)
{
	static int rid_offset = 0;

	if (rid_offset != 0)
		return rid_offset;

	rid_offset = lp_algorithmic_rid_base();

	if (rid_offset < BASE_RID) {  
		/* Try to prevent admin foot-shooting, we can't put algorithmic
		   rids below 1000, that's the 'well known RIDs' on NT */
		DEBUG(0, ("'algorithmic rid base' must be equal to or above %ld\n", BASE_RID));
		rid_offset = BASE_RID;
	}
	if (rid_offset & 1) {
		DEBUG(0, ("algorithmic rid base must be even\n"));
		rid_offset += 1;
	}
	return rid_offset;
}


uid_t fallback_pdb_user_rid_to_uid(uint32 user_rid)
{
	int rid_offset = algorithmic_rid_base();
	return (uid_t)(((user_rid & (~USER_RID_TYPE))- rid_offset)/RID_MULTIPLIER);
}


/*******************************************************************
 converts UNIX uid to an NT User RID.
 ********************************************************************/

uint32 fallback_pdb_uid_to_user_rid(uid_t uid)
{
	int rid_offset = algorithmic_rid_base();
	return (((((uint32)uid)*RID_MULTIPLIER) + rid_offset) | USER_RID_TYPE);
}

/*******************************************************************
 Converts NT group RID to a UNIX gid.
 ********************************************************************/

gid_t pdb_group_rid_to_gid(uint32 group_rid)
{
	int rid_offset = algorithmic_rid_base();
	return (gid_t)(((group_rid & (~GROUP_RID_TYPE))- rid_offset)/RID_MULTIPLIER);
}

/*******************************************************************
 converts NT Group RID to a UNIX uid.
 
 warning: you must not call that function only
 you must do a call to the group mapping first.
 there is not anymore a direct link between the gid and the rid.
 ********************************************************************/

uint32 pdb_gid_to_group_rid(gid_t gid)
{
	int rid_offset = algorithmic_rid_base();
	return (((((uint32)gid)*RID_MULTIPLIER) + rid_offset) | GROUP_RID_TYPE);
}

/*******************************************************************
 Decides if a RID is a well known RID.
 ********************************************************************/

static BOOL pdb_rid_is_well_known(uint32 rid)
{
	/* Not using rid_offset here, becouse this is the actual
	   NT fixed value (1000) */

	return (rid < BASE_RID);
}

/*******************************************************************
 Decides if a RID is a user or group RID.
 ********************************************************************/

BOOL pdb_rid_is_user(uint32 rid)
{
  /* lkcl i understand that NT attaches an enumeration to a RID
   * such that it can be identified as either a user, group etc
   * type.  there are 5 such categories, and they are documented.
   */
	/* However, they are not in the RID, just somthing you can query
	   seperatly.  Sorry luke :-) */

   if(pdb_rid_is_well_known(rid)) {
      /*
       * The only well known user RIDs are DOMAIN_USER_RID_ADMIN
       * and DOMAIN_USER_RID_GUEST.
       */
     if(rid == DOMAIN_USER_RID_ADMIN || rid == DOMAIN_USER_RID_GUEST)
       return True;
   } else if((rid & RID_TYPE_MASK) == USER_RID_TYPE) {
     return True;
   }
   return False;
}

/*******************************************************************
 Convert a rid into a name. Used in the lookup SID rpc.
 ********************************************************************/

BOOL local_lookup_sid(DOM_SID *sid, char *name, enum SID_NAME_USE *psid_name_use)
{
	uint32 rid;
	SAM_ACCOUNT *sam_account = NULL;
	GROUP_MAP map;

	if (!sid_peek_check_rid(get_global_sam_sid(), sid, &rid)){
		DEBUG(0,("local_sid_to_gid: sid_peek_check_rid return False! SID: %s\n",
			sid_string_static(&map.sid)));
		return False;
	}	
	*psid_name_use = SID_NAME_UNKNOWN;
	
	DEBUG(5,("local_lookup_sid: looking up RID %u.\n", (unsigned int)rid));
	
	if (rid == DOMAIN_USER_RID_ADMIN) {
		const char **admin_list = lp_admin_users(-1);
		*psid_name_use = SID_NAME_USER;
		if (admin_list) {
			const char *p = *admin_list;
			if(!next_token(&p, name, NULL, sizeof(fstring)))
				fstrcpy(name, "Administrator");
		} else {
			fstrcpy(name, "Administrator");
		}
		return True;
	}

	/*
	 * Don't try to convert the rid to a name if 
	 * running in appliance mode
	 */

	if (lp_hide_local_users())
		return False;
		
	if (!NT_STATUS_IS_OK(pdb_init_sam(&sam_account))) {
		return False;
	}
		
	/* This now does the 'generic' mapping in pdb_unix */
	/* 'guest' is also handled there */
	if (pdb_getsampwsid(sam_account, sid)) {
		fstrcpy(name, pdb_get_username(sam_account));
		*psid_name_use = SID_NAME_USER;

		pdb_free_sam(&sam_account);
			
		return True;
	}

	pdb_free_sam(&sam_account);
		
	if (pdb_getgrsid(&map, *sid, MAPPING_WITHOUT_PRIV)) {
		if (map.gid!=(gid_t)-1) {
			DEBUG(5,("local_lookup_sid: mapped group %s to gid %u\n", map.nt_name, (unsigned int)map.gid));
		} else {
			DEBUG(5,("local_lookup_sid: mapped group %s to no unix gid.  Returning name.\n", map.nt_name));
		}

		fstrcpy(name, map.nt_name);
		*psid_name_use = map.sid_name_use;
		return True;
	}

	if (pdb_rid_is_user(rid)) {
		uid_t uid;

		DEBUG(5, ("assuming RID %u is a user\n", (unsigned)rid));

       		uid = fallback_pdb_user_rid_to_uid(rid);
		slprintf(name, sizeof(fstring)-1, "unix_user.%u", (unsigned int)uid);	

		return False;  /* Indicates that this user was 'not mapped' */
	} else {
		gid_t gid;
		struct group *gr; 
			
		DEBUG(5, ("assuming RID %u is a group\n", (unsigned)rid));

		gid = pdb_group_rid_to_gid(rid);
		gr = getgrgid(gid);
			
		*psid_name_use = SID_NAME_ALIAS;
			
		DEBUG(5,("local_lookup_sid: looking up gid %u %s\n", (unsigned int)gid,
			 gr ? "succeeded" : "failed" ));
			
		if(!gr) {
			slprintf(name, sizeof(fstring)-1, "unix_group.%u", (unsigned int)gid);
			return False; /* Indicates that this group was 'not mapped' */
		}
			
		fstrcpy( name, gr->gr_name);
			
		DEBUG(5,("local_lookup_sid: found group %s for rid %u\n", name,
			 (unsigned int)rid ));
		return True;   
	}
}

/*******************************************************************
 Convert a name into a SID. Used in the lookup name rpc.
 ********************************************************************/

BOOL local_lookup_name(const char *c_user, DOM_SID *psid, enum SID_NAME_USE *psid_name_use)
{
	extern DOM_SID global_sid_World_Domain;
	DOM_SID local_sid;
	fstring user;
	SAM_ACCOUNT *sam_account = NULL;
	struct group *grp;
	GROUP_MAP map;
		
	*psid_name_use = SID_NAME_UNKNOWN;

	/*
	 * user may be quoted a const string, and map_username and
	 * friends can modify it. Make a modifiable copy. JRA.
	 */

	fstrcpy(user, c_user);

	sid_copy(&local_sid, get_global_sam_sid());

	/*
	 * Special case for MACHINE\Everyone. Map to the world_sid.
	 */

	if(strequal(user, "Everyone")) {
		sid_copy( psid, &global_sid_World_Domain);
		sid_append_rid(psid, 0);
		*psid_name_use = SID_NAME_ALIAS;
		return True;
	}

	/* 
	 * Don't lookup local unix users if running in appliance mode
	 */
	if (lp_hide_local_users()) 
		return False;

	(void)map_username(user);

	if (!NT_STATUS_IS_OK(pdb_init_sam(&sam_account))) {
		return False;
	}
	
	if (pdb_getsampwnam(sam_account, user)) {
		sid_copy(psid, pdb_get_user_sid(sam_account));
		*psid_name_use = SID_NAME_USER;
		
		pdb_free_sam(&sam_account);
		return True;
	}

	pdb_free_sam(&sam_account);

	/*
	 * Maybe it was a group ?
	 */

	/* check if it's a mapped group */
	if (pdb_getgrnam(&map, user, MAPPING_WITHOUT_PRIV)) {
		/* yes it's a mapped group */
		sid_copy(&local_sid, &map.sid);
		*psid_name_use = map.sid_name_use;
	} else {
		/* it's not a mapped group */
		grp = getgrnam(user);
		if(!grp)
			return False;
		
		/* 
		 *check if it's mapped, if it is reply it doesn't exist
		 *
		 * that's to prevent this case:
		 *
		 * unix group ug is mapped to nt group ng
		 * someone does a lookup on ug
		 * we must not reply as it doesn't "exist" anymore
		 * for NT. For NT only ng exists.
		 * JFM, 30/11/2001
		 */
		
		if (pdb_getgrgid(&map, grp->gr_gid, MAPPING_WITHOUT_PRIV)){
			return False;
		}
		
		sid_append_rid( &local_sid, pdb_gid_to_group_rid(grp->gr_gid));
		*psid_name_use = SID_NAME_ALIAS;
	}

	sid_copy( psid, &local_sid);

	return True;
}

/****************************************************************************
 Convert a uid to SID - locally.
****************************************************************************/

DOM_SID *local_uid_to_sid(DOM_SID *psid, uid_t uid)
{
	struct passwd *pass;
	SAM_ACCOUNT *sam_user = NULL;
	fstring str; /* sid string buffer */

	sid_copy(psid, get_global_sam_sid());

	if((pass = getpwuid_alloc(uid))) {

		if (NT_STATUS_IS_ERR(pdb_init_sam(&sam_user))) {
			passwd_free(&pass);
			return NULL;
		}
		
		if (pdb_getsampwnam(sam_user, pass->pw_name)) {
			sid_copy(psid, pdb_get_user_sid(sam_user));
		} else {
			sid_append_rid(psid, fallback_pdb_uid_to_user_rid(uid));
		}

		DEBUG(10,("local_uid_to_sid: uid %u -> SID (%s) (%s).\n", 
			  (unsigned)uid, sid_to_string( str, psid),
			  pass->pw_name ));

		passwd_free(&pass);
		pdb_free_sam(&sam_user);
	
	} else {
		sid_append_rid(psid, fallback_pdb_uid_to_user_rid(uid));

		DEBUG(10,("local_uid_to_sid: uid %u -> SID (%s) (unknown user).\n", 
			  (unsigned)uid, sid_to_string( str, psid)));
	}

	return psid;
}

/****************************************************************************
 Convert a SID to uid - locally.
****************************************************************************/

BOOL local_sid_to_uid(uid_t *puid, const DOM_SID *psid, enum SID_NAME_USE *name_type)
{
	fstring str;
	SAM_ACCOUNT *sam_user = NULL;

	*name_type = SID_NAME_UNKNOWN;

	if (NT_STATUS_IS_ERR(pdb_init_sam(&sam_user)))
		return False;
	
	if (pdb_getsampwsid(sam_user, psid)) {
		
		if (!IS_SAM_SET(sam_user,PDB_UID)&&!IS_SAM_CHANGED(sam_user,PDB_UID)) {
			pdb_free_sam(&sam_user);
			return False;
		}

		*puid = pdb_get_uid(sam_user);
			
		DEBUG(10,("local_sid_to_uid: SID %s -> uid (%u) (%s).\n", sid_to_string( str, psid),
			  (unsigned int)*puid, pdb_get_username(sam_user)));
		pdb_free_sam(&sam_user);
	} else {

		DOM_SID dom_sid;
		uint32 rid;
		GROUP_MAP map;

		pdb_free_sam(&sam_user);  

		if (pdb_getgrsid(&map, *psid, MAPPING_WITHOUT_PRIV)) {
			DEBUG(3, ("local_sid_to_uid: SID '%s' is a group, not a user... \n", sid_to_string(str, psid)));
			/* It's a group, not a user... */
			return False;
		}

		sid_copy(&dom_sid, psid);
		if (!sid_peek_check_rid(get_global_sam_sid(), psid, &rid)) {
			DEBUG(3, ("sid_peek_rid failed - sid '%s' is not in our domain\n", sid_to_string(str, psid)));
			return False;
		}

		if (!pdb_rid_is_user(rid)) {
			DEBUG(3, ("local_sid_to_uid: sid '%s' cannot be mapped to a uid algorithmicly becouse it is a group\n", sid_to_string(str, psid)));
			return False;
		}
		
		*puid = fallback_pdb_user_rid_to_uid(rid);
		
		DEBUG(5,("local_sid_to_uid: SID %s algorithmicly mapped to %ld mapped becouse SID was not found in passdb.\n", 
			 sid_to_string(str, psid), (signed long int)(*puid)));
	}

	*name_type = SID_NAME_USER;

	return True;
}

/****************************************************************************
 Convert a gid to SID - locally.
****************************************************************************/

DOM_SID *local_gid_to_sid(DOM_SID *psid, gid_t gid)
{
	GROUP_MAP map;

	sid_copy(psid, get_global_sam_sid());
	
	if (pdb_getgrgid(&map, gid, MAPPING_WITHOUT_PRIV)) {
		sid_copy(psid, &map.sid);
	} 
	else {
		sid_append_rid(psid, pdb_gid_to_group_rid(gid));
	}

	return psid;
}

/****************************************************************************
 Convert a SID to gid - locally.
****************************************************************************/

BOOL local_sid_to_gid(gid_t *pgid, const DOM_SID *psid, enum SID_NAME_USE *name_type)
{
	fstring str;
	GROUP_MAP map;

	*name_type = SID_NAME_UNKNOWN;

	/*
	 * We can only convert to a gid if this is our local
	 * Domain SID (ie. we are the controling authority).
	 *
	 * Or in the Builtin SID too. JFM, 11/30/2001
	 */

	if (pdb_getgrsid(&map, *psid, MAPPING_WITHOUT_PRIV)) {
		
		/* the SID is in the mapping table but not mapped */
		if (map.gid==(gid_t)-1)
			return False;

		*pgid = map.gid;
		*name_type = map.sid_name_use;
		DEBUG(10,("local_sid_to_gid: mapped SID %s (%s) -> gid (%u).\n", 
			  sid_to_string( str, psid),
			  map.nt_name, (unsigned int)*pgid));

	} else {
		uint32 rid;
		SAM_ACCOUNT *sam_user = NULL;
		if (NT_STATUS_IS_ERR(pdb_init_sam(&sam_user)))
			return False;
		
		if (pdb_getsampwsid(sam_user, psid)) {
			return False;
			pdb_free_sam(&sam_user);
		}

		pdb_free_sam(&sam_user);

		if (!sid_peek_check_rid(get_global_sam_sid(), psid, &rid)) {
			DEBUG(3, ("sid_peek_rid failed - sid '%s' is not in our domain\n", sid_to_string(str, psid)));
			return False;
		}

		if (pdb_rid_is_user(rid))
			return False;
		
		*pgid = pdb_group_rid_to_gid(rid);
		*name_type = SID_NAME_ALIAS;
		DEBUG(10,("local_sid_to_gid: SID %s -> gid (%u).\n", sid_to_string( str, psid),
			  (unsigned int)*pgid));
	}
	
	return True;
}

/*************************************************************
 Change a password entry in the local smbpasswd file.

It is currently being called by SWAT and by smbpasswd.
 
 --jerry
 *************************************************************/

BOOL local_password_change(const char *user_name, int local_flags,
			   const char *new_passwd, 
			   char *err_str, size_t err_str_len,
			   char *msg_str, size_t msg_str_len)
{
	struct passwd  *pwd = NULL;
	SAM_ACCOUNT 	*sam_pass=NULL;
	uint16 other_acb;

	*err_str = '\0';
	*msg_str = '\0';

	/* Get the smb passwd entry for this user */
	pdb_init_sam(&sam_pass);
	if(!pdb_getsampwnam(sam_pass, user_name)) {
		pdb_free_sam(&sam_pass);
		
		if (local_flags & LOCAL_ADD_USER) {
			pwd = getpwnam_alloc(user_name);
		} else if (local_flags & LOCAL_DELETE_USER) {
			/* Might not exist in /etc/passwd */
		} else {
			slprintf(err_str, err_str_len-1,"Failed to find entry for user %s.\n", user_name);
			return False;
		}
		
		if (pwd) {
			/* Local user found, so init from this */
			if (!NT_STATUS_IS_OK(pdb_init_sam_pw(&sam_pass, pwd))){
				slprintf(err_str, err_str_len-1, "Failed initialise SAM_ACCOUNT for user %s.\n", user_name);
				passwd_free(&pwd);
				return False;
			}
		
			passwd_free(&pwd);
		} else {
			if (!NT_STATUS_IS_OK(pdb_init_sam(&sam_pass))){
				slprintf(err_str, err_str_len-1, "Failed initialise SAM_ACCOUNT for user %s.\n", user_name);
				return False;
			}

	        	if (!pdb_set_username(sam_pass, user_name, PDB_CHANGED)) {
	                	slprintf(err_str, err_str_len - 1, "Failed to set username for user %s.\n", user_name);
	               	 	pdb_free_sam(&sam_pass);
	               	 	return False;
	        	}
		}
	} else {
		/* the entry already existed */
		local_flags &= ~LOCAL_ADD_USER;
	}

	/* the 'other' acb bits not being changed here */
	other_acb =  (pdb_get_acct_ctrl(sam_pass) & (!(ACB_WSTRUST|ACB_DOMTRUST|ACB_SVRTRUST|ACB_NORMAL)));
	if (local_flags & LOCAL_TRUST_ACCOUNT) {
		if (!pdb_set_acct_ctrl(sam_pass, ACB_WSTRUST | other_acb, PDB_CHANGED) ) {
			slprintf(err_str, err_str_len - 1, "Failed to set 'trusted workstation account' flags for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	} else if (local_flags & LOCAL_INTERDOM_ACCOUNT) {
		if (!pdb_set_acct_ctrl(sam_pass, ACB_DOMTRUST | other_acb, PDB_CHANGED)) {
			slprintf(err_str, err_str_len - 1, "Failed to set 'domain trust account' flags for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	} else {
		if (!pdb_set_acct_ctrl(sam_pass, ACB_NORMAL | other_acb, PDB_CHANGED)) {
			slprintf(err_str, err_str_len - 1, "Failed to set 'normal account' flags for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	}

	/*
	 * We are root - just write the new password
	 * and the valid last change time.
	 */

	if (local_flags & LOCAL_DISABLE_USER) {
		if (!pdb_set_acct_ctrl (sam_pass, pdb_get_acct_ctrl(sam_pass)|ACB_DISABLED, PDB_CHANGED)) {
			slprintf(err_str, err_str_len-1, "Failed to set 'disabled' flag for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	} else if (local_flags & LOCAL_ENABLE_USER) {
		if (!pdb_set_acct_ctrl (sam_pass, pdb_get_acct_ctrl(sam_pass)&(~ACB_DISABLED), PDB_CHANGED)) {
			slprintf(err_str, err_str_len-1, "Failed to unset 'disabled' flag for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	}
	
	if (local_flags & LOCAL_SET_NO_PASSWORD) {
		if (!pdb_set_acct_ctrl (sam_pass, pdb_get_acct_ctrl(sam_pass)|ACB_PWNOTREQ, PDB_CHANGED)) {
			slprintf(err_str, err_str_len-1, "Failed to set 'no password required' flag for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	} else if (local_flags & LOCAL_SET_PASSWORD) {
		/*
		 * If we're dealing with setting a completely empty user account
		 * ie. One with a password of 'XXXX', but not set disabled (like
		 * an account created from scratch) then if the old password was
		 * 'XX's then getsmbpwent will have set the ACB_DISABLED flag.
		 * We remove that as we're giving this user their first password
		 * and the decision hasn't really been made to disable them (ie.
		 * don't create them disabled). JRA.
		 */
		if ((pdb_get_lanman_passwd(sam_pass)==NULL) && (pdb_get_acct_ctrl(sam_pass)&ACB_DISABLED)) {
			if (!pdb_set_acct_ctrl (sam_pass, pdb_get_acct_ctrl(sam_pass)&(~ACB_DISABLED), PDB_CHANGED)) {
				slprintf(err_str, err_str_len-1, "Failed to unset 'disabled' flag for user %s.\n", user_name);
				pdb_free_sam(&sam_pass);
				return False;
			}
		}
		if (!pdb_set_acct_ctrl (sam_pass, pdb_get_acct_ctrl(sam_pass)&(~ACB_PWNOTREQ), PDB_CHANGED)) {
			slprintf(err_str, err_str_len-1, "Failed to unset 'no password required' flag for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
		
		if (!pdb_set_plaintext_passwd (sam_pass, new_passwd)) {
			slprintf(err_str, err_str_len-1, "Failed to set password for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	}	

	if (local_flags & LOCAL_ADD_USER) {
		if (pdb_add_sam_account(sam_pass)) {
			slprintf(msg_str, msg_str_len-1, "Added user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return True;
		} else {
			slprintf(err_str, err_str_len-1, "Failed to add entry for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
	} else if (local_flags & LOCAL_DELETE_USER) {
		if (!pdb_delete_sam_account(sam_pass)) {
			slprintf(err_str,err_str_len-1, "Failed to delete entry for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
		slprintf(msg_str, msg_str_len-1, "Deleted user %s.\n", user_name);
	} else {
		if(!pdb_update_sam_account(sam_pass)) {
			slprintf(err_str, err_str_len-1, "Failed to modify entry for user %s.\n", user_name);
			pdb_free_sam(&sam_pass);
			return False;
		}
		if(local_flags & LOCAL_DISABLE_USER)
			slprintf(msg_str, msg_str_len-1, "Disabled user %s.\n", user_name);
		else if (local_flags & LOCAL_ENABLE_USER)
			slprintf(msg_str, msg_str_len-1, "Enabled user %s.\n", user_name);
		else if (local_flags & LOCAL_SET_NO_PASSWORD)
			slprintf(msg_str, msg_str_len-1, "User %s password set to none.\n", user_name);
	}

	pdb_free_sam(&sam_pass);
	return True;
}
