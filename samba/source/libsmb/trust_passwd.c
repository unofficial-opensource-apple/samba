/* 
 *  Unix SMB/CIFS implementation.
 *  Routines to change trust account passwords.
 *  Copyright (C) Andrew Bartlett                   2001.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "includes.h"

/*********************************************************
 Change the domain password on the PDC.

 Just changes the password betwen the two values specified.

 Caller must have the cli connected to the netlogon pipe
 already.
**********************************************************/
static NTSTATUS just_change_the_password(struct cli_state *cli, TALLOC_CTX *mem_ctx, 
					 unsigned char orig_trust_passwd_hash[16],
					 unsigned char new_trust_passwd_hash[16])
{
	NTSTATUS result;
	uint32 neg_flags = 0x000001ff;

	result = cli_nt_setup_creds(cli, get_sec_chan(), orig_trust_passwd_hash, &neg_flags, 2);
	
	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(1,("just_change_the_password: unable to setup creds (%s)!\n",
			 nt_errstr(result)));
		return result;
	}

	result = cli_net_srv_pwset(cli, mem_ctx, global_myname(), new_trust_passwd_hash);

	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(0,("just_change_the_password: unable to change password (%s)!\n",
			 nt_errstr(result)));
	}
	return result;
}

/*********************************************************
 Change the domain password on the PDC.
 Store the password ourselves, but use the supplied password
 Caller must have already setup the connection to the NETLOGON pipe
**********************************************************/

NTSTATUS trust_pw_change_and_store_it(struct cli_state *cli, TALLOC_CTX *mem_ctx, 
				      unsigned char orig_trust_passwd_hash[16])
{
	unsigned char new_trust_passwd_hash[16];
	char *new_trust_passwd;
	char *str;
	NTSTATUS nt_status;
		
	/* Create a random machine account password */
	str = generate_random_str(DEFAULT_TRUST_ACCOUNT_PASSWORD_LENGTH);
	new_trust_passwd = talloc_strdup(mem_ctx, str);
	
	E_md4hash(new_trust_passwd, new_trust_passwd_hash);

	nt_status = just_change_the_password(cli, mem_ctx, orig_trust_passwd_hash,
					     new_trust_passwd_hash);
	
	if (NT_STATUS_IS_OK(nt_status)) {
		DEBUG(3,("%s : trust_pw_change_and_store_it: Changed password.\n", 
			 timestring(False)));
		/*
		 * Return the result of trying to write the new password
		 * back into the trust account file.
		 */
		if (!secrets_store_machine_password(new_trust_passwd)) {
			nt_status = NT_STATUS_UNSUCCESSFUL;
		}
	}

	return nt_status;
}

/*********************************************************
 Change the domain password on the PDC.
 Do most of the legwork ourselfs.  Caller must have
 already setup the connection to the NETLOGON pipe
**********************************************************/

NTSTATUS trust_pw_find_change_and_store_it(struct cli_state *cli, TALLOC_CTX *mem_ctx, 
					   const char *domain) 
{
	unsigned char old_trust_passwd_hash[16];
	char *up_domain;
	
	up_domain = talloc_strdup(mem_ctx, domain);

	if (!secrets_fetch_trust_account_password(domain,
						  old_trust_passwd_hash, 
						  NULL)) {
		DEBUG(0, ("could not fetch domain secrets for domain %s!\n", domain));
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	return trust_pw_change_and_store_it(cli, mem_ctx, old_trust_passwd_hash);
	
}					 
