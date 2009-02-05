/* 
   Unix SMB/CIFS implementation.

   Winbind daemon connection manager

   Copyright (C) Tim Potter 2001
   Copyright (C) Andrew Bartlett 2002
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
   We need to manage connections to domain controllers without having to
   mess up the main winbindd code with other issues.  The aim of the
   connection manager is to:
  
       - make connections to domain controllers and cache them
       - re-establish connections when networks or servers go down
       - centralise the policy on connection timeouts, domain controller
	 selection etc
       - manage re-entrancy for when winbindd becomes able to handle
	 multiple outstanding rpc requests
  
   Why not have connection management as part of the rpc layer like tng?
   Good question.  This code may morph into libsmb/rpc_cache.c or something
   like that but at the moment it's simply staying as part of winbind.	I
   think the TNG architecture of forcing every user of the rpc layer to use
   the connection caching system is a bad idea.	 It should be an optional
   method of using the routines.

   The TNG design is quite good but I disagree with some aspects of the
   implementation. -tpot

 */

/*
   TODO:

     - I'm pretty annoyed by all the make_nmb_name() stuff.  It should be
       moved down into another function.

     - There needs to be a utility function in libsmb/namequery.c that does
       cm_get_dc_name() 

     - Take care when destroying cli_structs as they can be shared between
       various sam handles.

 */

#include "winbindd.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_WINBIND

/* Global list of connections.	Initially a DLIST but can become a hash
   table or whatever later. */

struct winbindd_cm_conn {
	struct winbindd_cm_conn *prev, *next;
	fstring domain;
	fstring controller;
	fstring pipe_name;
	size_t mutex_ref_count;
	struct cli_state *cli;
	POLICY_HND pol;
};

static struct winbindd_cm_conn *cm_conns = NULL;

/* Get a domain controller name.  Cache positive and negative lookups so we
   don't go to the network too often when something is badly broken. */

#define GET_DC_NAME_CACHE_TIMEOUT 30 /* Seconds between dc lookups */

struct get_dc_name_cache {
	fstring domain_name;
	fstring srv_name;
	time_t lookup_time;
	struct get_dc_name_cache *prev, *next;
};

/*
  find the DC for a domain using methods appropriate for a ADS domain
*/
static BOOL cm_ads_find_dc(const char *domain, struct in_addr *dc_ip, fstring srv_name)
{
	ADS_STRUCT *ads;
	const char *realm = domain;

	if (strcasecmp(realm, lp_workgroup()) == 0)
		realm = lp_realm();

	ads = ads_init(realm, domain, NULL);
	if (!ads)
		return False;

	/* we don't need to bind, just connect */
	ads->auth.flags |= ADS_AUTH_NO_BIND;

	DEBUG(4,("cm_ads_find_dc: domain=%s\n", domain));

#ifdef HAVE_ADS
	/* a full ads_connect() is actually overkill, as we don't srictly need
	   to do the SASL auth in order to get the info we need, but libads
	   doesn't offer a better way right now */
	ads_connect(ads);
#endif

	if (!ads->config.realm)
		return False;

	fstrcpy(srv_name, ads->config.ldap_server_name);
	strupper(srv_name);
	*dc_ip = ads->ldap_ip;
	ads_destroy(&ads);
	
	DEBUG(4,("cm_ads_find_dc: using server='%s' IP=%s\n",
		 srv_name, inet_ntoa(*dc_ip)));
	
	return True;
}



