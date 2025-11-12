#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADERS 32

#define HTTP_STATUS_OK 200
#define HTTP_STATUS_NOT_FOUND 404
#define HTTP_STATUS_FORBIDDEN 403
#define HTTP_STATUS_INTERNAL_ERROR 500


typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_UNKNOWN
} http_method_t;

static const struct content_type_map {
    const char *ext;
    const char *mime;
} CONTENT_TYPES[] = {
    {".html", "text/html"},
    {".css",  "text/css"},

    {NULL, NULL}
};

struct header {
    char key[32];
    char value[128];
};

struct http_request {
    unsigned int version;
    http_method_t method;
    char path[128];
    struct header headers[HTTP_MAX_HEADERS];
    int header_count;
    char *body;
};

struct http_response {
    int status_code;
    char status_text[32];
    struct header headers[HTTP_MAX_HEADERS];
    int header_count;
    char *body;
    size_t body_length;
};

int parse_request(char *raw_request, struct http_request *request);
http_method_t parse_method(const char *method_str);
int add_header(struct http_response *response, const char *key, const char *value);
char* serialize_response(struct http_response *response, size_t *out_len);
void free_response(char *serialized);

const char* get_content_type(const char *file_path);
const char* get_status_text(int code);

#endif // HTTP_H