#include "cache.h"
#include "http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT "3030"
#define DEFAULT_WEB_ROOT "./public"
#define DEFAULT_THREAD_COUNT "4"
#define DEFAULT_VERBOSITY "4" // LOG_DEBUG

#define QUEUE_SIZE 4096
#define CLIENT_REQ_SIZE 1024 * 4

struct worker {
	int listen_fd;
	int epfd;
	char *web_root;
};

int handle_client(struct http_client *client) {

	//-- get ip
	char client_ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &client->addr.sin_addr, client_ip, sizeof(client_ip));
	in_port_t client_port = ntohs(client->addr.sin_port);

	//-- reading request
	char raw_request[CLIENT_REQ_SIZE];

	// ssize_t total_read = 0;
	// for (;;) {
	// 	ssize_t n = read(client->fd, raw_request + total_read, (size_t)CLIENT_REQ_SIZE - 1 - total_read);
	// 	LOG(LOG_ERROR, "total read: %u", total_read);
	// 	LOG(LOG_ERROR, "n: %d", n);
	// 	if (n > 0) {
	// 		total_read += n;
	// 		if (total_read >= CLIENT_REQ_SIZE - 1)
	// 			break;
	// 		continue;
	// 	}

	// 	if (n == 0) {
	// 		goto cleanup;
	// 	}
	// 	if (n == -1) {
	// 		LOG(LOG_ERROR, "read error: %d", n);
	// 		if (errno == EAGAIN || errno == EWOULDBLOCK)
	// 			break;    // drained
	// 		goto cleanup; // real error
	// 	};
	// }

	ssize_t n = read(client->fd, raw_request, (size_t)CLIENT_REQ_SIZE - 1);
	if (n == 0)
		return -1; // EOF client closed
	if (n == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0; 
		return -1;   
	}

	raw_request[n] = '\0'; // null-terminate string

	//-- parsing raw req
	struct http_request request = {0}; // zero init so logging is safe even if parse fails
	struct http_response response = {0};

	int file_fd = -1;
	char *file_content = NULL;
	off_t file_size = 0;
	bool large_file = false;

	if (parse_request(raw_request, &request) == -1) {
		response.status_code = HTTP_STATUS_BAD_REQUEST;
		goto finish;
	}

	LOG(LOG_INFO, "%s %s (%s:%d)", http_method_str(request.method), request.path, client_ip, client_port);
	for (int i = 0; i < request.header_count; i++) {
		LOG(LOG_DEBUG, "  %s: %s", request.headers[i].key, request.headers[i].value);
	}

	if (strstr(request.path, "..") != NULL) { // path traversal temp fix
		LOG(LOG_WARN, "blocked path traversal attempt: %s (%s:%d)", request.path, client_ip, client_port);

		response.status_code = HTTP_STATUS_FORBIDDEN;
		goto finish;
	}

	char file_path[256]; // NOLINT
	if (strcmp(request.path, "/") == 0) {
		snprintf(file_path, sizeof(file_path), "%s/index.html", client->web_root);
	} else {
		snprintf(file_path, sizeof(file_path), "%s%s", client->web_root, request.path);
	}

	//-- handling request
	struct cached_file *file_hit = cache_lookup(file_path);
	LOG(LOG_DEBUG, "file pointer: %p", file_hit);
	if (file_hit == NULL) { // cache miss
		LOG(LOG_WARN, "cache miss: %s", file_path);

		file_fd = open(file_path, O_RDONLY);
		if (file_fd == -1) {
			LOG(LOG_WARN, "open %s: %s", file_path, strerror(errno));
			response.status_code = HTTP_STATUS_NOT_FOUND;
			goto finish;
		}

		// read size
		struct stat file_stat;
		if (fstat(file_fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode)) {
			close(file_fd);
			response.status_code = HTTP_STATUS_NOT_FOUND;
			goto finish;
		}
		file_size = file_stat.st_size;

		// if file bigger than 8mb mark it and skip reading
		if (file_size > (1 << 23)) { // 2^23 (8mb)
			large_file = true;
			LOG(LOG_DEBUG, "(%s) using large_file mode", file_path);
			goto filler;
		}

		file_content = malloc(file_size);
		if (file_content == NULL) {
			LOG(LOG_ERROR, "failed to allocate memory for %s: %s", file_path, strerror(errno));
			close(file_fd);
			response.status_code = HTTP_STATUS_INTERNAL_ERROR;
			goto finish;
		}

		off_t total = 0;
		while (total < file_size) {
			ssize_t r = read(file_fd, file_content + total, file_size - total);
			if (r == 0)
				break;

			if (r == -1) {
				if (errno == EINTR)
					continue;
				break;
			}
			total += r;
		}
		close(file_fd);

		if (total != file_size) {
			LOG(LOG_ERROR, "short read %s: %lld/%lld", file_path, (long long)total, (long long)file_size);
			free(file_content);
			file_content = NULL;
			response.status_code = HTTP_STATUS_INTERNAL_ERROR;
			goto finish;
		}

		if (cache_insert(file_path, file_content, file_size) == -1) {
			LOG(LOG_ERROR, "cache insert failed: %s", file_path);
			free(file_content);
			file_content = NULL;
			response.status_code = HTTP_STATUS_INTERNAL_ERROR;
			goto finish;
		}
	} else { // cache hit
		file_content = file_hit->content;
		file_size = file_hit->size;
	}

