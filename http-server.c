#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>

static void die(const char *s) { perror(s); exit(1); }

void send_error(int error_code, char *reason_phrase, FILE *client_file, char *clientIP, char *get_request);

int main(int argc, char **argv) {

	if(argc != 5) {
		fprintf(stderr, "usage: %s <server-port> <webroot> <mdb-lookup-host> <mdb-lookup-port>\n", argv[0]);
		exit(1);
	}

	// Create a socket for TCP connection (to client)
	int servsock;
	if((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		die("socket failed");

	// Construct a server address structure (for connecting to client)
	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr)); // must zero out the structure
	servaddr.sin_family      = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port        = htons(atoi(argv[1])); // must be in network byte order

	// Bind to the local address
	if(bind(servsock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
		die("bind failed");

	// Start listening for incoming connections
	if(listen(servsock, 5 /* queue size for connection requests */ ) < 0)
		die("listen failed");
	
	int clntsock;
	socklen_t clntlen;
	struct sockaddr_in clntaddr;
	char *clientIP;

	// Make persistent TCP connection to mdb-lookup-host

	// Get IP address of mdb-lookup-host
	struct hostent *he;
	char *mdbHostName = argv[3];
	// get host ip from host name
	if((he = gethostbyname(mdbHostName)) == NULL) {
		die("gethostbyname failed");
	}
	char *mdbHostIP = inet_ntoa(*(struct in_addr *)he->h_addr);

	// Create a socket for TCP connection (to mdb-lookup-host)
	int mdbsock;
	if((mdbsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        	die("mdbsocket failed");
	
	// Construct a server address structure (for connecting to mdb-lookup-host)
	struct sockaddr_in mdbservaddr;
	memset(&mdbservaddr, 0, sizeof(mdbservaddr)); // must zero out the structure
	mdbservaddr.sin_family      = AF_INET;
	mdbservaddr.sin_addr.s_addr = inet_addr(mdbHostIP);
	mdbservaddr.sin_port        = htons(atoi(argv[4])); // must be in network byte order

	// Establish a TCP connection to mdb-lookup-host
	if(connect(mdbsock, (struct sockaddr *) &mdbservaddr, sizeof(mdbservaddr)) < 0)
		die("mdb-lookup-host connect failed");

	// Make file for mdb-lookup-host connection	
	FILE *mdb_lookup_host_file = fdopen(mdbsock, "rb+");
	if(mdb_lookup_host_file == NULL)
		die("mdb_lookup_host_file failed");

	for(;;) {

		clntlen = sizeof(clntaddr); // initialize the in-out parameter

		if((clntsock = accept(servsock, (struct sockaddr *) &clntaddr, &clntlen)) < 0)
			continue; // do not die if accept fails, just move onto next client

		clientIP = inet_ntoa(clntaddr.sin_addr);
		FILE *client_file = fdopen(clntsock, "rb+");
		if(client_file == NULL) {
			close(clntsock);
			continue; // do not die if client_file fails, just move onto next client
		}

		char get_request_to_truncate[4096];
		if(!fgets(get_request_to_truncate, sizeof(get_request_to_truncate), client_file)) {
			fclose(client_file);
			continue; // if fgets fails like if the client dies, move on from the dead client
		}
		
		int last = strlen(get_request_to_truncate) - 2;
		if(get_request_to_truncate[last] == '\r')
			get_request_to_truncate[last] = 0;
		
		char get_request[4096];
		strncpy(get_request, get_request_to_truncate, sizeof(get_request_to_truncate));
	
		char header_line[1024];
		while(fgets(header_line, sizeof(header_line), client_file)) { // skip through headers
			if(strcmp(header_line, "\r\n") == 0) // strcmp() returns 0 when two strings are equal
				break;
		}

		if(strcmp(header_line, "\r\n") != 0) {
			send_error(400, "Bad Request", client_file, clientIP, get_request);
			continue;
		}

		char *token_separators = "\t \r\n"; // tab, space, new line
		char *method = strtok(get_request_to_truncate, token_separators);
		char *requestURI = strtok(NULL, token_separators);
		char *httpVersion = strtok(NULL, token_separators);

		// 400 error must be dealt with before newRequestURI is created
		if(method == NULL || 
			requestURI == NULL || 
			httpVersion == NULL || 
			requestURI[0] != '/' || 
			strstr(requestURI, "/../") || 
			!strcmp(strrchr(requestURI, '/'), "/..")) {

			send_error(400, "Bad Request", client_file, clientIP, get_request);
			continue;
		
		}

		char newRequestURI[1024];
		if(strstr(requestURI, "/mdb-lookup") != requestURI) { // newRequestURI is only for non mdb-lookup stuff
			strncpy(newRequestURI, argv[2], sizeof(newRequestURI));
			strcat(newRequestURI, requestURI);
			if(requestURI[strlen(requestURI) - 1] == '/')
				strcat(newRequestURI, "index.html");
		}

		// determine error code and reason phrase
		struct stat sb;
		if(strcmp(method, "GET") || (strcmp(httpVersion, "HTTP/1.0") && strcmp(httpVersion, "HTTP/1.1"))) {
			send_error(501, "Not Implemented", client_file, clientIP, get_request);
			continue;
		} else if(strstr(requestURI, "/mdb-lookup") == requestURI) { // pointer to /mdb-lookup and requestURI are same if uri starts with it
			;
		} else if(access(newRequestURI, F_OK) != 0) {
			send_error(404, "Not Found", client_file, clientIP, get_request);
			continue;
		} else if(stat(newRequestURI, &sb) == 0 && S_ISDIR(sb.st_mode)) {
			send_error(403, "Forbidden", client_file, clientIP, get_request);
			continue;
		} 
		
		// from this point on, there should be no erroneous get requests
		char response_line[4096];
		snprintf(response_line, sizeof(response_line), "HTTP/1.0 %d %s \r\n\r\n", 200, "OK");
		fwrite(response_line, 1, strlen(response_line), client_file);
		if(strstr(requestURI, "/mdb-lookup") == requestURI) { // send mdb-lookup stuff here

			const char *form = 
				"<h1>mdb-lookup</h1>\n"
				"<p>\n"
				"<form method=GET action=/mdb-lookup>\n"
				"lookup: <input type=text name=key>\n"
				"<input type=submit>\n"
				"</form>\n"
				"<p>\n";

			const char *table_setup = "<p><table border>\n";
			
			// uri has no key, so send page with box that user can enter shit in
			fwrite(form, 1, strlen(form), client_file);

			if(strstr(requestURI, "?key=")) { // there is a key=..., return search results for that query

				char key[1024];
				strncpy(key, strstr(requestURI, "?key=") + 5, sizeof(key));
				int keyLength = strlen(key);
				key[keyLength] = '\n';
				key[keyLength + 1] = '\0';

				fwrite(key, 1, strlen(key), mdb_lookup_host_file);

				int i = 0; // keeps track of table entry number so that colors can be alternating
				char *color;
				char buf[1024]; // length will need to be smaller than html_buf
				char html_buf[4096];
				fwrite(table_setup, 1, strlen(table_setup), client_file);
				while(fgets(buf, sizeof(buf), mdb_lookup_host_file)) {
					if(strcmp(buf, "\n") == 0)
						break;
					if(i % 2 == 0)
						color = " bgcolor=yellow";
					else
						color = "";
					snprintf(html_buf, sizeof(html_buf), "<tr><td%s> %s\n", color, buf);
					fwrite(html_buf, 1, strlen(html_buf), client_file);
					i++;
				}

			}

		} else { // send requested webpage contents
			
			FILE *file_to_send = fopen(newRequestURI, "r");
			char buf[4096];
			int num_bytes_read;
			while((num_bytes_read = fread(buf, 1, sizeof(buf), file_to_send)))
				fwrite(buf, 1, num_bytes_read, client_file);
			fwrite(buf, 1, num_bytes_read, client_file);
			fclose(file_to_send);

		}
		
		fprintf(stdout, "%s \"%s\" %d %s\n", clientIP, get_request, 200, "OK");
		fflush(stdout);

		fclose(client_file);

	}

	// close mdb connection here right? well it won't exit the infinite loop anyways
	fclose(mdb_lookup_host_file);

}

void send_error(int error_code, char *reason_phrase, FILE *client_file, char *clientIP, char *get_request) {

	char response_line[4096];
	char response_html[4096];
	snprintf(response_line, sizeof(response_line), "HTTP/1.0 %d %s \r\n\r\n", error_code, reason_phrase);
	snprintf(response_html, sizeof(response_html), "<html><body><h1>%d %s</h1></body></html>", error_code, reason_phrase);
	fwrite(response_line, 1, strlen(response_line), client_file);
	fwrite(response_html, 1, strlen(response_html), client_file);
	fprintf(stdout, "%s \"%s\" %d %s\n", clientIP, get_request, error_code, reason_phrase);
	fclose(client_file);

}
