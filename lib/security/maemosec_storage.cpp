/* -*- mode:c++; tab-width:4; c-basic-offset:4; -*-
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

   \file maemosec_storage.cpp
   \ingroup sec_storage
   \brief The protected storage implementation

*/

#include "maemosec_storage.h"
using namespace std;
using namespace maemosec;

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/rand.h>

#include "maemosec_common.h"
#include "libbb5stub.h"

/**
 * \def DIGESTTYP
 * \brief The checksum used in signing (SHA1)
 */
#define DIGESTTYP EVP_sha1

/**
 * \def DIGESTLEN
 * \brief The length of the digest checksum
 */

#define DIGESTLEN EVP_MD_size(DIGESTTYP())
/**
 * \def EVPOK
 * \brief The "no error" return code of the EVP-functions
 * in openssl library
 */

#define EVPOK 1
/**
 * \def SYMKEYLEN
 * \brief The length of the symmetric crypto key of the AES256
 * crypto algorithm
 */

#define SYMKEYLEN 32

/**
 * \def CIPKEYLEN
 * \brief The length of the crypto key when encrypted with 
 * the RSA asymmetric crypto algorithm
 */
#define CIPKEYLEN 128

static const char sec_shared_root[]  = "/etc/secure";
static const char sec_private_root[] = ".maemosec-secure";

/**
 * \def signature_mark
 * \brief The signature marker in the storage file
 */
#define signature_mark "SIGNATURE:"

/**
 * \def key_mark
 * \brief The encryption key marker in the storage file
 */
#define key_mark       "CRYPTOKEY:"

static unsigned char
hex2bin(char* hex2str)
{
	unsigned char res;

	res  = *hex2str <= '9' ? *hex2str - '0' : 10 + *hex2str - 'a';
	res *= 0x10;
	hex2str++;
	res += *hex2str <= '9' ? *hex2str - '0' : 10 + *hex2str - 'a';
	return(res);
}


static int
get_storage_directory(storage::visibility_t visibility, 
					  storage::protection_t protection, 
					  string& dir_name) 
{
	int rc, access_mode;

	if (visibility == storage::vis_shared) {
		dir_name.assign(sec_shared_root);
		access_mode = 0755;

	} else if (visibility == storage::vis_private) {
		/*
		 * TODO: This is an ugly patch to force root 
		 * processes to handle the same files as user
		 * processes.
		 */
		if (0 == getuid()) {
			MAEMOSEC_DEBUG(1, "%s: warning: handling private storage as root", __func__);
			dir_name.assign("/home/user");
		} else
			dir_name.assign(GETENV("HOME",""));

		if (dir_name.empty()) {
			MAEMOSEC_ERROR("home not defined");
			return(EINVAL);
		}

		dir_name.append(PATH_SEP);
		dir_name.append(sec_private_root);
		access_mode = 0700;
	}

	dir_name.append(PATH_SEP);
	if (protection == storage::prot_signed) 
		dir_name.append("s");
	else if (protection == storage::prot_encrypted)
		dir_name.append("e");

	if (!directory_exists(dir_name.c_str())) {
		rc = create_directory(dir_name.c_str(), access_mode);
		if (0 != rc) {
			MAEMOSEC_ERROR("cannot create directory '%s'", dir_name.c_str());
			return(rc);
		} else
			MAEMOSEC_DEBUG(1, "Created directory '%s'", dir_name.c_str());
	}

	return(0);
}


