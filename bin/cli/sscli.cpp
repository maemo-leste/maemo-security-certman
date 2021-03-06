/* -*- mode:c++; tab-width:4; c-basic-offset:4;
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

#include <maemosec_common.h>
#include <maemosec_storage.h>
#include <libbb5stub.h>

using namespace std;
using namespace maemosec;

static void 
usage(void)
{
	printf(
		"Usage: sscli -s <storage:[P|S][s|e]> -<a|u|d|v|p> <filename>\n"
		" -s to give the storage name (must be first argument)\n"
		"       and optional attributes, separated by a colon\n"
		"    Attribute flags are P=private, S=shared, s=signed, e=encrypted\n"
		"    The default is a private, signed storage if no attributes are given\n"
		" -a to add a file to the storage\n"
		" -u to update a file in the storage.\n"
		"       With an encrypted storage the new contents is read from stdin\n"
		"       otherwise the current contents of the file in place is used\n"
		" -d to remove a file from the storage\n"
		" -v to verify the storage (if no filename is given, all files will be verified)\n"
		" -p to print the file contents to stdout\n"
		" -D to increase the debug output level\n"
		);
}

int
main(int argc, char* argv[])
{
	int a, len;
	string storagename;
	char* flags;
	storage* ss = NULL;
	bool was_changed = false;
	storage::visibility_t storvis  = storage::vis_private;
	storage::protection_t storprot = storage::prot_signed;

	if (1 == argc) {
		usage();
		return(-1);
	}

	bb5_init();

    while (1) {
		a = getopt(argc, argv, "s:a:u:d:p:v::Dh");
		if (a < 0) {
			break;
		}
		switch(a) 
		{
		case 's':
			flags = strchr(optarg, ':');
			if (flags) {
				storagename.assign(optarg, flags - optarg);
				flags++;
				while (*flags) {
					switch (*flags) 
						{
						case 'S':
							storvis = storage::vis_shared;
							break;

						case 'P':
							storvis = storage::vis_private;
							break;

						case 'e':
							storprot = storage::prot_encrypted;
							break;
							
						case 's':
							storprot = storage::prot_signed;
							break;

						default:
							MAEMOSEC_ERROR("Invalid attribute '%c'", *flags);
						}
					flags++;
				}
			} else
				storagename.assign(optarg);

			ss = new storage(storagename.c_str(), storvis, storprot);
			if (!ss) {
				MAEMOSEC_ERROR("cannot create storage");
				return(-1);
			}
			if (ss->nbrof_files() == 0) {
				MAEMOSEC_DEBUG(1, "Created new storage '%s' in %s.", 
						ss->name(), ss->filename());
			} else {
				MAEMOSEC_DEBUG(1, "Storage '%s' contains %d files.", 
						ss->name(), ss->nbrof_files());
			}
			break;

		case 'a':
			if (!ss) {
				MAEMOSEC_ERROR("create storage first");
				return(-1);
			}
			ss->add_file(optarg);
			was_changed = true;
			break;

		case 'd':
			if (!ss) {
				MAEMOSEC_ERROR("create storage first");
				return(-1);
			}
			ss->remove_file(optarg);
			was_changed = true;
			break;

		case 'v':
			if (!ss) {
				MAEMOSEC_ERROR("create storage first");
				return(-1);
			}

			if (optarg) {
				if (ss->verify_file(optarg)) {
					printf("Verify OK\n");
				} else {
					printf("Verification fails\n");
				}

			} else {
				storage::stringlist files;
				ss->get_files(files);
				for (size_t i = 0; i < files.size(); i++) {
					printf("%s: ", files[i]);
					if (ss->verify_file(files[i]))
						printf("OK\n");
					else
						printf("FAILED\n");
				}
			}
			break;

		case 'p':
			if (!ss) {
				MAEMOSEC_ERROR("create storage first");
				return(-1);
			}
			{
				unsigned char* buf;
				ssize_t len;
				int rc;
				
				rc = ss->get_file(optarg, &buf, &len);
				if (0 == rc) {
					for (ssize_t i = 0; i < len; i++) {
						putchar(*(buf + i));
					}
				} else {
					MAEMOSEC_ERROR("failed to open file (%d)", rc);
				}
				ss->release_buffer(buf);
			}
			break;

		case 'u':
			if (!ss) {
				MAEMOSEC_ERROR("create storage first");
				return(-1);
			}
			{
				int rc, c;
				unsigned char* buf;
				ssize_t len = 0, blen = 1024;

				buf = (unsigned char*)malloc(blen);
				if (!buf) {
					MAEMOSEC_ERROR("cannot malloc");
					return(-1);
				}
				do {
					c = getchar();
					if (c >= 0) {
						if (len == blen) {
							unsigned char* swp;

							blen *= 2;
							swp = (unsigned char*)malloc(blen);
							if (!swp) {
								MAEMOSEC_ERROR("cannot malloc %d", blen);
								return(-1);
							}
							memcpy(swp, buf, len);
							free(buf);
							buf = swp;
						}
						buf[len++] = (unsigned char)c;
					} else
						break;
				} while (true);

				rc = ss->put_file(optarg, buf, len);
				if (rc != 0) {
					MAEMOSEC_ERROR("cannot update '%s' (%d)", optarg, rc);
				} else {
					was_changed = true;
				}
				free(buf);
			}
			break;

			// An undocumented switch: generate random hexbytes
		case 'r':
			len = atoi(optarg);
			if (len > 0) {
				unsigned char* rdata = (unsigned char*) malloc(len);
				if (rdata) {
					bb5_get_random(rdata, len);
					for (int i = 0; i < len; i++)
						printf("%02x", rdata[i]);
					printf("\n");
					free(rdata);
				} else
					MAEMOSEC_ERROR("cannot malloc");
			}
			goto end;
		   
		default:
			usage();
			return(-1);
		}
	}

	if (was_changed) {
		MAEMOSEC_DEBUG(1, "Updating changes.");
		ss->commit();
		delete(ss);
	} 

  end:
	bb5_finish();
	return(0);
}

	