static BOOL cm_get_dc_name(const char *domain, fstring srv_name, struct in_addr *ip_out)
{
	static struct get_dc_name_cache *get_dc_name_cache;
	struct get_dc_name_cache *dcc;
	struct in_addr dc_ip;
	BOOL ret;

	/* Check the cache for previous lookups */

	for (dcc = get_dc_name_cache; dcc; dcc = dcc->next) {

		if (!strequal(domain, dcc->domain_name))
			continue; /* Not our domain */

		if ((time(NULL) - dcc->lookup_time) > 
		    GET_DC_NAME_CACHE_TIMEOUT) {

			/* Cache entry has expired, delete it */

			DEBUG(10, ("get_dc_name_cache entry expired for %s\n", domain));

			DLIST_REMOVE(get_dc_name_cache, dcc);
			SAFE_FREE(dcc);

			break;
		}

		/* Return a positive or negative lookup for this domain */

		if (dcc->srv_name[0]) {
			DEBUG(10, ("returning positive get_dc_name_cache entry for %s\n", domain));
			fstrcpy(srv_name, dcc->srv_name);
			return True;
		} else {
			DEBUG(10, ("returning negative get_dc_name_cache entry for %s\n", domain));
			return False;
		}
	}

	/* Add cache entry for this lookup. */

	DEBUG(10, ("Creating get_dc_name_cache entry for %s\n", domain));

	if (!(dcc = (struct get_dc_name_cache *) 
	      malloc(sizeof(struct get_dc_name_cache))))
		return False;

	ZERO_STRUCTP(dcc);

	fstrcpy(dcc->domain_name, domain);
	dcc->lookup_time = time(NULL);

	DLIST_ADD(get_dc_name_cache, dcc);

	zero_ip(&dc_ip);

	ret = False;
	if (lp_security() == SEC_ADS)
		ret = cm_ads_find_dc(domain, &dc_ip, srv_name);

	if (!ret) {
		/* fall back on rpc methods if the ADS methods fail */
		ret = rpc_find_dc(domain, srv_name, &dc_ip);
	}

	if (!ret)
		return False;

	/* We have a name so make the cache entry positive now */
	fstrcpy(dcc->srv_name, srv_name);

	DEBUG(3, ("cm_get_dc_name: Returning DC %s (%s) for domain %s\n", srv_name,
		  inet_ntoa(dc_ip), domain));

	*ip_out = dc_ip;

	return True;
}

/* Choose between anonymous or authenticated connections.  We need to use
   an authenticated connection if DCs have the RestrictAnonymous registry
   entry set > 0, or the "Additional restrictions for anonymous
   connections" set in the win2k Local Security Policy. 
   
   Caller to free() result in domain, username, password
*/

static void cm_get_ipc_userpass(char **username, char **domain, char **password)
{
	*username = secrets_fetch(SECRETS_AUTH_USER, NULL);
	*domain = secrets_fetch(SECRETS_AUTH_DOMAIN, NULL);
	*password = secrets_fetch(SECRETS_AUTH_PASSWORD, NULL);
	
	if (*username && **username) {

		if (!*domain || !**domain)
			*domain = smb_xstrdup(lp_workgroup());
		
		if (!*password || !**password)
			*password = smb_xstrdup("");

		DEBUG(3, ("IPC$ connections done by user %s\\%s\n", 
			  *domain, *username));

	} else {
		DEBUG(3, ("IPC$ connections done anonymously\n"));
		*username = smb_xstrdup("");
		*domain = smb_xstrdup("");
		*password = smb_xstrdup("");
	}
}

/* Open a new smb pipe connection to a DC on a given domain.  Cache
   negative creation attempts so we don't try and connect to broken
   machines too often. */

#define FAILED_CONNECTION_CACHE_TIMEOUT 30 /* Seconds between attempts */

struct failed_connection_cache {
	fstring domain_name;
	fstring controller;
	time_t lookup_time;
	NTSTATUS nt_status;
	struct failed_connection_cache *prev, *next;
};

static struct failed_connection_cache *failed_connection_cache;

/* Add an entry to the failed conneciton cache */

static void add_failed_connection_entry(struct winbindd_cm_conn *new_conn, 
					NTSTATUS result) 
{
	struct failed_connection_cache *fcc;

	SMB_ASSERT(!NT_STATUS_IS_OK(result));

	/* Check we already aren't in the cache */

	for (fcc = failed_connection_cache; fcc; fcc = fcc->next) {
		if (strequal(fcc->domain_name, new_conn->domain)) {
			DEBUG(10, ("domain %s already tried and failed\n",
				   fcc->domain_name));
			return;
		}
	}

	/* Create negative lookup cache entry for this domain and controller */

	if (!(fcc = (struct failed_connection_cache *)
	      malloc(sizeof(struct failed_connection_cache)))) {
		DEBUG(0, ("malloc failed in add_failed_connection_entry!\n"));
		return;
	}
	
	ZERO_STRUCTP(fcc);
	
	fstrcpy(fcc->domain_name, new_conn->domain);
	fstrcpy(fcc->controller, new_conn->controller);
	fcc->lookup_time = time(NULL);
	fcc->nt_status = result;
	
	DLIST_ADD(failed_connection_cache, fcc);
}
	
/* Open a connction to the remote server, cache failures for 30 seconds */