void
storage::init_storage(const char* name, visibility_t visibility, protection_t protection) 
{
	char* end, *c = NULL;
	unsigned char* data = (unsigned char*)MAP_FAILED;
	int fd = -1, rc;
	ssize_t len, rlen;
	EVP_MD_CTX *vfctx = EVP_MD_CTX_new();
	EVP_PKEY* pubkey = NULL;
	const char* type_prefix;

	if (vfctx == NULL) {
		MAEMOSEC_ERROR("EVP_MD_CTX Initialization error");
		return;
	}

	m_name = name;
	m_symkey = NULL;
	m_symkey_len = 0;
	m_prot = protection;

	if (bb5_get_cert(0) == NULL) {
		MAEMOSEC_ERROR("Initialization error");
		goto free_vfctx;
	}
	pubkey = X509_get_pubkey(bb5_get_cert(0));
	if (NULL == pubkey) {
		MAEMOSEC_ERROR("Cannot get public key");
		goto free_vfctx;
	}
	if (0 != get_storage_directory(visibility, protection, m_filename)) {
		MAEMOSEC_ERROR("Cannot bind storage '%s' to file", m_filename.c_str());
		goto free_vfctx;
	}
	m_filename.append(PATH_SEP);
	m_filename.append(name);
	data = map_file(m_filename.c_str(), O_RDONLY, &fd, &len, &rlen);

	if (MAP_FAILED == data) {
		/*
		 * File does not exist yet.
		 */
		if (prot_encrypted == m_prot) {
			/*
			 * Generate a new symmetric key and encrypt it by using
			 * the BB5 public key
			 */
			RSA *rsakey = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x10100005L
			if (EVP_PKEY_RSA == EVP_PKEY_base_id(pubkey)) 
			#else
			if (EVP_PKEY_RSA == EVP_PKEY_type(pubkey->type)) 
#endif
				rsakey = EVP_PKEY_get1_RSA(pubkey);
			
			if (!rsakey) {
				MAEMOSEC_ERROR("No RSA public key available");
				goto end;
			}

			m_symkey_len = SYMKEYLEN;
			m_symkey = (unsigned char*)malloc(RSA_size(rsakey));
			if (!m_symkey) {
				MAEMOSEC_ERROR("allocation error");
			}

			// Seed RSA PRNG
			rc = bb5_get_random(m_symkey, CIPKEYLEN);
			if (rc != CIPKEYLEN) {
				MAEMOSEC_ERROR("out of random numbers");
			}
			RAND_seed(m_symkey, CIPKEYLEN);

			// Generate random encryption key
			rc = bb5_get_random(m_symkey, m_symkey_len);
			if (rc != m_symkey_len) {
				MAEMOSEC_ERROR("cannot generate new encryption key");
				goto end;

			} else {
				unsigned char cipkey [RSA_size(rsakey)];
				int ciplen;

				ciplen = RSA_public_encrypt(m_symkey_len,
											m_symkey,
											cipkey,
											rsakey,
											RSA_PKCS1_PADDING);

				MAEMOSEC_DEBUG(1,"encrypt %d => %d", m_symkey_len, ciplen);
				RSA_free(rsakey);
				if (RSA_size(rsakey) != ciplen) {
					MAEMOSEC_ERROR("RSA_public_encrypt failed (%d)", ciplen);
				}
				memcpy(m_symkey, cipkey, ciplen);
				m_symkey_len = ciplen;
			}
			MAEMOSEC_DEBUG(1, "'%s' does not exist, created", m_filename.c_str());
		}
		goto end;

	} else {
		m_symkey_len = CIPKEYLEN;
	}

	rc = EVP_VerifyInit(vfctx, DIGESTTYP());
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_VerifyInit returns %d (%s)", rc, strerror(errno));
		goto free_vfctx;
	}

	/*
	 * Absolute path name is part of the signature.
	 */
	rc = EVP_VerifyUpdate(vfctx, m_filename.c_str(), strlen(m_filename.c_str()));
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_VerifyUpdate returns %d (%d)", rc, errno);
		goto free_vfctx;
	}

	/*
	 * Read associations
	 */
	c = (char*) data;
	end = (char*) (data + len);

	while (c && c < end 
		   && memcmp(c, signature_mark, strlen(signature_mark))) 
	{
		string aname;
		string digest;
		char* eol, *sep, *str;

		sep = strchr(c, ' ');
		if (!sep) {
			MAEMOSEC_ERROR("broken file '%s'", m_filename.c_str());
			goto end;
		}
		str = strchr(sep + 1, '*');
		if (!str) {
			MAEMOSEC_ERROR("broken file '%s'", m_filename.c_str());
			goto end;
		}
		eol = strchr(str + 1, '\n');
		if (!eol) {
			MAEMOSEC_ERROR("broken file '%s'", m_filename.c_str());
			goto end;
		}
		aname.append(sep + 2, eol - str - 1);
		digest.append(c, sep - c);
		MAEMOSEC_DEBUG(1, "%s => %s", aname.c_str(), digest.c_str());
		c = eol + 1;
		m_contents[aname] = digest;
	}

	// Check signature
	if (memcmp(c, signature_mark, strlen(signature_mark)) == 0) 
	{
		// TODO: remove ugly plain number
		unsigned char mdref [128];
		// unsigned char mdref [DIGESTLEN];
		size_t mdlen = 0;
		
		// Compute the current digest
		MAEMOSEC_DEBUG(1, "checking %d bytes of data", c - (char*)data);
		rc = EVP_VerifyUpdate(vfctx, data, c - (char*)data);
		if (rc != EVPOK) {
			MAEMOSEC_ERROR("EVP_VerifyUpdate returns %d (%d)", rc, errno);
			goto free_vfctx;
		}

		// Read the stored signature
		while (c < end && *c != '\n') 
			c++;
		if ('\n' == *c) {
			c++;
			while (c < end && *c && mdlen < sizeof(mdref)) {
				if (*c != '\n') {
					mdref[mdlen++] = hex2bin(c);
					c += 2;
				} else
					c++;
			}
		}

		MAEMOSEC_DEBUG(1, "loaded %d bytes of signature", mdlen);

		rc = EVP_VerifyFinal(vfctx, mdref, mdlen, pubkey);
		if (rc != EVPOK) {
			EVP_PKEY_free(pubkey);
			MAEMOSEC_ERROR("Storage integrity test failed");
			m_contents.clear();
			goto free_vfctx;
		} else {
			MAEMOSEC_DEBUG(1, "Storage integrity test OK");
		}

	} else
		MAEMOSEC_ERROR("invalid signature");

	if (c < end && '\n' == *c)
		c++;

	MAEMOSEC_DEBUG(1, "Checking encryption key");
	if (c + strlen(key_mark) >= end
		|| memcmp(c, key_mark, strlen(key_mark)) != 0)
	{
		MAEMOSEC_DEBUG(1, "No encryption key");
		if (prot_encrypted == m_prot) {
			MAEMOSEC_ERROR("missing encryption key");
			goto end;
		}
	} else {
		// There is an encryption key
		unsigned char* to; 
		int keylen = 0;

		m_prot = prot_encrypted;
		if (!m_symkey) {
			m_symkey = (unsigned char*)malloc(m_symkey_len);
			if (!m_symkey) {
				MAEMOSEC_ERROR("allocation error");
			}
		}
		c += strlen(key_mark);
		if (c < end && '\n' == *c)
			c++;
		to = m_symkey;
		while (c < end && keylen < m_symkey_len) {
			if (*c != '\n') {
				*to++ = hex2bin(c);
				keylen++;
				c += 2;
			} else
				c++;
		}
		if (keylen != m_symkey_len) {
			MAEMOSEC_ERROR("corrupt encryption key");
			goto end;
		}
	}

  end:
	if ((unsigned char*)MAP_FAILED != data)
		unmap_file(data, fd, len);
	if (pubkey)
		EVP_PKEY_free(pubkey);
  free_vfctx:
	EVP_MD_CTX_free(vfctx);

	MAEMOSEC_DEBUG(1, "Return");
}


