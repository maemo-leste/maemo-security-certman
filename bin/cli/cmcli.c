/* -*- mode:c; tab-width:4; c-basic-offset:4; -*-
 *
 * This file is part of maemo-security-certman
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Juhani M�kel� <ext-juhani.3.makela@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**
 \file cmcli.c
 \ingroup libcertman
 \brief A command-line utility for managing certificate stores

 This command-line utility can be used to list contents of certificate 
 stores, create new stores and manipulate the existing ones by adding
 and deleting certificates in them. It also serves as an example of how
 to use the certman library.
 
*/


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <maemosec_certman.h>
#include <maemosec_common.h>

extern int inspect_certificate(const char* pathname);

/*
 * Global options
 */
static int force_opt = 0;
static int save_cert = 0;
static int do_echo = 0;

/*
 * Utilities. Should maybe be added to libmaemosec_certman0
 */
static int
report_openssl_error(const char* str, size_t len, void* u)
{
	char* tmp = strrchr(str, '\n');
	if (tmp && ((tmp - str) == strlen(str)))
		*tmp = '\0';
	MAEMOSEC_DEBUG(1, "OpenSSL error '%s'", str);
	ERR_clear_error();
	return(0);
}

static int
remember_certificate(int pos, X509* cert, void* context)
{
    maemosec_key_id key_id;

    if (   0 == maemosec_certman_get_key_id((X509*)cert, key_id)
        && 0 == maemosec_certman_key_id_to_str(key_id, *(char**)context, MAEMOSEC_KEY_ID_STR_LEN)) 
    {
        MAEMOSEC_DEBUG(1, "%s: remember '%s'", __func__, *(char**)context);
        *(char**)(context) += MAEMOSEC_KEY_ID_STR_LEN;
    }
    return(0);
}

void
remember_certificates(domain_handle domain, char** inlist)
{
    char* tmp;
    tmp = (char*)malloc((MAEMOSEC_KEY_ID_STR_LEN) * maemosec_certman_nbrof_certs(domain) + 1);
    *inlist = tmp;
    maemosec_certman_iterate_certs(domain, remember_certificate, &tmp);
    *tmp = '\0';
}

static int
print_if_not_in_list(int pos, X509* cert, void* context)
{
    maemosec_key_id key_id;
    char key_id_str[MAEMOSEC_KEY_ID_STR_LEN] = "";
    char* seenlist = (char*)context;

    if (   0 == maemosec_certman_get_key_id((X509*)cert, key_id)
        && 0 == maemosec_certman_key_id_to_str(key_id, key_id_str, MAEMOSEC_KEY_ID_STR_LEN)) 
    {
        
        MAEMOSEC_DEBUG(1, "%s: check if '%s' is seen", __func__, key_id_str);
        if (NULL != seenlist) {
            while (*seenlist) {
                if (0 == strcmp(seenlist, key_id_str))
                    return(0);
                seenlist += strlen(seenlist) + 1;
            }
        }
        printf("%s\n", key_id_str);
    }
    return(0);
}

void
list_certificates(domain_handle domain, char* notinlist)
{
    maemosec_certman_iterate_certs(domain, print_if_not_in_list, notinlist);
}

void
show_certificate_id(domain_handle domain, X509* cert)
{
    maemosec_key_id key_id;
    char key_id_str[MAEMOSEC_KEY_ID_STR_LEN] = "";

    if (   0 == maemosec_certman_get_key_id((X509*)cert, key_id)
        && 0 == maemosec_certman_key_id_to_str(key_id, key_id_str, MAEMOSEC_KEY_ID_STR_LEN)) 
    {
        printf("%s\n", key_id_str);
    }
}

typedef enum {ft_x509_pem, ft_x509_der, ft_x509_sig, ft_pkcs12, ft_unknown} ft_filetype;

