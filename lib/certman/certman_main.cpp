/* -*- mode:c++; tab-width:4; c-basic-offset:4; -*- */

#include <maemosec_certman.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <regex.h>

// STL headers
#include <string>
#include <vector>
// #include <dequeue>
#include <stack>
#include <map>
using namespace std;

// OpenSSL headers
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

// #include <libbb5.h>
#include <libbb5stub.h>
#include <maemosec_common.h>
#include <maemosec_storage.h>
using namespace maemosec;

#include "x509_container.h"

/*
 * Storage name prefix and directory names
 */

static const char cert_storage_prefix [] = "certman";
static const char common_cert_dir     [] = "/etc/certs";
static const char priv_cert_dir       [] = ".maemosec-certs";
static const char priv_keys_dir       [] = ".maemosec-keys";

/*
 * Directory access bits
 */

#define PUBLIC_DIR_MODE 0755
#define PRIVATE_DIR_MODE 0700


// TODO: should this really be a public
EVP_PKEY *root_pkey = NULL;


namespace maemosec 
{
    /**
	 * \brief A secure certificate container
	 */
	typedef struct local_domain
	{
		storage* index;    ///< The secure storage containing the files
		string   dirname;  ///< The directory in which the actual files are
	};
}


static bool
verify_cert(X509_STORE* ctx, X509* cert)
{
	X509_STORE_CTX *csc;
	bool retval;
	int rc;

	csc = X509_STORE_CTX_new();
	if (csc == NULL) {
		MAEMOSEC_ERROR("cannot create new context");
		return(false);
	}

	rc = X509_STORE_CTX_init(csc, ctx, cert, NULL);
	if (rc == 0) {
		MAEMOSEC_ERROR("cannot initialize new context");
		return(false);
	}

	retval = (X509_verify_cert(csc) > 0);
	X509_STORE_CTX_free(csc);

	return(retval);
}


static bool
load_certs(vector<string> &certnames,
		   X509_STORE* certs
) {
	map<string, x509_container*> cert_map;
	stack<x509_container*> temp;

	// TODO: Is this logic necessary at all now that the 
	// certificates have been divided to domains? After all,
	// the tool should not force verification if that's not
	// what the user wants.

	// Load self signed certificates directly to X509 store
	// and put the rest into a map for quick access

	for (size_t i = 0; i < certnames.size(); i++) {
		x509_container* cert = new x509_container(certnames[i].c_str());

		if (strcmp(cert->key_id(), cert->issuer_key_id()) == 0
			|| strlen(cert->issuer_key_id()) == 0) 
		{
			MAEMOSEC_DEBUG(1, "self signed: %s", cert->subject_name());
			cert->m_verified = true;
			X509_STORE_add_cert(certs, cert->cert());
			delete(cert);
		} else {
			cert_map[cert->key_id()] = cert;
			MAEMOSEC_DEBUG(1, "%s\n\tkey    %s\n\tissuer %s", 
				  cert->subject_name(),
				  cert->key_id(), 
				  cert->issuer_key_id());
		}
	}

	// Load and verify the rest of the certificates in the proper order
	for (
		map<string, x509_container*>::const_iterator ii = cert_map.begin();
		ii != cert_map.end();
		ii++
	) {
		x509_container* cert = ii->second;
		x509_container* issuer;

		MAEMOSEC_DEBUG(2, "iterate next (%p,%d)", cert, cert->m_verified);

		if (!cert) {
			MAEMOSEC_DEBUG(0, "What hell? (%s)", ii->first.c_str());
			continue;
		}

		if (!cert->m_verified) {
			temp.push(cert);
			while (cert_map.count(cert->issuer_key_id())) {
				issuer = cert_map[cert->issuer_key_id()];
				if (issuer) {
					if (!issuer->m_verified)
						temp.push(issuer);
					else
						break;
				} else
					MAEMOSEC_ERROR("cannot find issuer %s for %s", 
						  cert->issuer_key_id(),
						  cert->subject_name()); 
				cert = issuer;
			}

			while (temp.size()) {
				MAEMOSEC_DEBUG(2, "pop %d", temp.size());
				cert = temp.top();
				temp.pop();
				if (verify_cert(certs, cert->cert())) {
					MAEMOSEC_DEBUG(2, "verified: %s", cert->subject_name());
					X509_STORE_add_cert(certs, cert->cert());
					cert->m_verified = true;
				} else {
					MAEMOSEC_ERROR("%s verification fails", cert->subject_name());
				}
			}
		}
	}
	MAEMOSEC_DEBUG(2, "erasing map");
	for (
		map<string, x509_container*>::const_iterator ii = cert_map.begin();
		ii != cert_map.end();
		ii++
	) {
		x509_container* cert = ii->second;
		delete(cert);
	}
	cert_map.clear();
	return(true);
}