#if 0
storage::storage(const char* name)
{
	init_storage(name, vis_private, prot_signed);
}
#endif


storage::storage(const char* name, visibility_t visibility, protection_t protection) 
{
	init_storage(name, visibility, protection);
}


storage::~storage()
{
	if (m_symkey)
		free(m_symkey);
}


size_t
storage::get_files(stringlist& names)
{
	size_t pos = 0;

	for (
		map<string, string>::const_iterator ii = m_contents.begin();
		ii != m_contents.end();
		ii++
	) {
		names.push_back(strdup(ii->first.c_str()));
		pos++;
	}
	return(pos);
}


void
storage::release(stringlist &list)
{
    for (size_t i = 0; i < list.size(); i++) {
        free((void*)list[i]);
    }
    list.clear();
}


ssize_t
storage::encrypted_length(ssize_t of_bytes)
{
	if (0 == (of_bytes % AES_BLOCK_SIZE))
		return(of_bytes + 1);
	else
		return(of_bytes + AES_BLOCK_SIZE - (of_bytes % AES_BLOCK_SIZE) + 1);
}


bool
storage::contains_file(const char* pathname)
{
	string truename;
	map<string,string>::iterator ii;

	absolute_pathname(pathname, truename);
	ii = m_contents.find(truename);
	return (ii != m_contents.end());
}