static ft_filetype
determine_filetype(FILE* fp, void** idata)
{
	X509* cert;
	PKCS12* cont;
	X509_SIG* ekey;

	*idata = NULL;
	rewind(fp);
	cert = PEM_read_X509(fp, NULL, 0, NULL);
	if (cert) {
		*idata = (void*)cert;
		return(ft_x509_pem);
	} else
		MAEMOSEC_DEBUG(1, "Not a PEM file");

	rewind(fp);
	cert = d2i_X509_fp(fp, NULL);
	if (cert) {
		*idata = (void*)cert;
		return(ft_x509_der);
	} else
		MAEMOSEC_DEBUG(1, "Not a DER file");

	rewind(fp);
	ekey = d2i_PKCS8_fp(fp, NULL);
	if (ekey) {
		*idata = (void*)ekey;
		return(ft_x509_sig);
	} else
		MAEMOSEC_DEBUG(1, "Not a PKCS8 file");

	rewind(fp);
	cont = d2i_PKCS12_fp(fp, NULL);
	if (cont) {
		*idata = (void*)cont;
		return(ft_pkcs12);
	} else
		MAEMOSEC_DEBUG(1, "Not a PKCS12 file");

	return(ft_unknown);
}


static int
show_cert(int pos, X509* cert, void* x)
{
	char nickname[255], keybuf[MAEMOSEC_KEY_ID_STR_LEN];
	maemosec_key_id key_id;
	int i;

	if (!cert)
		return(ENOENT);

	if (0 != maemosec_certman_get_nickname(cert, nickname, sizeof(nickname)))
		strcpy(nickname, "(no name)");

	if (0 == maemosec_certman_get_key_id(cert, key_id))
		maemosec_certman_key_id_to_str(key_id, keybuf, sizeof(keybuf));
	else {
		for (i = 0; i < MAEMOSEC_KEY_ID_STR_LEN; i++)
			keybuf[i] = '?';
		keybuf[i - 1] = '\0';
	}

	if (pos < -1) {
		if (pos < -2)
			for (i = -2; i > pos; i--)
				printf("   ");
		printf("%s", -2==pos?"   ":"+->");
	}
	printf("%s %s\n", keybuf, nickname);
	return(0);
}


static int
show_key(int pos, void* key_id, void* ctx)
{
	char keybuf[64];
	maemosec_certman_key_id_to_str(key_id, keybuf, sizeof(keybuf));
	printf("%s\n", keybuf);
	return(0);
}


static int
verify_cert(X509_STORE* store, X509* cert, int show_trust_chain)
{
	X509_STORE_CTX *csc;
	STACK_OF(X509) *chain = NULL;
	int retval;
	int rc;

	if (NULL == cert)
		return(0);

	csc = X509_STORE_CTX_new();
	if (NULL == csc) {
		fprintf(stderr, "ERROR: cannot create new context\n");
		return(0);
	}

	rc = X509_STORE_CTX_init(csc, store, cert, NULL);
	if (0 == rc) {
		fprintf(stderr, "ERROR: cannot initialize new context\n");
		return(0);
	}

	retval = (X509_verify_cert(csc) > 0);

	if (retval) {
		if (show_trust_chain) {
			int i;
			printf(" trust chain:\n");
			chain = X509_STORE_CTX_get1_chain(csc);
			for (i = sk_X509_num(chain); i > 0; i--) {
				X509* issuer = sk_X509_value(chain, i - 1);
				if (issuer) {
					show_cert(i - sk_X509_num(chain) - 2, issuer, NULL);
				}
			}
			sk_X509_pop_free(chain, X509_free);
		}
	} else
		printf(" Verification failed: %s\n", 
			   X509_verify_cert_error_string(X509_STORE_CTX_get_error(csc)));

	X509_STORE_CTX_free(csc);
	return(retval);
}


static X509*
get_cert(const char* from_file)
{
	FILE* fp;
	X509* cert;

	fp = fopen(from_file, "r");
	if (!fp) {
		fprintf(stderr, "Cannot read '%s' (%d)\n", from_file, errno);
		return(0);
	}
	cert = PEM_read_X509(fp, NULL, 0, NULL);
	if (!cert) {
		fprintf(stderr, "Cannot read certificate from '%s'\n", from_file);
	}
	fclose(fp);
	return(cert);
}