static NTSTATUS cm_open_connection(const char *domain, const int pipe_index,
			       struct winbindd_cm_conn *new_conn, BOOL keep_mutex)
{
	struct failed_connection_cache *fcc;
	NTSTATUS result;
	char *ipc_username, *ipc_domain, *ipc_password;
	struct in_addr dc_ip;
	int i;
	BOOL retry = True;
	BOOL got_mutex = False;

	ZERO_STRUCT(dc_ip);

	fstrcpy(new_conn->domain, domain);
	fstrcpy(new_conn->pipe_name, get_pipe_name_from_index(pipe_index));
	
	/* Look for a domain controller for this domain.  Negative results
	   are cached so don't bother applying the caching for this
	   function just yet.  */

	if (!cm_get_dc_name(domain, new_conn->controller, &dc_ip)) {
		result = NT_STATUS_DOMAIN_CONTROLLER_NOT_FOUND;
		add_failed_connection_entry(new_conn, result);
		return result;
	}
		
	/* Return false if we have tried to look up this domain and netbios
	   name before and failed. */

	for (fcc = failed_connection_cache; fcc; fcc = fcc->next) {
		
		if (!(strequal(domain, fcc->domain_name) &&
		      strequal(new_conn->controller, fcc->controller)))
			continue; /* Not our domain */

		if ((time(NULL) - fcc->lookup_time) > 
		    FAILED_CONNECTION_CACHE_TIMEOUT) {

			/* Cache entry has expired, delete it */

			DEBUG(10, ("cm_open_connection cache entry expired for %s, %s\n", domain, new_conn->controller));

			DLIST_REMOVE(failed_connection_cache, fcc);
			free(fcc);

			break;
		}

		/* The timeout hasn't expired yet so return false */

		DEBUG(10, ("returning negative open_connection_cache entry for %s, %s\n", domain, new_conn->controller));

		result = fcc->nt_status;
		SMB_ASSERT(!NT_STATUS_IS_OK(result));
		return result;
	}

	/* Initialise SMB connection */

	cm_get_ipc_userpass(&ipc_username, &ipc_domain, &ipc_password);

	DEBUG(5, ("connecting to %s from %s with username [%s]\\[%s]\n", 
	      new_conn->controller, global_myname(), ipc_domain, ipc_username));

	for (i = 0; retry && (i < 3); i++) {
		
		if (!secrets_named_mutex(new_conn->controller, WINBIND_SERVER_MUTEX_WAIT_TIME, &new_conn->mutex_ref_count)) {
			DEBUG(0,("cm_open_connection: mutex grab failed for %s\n", new_conn->controller));
			result = NT_STATUS_POSSIBLE_DEADLOCK;
			continue;
		}

		got_mutex = True;

		result = cli_full_connection(&new_conn->cli, global_myname(), new_conn->controller, 
			&dc_ip, 0, "IPC$", "IPC", ipc_username, ipc_domain, 
			ipc_password, 0, &retry);

		if (NT_STATUS_IS_OK(result))
			break;

		secrets_named_mutex_release(new_conn->controller, &new_conn->mutex_ref_count);
		got_mutex = False;
	}

	SAFE_FREE(ipc_username);
	SAFE_FREE(ipc_domain);
	SAFE_FREE(ipc_password);

	if (!NT_STATUS_IS_OK(result)) {
		if (got_mutex)
			secrets_named_mutex_release(new_conn->controller, &new_conn->mutex_ref_count);
		add_failed_connection_entry(new_conn, result);
		return result;
	}
	
	if ( !cli_nt_session_open (new_conn->cli, pipe_index) ) {
		result = NT_STATUS_PIPE_NOT_AVAILABLE;
		/* 
		 * only cache a failure if we are not trying to open the 
		 * **win2k** specific lsarpc UUID.  This could be an NT PDC 
		 * and therefore a failure is normal.  This should probably
		 * be abstracted to a check for 2k specific pipes and wondering
		 * if the PDC is an NT4 box.   but since there is only one 2k 
		 * specific UUID right now, i'm not going to bother.  --jerry
		 */
		if (got_mutex)
			secrets_named_mutex_release(new_conn->controller, &new_conn->mutex_ref_count);
		if ( !is_win2k_pipe(pipe_index) )
			add_failed_connection_entry(new_conn, result);
		cli_shutdown(new_conn->cli);
		return result;
	}