unsigned char* 
storage::map_file(const char* pathname, int mode, int* fd, ssize_t* len, ssize_t* rlen)
{
	int lfd, mflags, mprot;
	struct stat fs;
	unsigned char* res;
	ssize_t llen = 0;

	lfd = open(pathname, mode);
	if (lfd < 0) {
		return((unsigned char*)MAP_FAILED);
	}
	
	if (fstat(lfd, &fs) == -1) {
		close(lfd);
		MAEMOSEC_ERROR("cannot stat '%s'", pathname);
		return((unsigned char*)MAP_FAILED);
	}
	*rlen = llen = fs.st_size;
	MAEMOSEC_DEBUG(1, "'%s' is %d bytes long", pathname, llen);
	if (0 == llen) {
		close(lfd);
		MAEMOSEC_ERROR("'%s' is empty", pathname);
		return((unsigned char*)MAP_FAILED);
	}

	if (O_RDONLY == mode) {
		mflags = MAP_PRIVATE;
		if (prot_signed == m_prot)
			mprot = PROT_READ;
		else
			mprot = PROT_READ | PROT_WRITE;
	} else {
		mflags = MAP_SHARED;
		mprot = PROT_READ | PROT_WRITE;
		llen = encrypted_length(llen);
	}
	
	res = (unsigned char*)mmap(NULL, llen, mprot, mflags, lfd, 0);

	if (MAP_FAILED == res) {
		close(lfd);
		MAEMOSEC_ERROR("cannot mmap '%s' of %d bytes", pathname, llen);
		return((unsigned char*)MAP_FAILED);
	}
	*len = llen;
	*fd = lfd;
	return(res);
}


void
storage::unmap_file(unsigned char* data, int fd, ssize_t len)
{
	if (data)
		munmap(data, len);
	if (fd >= 0)
		close(fd);
}


bool
storage::compute_digest(unsigned char* data, ssize_t bytes, string& digest)
{
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	unsigned char md[DIGESTLEN];
	unsigned int mdlen;
	char hlp [3];
	int rc;
	bool rv = false;

	// EVP_MD_CTX_init(&mdctx);
	rc = EVP_DigestInit(mdctx, DIGESTTYP());
	if (EVPOK != rc) {
		MAEMOSEC_ERROR("EVP_DigestInit returns %d (%s)", rc, strerror(errno));
		goto err;
	}

	MAEMOSEC_DEBUG(1, "computing digest over %d bytes", bytes);

	rc = EVP_DigestUpdate(mdctx, data, bytes);
	if (EVPOK != rc) {
		MAEMOSEC_ERROR("EVP_DigestUpdate returns %d (%d)", rc, errno);
		goto err;
	}

	rc = EVP_DigestFinal(mdctx, md, &mdlen);
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_DigestFinal returns %d (%d)", rc, errno);
		goto err;
	}

	if ((int)mdlen != DIGESTLEN) {
		MAEMOSEC_ERROR("Digestlen mismatch (%d != %d)", mdlen, DIGESTLEN);
		goto err;
	}

	for (unsigned int i = 0; i < mdlen; i++) {
		sprintf(hlp, "%02x", md[i]);
		digest.append(hlp,2);
	}

	rv = true;
err:
	EVP_MD_CTX_free(mdctx);
	return(rv);
}


void
storage::compute_digest_of_file(const char* pathname, string& digest)
{
	int fd, rc;
	unsigned char* data;
	ssize_t len, rlen;

	digest.clear();
	data = map_file(pathname, O_RDONLY, &fd, &len, &rlen);
	if (MAP_FAILED == data) {
		MAEMOSEC_ERROR("cannot map '%s'", pathname);
		return;
	}
	compute_digest(data, len, digest);
	unmap_file(data, fd, len);
	MAEMOSEC_DEBUG(1, "Computed digest is '%s'", digest.c_str());
}


void
storage::add_file(const char* pathname)
{
	string truename;
	string digest;

	absolute_pathname(pathname, truename);
	if (contains_file(truename.c_str())) {
		MAEMOSEC_DEBUG(0, "'%s' already belongs to '%s'", 
			  truename.c_str(), m_name.c_str());
		return;
	}
	if (prot_encrypted == m_prot) {
		if (!encrypt_file_in_place(truename.c_str(), digest)) {
			return;
		}
	} else
		compute_digest_of_file(truename.c_str(), digest);
	m_contents[truename] = digest;
	MAEMOSEC_DEBUG(1, "%s => %s", truename.c_str(), digest.c_str());
}


void
storage::remove_file(const char* pathname)
{
	string truename;

	absolute_pathname(pathname, truename);
	if (!contains_file(truename.c_str())) {
		MAEMOSEC_DEBUG(0, "'%s' not found", truename.c_str());
		return;
	}
	m_contents.erase(m_contents.find(truename));
}