static void
local_storage_dir(string& to_this, const char* subarea)
{
	string curbinname;

	to_this.assign(GETENV("HOME",""));
	
	to_this.append(PATH_SEP);
	to_this.append(subarea);
	to_this.append(PATH_SEP);
#if 0
	process_name(curbinname);
	for (int i = 0; i < curbinname.length(); i++) {
		if (curbinname[i] == *PATH_SEP)
			curbinname[i] = '.';
	}
	to_this.append(curbinname);
#endif
	MAEMOSEC_DEBUG(1, "\nlocal cert dir = '%s'", to_this.c_str());
}


static void
decide_storage_name(const char* domain_name, int flags, string& dirname, string& storename)
{
	if (MAEMOSEC_CERTMAN_DOMAIN_PRIVATE == flags) {
		// Make private name
		local_storage_dir(dirname, priv_cert_dir);
		storename.insert(0, ".");
		storename.insert(0, cert_storage_prefix);
		if (domain_name) {
			dirname.append(domain_name);
			storename.append(domain_name);
		}
		MAEMOSEC_DEBUG(1, "\ndirname  = %s\nstorename = %s", dirname.c_str(), storename.c_str());
	} else {
		storename.assign(domain_name);
		storename.insert(0, ".");
		storename.insert(0, cert_storage_prefix);
		dirname.assign(common_cert_dir);
		dirname.append(PATH_SEP);
		dirname.append(domain_name);
		MAEMOSEC_DEBUG(1, "\ndirname  = %s\nstorename = %s", dirname.c_str(), storename.c_str());
	}
}


static void
remove_spec_chars(char* in_string)
{
	char* to, *from = in_string;

	to = in_string;
	while (*from) {
		if (isalnum(*from) || strchr("_-", *from))
			*to++ = tolower(*from);
		from++;
	}
	*to = '\0';
}


/*
 * Make a unique filename for each certificate
 */
static void
make_unique_filename(X509* of_cert, const char* in_dir, string& to_string)
{
	const char* c;
	char nbuf[1024], *name;
	long serial;
	int rc;
	struct stat fs;

	to_string.assign("");

#if 0
	name = X509_NAME_oneline(X509_get_subject_name(of_cert), nbuf, sizeof(nbuf));
	serial = ASN1_INTEGER_get(X509_get_serialNumber(of_cert));

	if (!name) {
		MAEMOSEC_ERROR("Cert has no name!!!");
		return;
	}

	MAEMOSEC_DEBUG(1,"Making filename out of '%s'\n+ in dir '%s'", name, in_dir);

	to_string.assign(in_dir);
	to_string.append(PATH_SEP);

	// Use the organization name from subject name as basis
	c = strstr(name, "O=");
	if (c) 
		c += 2;
	else
		c = name;

	while (*c && (strchr("/,=",*c) == NULL))
	{
		if (!isalnum(*c))
			to_string.append(1,'_');
		else
			to_string.append(1,*c);
		c++;
	}
	
	// Do not use the real serial number
	serial = 1;
	do {
		sprintf(nbuf, "%s.%ld.pem", to_string.c_str(), serial);
		rc = stat(nbuf, &fs);
		if (-1 == rc) {
			if (ENOENT == errno)
				break;
			else {
				MAEMOSEC_ERROR("cannot do stat on '%s' (%s)", nbuf, strerror(errno));
				return;
			}
		} else
			serial++;
	} while (serial < LONG_MAX);
	to_string.assign(nbuf);

#else
	to_string.assign(in_dir);
	to_string.append(PATH_SEP);
	maemosec_key_id key_id;
	if (0 == maemosec_certman_get_key_id(of_cert, key_id)) {
		append_hex(to_string, key_id, MAEMOSEC_KEY_ID_LEN);
		to_string.append(".pem");
	} else {
		MAEMOSEC_ERROR("Cannot get key id out of certificate");
		goto failed;
	}
#endif

  ok:
	MAEMOSEC_DEBUG(1, "=> %s", to_string.c_str());
	return;

  failed:
	;
}