static void
write_cert_to_file(X509 *cert)
{
	char filename[255];
	FILE *tof;
	maemosec_key_id key_id;

	if(NULL == cert)
		return;
	maemosec_certman_get_key_id(cert, key_id);
	maemosec_certman_key_id_to_str(key_id, filename, sizeof(filename));
	strcat(filename, ".pem");
	tof = fopen(filename, "w+");
	if (tof) {
		// printf("Save cert to '%s'\n", filename);
		PEM_write_X509(tof, cert);
		fclose(tof);
	}
}


static X509_STORE*
X509_STORE_dup(X509_STORE* model)
{
	X509_STORE* res = NULL;
	X509_OBJECT* obj;
	STACK_OF(X509_OBJECT) *objs;
	int i;

	objs = X509_STORE_get0_objects(model);
	if (model && objs) {
		res = X509_STORE_new();
		for (i = 0; i < sk_X509_OBJECT_num(objs); i++) {
			obj = sk_X509_OBJECT_value(objs, i);
			if (X509_LU_X509 == X509_OBJECT_get_type(obj)) {
				X509_STORE_add_cert(res, X509_OBJECT_get0_X509(obj));
			}
		}
	}
	return(res);
}

struct check_ssl_args {
	int result;
	int save;
	X509_STORE* store;
};

static int
check_ssl_certificate(X509_STORE_CTX *ctx, void* arg)
{
	int i;
	X509* cert;
	struct check_ssl_args *args = (struct check_ssl_args*) arg;

	if (!ctx) {
		MAEMOSEC_ERROR("%s: invalid call", __func__);
		return(0);
	}

	/* TODO: Save current context original purpose - although I don't think it
	 * makes sense at all. */

	cert = X509_STORE_CTX_get0_cert(ctx);

	/*
	 * Do not imitate this code. This is but a feeble attempt
	 * to study the incoming certificate chain.
	 */
	if (X509_STORE_CTX_get0_chain(ctx)) {
		STACK_OF(X509) *orig_untrusted_chain, *chain;

		chain = NULL;
		orig_untrusted_chain = NULL;

		orig_untrusted_chain = X509_STORE_CTX_get1_chain(ctx);

		for (i = sk_X509_num(orig_untrusted_chain); i > 1; i--) {
			char cname[256];
			X509* untr = sk_X509_value(orig_untrusted_chain, i - 1);
			if (untr) {
				if (args->save)
					write_cert_to_file(untr);
				maemosec_certman_get_nickname(untr, cname, sizeof(cname));

				// Verify for any purpose.
				X509_STORE_CTX_set_cert(ctx, untr);

				if (0 < X509_verify_cert(ctx)) {
					MAEMOSEC_DEBUG(1, "Accepted '%s'", cname);
				} else {
					MAEMOSEC_ERROR("Invalid cert '%s' in chain (%s)", 
								   cname, X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));
				}

				/* Cleanup frees 'chain' */
				X509_STORE_CTX_cleanup(ctx);

				X509_STORE_CTX_init(ctx, args->store, NULL, NULL);
				X509_STORE_CTX_set_cert(ctx, untr);

				/* Restore our chain */
				X509_STORE_CTX_set_chain(ctx, orig_untrusted_chain);

				/* Get copy of restored chain, so that we can set it again, so
				 * that OpenSSL doesn't free our final chain */
				chain = X509_STORE_CTX_get1_chain(ctx);
				X509_STORE_CTX_set_chain(ctx, chain);
			}
		}

		/* Free our stored chain, at this point there should be a proper copy of
		 * it in ctx again */
		sk_X509_pop_free(orig_untrusted_chain, X509_free);
	}

	/* Restore original cert */
	X509_STORE_CTX_set_cert(ctx, cert);

	/* TODO: Restore original purpose */

	X509* crt = X509_STORE_CTX_get0_cert(ctx);
	if (crt) {
		show_cert(0, crt, NULL);
		args->result = X509_verify_cert(ctx);
		if (0 == args->result) {
			printf(" Verification failed: %s\n", 
				   X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));
		} else {
			STACK_OF(X509) *chain;
			chain = X509_STORE_CTX_get0_chain(ctx);
			printf(" trust chain(%d):\n", sk_X509_num(chain));
			for (i = sk_X509_num(chain); i > 0; i--) {
				X509* issuer = sk_X509_value(chain, i - 1);
				if (issuer) {
					show_cert(i - sk_X509_num(chain) - 2, issuer, NULL);
				}
			}
		}
		if (args->save)
			write_cert_to_file(crt);
	}

	return(1);
}