bool 
storage::verify_file(const char* pathname)
{
	string digest;
	string truename;
	map<string,string>::iterator ii;

	absolute_pathname(pathname, truename);
	ii = m_contents.find(truename);

	if (m_contents.end() == ii) {
		MAEMOSEC_DEBUG(0, "'%s' not found", truename.c_str());
		return(false);
	}

	if (ii->first == truename) {
		if (prot_encrypted == m_prot)
			decrypt_file(truename.c_str(), NULL, NULL, digest);
		else
			compute_digest_of_file(truename.c_str(), digest);
		return(ii->second == digest);
	} else
		return(false);
}


static void 
checked_write(int to_fd, const char* str, EVP_MD_CTX* signature)
{
	ssize_t written, len = strlen(str);
	
	written = write(to_fd, str, len);
	if (written < len) {
		// TODO: Throw an exception
		MAEMOSEC_ERROR("failed to write %d bytes (written %d)", len, written);
	} else if (signature)
		EVP_SignUpdate(signature, str, len);
}

/**
 * \def WRAPPOINT
 * \brief This is just pretty printing; wrap long hexadecimal 
 * lines after this many digitpairs
 */
#define WRAPPOINT 32

void
storage::commit(void)
{
	int rc, fd = -1;
	EVP_MD_CTX *signctx = EVP_MD_CTX_new();
	unsigned char signmd[255];
	char tmp[3];
	int cols;
	mode_t abits;

	abits = S_IRUSR | S_IWUSR;
	if (0 == geteuid())
		abits |= S_IRGRP | S_IROTH;

	fd = creat(m_filename.c_str(), abits);
	if (fd < 0) {
		MAEMOSEC_ERROR("cannot create '%s'", m_filename.c_str());
		EVP_MD_CTX_free(signctx);
		return;
	}

	rc = EVP_SignInit(signctx, DIGESTTYP());
	/*
	 * Include absolute pathname in signature to
	 * prevent renaming.
	 */
	EVP_SignUpdate(signctx, m_filename.c_str(), strlen(m_filename.c_str()));

	for (
		map<string, string>::const_iterator ii = m_contents.begin();
		ii != m_contents.end();
		ii++
	) {
		// Use sha1sum compatible output
		const char* tmp = ii->second.c_str();
		if (!tmp) {
			MAEMOSEC_ERROR("m_contents broken");
			goto end;
		}
		checked_write(fd, tmp, signctx);
		checked_write(fd, " *", signctx);
		tmp = ii->first.c_str();
		if (!tmp) {
			MAEMOSEC_ERROR("m_contents broken");
			goto end;
		}
		checked_write(fd, tmp, signctx);
		checked_write(fd, "\n", signctx);
	}

	rc = bb5_rsakp_sign(signctx, signmd, sizeof(signmd));

	if (rc > 0) {
		string signature;
		
		checked_write(fd, signature_mark "\n", NULL);

		cols = 0;
		for (size_t i = 0; i < (size_t)rc; i++) {
			sprintf(tmp, "%02x", signmd[i]);
			signature.append(tmp, 2);
			if (WRAPPOINT == ++cols) {
				signature.append("\n");
				cols = 0;
			}
		}
		checked_write(fd, signature.c_str(), NULL);
	}

	if (prot_encrypted == m_prot) {
		string key;
		checked_write(fd, key_mark "\n", NULL);

		cols = 0;
		for (int i = 0; i < m_symkey_len; i++) {
			sprintf(tmp, "%02x", m_symkey[i]);
			key.append(tmp, 2);
			if (WRAPPOINT == ++cols) {
				key.append("\n");
				cols = 0;
			}
		}
		checked_write(fd, key.c_str(), NULL);
	}

end:
	EVP_MD_CTX_free(signctx);

	close(fd);
}


bool
storage::set_aes_key(int op, AES_KEY *ck)
{
	unsigned char* plakey;
	ssize_t plainsize;
	bool res = true;
	int rc = 0;

	plainsize = bb5_rsakp_decrypt(0, 0, m_symkey, m_symkey_len, &plakey);
	if (plainsize > 0) {
		if (AES_ENCRYPT == op)
			rc = AES_set_encrypt_key(plakey, 8 * plainsize, ck);
		else if (AES_DECRYPT == op)
			rc = AES_set_decrypt_key(plakey, 8 * plainsize, ck);
		else {
			MAEMOSEC_ERROR("unsupported cryptop %d", op);
			res = false;
		}
		memset(plakey, '\0', plainsize);
		if (rc != 0) {
			MAEMOSEC_ERROR("Cannot set AES key (%d)", rc);
			res = false;
		}
	} else {
		MAEMOSEC_ERROR("cannot decrypt (%d)", plainsize);
	}
	if (plakey)
		free(plakey);
	return(res);
}


