#include "../../extlib/mongoose/mongoose.h"
#include "http_wrapper.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Compile-time limits ───────────────────────────────────────────────────── */

#define STDHTTP_MAX_ROUTES      64
#define STDHTTP_MAX_MIDDLEWARES  8
#define STDHTTP_MAX_PARAMS      16
#define STDHTTP_MAX_HEADERS     16

/* Size of the buffer used to accumulate extra response headers.
 * Each "Name: Value\r\n" line is appended here. */
#define STDHTTP_EXTRA_HEADERS_BUF 2048

/* Thread-local scratch buffers used by the request accessor helpers. */
#define STDHTTP_SCRATCH_BUF 1024

/* ── Internal structs ──────────────────────────────────────────────────────── */

typedef struct {
    char              method[16];
    char              pattern[256];
    stdhttp_handler_fn fn;
} stdhttp_route_t;

typedef struct stdhttp_server {
    struct mg_mgr         mgr;
    int                   port;
    volatile int          running;
    stdhttp_route_t       routes[STDHTTP_MAX_ROUTES];
    int                   route_count;
    stdhttp_middleware_fn middlewares[STDHTTP_MAX_MIDDLEWARES];
    int                   middleware_count;
} stdhttp_server_t;

typedef struct {
    struct mg_connection  *conn;
    struct mg_http_message *hm;
    char   method[16];
    char   path[512];
    char   query[512];
    /* Route parameters extracted during pattern matching */
    char   param_names[STDHTTP_MAX_PARAMS][64];
    char   param_vals [STDHTTP_MAX_PARAMS][256];
    int    param_count;
    /* Null-terminated copy of the body (lazily built) */
    char  *body_buf;
    int    body_len;
} stdhttp_request_t;

typedef struct {
    struct mg_connection  *conn;
    struct mg_http_message *hm;   /* kept for mg_http_serve_file */
    int    status;
    char   extra_headers[STDHTTP_EXTRA_HEADERS_BUF];
    int    sent;
    /* Whether the caller set an explicit Content-Type via stdhttp_res_header */
    int    content_type_set;
} stdhttp_response_t;

/* ── Helpers ───────────────────────────────────────────────────────────────── */

/* Minimal URL decoder: decodes %XX sequences and '+' → ' ' in-place.
 * src and dst may alias.  dst must be at least strlen(src)+1 bytes. */
