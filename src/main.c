#include "http.h"

#include <asm-generic/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#define PORT 3030
#define QUEUE_SIZE 10
#define WEB_ROOT "./public"

#define CLIENT_REQ_SIZE 1024*4

int listen_and_serve(int port) {
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return -1;
	}

	// option to allow us to immediately reuse port by bypassing the wait period
	int opt = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in server_addr = {0};
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
		perror("bind failed");
		return -1;
	}

	if (listen(sockfd, QUEUE_SIZE) < 0) {
		perror("listen failed");
		close(sockfd);
		return -1;
	}

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);

		char client_ip[INET_ADDRSTRLEN] = {0};
		inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

		int client_port = ntohs(client_addr.sin_port);

		char raw_request[CLIENT_REQ_SIZE] = {0};
		read(clientfd, raw_request, (size_t)CLIENT_REQ_SIZE - 1);

		struct http_request request;
		parse_request(raw_request, &request);
		printf("\nnew request!! %s:%d (%s)\n", client_ip, client_port, request.path);
		for (int i = 0; i < request.header_count; i++) {
			printf("%s = %s\n", request.headers[i].key, request.headers[i].value);
		}


		// parse file path
		struct http_response response = {0};
		char* file_content = NULL;

		if (strstr(request.path, "..") != NULL) { // path traversal temp fix i think
			printf("woah dude bad bad path traversal\n");

			response.status_code = HTTP_STATUS_FORBIDDEN;
			strcpy(response.status_text, "FORBIDDEN");
			goto finish;
		}

		char file_path[256]; // NOLINT
		if (strcmp(request.path, "/") == 0) {
        	snprintf(file_path, sizeof(file_path), "%s/index.html", WEB_ROOT);
      	} else {
        	snprintf(file_path, sizeof(file_path), "%s%s", WEB_ROOT, request.path);
		} 

		// get file content
		int r_fd = open(file_path, O_RDONLY);
		if (r_fd < 0) {
			perror("open file");

			response.status_code = HTTP_STATUS_NOT_FOUND;
			goto finish;
		}

		struct stat file_stat;
  		fstat(r_fd, &file_stat);
		long file_size = file_stat.st_size;

		file_content = calloc(1, file_size);
		read(r_fd, file_content, file_size);
		close(r_fd);

		// fill response
		response.status_code = HTTP_STATUS_OK;

		response.body = file_content;
		response.body_length = file_size;

		const char *content_type = get_content_type(file_path);
		add_header(&response, "Content-Type", content_type);
		
		size_t ser_len;
		
finish:
		strcpy(response.status_text, get_status_text(response.status_code));
		char *serialized = serialize_response(&response, &ser_len);

		write(clientfd, serialized, ser_len);
		free(file_content);
		free(serialized);

		close(clientfd);
	};

	close(sockfd);
	return 0;
}

int main(void) {
	if(listen_and_serve(PORT) < 0) {
		perror("bruh");
		exit(1);
	}
	
	return 0;
}