bool
storage::cryptop(int op, unsigned char* data, unsigned char* to, ssize_t len, EVP_MD_CTX* digest)
{
	int rc, i;
	AES_KEY ck;
	unsigned char *from;
	unsigned char ibuf[AES_BLOCK_SIZE];
	unsigned char obuf[AES_BLOCK_SIZE];
	unsigned char cnt = 0;

	if (len % AES_BLOCK_SIZE != 0) {
		MAEMOSEC_ERROR("invalid length %d", len);
		return(false);
	}

	// TODO: Decrypt the symkey
	if (!set_aes_key(op, &ck)) {
		MAEMOSEC_ERROR("no cryptokey available");
		return(false);
	}

	from = data;
	while (len > 0) {
		if (AES_ENCRYPT == op) {
			if (digest) {
				rc = EVP_DigestUpdate(digest, from, AES_BLOCK_SIZE);
				if (rc != EVPOK) {
					MAEMOSEC_ERROR("EVP_DigestUpdate returns %d (%d)", rc, errno);
				}
			}
			for (i = 0; i < AES_BLOCK_SIZE; i++)
				from[i] ^= cnt;
			AES_encrypt(from, from, &ck);
		} else {
			if (!to) {
				AES_decrypt(from, obuf, &ck);
				for (i = 0; i < AES_BLOCK_SIZE; i++)
					obuf[i] ^= cnt;
			} else {
				AES_decrypt(from, to, &ck);
				for (i = 0; i < AES_BLOCK_SIZE; i++)
					to[i] ^= cnt;
			}
			if (digest) {
				if (to)
					rc = EVP_DigestUpdate(digest, to, AES_BLOCK_SIZE);
				else
					rc = EVP_DigestUpdate(digest, obuf, AES_BLOCK_SIZE);
				if (rc != EVPOK) {
					MAEMOSEC_ERROR("EVP_DigestUpdate returns %d (%d)", rc, errno);
					abort();
				}
			}
			if (to) {
				to += AES_BLOCK_SIZE;
			}
		}
		from += AES_BLOCK_SIZE;
		len -= AES_BLOCK_SIZE;
		cnt++;
	}
	memset(&ck, '\0', sizeof(ck));
	return(true);
}


bool
storage::encrypt_file_in_place(const char* pathname, string& digest)
{
	unsigned char* data;
	ssize_t len, rlen, tst;
	int fd, rc;
	bool res = false;
	unsigned int mdlen;
	char hlp [3];
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	unsigned char md[DIGESTLEN];

	data = map_file(pathname, O_RDWR, &fd, &len, &rlen);
	if (MAP_FAILED == data) {
		return(false);
	}

	// Fill the tail with zeroes
	memset(data + rlen, '\0', len - rlen);

	// EVP_MD_CTX_init(&mdctx);
	rc = EVP_DigestInit(mdctx, DIGESTTYP());
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_DigestInit returns %d (%s)", rc, strerror(errno));
		goto err;
	}

	res = cryptop(AES_ENCRYPT, data, NULL, len - 1, mdctx);
	if (res) {
		*(data + len - 1) = len - rlen;
	}

	// write the tail
	tst = lseek(fd, rlen, SEEK_SET);
	if (tst != rlen) {
		MAEMOSEC_ERROR("Seek error");
	}
	tst = write(fd, data + rlen, len - rlen);
	if (tst <= 0) {
		MAEMOSEC_ERROR("Write error");
	}

	unmap_file(data, fd, len);

	rc = EVP_DigestFinal(mdctx, md, &mdlen);
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_DigestFinal returns %d (%d)", rc, errno);
		res = false;
		goto err;
	}

	if ((int)mdlen != DIGESTLEN) {
		MAEMOSEC_ERROR("Digestlen mismatch (%d != %d)", mdlen, DIGESTLEN);
		res = false;
		goto err;
	}

	for (unsigned int i = 0; i < mdlen; i++) {
		sprintf(hlp, "%02x", md[i]);
		digest.append(hlp,2);
	}

err:
	EVP_MD_CTX_free(mdctx);
	if (res)
		MAEMOSEC_DEBUG(1, "Computed digest is '%s'", digest.c_str());

	return(res);
}


