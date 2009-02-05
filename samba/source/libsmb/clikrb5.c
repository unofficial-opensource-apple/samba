/* 
   Unix SMB/CIFS implementation.
   simple kerberos5 routines for active directory
   Copyright (C) Andrew Tridgell 2001
   Copyright (C) Luke Howard 2002
   
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

#ifdef HAVE_KRB5

#ifndef HAVE_KRB5_SET_REAL_TIME
/*
 * This function is not in the Heimdal mainline.
 */
 krb5_error_code krb5_set_real_time(krb5_context context, int32_t seconds, int32_t microseconds)
{
	krb5_error_code ret;
	int32_t sec, usec;

	ret = krb5_us_timeofday(context, &sec, &usec);
	if (ret)
		return ret;

#ifdef __APPLE__
#warning "krb5_set_real_time: hacking around issues for now"
#else
	context->kdc_sec_offset = seconds - sec;
	context->kdc_usec_offset = microseconds - usec;
#endif

	return 0;
}
#endif

#if defined(HAVE_KRB5_SET_DEFAULT_IN_TKT_ETYPES) && !defined(HAVE_KRB5_SET_DEFAULT_TGS_KTYPES)
 krb5_error_code krb5_set_default_tgs_ktypes(krb5_context ctx, const krb5_enctype *enc)
{
	return krb5_set_default_in_tkt_etypes(ctx, enc);
}
#endif

#if defined(HAVE_ADDR_TYPE_IN_KRB5_ADDRESS)
/* HEIMDAL */
 void setup_kaddr( krb5_address *pkaddr, struct sockaddr *paddr)
{
	pkaddr->addr_type = KRB5_ADDRESS_INET;
	pkaddr->address.length = sizeof(((struct sockaddr_in *)paddr)->sin_addr);
	pkaddr->address.data = (char *)&(((struct sockaddr_in *)paddr)->sin_addr);
}
#elif defined(HAVE_ADDRTYPE_IN_KRB5_ADDRESS)
/* MIT */
 void setup_kaddr( krb5_address *pkaddr, struct sockaddr *paddr)
{
	pkaddr->addrtype = ADDRTYPE_INET;
	pkaddr->length = sizeof(((struct sockaddr_in *)paddr)->sin_addr);
	pkaddr->contents = (char *)&(((struct sockaddr_in *)paddr)->sin_addr);
}
#else
 __ERROR__XX__UNKNOWN_ADDRTYPE
#endif

#if !defined(KRB5_PRINCIPAL2SALT)
/*
 * lib/krb5/krb/pr_to_salt.c
 *
 * Copyright 1990 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 *
 * krb5_principal2salt()
 */

/*
 * Convert a krb5_principal into the default salt for that principal.
 */
krb5_error_code
krb5_principal2salt_internal(context, pr, ret, use_realm)
    krb5_context context;
    register krb5_const_principal pr;
    krb5_data *ret;
    int use_realm;
{
    int size = 0, offset = 0;
    krb5_int32 nelem;
    register int i;

    if (pr == 0) {
	ret->length = 0;
	ret->data = 0;
	return 0;
    }

    nelem = krb5_princ_size(context, pr);

    if (use_realm)
	    size += krb5_princ_realm(context, pr)->length;

    for (i = 0; i < (int) nelem; i++)
	size += krb5_princ_component(context, pr, i)->length;

    ret->length = size;
    if (!(ret->data = malloc (size)))
	return ENOMEM;

    if (use_realm) {
	    offset = krb5_princ_realm(context, pr)->length;
	    memcpy(ret->data, krb5_princ_realm(context, pr)->data, offset);
    }

    for (i = 0; i < (int) nelem; i++) {
	memcpy(&ret->data[offset], krb5_princ_component(context, pr, i)->data,
	       krb5_princ_component(context, pr, i)->length);
	offset += krb5_princ_component(context, pr, i)->length;
    }
    return 0;
}

krb5_error_code
krb5_principal2salt(context, pr, ret)
    krb5_context context;
    register krb5_const_principal pr;
    krb5_data *ret;
{
	return krb5_principal2salt_internal(context, pr, ret, 1);
}

krb5_error_code
krb5_principal2salt_norealm(context, pr, ret)
    krb5_context context;
    register krb5_const_principal pr;
    krb5_data *ret;
{
	return krb5_principal2salt_internal(context, pr, ret, 0);
}
#define HAVE_KRB5_PRINCIPAL2SALT
#endif