filler:
	response.status_code = HTTP_STATUS_OK;

	response.body = file_content;
	response.body_length = file_size;

	const char *content_type = get_content_type(file_path);
	add_header(&response, "Content-Type", content_type);

	char lenbuf[32];
	snprintf(lenbuf, sizeof lenbuf, "%lld", (long long)response.body_length);
	add_header(&response, "Content-Length", lenbuf);
	add_header(&response, "Connection", "keep-alive");

finish: {

	// write status text from status code
	strcpy(response.status_text, get_status_text(response.status_code));

	// serialize and send response
	size_t ser_len;
	char *serialized = serialize_response_header(&response, &ser_len);
	if (serialized) {
		struct iovec iov[2];
		int iovcnt = 0;

		iov[iovcnt++] = (struct iovec){.iov_base = serialized, .iov_len = ser_len};

		if (file_content != NULL) { // there is content (alt is large file path)
			iov[iovcnt++] = (struct iovec){.iov_base = file_content, .iov_len = file_size};
		}

		ssize_t w = writev(client->fd, iov, iovcnt);
		LOG(LOG_DEBUG, "written %li", w);

		//-- handle large files
		if (large_file) {
			off_t offset = 0;
			for (;;) {

				ssize_t n = sendfile(client->fd, file_fd, &offset, file_size);
				if (n > 0) {
					if (offset >= file_size)
						break;
					continue;
				}

				if (n == -1) {
					if (errno == EAGAIN) {
						struct pollfd p = {.fd = client->fd, .events = POLLOUT};
						poll(&p, 1, -1); // wait so we can send the rest of the file
						continue;
					};
					if (errno == EINTR)
						continue;
					break; // real error
				};
			}
			close(file_fd);
		}

		free(serialized);
	}
}

cleanup: {
	// close(client->fd);
	// free(client);
	LOG(LOG_DEBUG, "finished with client");
	return 0;
}
}

int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void accept_all(struct worker *w) {
	for (;;) {

		struct http_client *client = malloc(sizeof(struct http_client)); // TODO: syscalls - zero-copy (maybe fine if we switch to keep-alive someday)
		socklen_t addr_len = sizeof(client->addr);
		client->web_root = w->web_root;
		client->fd = accept4(w->listen_fd, (struct sockaddr *)&client->addr, &addr_len, SOCK_NONBLOCK);
		if (client->fd == -1) {
			free(client);

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break; // queue drained
			if (errno == EINTR)
				continue; // interrupted mid accept, retry
			break;
		}

		struct epoll_event ev;
		ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
		ev.data.ptr = client;

		epoll_ctl(w->epfd, EPOLL_CTL_ADD, client->fd, &ev);
	}
}

void *worker_thread(void *arg) {
	struct worker *w = arg;
	struct epoll_event events[64];

	struct epoll_event lev = {.events = EPOLLIN, .data.ptr = NULL}; // NULL tags "it's the listener"
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &lev);

	while (1) {
		int n = epoll_wait(w->epfd, events, 64, -1);
		for (int i = 0; i < n; i++) {
			if (events[i].data.ptr == NULL) {
				accept_all(w);
			} else {
				struct http_client *client = (struct http_client *)events[i].data.ptr;

				struct epoll_event ev;
				ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
				ev.data.ptr = client;

				if (handle_client(client) == -1) {
					close(client->fd);
					free(client);
					LOG(LOG_DEBUG, "client disconnected");
					continue;
				}

				epoll_ctl(w->epfd, EPOLL_CTL_MOD, client->fd, &ev);
			}
		}
	}
}