int
verify_object(X509_STORE *certs, const char* name)
{
	int res = 0;

	if (file_exists(name)) {
		X509 *my_cert;
		my_cert = get_cert(optarg);
		if (my_cert) {
			res = verify_cert(certs, my_cert, 1);
			X509_free(my_cert);
		}
		return(res);
	} else {
		SSL_CTX *c_ctx=NULL;
		BIO *conn;
		BIO *bio_err;
		SSL *scon;
		struct check_ssl_args args;
		int i;
		long opts;

		bio_err = BIO_new_fp(stderr, BIO_NOCLOSE);
		SSL_library_init();
		SSL_load_error_strings();

		c_ctx=SSL_CTX_new(SSLv23_method());

		if (NULL == c_ctx)
			return(0);

		/* Support TLSv1_2 only */
		opts = SSL_CTX_get_options(c_ctx);
		opts |= SSL_OP_NO_SSLv2;
		opts |= SSL_OP_NO_SSLv3;
		opts |= SSL_OP_NO_TLSv1;
		opts |= SSL_OP_NO_TLSv1_1;
		SSL_CTX_set_options(c_ctx, opts);

		args.save = save_cert;
		args.result = 0;
		/* We need the store so that we can call X509_STORE_CTX_cleanup() and
		 * X509_STORE_CTX_init() again, this needs to be called after every
		 * x509_verify_cert() */
		args.store = certs;
		SSL_CTX_set_cert_verify_callback(c_ctx, check_ssl_certificate, &args);
		SSL_CTX_set_cert_store(c_ctx, X509_STORE_dup(certs));

		conn = BIO_new(BIO_s_connect());
		if (NULL == conn)
			return(0);

		MAEMOSEC_DEBUG(1, "Connecting to %s...", name);
		BIO_set_conn_hostname(conn, name);

		scon = SSL_new(c_ctx);
		SSL_set_bio(scon, conn, conn);
		
		for (;;) {
			i = SSL_connect(scon);

			if (BIO_sock_should_retry(i)) {
				int fd, width;
				fd_set readfds;

				BIO_printf(bio_err,"DELAY\n");

				fd = SSL_get_fd(scon);
				width=fd+1;
				FD_ZERO(&readfds);
				FD_SET(fd,&readfds);
				select(width,(void *)&readfds,NULL,NULL,NULL);
			} else
				break;
		}

		MAEMOSEC_DEBUG(1, "connection made, rc=%d\n", i);
		if (0 > i)
			ERR_print_errors(bio_err);

		SSL_shutdown(scon);
		SSL_free(scon);
		SSL_CTX_free(c_ctx);
		return(args.result);
	}
}