	if ((got_mutex) && !keep_mutex)
		secrets_named_mutex_release(new_conn->controller, &new_conn->mutex_ref_count);
	return NT_STATUS_OK;
}

/* Return true if a connection is still alive */

static BOOL connection_ok(struct winbindd_cm_conn *conn)
{
	if (!conn) {
		smb_panic("Invalid paramater passed to conneciton_ok():  conn was NULL!\n");
		return False;
	}

	if (!conn->cli) {
		DEBUG(0, ("Connection to %s for domain %s (pipe %s) has NULL conn->cli!\n", 
			  conn->controller, conn->domain, conn->pipe_name));
		smb_panic("connection_ok: conn->cli was null!");
		return False;
	}

	if (!conn->cli->initialised) {
		DEBUG(0, ("Connection to %s for domain %s (pipe %s) was never initialised!\n", 
			  conn->controller, conn->domain, conn->pipe_name));
		smb_panic("connection_ok: conn->cli->initialised is False!");
		return False;
	}

	if (conn->cli->fd == -1) {
		DEBUG(3, ("Connection to %s for domain %s (pipe %s) has died or was never started (fd == -1)\n", 
			  conn->controller, conn->domain, conn->pipe_name));
		return False;
	}
	
	return True;
}

/* Get a connection to the remote DC and open the pipe.  If there is already a connection, use that */

static NTSTATUS get_connection_from_cache(const char *domain, const char *pipe_name,
		struct winbindd_cm_conn **conn_out, BOOL keep_mutex) 
{
	struct winbindd_cm_conn *conn, conn_temp;
	NTSTATUS result;

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) && 
		    strequal(conn->pipe_name, pipe_name)) {
			if (!connection_ok(conn)) {
				if (conn->cli)
					cli_shutdown(conn->cli);
				ZERO_STRUCT(conn_temp);
				conn_temp.next = conn->next;
				DLIST_REMOVE(cm_conns, conn);
				SAFE_FREE(conn);
				conn = &conn_temp;  /* Just to keep the loop moving */
			} else {
				if (keep_mutex) {
					if (!secrets_named_mutex(conn->controller,
							WINBIND_SERVER_MUTEX_WAIT_TIME, &conn->mutex_ref_count))
						DEBUG(0,("get_connection_from_cache: mutex grab failed for %s\n",
							conn->controller));
				}
				break;
			}
		}
	}
	
	if (!conn) {
		if (!(conn = malloc(sizeof(*conn))))
			return NT_STATUS_NO_MEMORY;
		
		ZERO_STRUCTP(conn);
		
		if (!NT_STATUS_IS_OK(result = cm_open_connection(domain, get_pipe_index(pipe_name), conn, keep_mutex))) {
			DEBUG(3, ("Could not open a connection to %s for %s (%s)\n", 
				  domain, pipe_name, nt_errstr(result)));
		        SAFE_FREE(conn);
			return result;
		}
		DLIST_ADD(cm_conns, conn);		
	}
	
	*conn_out = conn;
	return NT_STATUS_OK;
}


/**********************************************************************************
**********************************************************************************/

BOOL cm_check_for_native_mode_win2k( const char *domain )
{
	NTSTATUS 		result;
	struct winbindd_cm_conn	conn;
	DS_DOMINFO_CTR		ctr;
	BOOL			ret = False;
	
	ZERO_STRUCT( conn );
	ZERO_STRUCT( ctr );
	
	
	if ( !NT_STATUS_IS_OK(result = cm_open_connection(domain, PI_LSARPC_DS, &conn, False)) ) {
		DEBUG(5, ("cm_check_for_native_mode_win2k: Could not open a connection to %s for PIPE_LSARPC (%s)\n", 
			  domain, nt_errstr(result)));
		return False;
	}
	
	if ( conn.cli ) {
		if ( !NT_STATUS_IS_OK(cli_ds_getprimarydominfo( conn.cli, 
				conn.cli->mem_ctx, DsRolePrimaryDomainInfoBasic, &ctr)) ) {
			ret = False;
			goto done;
		}
	}
				
	if ( (ctr.basic->flags & DSROLE_PRIMARY_DS_RUNNING) 
			&& !(ctr.basic->flags & DSROLE_PRIMARY_DS_MIXED_MODE) )
		ret = True;

done:
	if ( conn.cli )
		cli_shutdown( conn.cli );
	
	return ret;
}