static void
hex_to_key_id(const char* hstring, unsigned char* to_id)
{
	unsigned int val;

	memset(to_id, '\0', MAEMOSEC_KEY_ID_LEN);
	for (int i = 0; i < 2*MAEMOSEC_KEY_ID_LEN; i += 2) {
		if (0 == sscanf(hstring + i, "%02x", &val)) {
			MAEMOSEC_ERROR("invalid key file name '%s' at %d", hstring, i);
		}
		*to_id++ = (unsigned char) val;
	}
}


static X509*
load_cert_from_file(const char* from_file)
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


static int
store_key_to_file(maemosec_key_id key_id, EVP_PKEY* key, char* passwd)
{
	string storage_file_name;
	FILE* outfile;
	int rc;

	local_storage_dir(storage_file_name, priv_keys_dir);
	create_directory(storage_file_name.c_str(), PRIVATE_DIR_MODE);
	append_hex(storage_file_name, key_id, MAEMOSEC_KEY_ID_LEN);
	storage_file_name.append(".pem");

	outfile = fopen(storage_file_name.c_str(), "w");
	if (outfile) {
		chmod(storage_file_name.c_str(), S_IRUSR | S_IWUSR);
		rc = PEM_write_PKCS8PrivateKey(outfile, key, EVP_aes_256_ecb(), 
									   passwd, strlen(passwd), NULL, NULL);
		MAEMOSEC_DEBUG(1, "Stored key to '%s', rc = %d", 
					   storage_file_name.c_str(), rc);
		fclose(outfile);
		return(0);
	} else {
		MAEMOSEC_ERROR("Cannot open '%s' (%s)", storage_file_name.c_str(),
					   strerror(errno));
		return(errno);
	}
}


static int
return_pem_password(char* to_buf, int size, int rwflag, void* userdata)
{
	const char* password = (const char*) userdata;
	if (!password)
		return(-1);
	if (strlen(password) > size)
		return(-EINVAL);
	strcpy(to_buf, password);
	return(strlen(password));
}


static int
read_key_from_file(maemosec_key_id key_id, EVP_PKEY** key, char* passwd)
{
	string storage_file_name;
	FILE* infile;
	int rc;

	local_storage_dir(storage_file_name, priv_keys_dir);
	create_directory(storage_file_name.c_str(), PRIVATE_DIR_MODE);
	append_hex(storage_file_name, key_id, MAEMOSEC_KEY_ID_LEN);
	storage_file_name.append(".pem");

	infile = fopen(storage_file_name.c_str(), "r");
	if (infile) {
		chmod(storage_file_name.c_str(), S_IRUSR | S_IWUSR);
		*key = d2i_PKCS8PrivateKey_fp(infile, NULL, return_pem_password, passwd);
		if (*key) {
			MAEMOSEC_DEBUG(1, "Retrieved key from '%s'", storage_file_name.c_str());
		} else {
			MAEMOSEC_ERROR("Cannot retrieve key from '%s'", 
						   storage_file_name.c_str());
		}
		fclose(infile);
		return(0);
	} else {
		MAEMOSEC_ERROR("Cannot open '%s' (%s)", storage_file_name.c_str(),
					   strerror(errno));
		return(errno);
	}
}


static int
x509_equals(int pos, X509* cert, void* with_cert)
{
#if 1
	// The quick and easy method, but maybe not the right one
	return (X509_cmp(cert, (X509*)with_cert) == 0);
#else
	// The manual method, which involves dynamic allocation
	ASN1_BIT_STRING *lk, *rk;
	int res = 0;

	if (!cert || !with_cert)
		return(EINVAL);
	lk = X509_get_pubkey(cert);
	rk = X509_get_pubkey((X509*)with_cert);
	if (lk && rk) {
		res = (EVP_PKEY_cmp(lk,rk) == 0);
	}
	if (lk)
		EVP_PKEY_free(lk);
	if (lk)
		EVP_PKEY_free(rk);
	return(res);
#endif
}

static void
maemosec_certman_int_init(void)
{
	string my_app_name;

	if (process_name(my_app_name))
		MAEMOSEC_DEBUG(1, "Init '%s'", my_app_name.c_str());
	else
		MAEMOSEC_ERROR("Could not access process name");
}


static int
local_iterate_storage_names(storage::visibility_t of_visibility, 
							storage::protection_t of_protection, 
							const char* matching_names,
							maemosec_callback* cb_func,
							void* ctx)
{
	return(storage::iterate_storage_names(of_visibility, 
								 of_protection, 
								 matching_names,
								 cb_func, 
								 ctx));
}