void
get_input(char* to_buf, size_t maxlen, int hidden)
{
	int c;
	size_t pos = 0;
	struct termios old_io, new_io;

	/*
	 * Turn off echo
	 */
	if (hidden) {
		ioctl(0, TCGETS, &old_io);
		new_io = old_io;
		new_io.c_lflag &= ~ECHO;
		ioctl(0, TCSETS, &new_io);
	}

	do {
		c = fgetc(stdin);
		switch (c)
			{
			case '\n':
			case '\r':
			case EOF:
				*(to_buf + pos) = '\0';
				goto done;
			case '\b':
				if (pos) {
					pos--;
				} else {
					putchar('\a');
				}
				break;
			default:
				if (pos < maxlen) {
					*(to_buf + pos) = c;
					pos++;
				} else {
					putchar('\a');
				}
				break;
			}
	} while (1);

 done:
	if (hidden)
		ioctl(0, TCSETS, &old_io);
}


static int
install_private_key(X509_SIG* pkey)
{
	printf("%s\n", "Not implemented yet.");
	X509_SIG_free(pkey);
	return(0);
}


static int
show_storage_name(int ordnr, void* data, void* ctx)
{
	printf("\t%s\n", (char*)data);
	return(0);
}


static int
install_pkcs12(PKCS12* cont)
{
	char password[64] = "";
	char storename[64] = "";
	EVP_PKEY *pkey;
	X509 *ucert;
	STACK_OF(X509) *cas = NULL;
	int success, rc;
	domain_handle user_domain, cas_domain;

	success = PKCS12_verify_mac(cont, NULL, 0);
	if (success)
		success = PKCS12_parse(cont, NULL, &pkey, &ucert, &cas);
	else {
		printf("%s\n", "The file is encrypted.");
		do {
			success = PKCS12_verify_mac(cont, password, strlen(password));
			if (0 == success) {
				printf("%s: ", "Give password");
				get_input(password, sizeof(password), 1);
				printf("\n");
			}
		} while (0 == success);
	}

	success = PKCS12_parse(cont, password, &pkey, &ucert, &cas);
	if (0 == success) {
		fprintf(stderr, "%s\n", "ERROR: could not parse container. Quit.");
		goto done;
	}

	if (pkey && ucert) {
		maemosec_key_id key_id;
		printf("%s\n", "User certificate and private key detected");
		if (0 == maemosec_certman_get_key_id(ucert, key_id)) {
			printf("%s\n", "Writable certificate stores:");
			maemosec_certman_iterate_domains(MAEMOSEC_CERTMAN_DOMAIN_PRIVATE, 
											 show_storage_name,
											 NULL);
			printf("%s: ", "Give store name for user certificate");
			get_input(storename, sizeof(storename), 0);

			rc = maemosec_certman_open_domain(storename, 
											  MAEMOSEC_CERTMAN_DOMAIN_PRIVATE, 
											  &user_domain);

			if (0 == rc) {
				rc = maemosec_certman_add_cert(user_domain, ucert);
				if (0 == rc) {
                    if (!do_echo)
                        printf("Added user certificate to '%s'\n", storename);
                    else
                        show_certificate_id(user_domain, ucert);
				} else
					fprintf(stderr, 
                            "ERROR: could not add user certificate to '%s' (%d)\n", 
                            storename, rc);

				maemosec_certman_close_domain(user_domain);

				rc = maemosec_certman_store_key(key_id, pkey, password);
				if (0 == rc) {
                    if (!do_echo)
                        printf("Saved private key\n");
				} else
					fprintf(stderr, "ERROR: could not save private key (%d)\n", rc);
			} else {
				fprintf(stderr, "ERROR: could not open private domain (%d)\n", rc);
			}
		}
		X509_free(ucert);
		EVP_PKEY_free(pkey);
	}

	if (cas && sk_X509_num(cas)) {
		printf("%d CA certificates detected\n", sk_X509_num(cas));
		printf("%s\n", "Writable certificate stores:");
		maemosec_certman_iterate_domains(MAEMOSEC_CERTMAN_DOMAIN_PRIVATE, 
										 show_storage_name,
										 NULL);
		printf("%s: ", "Give store name for CA certificates");

		get_input(storename, sizeof(storename), 0);

		rc = maemosec_certman_open_domain(storename, 
										  MAEMOSEC_CERTMAN_DOMAIN_PRIVATE, 
										  &cas_domain);

		if (0 == rc) {
			int i;
			for (i = 0; i < sk_X509_num(cas); i++) {
				X509* cacert = sk_X509_value(cas, i);
				rc = maemosec_certman_add_cert(cas_domain, cacert);
				if (0 == rc) {
                    if (!do_echo)
                        printf("Added CA certificate to '%s'\n", storename);
                    else
                        show_certificate_id(cas_domain, cacert);
				} else
					fprintf(stderr, 
                            "ERROR: could not add CA certificate to '%s' (%d)\n", 
						   storename, rc);
			}
			maemosec_certman_close_domain(cas_domain);
		} else {
			fprintf(stderr, "ERROR: could not open private domain (%d)\n", rc);
		}
		sk_X509_free(cas);
	}
	
 done:						  
	PKCS12_free(cont);
	return(0);
}


