#include "http.h"
#include <winhttp.h>

/* ---- URLs ---- */

static bool copy_part(wchar_t *dst, size_t cap, const wchar_t *start, const wchar_t *end)
{
    size_t length = (size_t)(end - start);
    if (length >= cap) return false;
    memcpy(dst, start, length * sizeof(wchar_t));
    dst[length] = 0;
    return true;
}

static void lower_ascii(wchar_t *text)
{
    for (; *text; text++) if (*text >= L'A' && *text <= L'Z') *text += L'a' - L'A';
}

bool url_parse(const wchar_t *text, Url *out)
{
    memset(out, 0, sizeof(*out));
    for (const wchar_t *p = text; *p; p++)
        if (*p <= 0x20 || *p == 0x7F || *p == L'\\') return false;

    const wchar_t *separator = wcsstr(text, L"://");
    if (!separator || separator == text || !copy_part(out->scheme, COUNTOF(out->scheme), text, separator)) return false;
    for (const wchar_t *p = out->scheme; *p; p++)
        if (!((*p >= L'a' && *p <= L'z') || (*p >= L'A' && *p <= L'Z') ||
            (p > out->scheme && ((*p >= L'0' && *p <= L'9') || *p == L'+' || *p == L'-' || *p == L'.'))))
            return false;
    lower_ascii(out->scheme);

    const wchar_t *authority = separator + 3;
    const wchar_t *authority_end = authority + wcscspn(authority, L"/?#");
    const wchar_t *host = authority;
    for (const wchar_t *p = authority; p < authority_end; p++) if (*p == L'@') host = p + 1;
    if (host != authority)
    {
        if (host - 1 == authority || !copy_part(out->userinfo, COUNTOF(out->userinfo), authority, host - 1)) return false;
    }

    const wchar_t *host_end, *port = NULL;
    if (*host == L'[')
    {
        host_end = host;
        while (host_end < authority_end && *host_end != L']') host_end++;
        if (host_end == authority_end) return false;
        host_end++;
        if (host_end < authority_end) { if (*host_end != L':') return false; port = host_end + 1; }
    }
    else
    {
        host_end = host;
        while (host_end < authority_end && *host_end != L':') host_end++;
        if (host_end < authority_end) port = host_end + 1;
    }
    if (host_end == host || !copy_part(out->host, COUNTOF(out->host), host, host_end)) return false;
    lower_ascii(out->host);

    if (port)
    {
        if (port == authority_end || authority_end - port > 5) return false;
        int value = 0;
        for (const wchar_t *p = port; p < authority_end; p++)
        {
            if (*p < L'0' || *p > L'9') return false;
            value = value * 10 + (*p - L'0');
        }
        if (value < 1 || value > 65535) return false;
        out->port = value;
    }
    else out->port = wcscmp(out->scheme, L"https") == 0 ? 443 : wcscmp(out->scheme, L"http") == 0 ? 80 : 0;

    const wchar_t *path_end = authority_end + wcscspn(authority_end, L"?#");
    if (path_end == authority_end) str_copy(out->path, COUNTOF(out->path), L"/");
    else if (!copy_part(out->path, COUNTOF(out->path), authority_end, path_end)) return false;
    const wchar_t *query_end = path_end + wcscspn(path_end, L"#");
    if (!copy_part(out->query, COUNTOF(out->query), path_end, query_end)) return false;
    return copy_part(out->fragment, COUNTOF(out->fragment), query_end, query_end + wcslen(query_end));
}

bool url_is_http(const Url *url)
{
    return wcscmp(url->scheme, L"http") == 0 || wcscmp(url->scheme, L"https") == 0;
}

bool url_is_loopback(const Url *url)
{
    if (wcscmp(url->host, L"localhost") == 0 || wcscmp(url->host, L"loopback") == 0 || wcscmp(url->host, L"[::1]") == 0)
        return true;
    /* 127.0.0.0/8 written as four decimal parts. */
    int parts = 0, value = -1;
    for (const wchar_t *p = url->host;; p++)
    {
        if (*p >= L'0' && *p <= L'9')
        {
            value = (value < 0 ? 0 : value) * 10 + (*p - L'0');
            if (value > 255) return false;
        }
        else if (*p == L'.' || !*p)
        {
            if (value < 0 || (parts == 0 && value != 127)) return false;
            parts++;
            value = -1;
            if (!*p) break;
        }
        else return false;
    }
    return parts == 4;
}

