#ifndef STDJSON_WRAPPER_H
#define STDJSON_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * stdjson — Stasha standard library JSON wrapper around cJSON
 * All opaque node pointers are void* to remain C-interop friendly.
 * --------------------------------------------------------------------------- */

/* --- Parsing --- */

/* Parse a JSON string. Returns cJSON* on success, NULL on error.
   Call stdjson_last_error() for a description on failure. */
void*       stdjson_parse(const char *src);

/* Read a file, parse its contents as JSON. Returns cJSON* or NULL.
   The file is read entirely into a temporary heap buffer before parsing. */
void*       stdjson_parse_file(const char *path);

/* Thread-local last error message (valid until the next parse call on this thread). */
const char* stdjson_last_error(void);

/* --- Serialization --- */

/* Pretty-print the JSON tree. Caller must free the result with stdjson_free_str. */
char*       stdjson_stringify(void *val);

/* Compact (no whitespace) serialization. Caller must free with stdjson_free_str. */
char*       stdjson_stringify_compact(void *val);

/* Serialize and write to a file. pretty=1 for pretty-print, 0 for compact.
   Returns 0 on success, -1 on error. */
int         stdjson_write_file(const char *path, void *val, int pretty);

/* --- Memory --- */

/* Delete a cJSON tree (cJSON_Delete). */
void        stdjson_free(void *val);

/* Free a string returned by stdjson_stringify / stdjson_stringify_compact. */
void        stdjson_free_str(char *s);

/* --- Type inspection ---
 * Return values:
 *   0 = invalid (NULL or unrecognized)
 *   1 = false
 *   2 = true
 *   3 = null
 *   4 = number
 *   5 = string
 *   6 = array
 *   7 = object
 */
int stdjson_type(void *val);
int stdjson_is_null(void *val);
int stdjson_is_bool(void *val);
int stdjson_is_number(void *val);
int stdjson_is_string(void *val);
int stdjson_is_array(void *val);
int stdjson_is_object(void *val);

/* --- Value accessors --- */

/* Returns the string value of a cJSON_String node, or NULL. */
const char* stdjson_get_str_val(void *val);

/* Returns the numeric value of a cJSON_Number node, or 0.0. */
double      stdjson_get_num_val(void *val);

/* Returns 1 if the node is cJSON_True, 0 otherwise. */
int         stdjson_get_bool_val(void *val);

/* --- Constructors ---
 * Return detached cJSON nodes. Do NOT call stdjson_free on these unless you
 * are certain they have not been added to a parent object/array (the parent
 * owns them once added). */
void* stdjson_make_string(const char *s);
void* stdjson_make_number(double n);
void* stdjson_make_bool(int b);
void* stdjson_make_null(void);
void* stdjson_obj_new(void);
void* stdjson_array_new(void);

/* --- Object operations --- */

/* Look up a key in an object. Returns the child node or NULL. Do not free. */
void*       stdjson_obj_get(void *obj, const char *key);

/* Add/replace a key with an already-constructed item node.
   The object takes ownership of item. Returns 0 on success, -1 on error. */
int         stdjson_obj_set_item(void *obj, const char *key, void *item);

/* Convenience setters — construct and add a value in one call. */
int         stdjson_obj_set_str(void *obj, const char *key, const char *val);
int         stdjson_obj_set_num(void *obj, const char *key, double val);
int         stdjson_obj_set_bool(void *obj, const char *key, int val);
int         stdjson_obj_set_null(void *obj, const char *key);

/* Remove and delete a key from an object. Returns 0 on success, -1 on error. */
int         stdjson_obj_remove(void *obj, const char *key);

/* Number of key/value pairs in an object. */
int         stdjson_obj_size(void *obj);

/* Access the key at position idx (0-based) via linked-list traversal.
   Returns NULL if out of range. The returned pointer is into the cJSON
   structure; do not free or mutate it. */
const char* stdjson_obj_key_at(void *obj, int idx);

/* Access the value node at position idx. Returns NULL if out of range.
   Do not free the returned pointer. */
void*       stdjson_obj_val_at(void *obj, int idx);

/* Returns 1 if the object has the given key, 0 otherwise. */
int         stdjson_obj_has(void *obj, const char *key);

/* --- Array operations --- */

/* Number of elements in the array. */
int   stdjson_array_len(void *arr);

/* Element at idx (0-based). Returns NULL if out of range. Do not free. */
void* stdjson_array_get(void *arr, int idx);

/* Append an already-constructed item. Array takes ownership. Returns 0/-1. */
int   stdjson_array_push_item(void *arr, void *item);

/* Convenience push helpers. */
int   stdjson_array_push_str(void *arr, const char *s);
int   stdjson_array_push_num(void *arr, double n);
int   stdjson_array_push_bool(void *arr, int b);
int   stdjson_array_push_null(void *arr);

/* Remove and delete the element at idx. Returns 0 on success, -1 on error. */
int   stdjson_array_remove(void *arr, int idx);

/* --- Deep clone ---
 * Returns a fully independent copy of the node and all its children.
 * Caller owns the returned pointer and must free it with stdjson_free. */
void* stdjson_clone(void *val);

#ifdef __cplusplus
}
#endif

#endif /* STDJSON_WRAPPER_H */
