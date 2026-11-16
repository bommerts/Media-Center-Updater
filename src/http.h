/* URL handling and HTTP requests. Requests never follow redirects by themselves. */
#ifndef MCU_HTTP_H
#define MCU_HTTP_H

#include "common.h"

typedef struct Url
{
    wchar_t scheme[16];         /* lower case */
    wchar_t userinfo[256];
    wchar_t host[256];          /* lower case, IPv6 keeps its brackets */
    int port;
    wchar_t path[URLN];         /* always starts with '/' */
    wchar_t query[URLN];        /* includes the '?', or empty */
    wchar_t fragment[URLN];     /* includes the '#', or empty */
} Url;

bool url_parse(const wchar_t *text, Url *out);
bool url_is_http(const Url *url);                       /* http or https */
bool url_is_loopback(const Url *url);
void url_format(const Url *url, wchar_t *dst, size_t cap);
bool url_resolve(const Url *base, const wchar_t *reference, Url *out);
void url_escape_data(const wchar_t *text, wchar_t *dst, size_t cap);

typedef struct HttpRequest
{
    const wchar_t *method;          /* NULL means GET */
    const wchar_t *url;
    const wchar_t *headers;         /* "Name: value\r\n" lines, or NULL */
    const char *body;               /* request body, or NULL */
    size_t body_length;
    DWORD timeout_ms;               /* time allowed for the response (and for a body kept in memory) */
    const wchar_t *save_path;       /* when set, a successful response body is written to this file */
    bool save_create_new;           /* fail instead of replacing an existing file */
    uint64_t max_bytes;             /* largest body accepted; 0 uses a default for bodies kept in memory */
    ULONGLONG deadline;             /* GetTickCount64 value after which the transfer stops, or 0 */
} HttpRequest;

typedef struct HttpResponse
{
    int status;
    wchar_t location[URLN];         /* the Location header, or empty */
    char *body;                     /* NUL terminated; free with http_response_free */
    size_t body_length;
    uint64_t saved_bytes;
} HttpResponse;

typedef enum { HTTP_OK, HTTP_E_CONNECT, HTTP_E_TIMEOUT, HTTP_E_TOO_LARGE, HTTP_E_NETWORK, HTTP_E_FILE } HttpStatus;

/* Tests replace this with a function that returns canned responses. */
typedef HttpStatus (*HttpSendFn)(void *context, const HttpRequest *request, HttpResponse *response);

HttpStatus http_send(void *context, const HttpRequest *request, HttpResponse *response);
void http_response_free(HttpResponse *response);
/* Turns a transport failure into an error. Returns true when the status is HTTP_OK. */
bool http_check(HttpStatus status, Err *err);

#endif