// Visible part
extern "C" {

	typedef struct cb_relay_par {
		void* o_ctx;
		maemosec_callback* cb_func;
	};

	int 
	maemosec_certman_open(X509_STORE** my_cert_store)
	{
		X509* bb5cert;

		maemosec_certman_int_init();
		bb5_init();
		*my_cert_store = X509_STORE_new();
		bb5cert = bb5_get_cert(0);
		if (bb5cert)
			X509_STORE_add_cert(*my_cert_store, bb5cert);
		return(0);
	}

	int maemosec_certman_collect(const char* domain, int shared, X509_STORE* my_cert_store)
	{
		vector<string> x;
		const char* sep, *start = domain;
		storage::visibility_t storvis;

		do {
			string domainname, dirname, storagename;
				
			sep = strchr(start, ':');
			if (sep) {
				domainname.assign(start, sep - start);
				start = sep + 1;
			} else
				domainname.assign(start);

			if (shared) {
				storvis = storage::vis_shared;
				decide_storage_name(domainname.c_str(), MAEMOSEC_CERTMAN_DOMAIN_SHARED, 
									dirname, storagename);
			} else {
				decide_storage_name(domainname.c_str(), MAEMOSEC_CERTMAN_DOMAIN_PRIVATE, 
									dirname, storagename);
				storvis = storage::vis_private;
			}

			if (directory_exists(dirname.c_str())) {
				storage* store = new storage(storagename.c_str(), 
											 storvis, 
											 storage::prot_signed);
				storage::stringlist certs;
				int pos = store->get_files(certs);

				for (int i = 0; i < pos; i++) {
					if (store->verify_file(certs[i])) {
						MAEMOSEC_DEBUG(1, "Load '%s'", certs[i]);
						x.push_back(certs[i]);
					} else
						MAEMOSEC_ERROR("'%s' fails verification");
				}
				delete(store);
			} else {
				MAEMOSEC_ERROR("'%s' does not exists", storagename.c_str());
			}
		} while (sep);

		if (x.size()) {
			load_certs(x, my_cert_store);
		}
		return(0);
	}

	int
	maemosec_certman_close(X509_STORE* my_cert_store)
	{
		X509_STORE_free(my_cert_store);
		bb5_finish();
		return(0);
	}

	int 
	maemosec_certman_open_domain(const char* domain_name, 
							 int flags, 
							 domain_handle* handle)
	{
		string storename;
		storage* certstore;
		struct local_domain mydomain;
		storage::visibility_t storvis;
		int rc;

		*handle = NULL;
		decide_storage_name(domain_name, flags, mydomain.dirname, storename);

		if (MAEMOSEC_CERTMAN_DOMAIN_PRIVATE == flags) {
			rc = create_directory(mydomain.dirname.c_str(), PRIVATE_DIR_MODE);
			storvis = storage::vis_private;
		} else {
			rc = create_directory(mydomain.dirname.c_str(), PUBLIC_DIR_MODE);
			storvis = storage::vis_shared;
		}

		if (0 != rc) {
			return(rc);
		}
		mydomain.index = new storage(storename.c_str(), storvis, 
									 storage::prot_signed);
		if (mydomain.index) {
			*handle = new struct local_domain(mydomain);
			return(0);
		} else
			return(-1);
	}

	int 
	maemosec_certman_iterate_certs(
		domain_handle the_domain, 
		int cb_func(int,X509*,void*), 
		void* ctx)
	{
		storage::stringlist files;
		struct local_domain* mydomain;
		int i, pos, res = 0;

		if (!the_domain || !cb_func)
			return(-EINVAL);

		mydomain = (struct local_domain*)the_domain;
		pos = mydomain->index->get_files(files);
		MAEMOSEC_DEBUG(1, "domain contains %d certificates", pos);
		for (i = 0; i < pos; i++) {
			X509* cert = load_cert_from_file(files[i]);
			MAEMOSEC_DEBUG(1, "%d: %p", i, cert);
			if (cert) {
				res = cb_func(i, cert, ctx);
				if (res != -1)
					X509_free(cert);
				else
					res = 0;
				if (res)
					break;
			} else
				return(-ENOENT);
		}
		return(i);
	}

	int
	maemosec_certman_nbrof_certs(domain_handle in_domain)
	{
		if (in_domain)
			return(((struct local_domain*)in_domain)->index->nbrof_files());
		else
			return(-1);
	}

	int 
	maemosec_certman_add_cert(domain_handle to_domain, X509* cert)
	{
		struct local_domain* mydomain = (struct local_domain*)to_domain;
		FILE* to_file;
		string filename;
		int pos, rc = 0;

		if (!to_domain || !cert)
			return(EINVAL);

		pos = maemosec_certman_iterate_certs(to_domain, x509_equals, cert);
		if (pos < maemosec_certman_nbrof_certs(to_domain)) {
			MAEMOSEC_DEBUG(0,"The certificate is already in the domain");
			return(EEXIST);
		}

		make_unique_filename(cert, mydomain->dirname.c_str(), filename);
		
		to_file = fopen(filename.c_str(), "w+");
		if (to_file) {
			if (PEM_write_X509(to_file, cert)) {
				MAEMOSEC_DEBUG(1, "written %s", filename.c_str());
			} else {
				MAEMOSEC_DEBUG(1, "cannot write to %s (%s)", filename.c_str(), 
					  strerror(errno));
				rc = errno;
			}
			fclose(to_file);
		} else
			rc = errno;

		if (0 == rc) {
			mydomain->index->add_file(filename.c_str());
			mydomain->index->commit();
		}
		return(rc);
	}


	int
	maemosec_certman_rm_cert(domain_handle to_domain, int pos)
	{
		int count, rc;
		storage::stringlist certs;
		struct local_domain* mydomain = (struct local_domain*)to_domain;

		if (!to_domain)
			return(EINVAL);
		count = mydomain->index->get_files(certs);
		if (pos < 0 || pos >= count)
			return(EINVAL);
		mydomain->index->remove_file(certs[pos]);
		mydomain->index->commit();
		return(0);
	}

	int 
	maemosec_certman_close_domain(domain_handle handle)
	{
		struct local_domain* mydomain;

		if (!handle)
			return(EINVAL);
		mydomain = (struct local_domain*)handle;
		delete(mydomain->index);
		delete(mydomain);
	}

	int 
	maemosec_certman_get_key_id(X509* of_cert, maemosec_key_id to_this)
	{
		if (!of_cert && !to_this)
			return(EINVAL);
		if (X509_pubkey_digest(of_cert, EVP_sha1(), to_this, NULL))
			return(0);
		else
			return(EINVAL);
	}

	int 
	maemosec_certman_store_key(maemosec_key_id with_id, 
							   EVP_PKEY* the_key, 
							   char* with_passwd)
	{
		return(store_key_to_file(with_id, the_key, with_passwd));
	}

	int
	maemosec_certman_retrieve_key(maemosec_key_id with_id, 
							   EVP_PKEY** the_key, 
							   char* with_passwd)
	{
		return(read_key_from_file(with_id, the_key, with_passwd));
	}

	static int
	cb_relay(int ord_nr, void* filename, void* ctx)
	{
		maemosec_key_id key_id;
		struct cb_relay_par *pars = (struct cb_relay_par*) ctx;
		hex_to_key_id((const char*)filename, key_id);
		return(pars->cb_func(ord_nr, key_id, pars->o_ctx));
	}


	int
	maemosec_certman_iterate_keys(maemosec_callback* cb_func, void* ctx)
	{
		string keystore_name;
		string name_expression;
		struct cb_relay_par relay_pars;

		name_expression = "^";
		for (int i = 0; i < MAEMOSEC_KEY_ID_LEN; i++)
			name_expression.append("[0-9a-f][0-9a-f]");
		name_expression.append("\\.pem$");
		local_storage_dir(keystore_name, priv_keys_dir);
		relay_pars.o_ctx = ctx;
		relay_pars.cb_func = cb_func;
		return(iterate_files(keystore_name.c_str(), 
							 name_expression.c_str(), 
							 cb_relay, 
							 &relay_pars));
	}


	int
	maemosec_certman_iterate_domains(int flags,
									 maemosec_callback* cb_func,
									 void* ctx)
	{
		storage::visibility_t vis;
		string storage_names = cert_storage_prefix;

		storage_names.assign(cert_storage_prefix);
		storage_names.append("\\..*");
		if (MAEMOSEC_CERTMAN_DOMAIN_SHARED == flags)
			vis = storage::vis_shared;
		else if (MAEMOSEC_CERTMAN_DOMAIN_PRIVATE == flags)
			vis = storage::vis_private;
		else
			return(0 - EINVAL);
		return(local_iterate_storage_names(vis, 
										   storage::prot_signed, 
										   storage_names.c_str(), 
										   cb_func, 
										   ctx));
	}

} // extern "C"