/* Return a LSA policy handle on a domain */

CLI_POLICY_HND *cm_get_lsa_handle(const char *domain)
{
	struct winbindd_cm_conn *conn;
	uint32 des_access = SEC_RIGHTS_MAXIMUM_ALLOWED;
	NTSTATUS result;
	static CLI_POLICY_HND hnd;

	/* Look for existing connections */

	if (!NT_STATUS_IS_OK(result = get_connection_from_cache(domain, PIPE_LSARPC, &conn, False)))
		return NULL;

	/* This *shitty* code needs scrapping ! JRA */
	if (policy_handle_is_valid(&conn->pol)) {
		hnd.pol = conn->pol;
		hnd.cli = conn->cli;
		return &hnd;
	}
	
	result = cli_lsa_open_policy(conn->cli, conn->cli->mem_ctx, False, 
				     des_access, &conn->pol);

	if (!NT_STATUS_IS_OK(result)) {
		/* Hit the cache code again.  This cleans out the old connection and gets a new one */
		if (conn->cli->fd == -1) { /* Try again, if the remote host disapeared */
			if (!NT_STATUS_IS_OK(result = get_connection_from_cache(domain, PIPE_LSARPC, &conn, False)))
				return NULL;

			result = cli_lsa_open_policy(conn->cli, conn->cli->mem_ctx, False, 
						     des_access, &conn->pol);
		}

		if (!NT_STATUS_IS_OK(result)) {
			cli_shutdown(conn->cli);
			DLIST_REMOVE(cm_conns, conn);
			SAFE_FREE(conn);
			return NULL;
		}
	}	

	hnd.pol = conn->pol;
	hnd.cli = conn->cli;

	return &hnd;
}

/* Return a SAM policy handle on a domain */

CLI_POLICY_HND *cm_get_sam_handle(char *domain)
{ 
	struct winbindd_cm_conn *conn;
	uint32 des_access = SEC_RIGHTS_MAXIMUM_ALLOWED;
	NTSTATUS result;
	static CLI_POLICY_HND hnd;

	/* Look for existing connections */

	if (!NT_STATUS_IS_OK(result = get_connection_from_cache(domain, PIPE_SAMR, &conn, False)))
		return NULL;
	
	/* This *shitty* code needs scrapping ! JRA */
	if (policy_handle_is_valid(&conn->pol)) {
		hnd.pol = conn->pol;
		hnd.cli = conn->cli;
		return &hnd;
	}
	result = cli_samr_connect(conn->cli, conn->cli->mem_ctx,
				  des_access, &conn->pol);

	if (!NT_STATUS_IS_OK(result)) {
		/* Hit the cache code again.  This cleans out the old connection and gets a new one */
		if (conn->cli->fd == -1) { /* Try again, if the remote host disapeared */
			if (!NT_STATUS_IS_OK(result = get_connection_from_cache(domain, PIPE_SAMR, &conn, False)))
				return NULL;

			result = cli_samr_connect(conn->cli, conn->cli->mem_ctx,
						  des_access, &conn->pol);
		}

		if (!NT_STATUS_IS_OK(result)) {
			cli_shutdown(conn->cli);
			DLIST_REMOVE(cm_conns, conn);
			SAFE_FREE(conn);
			return NULL;
		}
	}	

	hnd.pol = conn->pol;
	hnd.cli = conn->cli;

	return &hnd;
}

#if 0  /* This code now *well* out of date */

/* Return a SAM domain policy handle on a domain */