static int
dont_change_private_stores_as_root(int argc, char* argv[])
{
	if (0 == getuid()) {
		int i;
		fprintf(stderr, "ERROR: do not modify private stores as root\n"
				"use 'su <user-id> -c \"%s", argc?argv[0]:"cmcli...");
		for (i = 1; i < argc; i++)
			fprintf(stderr, " %s", argv[i]);
		fprintf(stderr, "%s\n", "\"' in stead");
		return(1);
	}
	return(0);
}


static int
install_file(domain_handle into_domain, const char* filename)
{
	FILE* fp = fopen(filename, "r");
	ft_filetype ft;
	void* idata = NULL;
	int rc = 0;
    char *seenlist = NULL;

	if (!fp) {
		fprintf(stderr, "ERROR: cannot open file '%s' (%s)\n",
				filename, strerror(errno));
        return errno;
	}

	if (NULL != into_domain)
		remember_certificates(into_domain, &seenlist);

	ft = determine_filetype(fp, &idata);
	switch (ft) 
		{
		case ft_x509_pem:
		case ft_x509_der:
			if (NULL != into_domain) {
				rc = maemosec_certman_add_cert(into_domain, (X509*)idata);
				if (0 != rc) {
					if (EACCES != rc || !dont_change_private_stores_as_root(0, NULL)) {
						fprintf(stderr, "ERROR: cannot install certificate (%d)\n", rc);
					}
				} else {
                    if (do_echo) {
                        print_if_not_in_list(0, (X509*)idata, seenlist);
                    }
                }
			} else
				fprintf(stderr, "ERROR: must specify domain first\n");
			X509_free((X509*)idata);
			rc = EINVAL;
			break;

		case ft_x509_sig:
			rc = install_private_key((X509_SIG*)idata);
			break;
			
		case ft_pkcs12:
			rc = install_pkcs12((PKCS12*)idata);
			break;
		default:
			rc = EINVAL;
		}
	fclose(fp);
    free(seenlist);
	return(rc);
}


static void
usage(void)
{
	printf(
		"Usage:\n"
		"cmcli [-<T|t> <domain>[:<domain>...]] [-<c|p> <domain>]\n"
		       "-a <cert-file [<cert-file>...]> -i <pkcs12-file>\n"
		       "-v <cert-file|hostname:port>\n"
		       "-k <fingerprint> -r <key-id> -b <file>\n" 
		       "[-DLl] -d{d}* [-fe]\n"
		" -T to load CA certificates from one or more shared domains\n"
		" -t to load CA certificates from one or more private domains\n"
		" -c to open/create a shared domain for modifications\n"
		" -p to open/create a private domain for modifications\n"
		" -a to add a certificate to the given domain\n"
		" -i to install a PKCS#12 container or a single private key\n"
		" -v to verify a certificate or a SSL-server against the trusted domains\n"
		" -k to display a private key specified by its fingerprint\n"
		" -r to remove the certificate identified by key id from domain\n"
		" -D to list certificate domains\n"
		" -L to list certificates\n"
		" -K to list private\n"
		" -d, -dd... to increase level of debug info shown\n"
		" -f to force an operation despite warnings\n"
		" -l to not resolve symlinks (needed in scratchbox)\n"
		" -e to echo added certificate ids to stdout\n"
		);
}

