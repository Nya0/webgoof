#include "http.h"

#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

http_method_t parse_method(const char *method_str) {
    if (strcmp(method_str, "GET") == 0) {return HTTP_GET;}
    if (strcmp(method_str, "POST") == 0) {return HTTP_POST;}
    if (strcmp(method_str, "PUT") == 0) {return HTTP_PUT;}
    if (strcmp(method_str, "DELETE") == 0) {return HTTP_DELETE;}
    return HTTP_UNKNOWN;
}

int parse_request(char* raw_request, struct http_request *request) {
    char *line_saveptr;
    char *term_saveptr;

    char *line = strtok_r(raw_request, "\r\n", &line_saveptr);
    if (!line) {return -1;}

    // parse request protocol line (GET / HTTP/1.1)
    // 1. method
    char *term = strtok_r(line, " ", &term_saveptr);
    if (!term) {return -1;}
    request->method = parse_method(term);

    // 2. path
    term = strtok_r(NULL, " ", &term_saveptr);
    if (!term) {return -1;}
    strncpy(request->path, term, sizeof(request->path) - 1);
    request->path[sizeof(request->path) - 1] = '\0';

    // 3. version
    term = strtok_r(NULL, " ", &term_saveptr);
    if (!term) {return -1;}
    request->version = 1;  // TODO: actually parse "HTTP/1.1"

    // parse headers
    line = strtok_r(NULL, "\r\n", &line_saveptr);

    int header_count = 0;
    while (line != NULL && header_count < 16) { // NOLINT
        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *key = line;
            char *value = colon + 1;

            while (*value == ' ' || *value == '\t') {value++;}

            strncpy(request->headers[header_count].key, key,
                    sizeof(request->headers[header_count].key) - 1);
            request->headers[header_count].key[sizeof(request->headers[header_count].key) - 1] = '\0';

            strncpy(request->headers[header_count].value, value,
                    sizeof(request->headers[header_count].value) - 1);
            request->headers[header_count].value[sizeof(request->headers[header_count].value) - 1] = '\0';

            header_count++;
        }

        line = strtok_r(NULL, "\r\n", &line_saveptr);
    }

    request->header_count = header_count;
    return 0;  // success
}

int add_header(struct http_response *response, const char *key, const char *value) {
    if ((size_t)response->header_count >= HTTP_MAX_HEADERS) {
        perror("headers full");
        return -1;
    }

    int index = response->header_count;

    strncpy(response->headers[index].key, key,
            sizeof(response->headers[index].key) - 1);
    response->headers[index].key[sizeof(response->headers[index].key) - 1] = '\0';

    strncpy(response->headers[index].value, value,
            sizeof(response->headers[index].value) - 1);
    response->headers[index].value[sizeof(response->headers[index].value) - 1] = '\0';

    response->header_count++;
    return 1;
}

char* serialize_response(struct http_response *response, size_t *out_len) {
    char header_buffer[8192]; // NOLINT
    size_t header_len = 0;

    // status
    header_len += sprintf(header_buffer + header_len, "HTTP/1.0 %d %s\r\n",
                    response->status_code, response->status_text);
    

    // headers
    for (int i = 0; i < response->header_count; i++) {
        header_len += sprintf(header_buffer + header_len, "%s: %s\r\n",
                    response->headers[i].key, 
                    response->headers[i].value);
    }

    // indicate end of header
    header_len += sprintf(header_buffer + header_len, "\r\n");

    // combine -- memcpy cuz string operations stop at null byte
    size_t total_size = header_len + response->body_length;
    char *result = malloc(total_size); // NO CALLOC CUZ WE USE EVERYTHING
    if (!result) {return NULL;}
    
    memcpy(result, header_buffer, header_len);
    
    if (response->body && response->body_length > 0) {
        memcpy(result + header_len, response->body, response->body_length);
    }

    // blah set len
    *out_len = total_size;
    return result;
}

const char* get_content_type(const char *file_path) {
    const char *ext = strrchr(file_path, '.');


    // oh my wow so bad
    if (ext) {
        for (int i = 0; CONTENT_TYPES[i].ext != NULL; i++) {
            if (strcasecmp(ext, CONTENT_TYPES[i].ext) == 0) {
                return CONTENT_TYPES[i].mime;
            }
        }
    }

    return "application/octet-stream";
}

const char* get_status_text(int code) {
    switch (code) {
        case HTTP_STATUS_OK: return "OK"; 
        case HTTP_STATUS_NOT_FOUND: return "Not Found"; 
        case HTTP_STATUS_FORBIDDEN: return "Forbidden"; 
        case HTTP_STATUS_INTERNAL_ERROR: return "Internal Server Error";
        default: return "";
    }
};