CLI_POLICY_HND *cm_get_sam_dom_handle(char *domain, DOM_SID *domain_sid)
{
	struct winbindd_cm_conn *conn, *basic_conn = NULL;
	static CLI_POLICY_HND hnd;
	NTSTATUS result;
	uint32 des_access = SEC_RIGHTS_MAXIMUM_ALLOWED;

	/* Look for existing connections */

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) &&
		    strequal(conn->pipe_name, PIPE_SAMR) &&
		    conn->pipe_data.samr.pipe_type == SAM_PIPE_DOM) {

			if (!connection_ok(conn)) {
				/* Shutdown cli?  Free conn?  Allow retry of DC? */
				DLIST_REMOVE(cm_conns, conn);
				return NULL;
			}

			goto ok;
		}
	}

	/* Create a basic handle to open a domain handle from */

	if (!cm_get_sam_handle(domain))
		return False;

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) &&
		    strequal(conn->pipe_name, PIPE_SAMR) &&
		    conn->pipe_data.samr.pipe_type == SAM_PIPE_BASIC)
			basic_conn = conn;
	}
	
	if (!(conn = (struct winbindd_cm_conn *)
	      malloc(sizeof(struct winbindd_cm_conn))))
		return NULL;
	
	ZERO_STRUCTP(conn);

	fstrcpy(conn->domain, basic_conn->domain);
	fstrcpy(conn->controller, basic_conn->controller);
	fstrcpy(conn->pipe_name, basic_conn->pipe_name);

	conn->pipe_data.samr.pipe_type = SAM_PIPE_DOM;
	conn->cli = basic_conn->cli;

	result = cli_samr_open_domain(conn->cli, conn->cli->mem_ctx,
				      &basic_conn->pol, des_access, 
				      domain_sid, &conn->pol);

	if (!NT_STATUS_IS_OK(result))
		return NULL;

	/* Add to list */

	DLIST_ADD(cm_conns, conn);

 ok:
	hnd.pol = conn->pol;
	hnd.cli = conn->cli;

	return &hnd;
}

/* Return a SAM policy handle on a domain user */

CLI_POLICY_HND *cm_get_sam_user_handle(char *domain, DOM_SID *domain_sid,
				       uint32 user_rid)
{
	struct winbindd_cm_conn *conn, *basic_conn = NULL;
	static CLI_POLICY_HND hnd;
	NTSTATUS result;
	uint32 des_access = SEC_RIGHTS_MAXIMUM_ALLOWED;

	/* Look for existing connections */

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) &&
		    strequal(conn->pipe_name, PIPE_SAMR) &&
		    conn->pipe_data.samr.pipe_type == SAM_PIPE_USER &&
		    conn->pipe_data.samr.rid == user_rid) {

			if (!connection_ok(conn)) {
				/* Shutdown cli?  Free conn?  Allow retry of DC? */
				DLIST_REMOVE(cm_conns, conn);
				return NULL;
			}
		
			goto ok;
		}
	}

	/* Create a domain handle to open a user handle from */

	if (!cm_get_sam_dom_handle(domain, domain_sid))
		return NULL;

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) &&
		    strequal(conn->pipe_name, PIPE_SAMR) &&
		    conn->pipe_data.samr.pipe_type == SAM_PIPE_DOM)
			basic_conn = conn;
	}
	
	if (!basic_conn) {
		DEBUG(0, ("No domain sam handle was created!\n"));
		return NULL;
	}

	if (!(conn = (struct winbindd_cm_conn *)
	      malloc(sizeof(struct winbindd_cm_conn))))
		return NULL;
	
	ZERO_STRUCTP(conn);

	fstrcpy(conn->domain, basic_conn->domain);
	fstrcpy(conn->controller, basic_conn->controller);
	fstrcpy(conn->pipe_name, basic_conn->pipe_name);
	
	conn->pipe_data.samr.pipe_type = SAM_PIPE_USER;
	conn->cli = basic_conn->cli;
	conn->pipe_data.samr.rid = user_rid;

	result = cli_samr_open_user(conn->cli, conn->cli->mem_ctx,
				    &basic_conn->pol, des_access, user_rid,
				    &conn->pol);

	if (!NT_STATUS_IS_OK(result))
		return NULL;

	/* Add to list */

	DLIST_ADD(cm_conns, conn);

 ok:
	hnd.pol = conn->pol;
	hnd.cli = conn->cli;

	return &hnd;
}

/* Return a SAM policy handle on a domain group */

