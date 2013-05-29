/*
    invitation.c -- Create and accept invitations
    Copyright (C) 2013 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include "control_common.h"
#include "crypto.h"
#include "ecdsa.h"
#include "ecdsagen.h"
#include "invitation.h"
#include "names.h"
#include "netutl.h"
#include "rsagen.h"
#include "sptps.h"
#include "tincctl.h"
#include "utils.h"
#include "xalloc.h"

#ifdef HAVE_MINGW
#define SCRIPTEXTENSION ".bat"
#else
#define SCRIPTEXTENSION ""
#endif

int addressfamily = AF_UNSPEC;

char *get_my_hostname() {
	char *hostname = NULL;
	char *name = get_my_name(false);
	char *filename = NULL;

	// Use first Address statement in own host config file
	if(check_id(name)) {
		xasprintf(&filename, "%s" SLASH "hosts" SLASH "%s", confbase, name);
		FILE *f = fopen(filename, "r");
		if(f) {
			while(fgets(line, sizeof line, f)) {
				if(!rstrip(line))
					continue;
				char *p = line, *q;
				p += strcspn(p, "\t =");
				if(!*p)
					continue;
				q = p + strspn(p, "\t ");
				if(*q == '=')
					q += 1 + strspn(q + 1, "\t ");
				*p = 0;
				if(strcasecmp(line, "Address"))
					continue;
				p = q + strcspn(q, "\t ");
				if(*p)
					*p++ = 0;
				p += strspn(p, "\t ");
				p[strcspn(p, "\t ")] = 0;
				if(*p) {
					if(strchr(q, ':'))
						xasprintf(&hostname, "[%s]:%s", q, p);
					else
						xasprintf(&hostname, "%s:%s", q, p);
				} else {
					hostname = xstrdup(q);
				}
				break;
			}
			fclose(f);
		}
	}

	if(hostname) {
		free(filename);
		return hostname;
	}

	// If that doesn't work, guess externally visible hostname
	fprintf(stderr, "Trying to discover externally visible hostname...\n");
	struct addrinfo *ai = str2addrinfo("ifconfig.me", "80", SOCK_STREAM);
	static const char request[] = "GET /host HTTP/1.0\r\n\r\n";
	if(ai) {
		int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(s >= 0) {
			if(connect(s, ai->ai_addr, ai->ai_addrlen)) {
				closesocket(s);
				s = -1;
			}
		}
		if(s >= 0) {
			send(s, request, sizeof request - 1, 0);
			int len = recv(s, line, sizeof line - 1, MSG_WAITALL);
			if(len > 0) {
				line[len] = 0;
				if(line[len - 1] == '\n')
					line[--len] = 0;
				char *p = strrchr(line, '\n');
				if(p && p[1])
					hostname = xstrdup(p + 1);
			}
			closesocket(s);
		}
		freeaddrinfo(ai);
	}

	// Check that the hostname is reasonable
	if(hostname) {
		for(char *p = hostname; *p; p++) {
			if(isalnum(*p) || *p == '-' || *p == '.')
				continue;
			// If not, forget it.
			free(hostname);
			hostname = NULL;
			break;
		}
	}

again:
	printf("Please enter your host's external address or hostname");
	if(hostname)
		printf(" [%s]", hostname);
	printf(": ");
	fflush(stdout);

	if(!fgets(line, sizeof line, stdin)) {
		fprintf(stderr, "Error while reading stdin: %s\n", strerror(errno));
		free(hostname);
		return NULL;
	}

	if(!rstrip(line)) {
		if(hostname)
			goto done;
		else
			goto again;
	}

	for(char *p = line; *p; p++) {
		if(isalnum(*p) || *p == '-' || *p == '.')
			continue;
		fprintf(stderr, "Invalid address or hostname.\n");
		goto again;
	}

	free(hostname);
	hostname = xstrdup(line);

done:
	if(filename) {
		FILE *f = fopen(filename, "a");
		if(f) {
			fprintf(f, "\nAddress = %s\n", hostname);
			fclose(f);
		} else {
			fprintf(stderr, "Could not append Address to %s: %s\n", filename, strerror(errno));
		}
		free(filename);
	}

	return hostname;
}

static bool fcopy(FILE *out, const char *filename) {
	FILE *in = fopen(filename, "r");
	if(!in) {
		fprintf(stderr, "Could not open %s: %s\n", filename, strerror(errno));
		return false;
	}

	char buf[1024];
	size_t len;
	while((len = fread(buf, 1, sizeof buf, in)))
		fwrite(buf, len, 1, out);
	fclose(in);
	return true;
}

int cmd_invite(int argc, char *argv[]) {
	if(argc < 2) {
		fprintf(stderr, "Not enough arguments!\n");
		return 1;
	}

	// Check validity of the new node's name
	if(!check_id(argv[1])) {
		fprintf(stderr, "Invalid name for node.\n");
		return 1;
	}

	char *myname = get_my_name(true);
	if(!myname)
		return 1;

	// Ensure no host configuration file with that name exists
	char *filename = NULL;
	xasprintf(&filename, "%s" SLASH "hosts" SLASH "%s", confbase, argv[1]);
	if(!access(filename, F_OK)) {
		free(filename);
		fprintf(stderr, "A host config file for %s already exists!\n", argv[1]);
		return 1;
	}
	free(filename);

	// If a daemon is running, ensure no other nodes now about this name
	bool found = false;
	if(connect_tincd(false)) {
		sendline(fd, "%d %d", CONTROL, REQ_DUMP_NODES);

		while(recvline(fd, line, sizeof line)) {
			char node[4096];
			int code, req;
			if(sscanf(line, "%d %d %s", &code, &req, node) != 3)
				break;
			if(!strcmp(node, argv[1]))
				found = true;
		}

		if(found) {
			fprintf(stderr, "A node with name %s is already known!\n", argv[1]);
			return 1;
		}
	}

	char hash[25];

	xasprintf(&filename, "%s" SLASH "invitations", confbase);
	if(mkdir(filename, 0700) && errno != EEXIST) {
		fprintf(stderr, "Could not create directory %s: %s\n", filename, strerror(errno));
		free(filename);
		return 1;
	}

	// Count the number of valid invitations, clean up old ones
	DIR *dir = opendir(filename);
	if(!dir) {
		fprintf(stderr, "Could not read directory %s: %s\n", filename, strerror(errno));
		free(filename);
		return 1;
	}

	errno = 0;
	int count = 0;
	struct dirent *ent;
	time_t deadline = time(NULL) - 604800; // 1 week in the past

	while((ent = readdir(dir))) {
		if(strlen(ent->d_name) != 24)
			continue;
		char *invname;
		struct stat st;
		xasprintf(&invname, "%s" SLASH "%s", filename, ent->d_name);
		if(!stat(invname, &st)) {
			if(deadline < st.st_mtime)
				count++;
			else
				unlink(invname);
		} else {
			fprintf(stderr, "Could not stat %s: %s\n", invname, strerror(errno));
			errno = 0;
		}
		free(invname);
	}

	if(errno) {
		fprintf(stderr, "Error while reading directory %s: %s\n", filename, strerror(errno));
		closedir(dir);
		free(filename);
		return 1;
	}
		
	closedir(dir);
	free(filename);

	ecdsa_t *key;
	xasprintf(&filename, "%s" SLASH "invitations" SLASH "ecdsa_key.priv", confbase);

	// Remove the key if there are no outstanding invitations.
	if(!count)
		unlink(filename);

	// Create a new key if necessary.
	FILE *f = fopen(filename, "r");
	if(!f) {
		if(errno != ENOENT) {
			fprintf(stderr, "Could not read %s: %s\n", filename, strerror(errno));
			free(filename);
			return 1;
		}

		key = ecdsa_generate();
		if(!key) {
			free(filename);
			return 1;
		}
		f = fopen(filename, "w");
		if(!f) {
			fprintf(stderr, "Could not write %s: %s\n", filename, strerror(errno));
			free(filename);
			return 1;
		}
		chmod(filename, 0600);
		ecdsa_write_pem_private_key(key, f);
	} else {
		key = ecdsa_read_pem_private_key(f);
		if(!key)
			fprintf(stderr, "Could not read private key from %s\n", filename);
	}
	fclose(f);
	free(filename);
	if(!key)
		return 1;

	// Create a hash of the key.
	char *fingerprint = ecdsa_get_base64_public_key(key);
	digest_t *digest = digest_open_by_name("sha256", 18);
	if(!digest)
		abort();
	digest_create(digest, fingerprint, strlen(fingerprint), hash);
	b64encode_urlsafe(hash, hash, 18);

	// Create a random cookie for this invitation.
	char cookie[25];
	randomize(cookie, 18);
	b64encode_urlsafe(cookie, cookie, 18);

	// Create a file containing the details of the invitation.
	xasprintf(&filename, "%s" SLASH "invitations" SLASH "%s", confbase, cookie);
	int ifd = open(filename, O_RDWR | O_CREAT | O_EXCL, 0600);
	if(!ifd) {
		fprintf(stderr, "Could not create invitation file %s: %s\n", filename, strerror(errno));
		free(filename);
		return 1;
	}
	free(filename);
	f = fdopen(ifd, "w");
	if(!f)
		abort();

	// Fill in the details.
	fprintf(f, "Name = %s\n", argv[1]);
	if(netname)
		fprintf(f, "NetName = %s\n", netname);
	fprintf(f, "ConnectTo = %s\n", myname);
	// TODO: copy Broadcast and Mode
	fprintf(f, "#---------------------------------------------------------------#\n");
	fprintf(f, "Name = %s\n", myname);

	xasprintf(&filename, "%s" SLASH "hosts" SLASH "%s", confbase, myname);
	fcopy(f, filename);
	fclose(f);

	// Create an URL from the local address, key hash and cookie
	char *address = get_my_hostname();
	printf("%s/%s%s\n", address, hash, cookie);
	free(filename);
	free(address);

	return 0;
}

static int sock;
static char cookie[18];
static sptps_t sptps;
static char *data;
static size_t datalen;
static bool success = false;

static char cookie[18], hash[18];

static char *get_line(const char **data) {
	if(!data || !*data)
		return NULL;

	if(!**data) {
		*data = NULL;
		return NULL;
	}

	static char line[1024];
	const char *end = strchr(*data, '\n');
	size_t len = end ? end - *data : strlen(*data);
	if(len >= sizeof line) {
		fprintf(stderr, "Maximum line length exceeded!\n");
		return NULL;
	}
	if(len && !isprint(**data))
		abort();

	memcpy(line, *data, len);
	line[len] = 0;

	if(end)
		*data = end + 1;
	else
		*data = NULL;

	return line;
}

static char *get_value(const char *data, const char *var) {
	char *line = get_line(&data);
	if(!line)
		return NULL;

	char *sep = line + strcspn(line, " \t=");
	char *val = sep + strspn(sep, " \t");
	if(*val == '=')
		val += 1 + strspn(val + 1, " \t");
	*sep = 0;
	if(strcasecmp(line, var))
		return NULL;
	return val;
}

static char *grep(const char *data, const char *var) {
	static char value[1024];

	const char *p = data;
	int varlen = strlen(var);

	// Skip all lines not starting with var
	while(strncasecmp(p, var, varlen) || !strchr(" \t=", p[varlen])) {
		p = strchr(p, '\n');
		if(!p)
			break;
		else
			p++;
	}

	if(!p)
		return NULL;

	p += varlen;
	p += strspn(p, " \t");
	if(*p == '=')
		p += 1 + strspn(p + 1, " \t");

	const char *e = strchr(p, '\n');
	if(!e)
		return xstrdup(p);

	if(e - p >= sizeof value) {
		fprintf(stderr, "Maximum line length exceeded!\n");
		return NULL;
	}

	memcpy(value, p, e - p);
	value[e - p] = 0;
	return value;
}

static bool finalize_join(void) {
	char *name = xstrdup(get_value(data, "Name"));
	if(!name) {
		fprintf(stderr, "No Name found in invitation!\n");
		return false;
	}

	if(!check_id(name)) {
		fprintf(stderr, "Invalid Name found in invitation: %s!\n", name);
		return false;
	}

	if(!netname)
		netname = grep(data, "NetName");

make_names:
	if(!confbasegiven) {
		free(confbase);
		confbase = NULL;
	}

	make_names();

	free(tinc_conf);
	free(hosts_dir);

	xasprintf(&tinc_conf, "%s" SLASH "tinc.conf", confbase);
	xasprintf(&hosts_dir, "%s" SLASH "hosts", confbase);

	if(!access(tinc_conf, F_OK)) {
		fprintf(stderr, "Configuration file %s already exists!\n", tinc_conf);
		if(!tty || confbasegiven)
			return false;
ask_netname:
		fprintf(stderr, "Enter a new netname: ");
		if(!fgets(line, sizeof line, stdin)) {
			fprintf(stderr, "Error while reading stdin: %s\n", strerror(errno));
			return false;
		}
		if(!*line || *line == '\n')
			goto ask_netname;

		line[strlen(line) - 1] = 0;
		netname = line;
		goto make_names;
	}	

	if(mkdir(confbase, 0755) && errno != EEXIST) {
		fprintf(stderr, "Could not create directory %s: %s\n", confbase, strerror(errno));
		return false;
	}

	if(mkdir(hosts_dir, 0755) && errno != EEXIST) {
		fprintf(stderr, "Could not create directory %s: %s\n", hosts_dir, strerror(errno));
		return false;
	}

	FILE *f = fopen(tinc_conf, "w");
	if(!f) {
		fprintf(stderr, "Could not create file %s: %s\n", tinc_conf, strerror(errno));
		return false;
	}

	fprintf(f, "Name = %s\n", name);

	char *filename;
	xasprintf(&filename, "%s" SLASH "%s", hosts_dir, name);
	FILE *fh = fopen(filename, "w");
	if(!fh) {
		fprintf(stderr, "Could not create file %s: %s\n", filename, strerror(errno));
		return false;
	}

	// Filter first chunk on approved keywords, split between tinc.conf and hosts/Name
	// Other chunks go unfiltered to their respective host config files
	const char *p = data;
	char *l, *value;

	while((l = get_line(&p))) {
		// Ignore comments
		if(*l == '#')
			continue;

		// Split line into variable and value
		int len = strcspn(l, "\t =");
		value = l + len;
		value += strspn(value, "\t ");
		if(*value == '=') {
			value++;
			value += strspn(value, "\t ");
		}
		l[len] = 0;

		// Is it a Name?
		if(!strcasecmp(l, "Name"))
			if(strcmp(value, name))
				break;
			else
				continue;
		else if(!strcasecmp(l, "NetName"))
			continue;

		// Check the list of known variables
		bool found = false;
		int i;
		for(i = 0; variables[i].name; i++) {
			if(strcasecmp(l, variables[i].name))
				continue;
			found = true;
			break;
		}

		// Ignore unknown and unsafe variables
		if(!found) {
			fprintf(stderr, "Ignoring unknown variable '%s' in invitation.\n", l);
			continue;
		} else if(!(variables[i].type & VAR_SAFE)) {
			fprintf(stderr, "Ignoring unsafe variable '%s' in invitation.\n", l);
			continue;
		}

		// Copy the safe variable to the right config file
		fprintf(variables[i].type & VAR_HOST ? fh : f, "%s = %s\n", l, value);
	}

	fclose(f);
	free(filename);

	while(l && !strcasecmp(l, "Name")) {
		if(!check_id(value)) {
			fprintf(stderr, "Invalid Name found in invitation.\n");
			return false;
		}

		if(!strcmp(value, name)) {
			fprintf(stderr, "Secondary chunk would overwrite our own host config file.\n");
			return false;
		}

		xasprintf(&filename, "%s" SLASH "%s", hosts_dir, value);
		f = fopen(filename, "w");

		if(!f) {
			fprintf(stderr, "Could not create file %s: %s\n", filename, strerror(errno));
			return false;
		}

		while((l = get_line(&p))) {
			if(!strcmp(l, "#---------------------------------------------------------------#"))
				continue;
			int len = strcspn(l, "\t =");
			if(len == 4 && !strncasecmp(l, "Name", 4)) {
				value = l + len;
				value += strspn(value, "\t ");
				if(*value == '=') {
					value++;
					value += strspn(value, "\t ");
				}
				l[len] = 0;
				break;
			}

			fputs(l, f);
			fputc('\n', f);
		}

		fclose(f);
		free(filename);
	}

	// Generate our key and send a copy to the server
	ecdsa_t *key = ecdsa_generate();
	if(!key)
		return false;

	char *b64key = ecdsa_get_base64_public_key(key);
	if(!b64key)
		return false;

	xasprintf(&filename, "%s" SLASH "ecdsa_key.priv", confbase);
	f = fopen(filename, "w");

#ifdef HAVE_FCHMOD
	/* Make it unreadable for others. */
	fchmod(fileno(f), 0600);