extern int resolve_symlinks;

/**
 * \brief The main program
 * Execute the command without any parameters to get the help
 */

int
main(int argc, char* argv[])
{
	int rc, i, a, flags;
	domain_handle my_domain = NULL;
	X509_STORE* certs = NULL;
	maemosec_key_id my_key_id;
    char *ocerts = NULL;

	if (1 == argc) {
		usage();
		return(-1);
	}

	ERR_print_errors_cb(report_openssl_error, NULL);

	rc = maemosec_certman_open(&certs);
	if (rc != 0) {
		fprintf(stderr, "ERROR: cannot open certificate repository (%d)\n", rc);
		return(-1);
	}

    while (1) {
		a = getopt(argc, argv, "T:t:c:p:a:i:v:k:r:DLKdfhsleA:?j");
		if (a < 0) {
			break;
		}
		switch(a) 
		{
        case 'e':
            do_echo = 1;
            break;

		case 'T':
		case 't':
			rc = maemosec_certman_collect(optarg, ('T' == a), certs);
			if (rc != 0) {
				fprintf(stderr, "ERROR: cannot open domain '%s' (%d)\n", 
						optarg, rc);
				return(-1);
			}
			break;

		case 'v':
			if (verify_object(certs, optarg))
				/*
				 * Error messages are printed already inside 
				 * verify object.
				 */
				printf("Verified OK\n");
			break;

		case 'D':
			printf("Shared domains%s:\n", geteuid()?" (read only)":"");
			maemosec_certman_iterate_domains(MAEMOSEC_CERTMAN_DOMAIN_SHARED, 
											 show_storage_name,
											 NULL);
			printf("Private domains:\n");
			maemosec_certman_iterate_domains(MAEMOSEC_CERTMAN_DOMAIN_PRIVATE, 
											 show_storage_name,
											 NULL);
			break;

		case 'L':
			if (my_domain)
				maemosec_certman_iterate_certs(my_domain, show_cert, NULL);
			else {
                STACK_OF(X509_OBJECT) *objs;
                objs = X509_STORE_get0_objects(certs);
				for (i = 0; i < sk_X509_OBJECT_num(objs); i++) {
					X509_OBJECT* obj = sk_X509_OBJECT_value(objs, i);
					if (X509_OBJECT_get_type(obj) == X509_LU_X509) {
						show_cert(i, X509_OBJECT_get0_X509(obj), NULL);
					}
				}
			}
			break;

		case 'K':
			maemosec_certman_iterate_keys(show_key, NULL);
			break;

		case 'c':
		case 'p':
			if ('c' == a)
				flags = MAEMOSEC_CERTMAN_DOMAIN_SHARED;
			else
				flags = MAEMOSEC_CERTMAN_DOMAIN_PRIVATE;
			rc = maemosec_certman_open_domain(optarg, flags, &my_domain);
			if (0 != rc) {
				if (EACCES != rc || !dont_change_private_stores_as_root(argc, argv)) {
					fprintf(stderr, "ERROR: cannot open/create domain '%s' (%d)\n", 
							optarg, rc);
				}
				return(-1);
			}
			break;

		case 'A':
			inspect_certificate(optarg);
			break;

		case 'l':
			resolve_symlinks = 0;
			break;

		case 'a':
			if (!my_domain) {
				fprintf(stderr, "ERROR: must specify domain first\n");
				return(-1);
			}
            if (do_echo)
                remember_certificates(my_domain, &ocerts);

			MAEMOSEC_DEBUG(1, "Adding %d certificates\n", argc - optind + 1);
			for (i = optind - 1; i < argc; i++) {
                STACK_OF(X509_OBJECT) *objs;

				MAEMOSEC_DEBUG(1, "Add %s\n", argv[i]);
				/*
				 * Verify if trusted domains have been given
				 */
                objs = X509_STORE_get0_objects(certs);
				if (sk_X509_OBJECT_num(objs)) {
					if (!verify_cert(certs, get_cert(argv[i]), 0)) {
						printf("Reject %s\n", argv[i]);
						argv[i] = "";
					}
				}
			}
			rc = maemosec_certman_add_certs(my_domain, argv + optind - 1, argc - optind + 1);
			if (0 < rc) {
                if (!do_echo)
                    printf("Added %d certificates\n", rc);
                else
                    list_certificates(my_domain, ocerts);
			} else if (0 != errno) {
				if (EACCES != errno || !dont_change_private_stores_as_root(argc, argv)) {
					fprintf(stderr, "ERROR: cannot add any certificates (%s)\n", strerror(errno));
				}
			}
			goto end;
			break;

		case 'i':
			rc = install_file(my_domain, optarg);
            if (0 > rc) {
                fprintf(stderr, "ERROR: cannot install certificates (%s)\n", strerror(rc));
            }
			break;

		case 'k':
			if (0 == maemosec_certman_str_to_key_id(optarg, my_key_id)) {
				EVP_PKEY* my_key = NULL;
				char password[64];

				show_key(0, my_key_id, NULL);
				printf("Give password: ");
				get_input(password, sizeof(password), 1);
				printf("\n");
				rc = maemosec_certman_retrieve_key(my_key_id,
												   &my_key,
												   password);
				if (0 == rc) {
					BIO* outfile = BIO_new_fp(stdout, BIO_NOCLOSE);
					if (outfile) {
						if (!PEM_write_bio_PrivateKey(outfile,
													  my_key,
													  NULL,
													  NULL,
													  0,
													  NULL,
													  NULL))
						{
							if (0 == errno)
								rc = EACCES;
							else
								rc = errno;
							fprintf(stderr, "ERROR: failed to write key (%s)", 
									strerror(rc));
							
						}
						BIO_free(outfile);
					}
				} else {
					fprintf(stderr, 
							"ERROR: cannot read private key (%d)\n",
							rc);
				}
				if (my_key)
					EVP_PKEY_free(my_key);
			}
			break;

		case 'r':
			if (!my_domain) {
				fprintf(stderr, "ERROR: must specify domain first\n");
				return(-1);
			}
			if (0 == maemosec_certman_str_to_key_id(optarg, my_key_id)) {
				rc = maemosec_certman_rm_cert(my_domain, my_key_id);
				if (0 != rc) {
					fprintf(stderr, "ERROR: cannot remove certificate (%d)\n", rc);
				}
			} else
				printf("Removed certificate");
			break;

		case 'f':
			force_opt++;
			break;

		case 's':
			save_cert = 1;
			break;

        case 'j':
            /*
             * Copytest
             */
            if (NULL != certs) {
                X509_STORE* dup = X509_STORE_dup(certs);
                MAEMOSEC_DEBUG(1, "original %d, copy %d", sk_X509_num(certs->objs), sk_X509_num(dup->objs));
                STACK_OF(X509_OBJECT) *dup_objs;
                STACK_OF(X509_OBJECT) *certs_objs;
                dup_objs = X509_STORE_get0_objects(dup);
                certs_objs = X509_STORE_get0_objects(certs);

                if (sk_X509_OBJECT_num(dup_objs) < sk_X509_OBJECT_num(certs_objs)) {
                    printf("copy failed: %d != %d\n", sk_X509_OBJECT_num(dup_objs), sk_X509_OBJECT_num(certs_objs));
                }
                X509_STORE_free(dup);
            }
            break;

		default:
			usage();
			return(-1);
		}
	}

end:
	if (NULL != ocerts)
		free(ocerts);
	if (my_domain)
		maemosec_certman_close_domain(my_domain);

	maemosec_certman_close(certs);
	return(0);
}
