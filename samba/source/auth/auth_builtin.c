/* 
   Unix SMB/CIFS implementation.
   Generic authenticaion types
   Copyright (C) Andrew Bartlett         2001-2002
   Copyright (C) Jelmer Vernooij              2002
   
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
#define DBGC_CLASS DBGC_AUTH

/**
 * Return a guest logon for guest users (username = "")
 *
 * Typically used as the first module in the auth chain, this allows
 * guest logons to be dealt with in one place.  Non-guest logons 'fail'
 * and pass onto the next module.
 **/

static NTSTATUS check_guest_security(const struct auth_context *auth_context,
				     void *my_private_data, 
				     TALLOC_CTX *mem_ctx,
				     const auth_usersupplied_info *user_info, 
				     auth_serversupplied_info **server_info)
{
	NTSTATUS nt_status = NT_STATUS_LOGON_FAILURE;

	if (!(user_info->internal_username.str 
	      && *user_info->internal_username.str)) {
		nt_status = make_server_info_guest(server_info);
	}

	return nt_status;
}

/* Guest modules initialisation */

NTSTATUS auth_init_guest(struct auth_context *auth_context, const char *options, auth_methods **auth_method) 
{
	if (!make_auth_methods(auth_context, auth_method))
		return NT_STATUS_NO_MEMORY;

	(*auth_method)->auth = check_guest_security;
	(*auth_method)->name = "guest";
	return NT_STATUS_OK;
}

/** 
 * Return an error based on username
 *
 * This function allows the testing of obsure errors, as well as the generation
 * of NT_STATUS -> DOS error mapping tables.
 *
 * This module is of no value to end-users.
 *
 * The password is ignored.
 *
 * @return An NTSTATUS value based on the username
 **/

static NTSTATUS check_name_to_ntstatus_security(const struct auth_context *auth_context,
						void *my_private_data, 
						TALLOC_CTX *mem_ctx,
						const auth_usersupplied_info *user_info, 
						auth_serversupplied_info **server_info)
{
	NTSTATUS nt_status;
	fstring user;
	long error_num;
	fstrcpy(user, user_info->smb_name.str);
	
	if (strncasecmp("NT_STATUS", user, strlen("NT_STATUS")) == 0) {
		strupper(user);
		return nt_status_string_to_code(user);
	}

	strlower(user);
	error_num = strtoul(user, NULL, 16);
	
	DEBUG(5,("check_name_to_ntstatus_security: Error for user %s was %lx\n", user, error_num));

	nt_status = NT_STATUS(error_num);
	
	return nt_status;
}

/** Module initailisation function */

NTSTATUS auth_init_name_to_ntstatus(struct auth_context *auth_context, const char *param, auth_methods **auth_method) 
{
	if (!make_auth_methods(auth_context, auth_method))
		return NT_STATUS_NO_MEMORY;

	(*auth_method)->auth = check_name_to_ntstatus_security;
	(*auth_method)->name = "name_to_ntstatus";
	return NT_STATUS_OK;
}

/** 
 * Return a 'fixed' challenge instead of a varaible one.
 *
 * The idea of this function is to make packet snifs consistant
 * with a fixed challenge, so as to aid debugging.
 *
 * This module is of no value to end-users.
 *
 * This module does not actually authenticate the user, but
 * just pretenteds to need a specified challenge.  
 * This module removes *all* security from the challenge-response system
 *
 * @return NT_STATUS_UNSUCCESSFUL
 **/

static NTSTATUS check_fixed_challenge_security(const struct auth_context *auth_context,
					       void *my_private_data, 
					       TALLOC_CTX *mem_ctx,
					       const auth_usersupplied_info *user_info, 
					       auth_serversupplied_info **server_info)
{
	return NT_STATUS_UNSUCCESSFUL;
}

/****************************************************************************
 Get the challenge out of a password server.
****************************************************************************/

static DATA_BLOB auth_get_fixed_challenge(const struct auth_context *auth_context,
					  void **my_private_data, 
					  TALLOC_CTX *mem_ctx)
{
	const char *challenge = "I am a teapot";   
	return data_blob(challenge, 8);
}


/** Module initailisation function */

NTSTATUS auth_init_fixed_challenge(struct auth_context *auth_context, const char *param, auth_methods **auth_method) 
{
	if (!make_auth_methods(auth_context, auth_method))
		return NT_STATUS_NO_MEMORY;

	(*auth_method)->auth = check_fixed_challenge_security;
	(*auth_method)->get_chal = auth_get_fixed_challenge;
	(*auth_method)->name = "fixed_challenge";
	return NT_STATUS_OK;
}

/**
 * Outsorce an auth module to an external loadable .so
 *
 * Only works on systems with dlopen() etc.
 **/

/* Plugin modules initialisation */

NTSTATUS auth_init_plugin(struct auth_context *auth_context, const char *param, auth_methods **auth_method) 
{
	void * dl_handle;
	char *plugin_param, *plugin_name, *p;
	auth_init_function plugin_init;

	if (param == NULL) {
		DEBUG(0, ("auth_init_plugin: The plugin module needs an argument!\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	plugin_name = smb_xstrdup(param);
	p = strchr(plugin_name, ':');
	if (p) {
		*p = 0;
		plugin_param = p+1;
		trim_string(plugin_param, " ", " ");
	} else plugin_param = NULL;

	trim_string(plugin_name, " ", " ");

	DEBUG(5, ("auth_init_plugin: Trying to load auth plugin %s\n", plugin_name));
	dl_handle = sys_dlopen(plugin_name, RTLD_NOW );
	if (!dl_handle) {
		DEBUG(0, ("auth_init_plugin: Failed to load auth plugin %s using sys_dlopen (%s)\n",
					plugin_name, sys_dlerror()));
		return NT_STATUS_UNSUCCESSFUL;
	}
    
	plugin_init = sys_dlsym(dl_handle, "auth_init");
	if (!plugin_init){
		DEBUG(0, ("Failed to find function 'auth_init' using sys_dlsym in sam plugin %s (%s)\n",
					plugin_name, sys_dlerror()));	    
		return NT_STATUS_UNSUCCESSFUL;
	}

	DEBUG(5, ("Starting sam plugin %s with paramater %s\n", plugin_name, plugin_param?plugin_param:"(null)"));
	return plugin_init(auth_context, plugin_param, auth_method);
}