int make_listener(in_port_t port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		LOG(LOG_ERROR, "create socket: %s", port, strerror(errno));
		return fd;
	}

	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt);

	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = INADDR_ANY;
	a.sin_port = htons(port);

	set_nonblocking(fd);

	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1) {
		LOG(LOG_ERROR, "bind to port %d: %s", port, strerror(errno));
		return -1;
	}

	if (listen(fd, QUEUE_SIZE) == -1) {
		LOG(LOG_ERROR, "listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int listen_and_serve(in_port_t port, char *web_root, int thread_count) {
	signal(SIGPIPE, SIG_IGN); // stop a broken client from closing the server

	for (int i = 0; i < thread_count; i++) {
		pthread_t thread;

		struct worker *w = calloc(1, sizeof *w);
		w->epfd = epoll_create1(0);
		w->listen_fd = make_listener(port);
		if (w->listen_fd == -1) {
			LOG(LOG_ERROR, "thread (%d) socket: %s", i, strerror(errno));
			return -1;
		}
		w->web_root = web_root;

		if (pthread_create(&thread, NULL, worker_thread, w) == -1) {
			LOG(LOG_DEBUG, "thread (%d) creation: %s", i, strerror(errno));
			exit(1);
		}
		pthread_detach(thread);

		LOG(LOG_DEBUG, "thread (%d) ready", i);
	}

	LOG(LOG_INFO, "listening on http://localhost:%d (serving %s with %d threads)", port, web_root, thread_count);
	pause();

	return 0;
}

typedef struct options {
	char *port;
	char *web_root;
	char *thread_count;
	char *verbosity;
} options;

options parse_args(int argc, char *argv[]) {
	options opts = {0};
	int c;

	opts.port = DEFAULT_PORT;
	opts.web_root = DEFAULT_WEB_ROOT;
	opts.verbosity = DEFAULT_VERBOSITY;

	while ((c = getopt(argc, argv, "hp:w:t:v:")) != -1) {
		switch (c) {
		case 'p':
			opts.port = optarg;
			break;
		case 'w':
			opts.web_root = optarg;
			break;
		case 't':
			opts.thread_count = optarg;
			break;
		case 'v':
			opts.verbosity = optarg;
			break;
		case 'h':
			printf("Usage: %s [-w web_root] [-p port] [-t threads] [-v level]\n", argv[0]);
			printf("  -w web_root  Web root directory (default: %s)\n", DEFAULT_WEB_ROOT);
			printf("  -p port      Server port (default: %s)\n", DEFAULT_PORT);
			printf("  -t threads   Thread Count (default: number of online CPUs)\n");
			printf("  -v           Verbosity level (default: %s) (0:NONE, 1:ERROR, 2:WARN, 3:INFO, 4:DEBUG)\n", DEFAULT_VERBOSITY);
			printf("  -h           Show this help\n");
			exit(EXIT_SUCCESS);
		case '?':
		default:
			fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	return opts;
}

int main(int argc, char *argv[]) {
	log_init(stderr);
	cache_init(100);

	options opts = parse_args(argc, argv);

	printf("\033[31m"); // red color for error area

	struct rlimit limit = {0};
	if (getrlimit(RLIMIT_NPROC, &limit) == -1) {
		printf("failed to get system thread limit");
		exit(1);
	}

	int port_input = atoi(opts.port);
	if (port_input < 1 || port_input > 65535) {
		printf("invalid port: %s\n", opts.port);
		fflush(stderr);
		exit(1);
	}
	in_port_t port_num = (in_port_t)port_input;

	// default to online CPU count
	uint16_t thread_count;
	if (opts.thread_count != NULL) {
		thread_count = atoi(opts.thread_count);
	} else {
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);
		thread_count = (cpus > 0) ? (uint16_t)cpus : (uint16_t)atoi(DEFAULT_THREAD_COUNT);
	}
	if (thread_count < 1 || thread_count > limit.rlim_cur) {
		printf("thread range 1-%lu\n", limit.rlim_cur);
		exit(1);
	}

	int verbosity = atoi(opts.verbosity);
	if (verbosity < LOG_NONE || verbosity > LOG_DEBUG) {
		printf("log level must be within range (0:NONE, 1:ERROR, 2:WARN, 3:INFO, 4:DEBUG)\n");
		exit(1);
	}
	log_set_verbosity((log_level_t)verbosity);
	printf("LOG_LEVEL = %i\n", LOG_LEVEL);

	if (access(opts.web_root, R_OK | X_OK) == -1) { // list dir | open files inside
		printf("web_root not accessible: %s\n", opts.web_root);
		exit(1);
	}

	printf("\033[0m"); // reset term color

	if (listen_and_serve(port_num, opts.web_root, thread_count) == -1) {
		printf("server failed to start on port %u\n", port_num);
		exit(1);
	}

	return 0;
}
