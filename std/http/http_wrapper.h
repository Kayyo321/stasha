#ifndef STDHTTP_WRAPPER_H
#define STDHTTP_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque callback types ─────────────────────────────────────────────────── */

/* Called by the dispatcher with (req_ctx, res_ctx) opaque pointers */
typedef void (*stdhttp_handler_fn)(void *req_ctx, void *res_ctx);

/* Middleware: called before routing for every request.
 * Return 0 to continue to the next middleware / handler.
 * Return non-zero to stop (response must already have been sent). */
typedef int (*stdhttp_middleware_fn)(void *req_ctx, void *res_ctx);

/* ── Server lifecycle ──────────────────────────────────────────────────────── */

/* Allocate and initialise server state on the given port.
 * Does NOT start listening — call stdhttp_listen() for that.
 * Returns NULL on allocation failure. */
void *stdhttp_create(int port);

/* Stop (if running) and free all resources owned by the server. */
void  stdhttp_free(void *srv);

/* ── Route / middleware registration ──────────────────────────────────────── */

/* Register a route handler.
 *   method  – "GET", "POST", "PUT", "DELETE", "PATCH", or "ANY"
 *   pattern – URL path, may contain :param segments, e.g. "/users/:id"
 *   fn      – handler called with (req_ctx, res_ctx)
 */
void stdhttp_route(void *srv,
                   const char *method,
                   const char *pattern,
                   stdhttp_handler_fn fn);

/* Register a middleware function (executed in registration order). */
void stdhttp_use(void *srv, stdhttp_middleware_fn fn);

/* ── Server control ────────────────────────────────────────────────────────── */

/* Start listening.  Blocks until stdhttp_stop() is called or a fatal error
 * occurs.  Returns 0 on clean shutdown, -1 on error. */
int  stdhttp_listen(void *srv);

/* Signal the event loop to stop.  Safe to call from a signal handler or
 * another thread. */
void stdhttp_stop(void *srv);

/* ── Request accessors (req_ctx is stdhttp_request_t *) ───────────────────── */

/* HTTP method string, e.g. "GET". */
const char *stdhttp_req_method(void *req_ctx);

/* URL path component only (no query string), e.g. "/users/42". */
const char *stdhttp_req_path(void *req_ctx);

/* Raw query string after the '?', or "" if absent. */
const char *stdhttp_req_query_string(void *req_ctx);

/* Decoded value for a single query-string key, or NULL if not present.
 * The returned pointer is valid until the next call on the same thread. */
const char *stdhttp_req_query(void *req_ctx, const char *key);

/* Value of the named request header, or NULL if not present.
 * The returned pointer is valid until the next call on the same thread. */
const char *stdhttp_req_header(void *req_ctx, const char *name);

/* Request body as a null-terminated string (may contain embedded NULs in
 * binary payloads — use stdhttp_req_body_len() for the true length). */
const char *stdhttp_req_body(void *req_ctx);

/* Length of the request body in bytes. */
int         stdhttp_req_body_len(void *req_ctx);

/* Value of a named route parameter (e.g. "id" for pattern "/users/:id"),
 * or NULL if the parameter is not part of the matched route. */
const char *stdhttp_req_param(void *req_ctx, const char *name);

/* Internal — called by the dispatcher to populate route parameters. */
void        stdhttp_req_set_param(void *req_ctx,
                                  const char *name,
                                  const char *val);

/* ── Response functions (res_ctx is stdhttp_response_t *) ─────────────────── */

/* Override the HTTP status code (default is 200). */
void stdhttp_res_status(void *res_ctx, int code);

/* Append a response header.  May be called multiple times. */
void stdhttp_res_header(void *res_ctx,
                        const char *name,
                        const char *val);

/* Send a response body with the given byte length.
 * Content-Type defaults to text/plain unless overridden via
 * stdhttp_res_header() before this call. */
void stdhttp_res_send(void *res_ctx, const char *body, int len);

/* Convenience: send a null-terminated string body (text/plain). */
void stdhttp_res_send_str(void *res_ctx, const char *body);

/* Send a response with Content-Type: application/json. */
void stdhttp_res_json(void *res_ctx, const char *json_str);

/* Send a response with Content-Type: text/html. */
void stdhttp_res_html(void *res_ctx, const char *html);

/* Serve a static file from the filesystem. */
void stdhttp_res_file(void *res_ctx, const char *path);

/* Send a redirect response.  code should be 301 or 302. */
void stdhttp_res_redirect(void *res_ctx, const char *url, int code);

/* Returns 1 if a response has already been sent for this request, 0 otherwise.
 * Handlers and middleware should check this before attempting a second send. */
int  stdhttp_res_is_sent(void *res_ctx);

#ifdef __cplusplus
}
#endif

#endif /* STDHTTP_WRAPPER_H */