void url_format(const Url *url, wchar_t *dst, size_t cap)
{
    bool default_port = (wcscmp(url->scheme, L"https") == 0 && url->port == 443) ||
        (wcscmp(url->scheme, L"http") == 0 && url->port == 80) || url->port == 0;
    if (default_port) str_fmt(dst, cap, L"%s://%s%s%s%s", url->scheme, url->host, url->path, url->query, url->fragment);
    else str_fmt(dst, cap, L"%s://%s:%d%s%s%s", url->scheme, url->host, url->port, url->path, url->query, url->fragment);
}

bool url_resolve(const Url *base, const wchar_t *reference, Url *out)
{
    wchar_t *combined = xalloc(sizeof(wchar_t) * URLN * 3);
    size_t cap = URLN * 3;
    wchar_t origin[400];
    Url root = *base;
    root.path[0] = root.query[0] = root.fragment[0] = 0;
    url_format(&root, origin, COUNTOF(origin));

    const wchar_t *scheme_end = wcsstr(reference, L"://");
    if (scheme_end && scheme_end - reference < 16 && wcscspn(reference, L"/?#") > (size_t)(scheme_end - reference))
        str_copy(combined, cap, reference);
    else if (reference[0] == L'/' && reference[1] == L'/') str_fmt(combined, cap, L"%s:%s", base->scheme, reference);
    else if (reference[0] == L'/') str_fmt(combined, cap, L"%s%s", origin, reference);
    else if (reference[0] == L'?' || reference[0] == L'#' || !reference[0])
        str_fmt(combined, cap, L"%s%s%s", origin, base->path, reference);
    else
    {
        wchar_t directory[URLN];
        str_copy(directory, COUNTOF(directory), base->path);
        wchar_t *slash = wcsrchr(directory, L'/');
        if (slash) slash[1] = 0;
        str_fmt(combined, cap, L"%s%s%s", origin, directory, reference);
    }
    bool ok = wcslen(combined) < URLN && url_parse(combined, out);
    free(combined);
    return ok;
}

void url_escape_data(const wchar_t *text, wchar_t *dst, size_t cap)
{
    char *utf8 = wide_to_utf8(text, NULL);
    size_t n = 0;
    if (cap) dst[0] = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && n + 4 < cap; p++)
    {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~')
            dst[n++] = *p;
        else
        {
            static const wchar_t hex[] = L"0123456789ABCDEF";
            dst[n++] = L'%';
            dst[n++] = hex[*p >> 4];
            dst[n++] = hex[*p & 15];
        }
        dst[n] = 0;
    }
    free(utf8);
}

/* ---- Requests ---- */

static HINTERNET session_for(bool direct)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static HINTERNET direct_session, proxy_session;
    AcquireSRWLockExclusive(&lock);
    HINTERNET *slot = direct ? &direct_session : &proxy_session;
    /* Addresses on this PC never go through a proxy. Everything else follows the Windows proxy settings. */
    if (!*slot)
        *slot = WinHttpOpen(L"MediaCenterUpdater/1.0",
            direct ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET session = *slot;
    ReleaseSRWLockExclusive(&lock);
    return session;
}

static HttpStatus map_error(DWORD error)
{
    switch (error)
    {
    case ERROR_WINHTTP_TIMEOUT: return HTTP_E_TIMEOUT;
    case ERROR_WINHTTP_CANNOT_CONNECT: return HTTP_E_CONNECT;
    default: return HTTP_E_NETWORK;
    }
}