static void url_decode(const char *src, size_t src_len, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (size_t i = 0; i < src_len && j + 1 < dst_sz; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            char *end;
            long  val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                dst[j++] = (char)val;
                i += 2;
                continue;
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            continue;
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ── Route matching ────────────────────────────────────────────────────────── */

/* Split 'str' (length 'len') on '/' into at most 'max' segments.
 * Returns the number of segments found.  Segments are null-terminated
 * strings stored in segs[]; the backing memory is in buf[]. */
static int split_path(const char *str, size_t len,
                      char segs[][256], int max,
                      char buf[], size_t buf_sz) {
    /* Copy into buf first so we can null-terminate segments */
    size_t copy_len = len < buf_sz - 1 ? len : buf_sz - 1;
    memcpy(buf, str, copy_len);
    buf[copy_len] = '\0';

    int    count = 0;
    char  *p     = buf;
    /* Skip a leading '/' */
    if (*p == '/') p++;

    while (*p && count < max) {
        segs[count++][0] = '\0'; /* will be filled below */
        char *seg_start   = p;
        char *slash       = strchr(p, '/');
        size_t seg_len;
        if (slash) {
            seg_len = (size_t)(slash - seg_start);
            p       = slash + 1;
        } else {
            seg_len = strlen(seg_start);
            p      += seg_len;
        }
        size_t copy = seg_len < 255 ? seg_len : 255;
        memcpy(segs[count - 1], seg_start, copy);
        segs[count - 1][copy] = '\0';
    }
    return count;
}

/* Try to match 'pattern' against 'path'.
 * If successful, extracted :param names and their values are written into
 * names[][]/vals[][] and *param_count is set.
 * Returns 1 on match, 0 on no match. */
static int match_route(const char *pattern,
                       const char *path,
                       char        names[][64],
                       char        vals[][256],
                       int        *param_count) {
    char pat_buf[1024], pth_buf[1024];
    char pat_segs[32][256], pth_segs[32][256];

    int npat = split_path(pattern, strlen(pattern),
                          pat_segs, 32, pat_buf, sizeof pat_buf);
    int npth = split_path(path,    strlen(path),
                          pth_segs, 32, pth_buf, sizeof pth_buf);

    if (npat != npth) return 0;

    *param_count = 0;

    for (int i = 0; i < npat; i++) {
        if (pat_segs[i][0] == ':') {
            /* Param segment — capture value */
            if (*param_count < STDHTTP_MAX_PARAMS) {
                size_t nlen = strlen(pat_segs[i] + 1);
                size_t vlen = strlen(pth_segs[i]);
                if (nlen > 63)  nlen = 63;
                if (vlen > 255) vlen = 255;
                memcpy(names[*param_count], pat_segs[i] + 1, nlen);
                names[*param_count][nlen] = '\0';
                memcpy(vals[*param_count],  pth_segs[i],     vlen);
                vals[*param_count][vlen]  = '\0';
                (*param_count)++;
            }
        } else if (strcmp(pat_segs[i], pth_segs[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ── Response helpers ──────────────────────────────────────────────────────── */

/* Append a "Name: Value\r\n" header line to the extra_headers buffer.
 * Silently truncates if the buffer would overflow. */
static void append_extra_header(stdhttp_response_t *res,
                                const char         *name,
                                const char         *val) {
    size_t used = strlen(res->extra_headers);
    size_t avail = sizeof(res->extra_headers) - used - 1;
    /* Format: "Name: Value\r\n" */
    size_t needed = strlen(name) + 2 + strlen(val) + 2; /* ": " + "\r\n" */
    if (needed > avail) return; /* silently ignore overflow */
    snprintf(res->extra_headers + used, avail + 1,
             "%s: %s\r\n", name, val);
}

/* Check whether the caller has already set a Content-Type header. */
static int has_content_type(const stdhttp_response_t *res) {
    return res->content_type_set;
}

/* ── Mongoose event handler ────────────────────────────────────────────────── */

static void mg_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    stdhttp_server_t *srv = (stdhttp_server_t *)c->fn_data;
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    /* ── Build request context ── */
    stdhttp_request_t req;
    memset(&req, 0, sizeof req);
    req.conn = c;
    req.hm   = hm;

    /* Method */
    size_t mlen = hm->method.len < 15 ? hm->method.len : 15;
    memcpy(req.method, hm->method.buf, mlen);
    req.method[mlen] = '\0';

    /* Path (URI without query string) */
    {
        const char *uri = hm->uri.buf;
        size_t      uri_len = hm->uri.len;
        const char *qmark = memchr(uri, '?', uri_len);
        size_t      path_len = qmark ? (size_t)(qmark - uri) : uri_len;
        if (path_len >= sizeof req.path) path_len = sizeof req.path - 1;
        memcpy(req.path, uri, path_len);
        req.path[path_len] = '\0';
    }

    /* Query string */
    {
        size_t qlen = hm->query.len < sizeof(req.query) - 1
                    ? hm->query.len : sizeof(req.query) - 1;
        memcpy(req.query, hm->query.buf, qlen);
        req.query[qlen] = '\0';
    }

    /* Body — make a null-terminated copy */
    req.body_len = (int)hm->body.len;
    req.body_buf = NULL;
    if (req.body_len > 0) {
        req.body_buf = (char *)malloc((size_t)req.body_len + 1);
        if (req.body_buf) {
            memcpy(req.body_buf, hm->body.buf, (size_t)req.body_len);
            req.body_buf[req.body_len] = '\0';
        }
    }

    /* ── Build response context ── */
    stdhttp_response_t res;
    memset(&res, 0, sizeof res);
    res.conn   = c;
    res.hm     = hm;
    res.status = 200;

    /* ── Run middlewares ── */
    for (int i = 0; i < srv->middleware_count; i++) {
        if (res.sent) break;
        int stop = srv->middlewares[i]((void *)&req, (void *)&res);
        if (stop) goto cleanup;
    }

    /* ── Route dispatch ── */
    if (!res.sent) {
        int matched = 0;
        for (int i = 0; i < srv->route_count; i++) {
            stdhttp_route_t *rt = &srv->routes[i];

            /* Method check */
            int method_ok = (strcmp(rt->method, "ANY") == 0)
                         || (strcmp(rt->method, req.method) == 0);
            if (!method_ok) continue;

            /* Pattern match */
            char   names[STDHTTP_MAX_PARAMS][64];
            char   vals [STDHTTP_MAX_PARAMS][256];
            int    pc   = 0;
            if (!match_route(rt->pattern, req.path, names, vals, &pc)) continue;

            /* Populate params */
            req.param_count = 0;
            for (int p = 0; p < pc; p++) {
                stdhttp_req_set_param((void *)&req, names[p], vals[p]);
            }

            matched = 1;
            rt->fn((void *)&req, (void *)&res);
            break;
        }

        if (!matched && !res.sent) {
            mg_http_reply(c, 404, "", "Not Found\n");
            res.sent = 1;
        }
    }

cleanup:
    if (req.body_buf) free(req.body_buf);
}

/* ── Server lifecycle ──────────────────────────────────────────────────────── */

void *stdhttp_create(int port) {
    stdhttp_server_t *srv = (stdhttp_server_t *)calloc(1, sizeof *srv);
    if (!srv) return NULL;
    srv->port    = port;
    srv->running = 0;
    return srv;
}

void stdhttp_free(void *srv_opaque) {
    if (!srv_opaque) return;
    stdhttp_server_t *srv = (stdhttp_server_t *)srv_opaque;
    /* If still running, signal stop (mg_mgr was freed by stdhttp_listen) */
    srv->running = 0;
    free(srv);
}

void stdhttp_route(void *srv_opaque,
                   const char *method,
                   const char *pattern,
                   stdhttp_handler_fn fn) {
    stdhttp_server_t *srv = (stdhttp_server_t *)srv_opaque;
    if (!srv || srv->route_count >= STDHTTP_MAX_ROUTES) return;

    stdhttp_route_t *rt = &srv->routes[srv->route_count++];
    snprintf(rt->method,  sizeof rt->method,  "%s", method  ? method  : "ANY");
    snprintf(rt->pattern, sizeof rt->pattern, "%s", pattern ? pattern : "/");
    rt->fn = fn;
}

void stdhttp_use(void *srv_opaque, stdhttp_middleware_fn fn) {
    stdhttp_server_t *srv = (stdhttp_server_t *)srv_opaque;
    if (!srv || srv->middleware_count >= STDHTTP_MAX_MIDDLEWARES) return;
    srv->middlewares[srv->middleware_count++] = fn;
}

int stdhttp_listen(void *srv_opaque) {
    stdhttp_server_t *srv = (stdhttp_server_t *)srv_opaque;
    if (!srv) return -1;

    mg_mgr_init(&srv->mgr);

    char url[64];
    snprintf(url, sizeof url, "http://0.0.0.0:%d", srv->port);

    struct mg_connection *lc = mg_http_listen(&srv->mgr, url,
                                               mg_ev_handler, srv);
    if (!lc) {
        mg_mgr_free(&srv->mgr);
        return -1;
    }

    srv->running = 1;
    while (srv->running) {
        mg_mgr_poll(&srv->mgr, 100);
    }
    mg_mgr_free(&srv->mgr);
    return 0;
}

void stdhttp_stop(void *srv_opaque) {
    stdhttp_server_t *srv = (stdhttp_server_t *)srv_opaque;
    if (srv) srv->running = 0;
}

/* ── Request accessors ─────────────────────────────────────────────────────── */

const char *stdhttp_req_method(void *req_ctx) {
    return ((stdhttp_request_t *)req_ctx)->method;
}

const char *stdhttp_req_path(void *req_ctx) {
    return ((stdhttp_request_t *)req_ctx)->path;
}

const char *stdhttp_req_query_string(void *req_ctx) {
    return ((stdhttp_request_t *)req_ctx)->query;
}

const char *stdhttp_req_query(void *req_ctx, const char *key) {
    static _Thread_local char buf[STDHTTP_SCRATCH_BUF];
    stdhttp_request_t *req = (stdhttp_request_t *)req_ctx;

    struct mg_str qs  = { req->query, strlen(req->query) };
    struct mg_str kst = mg_str(key);
    /* mg_http_var searches key=value pairs in an encoded query string. */
    struct mg_str val = mg_http_var(qs, kst);
    if (val.buf == NULL) return NULL;

    /* URL-decode the value */
    size_t vlen = val.len < sizeof(buf) - 1 ? val.len : sizeof(buf) - 1;
    url_decode(val.buf, vlen, buf, sizeof buf);
    return buf;
}

const char *stdhttp_req_header(void *req_ctx, const char *name) {
    static _Thread_local char buf[STDHTTP_SCRATCH_BUF];
    stdhttp_request_t *req = (stdhttp_request_t *)req_ctx;

    struct mg_str *val = mg_http_get_header(req->hm, name);
    if (!val) return NULL;

    size_t vlen = val->len < sizeof(buf) - 1 ? val->len : sizeof(buf) - 1;
    memcpy(buf, val->buf, vlen);
    buf[vlen] = '\0';
    return buf;
}

const char *stdhttp_req_body(void *req_ctx) {
    stdhttp_request_t *req = (stdhttp_request_t *)req_ctx;
    return req->body_buf ? req->body_buf : "";
}

int stdhttp_req_body_len(void *req_ctx) {
    return ((stdhttp_request_t *)req_ctx)->body_len;
}

const char *stdhttp_req_param(void *req_ctx, const char *name) {
    stdhttp_request_t *req = (stdhttp_request_t *)req_ctx;
    for (int i = 0; i < req->param_count; i++) {
        if (strcmp(req->param_names[i], name) == 0) {
            return req->param_vals[i];
        }
    }
    return NULL;
}

void stdhttp_req_set_param(void *req_ctx, const char *name, const char *val) {
    stdhttp_request_t *req = (stdhttp_request_t *)req_ctx;
    if (req->param_count >= STDHTTP_MAX_PARAMS) return;
    int i = req->param_count++;
    snprintf(req->param_names[i], sizeof req->param_names[i], "%s", name);
    snprintf(req->param_vals[i],  sizeof req->param_vals[i],  "%s", val);
}

/* ── Response functions ────────────────────────────────────────────────────── */

void stdhttp_res_status(void *res_ctx, int code) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (!res->sent) res->status = code;
}

void stdhttp_res_header(void *res_ctx, const char *name, const char *val) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (res->sent) return;
    /* Track whether caller set Content-Type so send helpers don't override it */
    if (strcasecmp(name, "Content-Type") == 0) {
        res->content_type_set = 1;
    }
    append_extra_header(res, name, val);
}

void stdhttp_res_send(void *res_ctx, const char *body, int len) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (res->sent) return;

    /* Append text/plain if caller did not supply Content-Type */
    char headers[STDHTTP_EXTRA_HEADERS_BUF + 64];
    if (!has_content_type(res)) {
        snprintf(headers, sizeof headers,
                 "Content-Type: text/plain\r\n%s", res->extra_headers);
    } else {
        snprintf(headers, sizeof headers, "%s", res->extra_headers);
    }

    mg_http_reply(res->conn, res->status, headers, "%.*s", len, body ? body : "");
    res->sent = 1;
}

void stdhttp_res_send_str(void *res_ctx, const char *body) {
    stdhttp_res_send(res_ctx, body, body ? (int)strlen(body) : 0);
}

void stdhttp_res_json(void *res_ctx, const char *json_str) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (res->sent) return;

    char headers[STDHTTP_EXTRA_HEADERS_BUF + 64];
    if (!has_content_type(res)) {
        snprintf(headers, sizeof headers,
                 "Content-Type: application/json\r\n%s", res->extra_headers);
    } else {
        snprintf(headers, sizeof headers, "%s", res->extra_headers);
    }

    int len = json_str ? (int)strlen(json_str) : 0;
    mg_http_reply(res->conn, res->status, headers, "%.*s", len, json_str ? json_str : "");
    res->sent = 1;
}

void stdhttp_res_html(void *res_ctx, const char *html) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (res->sent) return;

    char headers[STDHTTP_EXTRA_HEADERS_BUF + 64];
    if (!has_content_type(res)) {
        snprintf(headers, sizeof headers,
                 "Content-Type: text/html\r\n%s", res->extra_headers);
    } else {
        snprintf(headers, sizeof headers, "%s", res->extra_headers);
    }

    int len = html ? (int)strlen(html) : 0;
    mg_http_reply(res->conn, res->status, headers, "%.*s", len, html ? html : "");
    res->sent = 1;
}

void stdhttp_res_file(void *res_ctx, const char *path) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (res->sent || !path) return;

    struct mg_http_serve_opts opts;
    memset(&opts, 0, sizeof opts);
    /* extra_headers may be empty string — that is fine for mongoose */
    opts.extra_headers = res->extra_headers[0] ? res->extra_headers : NULL;
    opts.fs            = NULL; /* use POSIX filesystem */

    mg_http_serve_file(res->conn, res->hm, path, &opts);
    res->sent = 1;
}

void stdhttp_res_redirect(void *res_ctx, const char *url, int code) {
    stdhttp_response_t *res = (stdhttp_response_t *)res_ctx;
    if (res->sent || !url) return;

    /* Build Location header plus any caller-supplied extras */
    char headers[STDHTTP_EXTRA_HEADERS_BUF + 256];
    snprintf(headers, sizeof headers,
             "Location: %s\r\n%s", url, res->extra_headers);

    mg_http_reply(res->conn, code, headers, "");
    res->sent = 1;
}

int stdhttp_res_is_sent(void *res_ctx) {
    return ((stdhttp_response_t *)res_ctx)->sent;
}