#if defined(HAVE_KRB5_PRINCIPAL2SALT) && defined(HAVE_KRB5_USE_ENCTYPE) && defined(HAVE_KRB5_STRING_TO_KEY)
 int create_kerberos_key_from_string(krb5_context context,
					krb5_principal host_princ,
					krb5_data *password,
					krb5_keyblock *key,
					krb5_enctype enctype)
{
	int ret;
	krb5_data salt;
	krb5_encrypt_block eblock;

	ret = krb5_principal2salt(context, host_princ, &salt);
	if (ret) {
		DEBUG(1,("krb5_principal2salt failed (%s)\n", error_message(ret)));
		return ret;
	}
	krb5_use_enctype(context, &eblock, enctype);
	return krb5_string_to_key(context, &eblock, key, password, &salt);
}
#elif defined(HAVE_KRB5_GET_PW_SALT) && defined(HAVE_KRB5_STRING_TO_KEY_SALT)
 int create_kerberos_key_from_string(krb5_context context,
					krb5_principal host_princ,
					krb5_data *password,
					krb5_keyblock *key,
					krb5_enctype enctype)
{
	int ret;
	krb5_salt salt;

	ret = krb5_get_pw_salt(context, host_princ, &salt);
	if (ret) {
		DEBUG(1,("krb5_get_pw_salt failed (%s)\n", error_message(ret)));
		return ret;
	}
	return krb5_string_to_key_salt(context, enctype, password->data,
		salt, key);
}
#else
 __ERROR_XX_UNKNOWN_CREATE_KEY_FUNCTIONS
#endif

#if defined(HAVE_KRB5_GET_PERMITTED_ENCTYPES)
krb5_error_code get_kerberos_allowed_etypes(krb5_context context, 
					    krb5_enctype **enctypes)
{
	return krb5_get_permitted_enctypes(context, enctypes);
}
#elif defined(HAVE_KRB5_GET_DEFAULT_IN_TKT_ETYPES)
krb5_error_code get_kerberos_allowed_etypes(krb5_context context, 
					    krb5_enctype **enctypes)
{
	return krb5_get_default_in_tkt_etypes(context, enctypes);
}
#else
#warning  __ERROR_XX_UNKNOWN_GET_ENCTYPES_FUNCTIONS
#endif

 void free_kerberos_etypes(krb5_context context, 
			   krb5_enctype *enctypes)
{
#if defined(HAVE_KRB5_FREE_KTYPES)
	krb5_free_ktypes(context, enctypes);
	return;
#else
	SAFE_FREE(enctypes);
	return;
#endif
}

#if defined(HAVE_KRB5_AUTH_CON_SETKEY) && !defined(HAVE_KRB5_AUTH_CON_SETUSERUSERKEY)
 krb5_error_code krb5_auth_con_setuseruserkey(krb5_context context,
					krb5_auth_context auth_context,
					krb5_keyblock *keyblock)
{
	return krb5_auth_con_setkey(context, auth_context, keyblock);
}
#endif

 void get_auth_data_from_tkt(DATA_BLOB *auth_data, krb5_ticket *tkt)
{
#if defined(HAVE_KRB5_TKT_ENC_PART2)
	if (tkt->enc_part2)
		*auth_data = data_blob(tkt->enc_part2->authorization_data[0]->contents,
			tkt->enc_part2->authorization_data[0]->length);
#else
	if (tkt->ticket.authorization_data && tkt->ticket.authorization_data->len)
		*auth_data = data_blob(tkt->ticket.authorization_data->val->ad_data.data,
			tkt->ticket.authorization_data->val->ad_data.length);
#endif
}

 krb5_const_principal get_principal_from_tkt(krb5_ticket *tkt)
{
#if defined(HAVE_KRB5_TKT_ENC_PART2)
	return tkt->enc_part2->client;
#else
	return tkt->client;
#endif
}

#if !defined(HAVE_KRB5_LOCATE_KDC)
 krb5_error_code krb5_locate_kdc(krb5_context ctx, const krb5_data *realm, struct sockaddr **addr_pp, int *naddrs, int get_masters)
{
#ifdef __APPLE__
#warning "krb5_locate_kdc not exported"
#else
	krb5_krbhst_handle hnd;
	krb5_krbhst_info *hinfo;
	krb5_error_code rc;
	int num_kdcs, i;
	struct sockaddr *sa;

	*addr_pp = NULL;
	*naddrs = 0;

	rc = krb5_krbhst_init(ctx, realm->data, KRB5_KRBHST_KDC, &hnd);
	if (rc) {
		DEBUG(0, ("krb5_locate_kdc: krb5_krbhst_init failed (%s)\n", error_message(rc)));
		return rc;
	}

	for ( num_kdcs = 0; (rc = krb5_krbhst_next(ctx, hnd, &hinfo) == 0); num_kdcs++)
		;

	krb5_krbhst_reset(ctx, hnd);

	if (!num_kdcs) {
		DEBUG(0, ("krb5_locate_kdc: zero kdcs found !\n"));
		krb5_krbhst_free(ctx, hnd);
		return -1;
	}

	sa = malloc( sizeof(struct sockaddr) * num_kdcs );
	if (!sa) {
		DEBUG(0, ("krb5_locate_kdc: malloc failed\n"));
		krb5_krbhst_free(ctx, hnd);
		naddrs = 0;
		return -1;
	}

	memset(*addr_pp, '\0', sizeof(struct sockaddr) * num_kdcs );

	for (i = 0; i < num_kdcs && (rc = krb5_krbhst_next(ctx, hnd, &hinfo) == 0); i++) {
		if (hinfo->ai->ai_family == AF_INET)
			memcpy(&sa[i], hinfo->ai->ai_addr, sizeof(struct sockaddr));
	}

	krb5_krbhst_free(ctx, hnd);

	*naddrs = num_kdcs;
	*addr_pp = sa;
#endif /* __APPLE__ */
	return 0;
}
#endif