HttpStatus http_send(void *context, const HttpRequest *request, HttpResponse *response)
{
    (void)context;
    memset(response, 0, sizeof(*response));
    Url *url = xalloc(sizeof(Url));
    wchar_t *object = xalloc(sizeof(wchar_t) * URLN * 2);
    char *buffer = xalloc(65536);
    HINTERNET connection = NULL, handle = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    HttpStatus result = HTTP_E_NETWORK;
    ULONGLONG started = GetTickCount64();
    DWORD timeout = request->timeout_ms ? request->timeout_ms : 30000;

    if (!url_parse(request->url, url) || !url_is_http(url) || url->userinfo[0]) goto done;
    HINTERNET session = session_for(url_is_loopback(url));
    if (!session) goto done;
    connection = WinHttpConnect(session, url->host, (INTERNET_PORT)url->port, 0);
    if (!connection) { result = map_error(GetLastError()); goto done; }
    str_fmt(object, URLN * 2, L"%s%s", url->path, url->query);
    handle = WinHttpOpenRequest(connection, request->method ? request->method : L"GET", object, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, wcscmp(url->scheme, L"https") == 0 ? WINHTTP_FLAG_SECURE : 0);
    if (!handle) { result = map_error(GetLastError()); goto done; }

    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    WinHttpSetTimeouts(handle, (int)timeout, (int)timeout, (int)timeout, (int)timeout);

    if (!WinHttpSendRequest(handle, request->headers ? request->headers : WINHTTP_NO_ADDITIONAL_HEADERS,
            request->headers ? (DWORD)-1 : 0, request->body ? (LPVOID)request->body : WINHTTP_NO_REQUEST_DATA,
            (DWORD)request->body_length, (DWORD)request->body_length, 0) ||
        !WinHttpReceiveResponse(handle, NULL))
    {
        result = map_error(GetLastError());
        goto done;
    }

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX))
        goto done;
    response->status = (int)status;
    size = sizeof(response->location);
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, response->location, &size,
            WINHTTP_NO_HEADER_INDEX))
        response->location[0] = 0;

    bool save = request->save_path && status >= 200 && status < 300;
    uint64_t limit = request->max_bytes ? request->max_bytes : (save ? UINT64_MAX : 32u * 1024 * 1024);
    wchar_t length_text[32];
    size = sizeof(length_text);
    if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &size,
            WINHTTP_NO_HEADER_INDEX) && _wcstoui64(length_text, NULL, 10) > limit)
    {
        result = HTTP_E_TOO_LARGE;
        goto done;
    }
    if (save)
    {
        file = CreateFileW(request->save_path, GENERIC_WRITE, 0, NULL,
            request->save_create_new ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) { result = HTTP_E_FILE; goto done; }
    }

    size_t capacity = 0;
    uint64_t total = 0;
    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        if ((request->deadline && now >= request->deadline) || (!save && now - started > timeout))
        {
            result = HTTP_E_TIMEOUT;
            goto done;
        }
        DWORD read = 0;
        if (!WinHttpReadData(handle, buffer, 65536, &read)) { result = map_error(GetLastError()); goto done; }
        if (!read) break;
        total += read;
        if (total > limit) { result = HTTP_E_TOO_LARGE; goto done; }
        if (save)
        {
            DWORD written = 0;
            if (!WriteFile(file, buffer, read, &written, NULL) || written != read) { result = HTTP_E_FILE; goto done; }
        }
        else
        {
            if (response->body_length + read + 1 > capacity)
            {
                capacity = capacity ? capacity * 2 : 65536;
                while (response->body_length + read + 1 > capacity) capacity *= 2;
                response->body = xrealloc(response->body, capacity);
            }
            memcpy(response->body + response->body_length, buffer, read);
            response->body_length += read;
        }
    }
    if (!save)
    {
        if (!response->body) response->body = xalloc(1);
        response->body[response->body_length] = 0;
    }
    response->saved_bytes = save ? total : 0;
    result = HTTP_OK;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (handle) WinHttpCloseHandle(handle);
    if (connection) WinHttpCloseHandle(connection);
    if (result != HTTP_OK) http_response_free(response);
    free(buffer);
    free(object);
    free(url);
    return result;
}

void http_response_free(HttpResponse *response)
{
    free(response->body);
    response->body = NULL;
    response->body_length = 0;
}

bool http_check(HttpStatus status, Err *err)
{
    switch (status)
    {
    case HTTP_OK: return true;
    case HTTP_E_CONNECT: return err_set(err, ERR_CONNECT, L"The server could not be reached.");
    case HTTP_E_TIMEOUT: return err_set(err, ERR_TIMEOUT, L"The request timed out.");
    case HTTP_E_NETWORK: return err_set(err, ERR_NETWORK, L"The server could not be reached.");
    default: return err_set(err, ERR_OTHER, L"The response could not be saved.");
    }
}