#endif

	if(!ecdsa_write_pem_private_key(key, f)) {
		fprintf(stderr, "Error writing private key!\n");
		ecdsa_free(key);
		fclose(f);
		return false;
	}

	fclose(f);

	fprintf(fh, "ECDSAPublicKey = %s\n", b64key);

	sptps_send_record(&sptps, 1, b64key, strlen(b64key));
	free(b64key);


	rsa_t *rsa = rsa_generate(2048, 0x1001);
	xasprintf(&filename, "%s" SLASH "rsa_key.priv", confbase);
	f = fopen(filename, "w");

#ifdef HAVE_FCHMOD
	/* Make it unreadable for others. */
	fchmod(fileno(f), 0600);
#endif

	rsa_write_pem_private_key(rsa, f);
	fclose(f);

	rsa_write_pem_public_key(rsa, fh);
	fclose(fh);

	ecdsa_free(key);
	rsa_free(rsa);

	fprintf(stderr, "Invitation succesfully accepted.\n");
	shutdown(sock, SHUT_RDWR);
	success = true;

	return true;
}

static bool invitation_send(void *handle, uint8_t type, const char *data, size_t len) {
	while(len) {
		int result = send(sock, data, len, 0);
		if(result == -1 && errno == EINTR)
			continue;
		else if(result <= 0)
			return false;
		data += result;
		len -= result;
	}
	return true;
}