/*
  we can't use krb5_mk_req because w2k wants the service to be in a particular format
*/
static krb5_error_code krb5_mk_req2(krb5_context context, 
				    krb5_auth_context *auth_context, 
				    const krb5_flags ap_req_options,
				    const char *principal,
				    krb5_ccache ccache, 
				    krb5_data *outbuf)
{
	krb5_error_code 	  retval;
	krb5_principal	  server;
	krb5_creds 		* credsp;
	krb5_creds 		  creds;
	krb5_data in_data;
	
	retval = krb5_parse_name(context, principal, &server);
	if (retval) {
		DEBUG(1,("Failed to parse principal %s\n", principal));
		return retval;
	}
	
	/* obtain ticket & session key */
	memset((char *)&creds, 0, sizeof(creds));
	if ((retval = krb5_copy_principal(context, server, &creds.server))) {
		DEBUG(1,("krb5_copy_principal failed (%s)\n", 
			 error_message(retval)));
		goto cleanup_princ;
	}
	
	if ((retval = krb5_cc_get_principal(context, ccache, &creds.client))) {
		DEBUG(1,("krb5_cc_get_principal failed (%s)\n", 
			 error_message(retval)));
		goto cleanup_creds;
	}

	if ((retval = krb5_get_credentials(context, 0,
					   ccache, &creds, &credsp))) {
		DEBUG(1,("krb5_get_credentials failed for %s (%s)\n", 
			 principal, error_message(retval)));
		goto cleanup_creds;
	}

	/* cope with the ticket being in the future due to clock skew */
	if ((unsigned)credsp->times.starttime > time(NULL)) {
		time_t t = time(NULL);
		int time_offset = (unsigned)credsp->times.starttime - t;
		DEBUG(4,("Advancing clock by %d seconds to cope with clock skew\n", time_offset));
		krb5_set_real_time(context, t + time_offset + 1, 0);
	}

	in_data.length = 0;
	retval = krb5_mk_req_extended(context, auth_context, ap_req_options, 
				      &in_data, credsp, outbuf);
	if (retval) {
		DEBUG(1,("krb5_mk_req_extended failed (%s)\n", 
			 error_message(retval)));
	}
	
	krb5_free_creds(context, credsp);

cleanup_creds:
	krb5_free_cred_contents(context, &creds);

cleanup_princ:
	krb5_free_principal(context, server);

	return retval;
}

/*
  get a kerberos5 ticket for the given service 
*/
DATA_BLOB krb5_get_ticket(const char *principal, time_t time_offset)
{
	krb5_error_code retval;
	krb5_data packet;
	krb5_ccache ccdef;
	krb5_context context;
	krb5_auth_context auth_context = NULL;
	DATA_BLOB ret;
	krb5_enctype enc_types[] = {
#ifdef ENCTYPE_ARCFOUR_HMAC
		ENCTYPE_ARCFOUR_HMAC, 
#endif
				    ENCTYPE_DES_CBC_MD5, 
				    ENCTYPE_DES_CBC_CRC, 
				    ENCTYPE_NULL};

	retval = krb5_init_context(&context);
	if (retval) {
		DEBUG(1,("krb5_init_context failed (%s)\n", 
			 error_message(retval)));
		goto failed;
	}

	if (time_offset != 0) {
		krb5_set_real_time(context, time(NULL) + time_offset, 0);
	}

	if ((retval = krb5_cc_default(context, &ccdef))) {
		DEBUG(1,("krb5_cc_default failed (%s)\n",
			 error_message(retval)));
		goto failed;
	}

	if ((retval = krb5_set_default_tgs_ktypes(context, enc_types))) {
		DEBUG(1,("krb5_set_default_tgs_ktypes failed (%s)\n",
			 error_message(retval)));
		goto failed;
	}

	if ((retval = krb5_mk_req2(context, 
				   &auth_context, 
				   0, 
				   principal,
				   ccdef, &packet))) {
		goto failed;
	}

	ret = data_blob(packet.data, packet.length);
/* Hmm, heimdal dooesn't have this - what's the correct call? */
/* 	krb5_free_data_contents(context, &packet); */
	krb5_free_context(context);
	return ret;

failed:
	if ( context )
		krb5_free_context(context);
		
	return data_blob(NULL, 0);
}

#else /* HAVE_KRB5 */
 /* this saves a few linking headaches */
 DATA_BLOB krb5_get_ticket(const char *principal, time_t time_offset)
 {
	 DEBUG(0,("NO KERBEROS SUPPORT\n"));
	 return data_blob(NULL, 0);
 }
#endif