bool 
storage::encrypt_file(const char* pathname, unsigned char* from_buf, ssize_t len, string& digest)
{
	unsigned char* locdata;
	ssize_t rlen, tst;
	int fd, rc;
	bool res = false;
	unsigned int mdlen;
	char hlp [3];
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	unsigned char md[DIGESTLEN];

	rlen = encrypted_length(len);

	locdata = (unsigned char*) malloc(rlen);
	if (!locdata) {
		MAEMOSEC_ERROR("cannot allocate");
		goto err;
	}

	memcpy(locdata, from_buf, len);
	// Fill the tail with zeroes
	memset(locdata + len, '\0', rlen - len);

	// EVP_MD_CTX_init(&mdctx);
	rc = EVP_DigestInit(mdctx, DIGESTTYP());
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_DigestInit returns %d (%s)", rc, strerror(errno));
		free(locdata);
		goto err;
	}

	res = cryptop(AES_ENCRYPT, locdata, NULL, rlen - 1, mdctx);
	if (res) {
		*(locdata + rlen - 1) = rlen - len;
	}

	fd = open(pathname, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0) {
		MAEMOSEC_ERROR("cannot create '%s' (%d)", pathname, errno);
		free(locdata);
		res = false;
		goto err;
	}

	tst = write(fd, locdata, rlen);
	if (tst != rlen) {
		MAEMOSEC_ERROR("cannot write %d bytes to '%s', written only %d (%d)", 
			  rlen, pathname, tst, errno);
		free(locdata);
		close(fd);
		res = false;
		goto err;
	}

	free(locdata);
	close(fd);

	rc = EVP_DigestFinal(mdctx, md, &mdlen);
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_DigestFinal returns %d (%d)", rc, errno);
		res = false;
		goto err;
	}

	if ((int)mdlen != DIGESTLEN) {
		MAEMOSEC_ERROR("Digestlen mismatch (%d != %d)", mdlen, DIGESTLEN);
		res = false;
		goto err;
	}

	for (unsigned int i = 0; i < mdlen; i++) {
		sprintf(hlp, "%02x", md[i]);
		digest.append(hlp,2);
	}

err:
	EVP_MD_CTX_free(mdctx);
	if (res)
		MAEMOSEC_DEBUG(1, "Computed digest is '%s'", digest.c_str());

	return(res);
}


bool
storage::decrypt_file(const char* pathname, 
					  unsigned char** to_buf, 
					  ssize_t* len, 
					  string& digest)
{
	int fd, rc;
	unsigned char* data, *locbuf;
	ssize_t llen, rlen, len_difference;
	unsigned int mdlen;
	char hlp [3];
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	unsigned char md[DIGESTLEN];
	bool rv = false;

	digest.clear();
	data = map_file(pathname, O_RDONLY, &fd, &llen, &rlen);
	if (MAP_FAILED == data) {
		MAEMOSEC_ERROR("cannot map '%s'", pathname);
		goto err;
	}
	/*
	 * The last byte encodes the amount of padding that
	 * were needed to make file size to align with crypto
	 * block size. Check sanity.
	 */
	len_difference = *(data + llen - 1);
	if (AES_BLOCK_SIZE < len_difference	|| rlen < len_difference) {
		MAEMOSEC_ERROR("'%s' is corrupted", pathname);
		goto err;
	}
	rlen -= len_difference;
	MAEMOSEC_DEBUG(1, "real len is %d bytes", rlen);
	if (to_buf) {
		*to_buf = locbuf = (unsigned char*) malloc (llen - 1);
		if (!locbuf) {
			MAEMOSEC_ERROR("cannot allocate %d bytes", llen - 1);
			goto err;
		}
		memset(locbuf, '\0', rlen);
	} else
		locbuf = NULL;

	// EVP_MD_CTX_init(&mdctx);
	rc = EVP_DigestInit(mdctx, DIGESTTYP());
	if (EVPOK != rc) {
		MAEMOSEC_ERROR("EVP_DigestInit returns %d (%s)", rc, strerror(errno));
		goto err;
	}

	if (!cryptop(AES_DECRYPT, data, locbuf, llen - 1, mdctx)) {
		MAEMOSEC_ERROR("Decryption failed");
		goto err;
	}

	rc = EVP_DigestFinal(mdctx, md, &mdlen);
	if (rc != EVPOK) {
		MAEMOSEC_ERROR("EVP_DigestFinal returns %d (%d)", rc, errno);
		goto err;
	}
	if ((int)mdlen != DIGESTLEN) {
		MAEMOSEC_ERROR("Digestlen mismatch (%d != %d)", mdlen, DIGESTLEN);
		goto err;
	}

	for (unsigned int i = 0; i < mdlen; i++) {
		sprintf(hlp, "%02x", md[i]);
		digest.append(hlp,2);
	}
	unmap_file(data, fd, llen);
	if (len)
		*len = rlen;
	rv = true;
err:
	EVP_MD_CTX_free(mdctx);
	MAEMOSEC_DEBUG(1, "Computed digest is '%s'", digest.c_str());
	return(rv);
}