static bool invitation_receive(void *handle, uint8_t type, const char *msg, uint16_t len) {
	switch(type) {
		case SPTPS_HANDSHAKE:
			return sptps_send_record(&sptps, 0, cookie, sizeof cookie);

		case 0:
			data = xrealloc(data, datalen + len + 1);
			memcpy(data + datalen, msg, len);
			datalen += len;
			data[datalen] = 0;
			break;

		case 1:
			return finalize_join();

		default:
			return false;
	}

	return true;
}

int cmd_join(int argc, char *argv[]) {
	free(data);
	data = NULL;
	datalen = 0;

	if(argc > 2) {
		fprintf(stderr, "Too many arguments!\n");
		return 1;
	}

	// Make sure confdir exists.
	if(mkdir(confdir, 0755) && errno != EEXIST) {
		fprintf(stderr, "Could not create directory %s: %s\n", CONFDIR, strerror(errno));
		return 1;
	}

	// Either read the invitation from the command line or from stdin.
	char *invitation;

	if(argc > 1) {
		invitation = argv[1];
	} else {
		if(tty) {
			printf("Enter invitation URL: ");
			fflush(stdout);
		}
		errno = EPIPE;
		if(!fgets(line, sizeof line, stdin)) {
			fprintf(stderr, "Error while reading stdin: %s\n", strerror(errno));
			return false;
		}
		invitation = line;
	}

	// Parse the invitation URL.
	rstrip(line);

	char *slash = strchr(invitation, '/');
	if(!slash)
		goto invalid;

	*slash++ = 0;

	if(strlen(slash) != 48)
		goto invalid;

	char *address = invitation;
	char *port = NULL;
	if(*address == '[') {
		address++;
		char *bracket = strchr(address, ']');
		if(!bracket)
			goto invalid;
		*bracket = 0;
		if(bracket[1] == ':')
			port = bracket + 2;
	} else {
		port = strchr(address, ':');
		if(port)
			*port++ = 0;
	}

	if(!port || !*port)
		port = "655";

	if(!b64decode(slash, hash, 18) || !b64decode(slash + 24, cookie, 18))
		goto invalid;

	// Generate a throw-away key for the invitation.
	ecdsa_t *key = ecdsa_generate();
	if(!key)
		return 1;

	char *b64key = ecdsa_get_base64_public_key(key);

	// Connect to the tinc daemon mentioned in the URL.
	struct addrinfo *ai = str2addrinfo(address, port, SOCK_STREAM);
	if(!ai)
		return 1;

	sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if(sock <= 0) {
		fprintf(stderr, "Could not open socket: %s\n", strerror(errno));
		return 1;
	}

	if(connect(sock, ai->ai_addr, ai->ai_addrlen)) {
		fprintf(stderr, "Could not connect to %s port %s: %s\n", address, port, strerror(errno));
		closesocket(sock);
		return 1;
	}

	fprintf(stderr, "Connected to %s port %s...\n", address, port);

	// Tell him we have an invitation, and give him our throw-away key.
	int len = snprintf(line, sizeof line, "0 ?%s %d.%d\n", b64key, PROT_MAJOR, PROT_MINOR);
	if(len <= 0 || len >= sizeof line)
		abort();

	if(!sendline(sock, "0 ?%s %d.%d", b64key, PROT_MAJOR, 1)) {
		fprintf(stderr, "Error sending request to %s port %s: %s\n", address, port, strerror(errno));
		closesocket(sock);
		return 1;
	}

	char hisname[4096] = "";
	int code, hismajor, hisminor = 0;

	if(!recvline(sock, line, sizeof line) || sscanf(line, "%d %s %d.%d", &code, hisname, &hismajor, &hisminor) < 3 || code != 0 || hismajor != PROT_MAJOR || !check_id(hisname) || !recvline(sock, line, sizeof line) || !rstrip(line) || sscanf(line, "%d ", &code) != 1 || code != ACK || strlen(line) < 3) {
		fprintf(stderr, "Cannot read greeting from peer\n");
		closesocket(sock);
		return 1;
	}

	// Check if the hash of the key he have us matches the hash in the URL.
	char *fingerprint = line + 2;
	digest_t *digest = digest_open_by_name("sha256", 18);
	if(!digest)
		abort();
	char hishash[18];
	if(!digest_create(digest, fingerprint, strlen(fingerprint), hishash)) {
		fprintf(stderr, "Could not create digest\n%s\n", line + 2);
		return 1;
	}
	if(memcmp(hishash, hash, 18)) {
		fprintf(stderr, "Peer has an invalid key!\n%s\n", line + 2);
		return 1;

	}
	
	ecdsa_t *hiskey = ecdsa_set_base64_public_key(fingerprint);
	if(!hiskey)
		return 1;

	// Start an SPTPS session
	if(!sptps_start(&sptps, NULL, true, false, key, hiskey, "tinc invitation", 15, invitation_send, invitation_receive))
		return 1;

	// Feed rest of input buffer to SPTPS
	if(!sptps_receive_data(&sptps, buffer, blen))
		return 1;

	while((len = recv(sock, line, sizeof line, 0))) {
		if(len < 0) {
			if(errno == EINTR)
				continue;
			fprintf(stderr, "Error reading data from %s port %s: %s\n", address, port, strerror(errno));
			return 1;
		}

		if(!sptps_receive_data(&sptps, line, len))
			return 1;
	}
	
	sptps_stop(&sptps);
	ecdsa_free(hiskey);
	ecdsa_free(key);
	closesocket(sock);

	if(!success) {
		fprintf(stderr, "Connection closed by peer, invitation cancelled.\n");
		return 1;
	}

	return 0;

invalid:
	fprintf(stderr, "Invalid invitation URL.\n");
	return 1;
}