CLI_POLICY_HND *cm_get_sam_group_handle(char *domain, DOM_SID *domain_sid,
					uint32 group_rid)
{
	struct winbindd_cm_conn *conn, *basic_conn = NULL;
	static CLI_POLICY_HND hnd;
	NTSTATUS result;
	uint32 des_access = SEC_RIGHTS_MAXIMUM_ALLOWED;

	/* Look for existing connections */

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) &&
		    strequal(conn->pipe_name, PIPE_SAMR) &&
		    conn->pipe_data.samr.pipe_type == SAM_PIPE_GROUP &&
		    conn->pipe_data.samr.rid == group_rid) {

			if (!connection_ok(conn)) {
				/* Shutdown cli?  Free conn?  Allow retry of DC? */
				DLIST_REMOVE(cm_conns, conn);
				return NULL;
			}
		
			goto ok;
		}
	}

	/* Create a domain handle to open a user handle from */

	if (!cm_get_sam_dom_handle(domain, domain_sid))
		return NULL;

	for (conn = cm_conns; conn; conn = conn->next) {
		if (strequal(conn->domain, domain) &&
		    strequal(conn->pipe_name, PIPE_SAMR) &&
		    conn->pipe_data.samr.pipe_type == SAM_PIPE_DOM)
			basic_conn = conn;
	}
	
	if (!basic_conn) {
		DEBUG(0, ("No domain sam handle was created!\n"));
		return NULL;
	}

	if (!(conn = (struct winbindd_cm_conn *)
	      malloc(sizeof(struct winbindd_cm_conn))))
		return NULL;
	
	ZERO_STRUCTP(conn);

	fstrcpy(conn->domain, basic_conn->domain);
	fstrcpy(conn->controller, basic_conn->controller);
	fstrcpy(conn->pipe_name, basic_conn->pipe_name);
	
	conn->pipe_data.samr.pipe_type = SAM_PIPE_GROUP;
	conn->cli = basic_conn->cli;
	conn->pipe_data.samr.rid = group_rid;

	result = cli_samr_open_group(conn->cli, conn->cli->mem_ctx,
				    &basic_conn->pol, des_access, group_rid,
				    &conn->pol);

	if (!NT_STATUS_IS_OK(result))
		return NULL;

	/* Add to list */

	DLIST_ADD(cm_conns, conn);

 ok:
	hnd.pol = conn->pol;
	hnd.cli = conn->cli;

	return &hnd;
}

#endif

/* Get a handle on a netlogon pipe.  This is a bit of a hack to re-use the
   netlogon pipe as no handle is returned. */

NTSTATUS cm_get_netlogon_cli(const char *domain, const unsigned char *trust_passwd,
			     struct cli_state **cli)
{
	NTSTATUS result = NT_STATUS_DOMAIN_CONTROLLER_NOT_FOUND;
	struct winbindd_cm_conn *conn;
	uint32 neg_flags = 0x000001ff;

	if (!cli)
		return NT_STATUS_INVALID_PARAMETER;

	/* Open an initial conection - keep the mutex. */

	if (!NT_STATUS_IS_OK(result = get_connection_from_cache(domain, PIPE_NETLOGON, &conn, True)))
		return result;
	
	result = cli_nt_setup_creds(conn->cli, get_sec_chan(), trust_passwd, &neg_flags, 2);

	if (conn->mutex_ref_count)
		secrets_named_mutex_release(conn->controller, &conn->mutex_ref_count);

	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(0, ("error connecting to domain password server: %s\n",
			  nt_errstr(result)));
		
		/* Hit the cache code again.  This cleans out the old connection and gets a new one */
		if (conn->cli->fd == -1) {

			if (!NT_STATUS_IS_OK(result = get_connection_from_cache(domain, PIPE_NETLOGON, &conn, True)))
				return result;
			
			/* Try again */
			result = cli_nt_setup_creds( conn->cli, get_sec_chan(),trust_passwd, &neg_flags, 2);

			if (conn->mutex_ref_count)
				secrets_named_mutex_release(conn->controller, &conn->mutex_ref_count);
		}
		
		if (!NT_STATUS_IS_OK(result)) {
			cli_shutdown(conn->cli);
			DLIST_REMOVE(cm_conns, conn);
			SAFE_FREE(conn);
			return result;
		}
	}

	*cli = conn->cli;

	return result;
}

/* Dump the current connection status */

static void dump_conn_list(void)
{
	struct winbindd_cm_conn *con;

	DEBUG(0, ("\tDomain	     Controller	     Pipe\n"));

	for(con = cm_conns; con; con = con->next) {
		char *msg;

		/* Display pipe info */
		
		if (asprintf(&msg, "\t%-15s %-15s %-16s", con->domain, con->controller, con->pipe_name) < 0) {
			DEBUG(0, ("Error: not enough memory!\n"));
		} else {
			DEBUG(0, ("%s\n", msg));
			SAFE_FREE(msg);
		}
	}
}

void winbindd_cm_status(void)
{
	/* List open connections */

	DEBUG(0, ("winbindd connection manager status:\n"));

	if (cm_conns)
		dump_conn_list();
	else
		DEBUG(0, ("\tNo active connections\n"));
}