int
storage::get_file(const char* pathname, unsigned char** to_buf, ssize_t* bytes)
{
	string truename, digest;
	ssize_t rlen, llen;
	unsigned char* data = NULL;
	int fd, res = 0;
	

	if (!to_buf || !bytes) {
		return(EINVAL);
	}
	*to_buf = NULL;
	*bytes = 0;

	absolute_pathname(pathname, truename);
	if (!contains_file(truename.c_str())) {
		MAEMOSEC_ERROR("'%s' not found", truename.c_str());
		return(EINVAL);
	}

	if (prot_encrypted == m_prot) {
		if (decrypt_file(truename.c_str(), to_buf, bytes, digest)) {
			if (digest == m_contents[truename]) {
				return(0);
			} else {
				MAEMOSEC_ERROR("Digest does not match");
				return(-1);
			}
		} else {
			MAEMOSEC_ERROR("Failed to decrypt");
			return(-1);
		}

	} else {
		data = map_file(truename.c_str(), O_RDONLY, &fd, &llen, &rlen);
		if (MAP_FAILED != data) {
			compute_digest(data, llen, digest);
			if (digest == m_contents[truename]) {
				*to_buf = (unsigned char*) malloc(llen);
				if (NULL != *to_buf) {
					memcpy(*to_buf, data, llen);
					*bytes = llen;
				} else {
					MAEMOSEC_ERROR("cannot allocate '%d' bytes", *bytes);
					res = -1;
					goto end;
				}
			} else {
				MAEMOSEC_ERROR("Digest does not match");
				res = -1;
				goto end;
			}
		} else {
			MAEMOSEC_ERROR("map failed");
			return(errno);
		}
	}
  end:
	if (data) {
		unmap_file(data, fd, *bytes);
	}
	return(res);
}


int
storage::put_file(const char* pathname, unsigned char* data, ssize_t bytes)
{
	string truename, digest;
	ssize_t rlen;
	int fd, rc;

	if (!data || !bytes) {
		return(EINVAL);
	}

	if (prot_encrypted == m_prot) {
		if (!encrypt_file(pathname, data, bytes, digest)) {
			return(EFAULT);
		}

	} else {
		fd = open(pathname, O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (-1 == fd) {
			MAEMOSEC_ERROR("cannot open '%s' (%d)", pathname, errno);
			return(errno);
		}

		rlen = write(fd, data, bytes);
		if (rlen != bytes) {
			MAEMOSEC_ERROR("cannot write %d bytes to '%s', written only %d (%d)", 
				  bytes, pathname, rlen, errno);
			close(fd);
			return(errno);
		}

		close(fd);
		compute_digest(data, bytes, digest);
	}

	absolute_pathname(pathname, truename);
	m_contents[truename] = digest;
	return(0);
}


void
storage::release_buffer(unsigned char* buf)
{
	if (buf) 
		free(buf);
}


int
storage::nbrof_files(void)
{
	return(m_contents.size());
}


const char* 
storage::name(void)
{
	return(m_name.c_str());
}


const char* 
storage::filename(void)
{
	return(m_filename.c_str());
}


int
storage::iterate_storage_names(storage::visibility_t of_visibility, 
					  storage::protection_t of_protection, 
					  const char* matching_names,
					  maemosec_callback* cb_func,
					  void* ctx)
{
	string directory_name;
	
	if (0 != get_storage_directory(of_visibility, of_protection, directory_name)) {
		MAEMOSEC_ERROR("Cannot get storage name");
		return(-ENOENT);
	}
	MAEMOSEC_DEBUG(1, "Iterating '%s' in '%s'", matching_names, directory_name.c_str());
	return (iterate_files(directory_name.c_str(), matching_names, cb_func, ctx));
}
