#include "json_wrapper.h"
#include "../../extlib/cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Thread-local error buffer
 * --------------------------------------------------------------------------- */

static _Thread_local char stdjson_err_buf[256];

const char *stdjson_last_error(void) {
    return stdjson_err_buf;
}

/* Internal helper: record an error message into the thread-local buffer. */
static void set_error(const char *msg) {
    if (!msg) {
        stdjson_err_buf[0] = '\0';
        return;
    }
    strncpy(stdjson_err_buf, msg, sizeof(stdjson_err_buf) - 1);
    stdjson_err_buf[sizeof(stdjson_err_buf) - 1] = '\0';
}

/* ---------------------------------------------------------------------------
 * cJSON type bit constants (from cJSON.h, reproduced for clarity)
 *   cJSON_Invalid = 0
 *   cJSON_False   = 1
 *   cJSON_True    = 2
 *   cJSON_NULL    = 4
 *   cJSON_Number  = 8
 *   cJSON_String  = 16
 *   cJSON_Array   = 32
 *   cJSON_Object  = 64
 * --------------------------------------------------------------------------- */

#define CJSON_TYPE(v)  (((cJSON *)(v))->type & 0xFF)

/* ---------------------------------------------------------------------------
 * Parsing
 * --------------------------------------------------------------------------- */

void *stdjson_parse(const char *src) {
    if (!src) {
        set_error("null source string");
        return NULL;
    }
    stdjson_err_buf[0] = '\0';
    cJSON *root = cJSON_Parse(src);
    if (!root) {
        const char *ep = cJSON_GetErrorPtr();
        if (ep) {
            /* Show up to 80 chars around the error position for context. */
            char msg[128];
            snprintf(msg, sizeof(msg), "parse error near: %.80s", ep);
            set_error(msg);
        } else {
            set_error("unknown parse error");
        }
    }
    return root;
}

void *stdjson_parse_file(const char *path) {
    if (!path) {
        set_error("null file path");
        return NULL;
    }
    stdjson_err_buf[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }

    /* Determine file size. */
    if (fseek(f, 0, SEEK_END) != 0) {
        set_error("fseek failed");
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        set_error("ftell failed");
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        set_error("out of memory reading file");
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
        set_error("file read error");
        free(buf);
        return NULL;
    }
    buf[size] = '\0';

    void *result = stdjson_parse(buf);
    free(buf);
    return result;
}

/* ---------------------------------------------------------------------------
 * Serialization
 * --------------------------------------------------------------------------- */

char *stdjson_stringify(void *val) {
    if (!val) return NULL;
    return cJSON_Print((cJSON *)val);
}

char *stdjson_stringify_compact(void *val) {
    if (!val) return NULL;
    return cJSON_PrintUnformatted((cJSON *)val);
}

int stdjson_write_file(const char *path, void *val, int pretty) {
    if (!path || !val) return -1;

    char *s = pretty ? stdjson_stringify(val) : stdjson_stringify_compact(val);
    if (!s) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(s);
        return -1;
    }

    size_t len = strlen(s);
    size_t written = fwrite(s, 1, len, f);
    fclose(f);
    free(s);

    return (written == len) ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Memory
 * --------------------------------------------------------------------------- */

void stdjson_free(void *val) {
    if (val) cJSON_Delete((cJSON *)val);
}

void stdjson_free_str(char *s) {
    free(s);
}

/* ---------------------------------------------------------------------------
 * Type inspection
 * --------------------------------------------------------------------------- */

int stdjson_type(void *val) {
    if (!val) return 0;
    int t = CJSON_TYPE(val);
    switch (t) {
        case cJSON_False:   return 1;
        case cJSON_True:    return 2;
        case cJSON_NULL:    return 3;
        case cJSON_Number:  return 4;
        case cJSON_String:  return 5;
        case cJSON_Array:   return 6;
        case cJSON_Object:  return 7;
        default:            return 0;
    }
}

int stdjson_is_null(void *val) {
    if (!val) return 0;
    return cJSON_IsNull((cJSON *)val) ? 1 : 0;
}

int stdjson_is_bool(void *val) {
    if (!val) return 0;
    return cJSON_IsBool((cJSON *)val) ? 1 : 0;
}

int stdjson_is_number(void *val) {
    if (!val) return 0;
    return cJSON_IsNumber((cJSON *)val) ? 1 : 0;
}

int stdjson_is_string(void *val) {
    if (!val) return 0;
    return cJSON_IsString((cJSON *)val) ? 1 : 0;
}

int stdjson_is_array(void *val) {
    if (!val) return 0;
    return cJSON_IsArray((cJSON *)val) ? 1 : 0;
}

int stdjson_is_object(void *val) {
    if (!val) return 0;
    return cJSON_IsObject((cJSON *)val) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Value accessors
 * --------------------------------------------------------------------------- */

const char *stdjson_get_str_val(void *val) {
    if (!val) return NULL;
    cJSON *node = (cJSON *)val;
    if (!cJSON_IsString(node)) return NULL;
    return node->valuestring;
}

double stdjson_get_num_val(void *val) {
    if (!val) return 0.0;
    cJSON *node = (cJSON *)val;
    if (!cJSON_IsNumber(node)) return 0.0;
    return node->valuedouble;
}

int stdjson_get_bool_val(void *val) {
    if (!val) return 0;
    cJSON *node = (cJSON *)val;
    return cJSON_IsTrue(node) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Constructors
 * --------------------------------------------------------------------------- */

void *stdjson_make_string(const char *s) {
    return cJSON_CreateString(s ? s : "");
}

void *stdjson_make_number(double n) {
    return cJSON_CreateNumber(n);
}

void *stdjson_make_bool(int b) {
    return cJSON_CreateBool(b);
}

void *stdjson_make_null(void) {
    return cJSON_CreateNull();
}

void *stdjson_obj_new(void) {
    return cJSON_CreateObject();
}

void *stdjson_array_new(void) {
    return cJSON_CreateArray();
}

/* ---------------------------------------------------------------------------
 * Object operations
 * --------------------------------------------------------------------------- */

void *stdjson_obj_get(void *obj, const char *key) {
    if (!obj || !key) return NULL;
    return cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
}

int stdjson_obj_set_item(void *obj, const char *key, void *item) {
    if (!obj || !key || !item) return -1;
    /* cJSON_AddItemToObject detaches item from any previous parent and adds it.
       If the key already exists the old value is deleted and replaced. */
    cJSON_DeleteItemFromObjectCaseSensitive((cJSON *)obj, key);
    if (!cJSON_AddItemToObject((cJSON *)obj, key, (cJSON *)item)) return -1;
    return 0;
}

int stdjson_obj_set_str(void *obj, const char *key, const char *val) {
    if (!obj || !key) return -1;
    void *item = stdjson_make_string(val);
    if (!item) return -1;
    return stdjson_obj_set_item(obj, key, item);
}

int stdjson_obj_set_num(void *obj, const char *key, double val) {
    if (!obj || !key) return -1;
    void *item = stdjson_make_number(val);
    if (!item) return -1;
    return stdjson_obj_set_item(obj, key, item);
}

int stdjson_obj_set_bool(void *obj, const char *key, int val) {
    if (!obj || !key) return -1;
    void *item = stdjson_make_bool(val);
    if (!item) return -1;
    return stdjson_obj_set_item(obj, key, item);
}

int stdjson_obj_set_null(void *obj, const char *key) {
    if (!obj || !key) return -1;
    void *item = stdjson_make_null();
    if (!item) return -1;
    return stdjson_obj_set_item(obj, key, item);
}

int stdjson_obj_remove(void *obj, const char *key) {
    if (!obj || !key) return -1;
    cJSON_DeleteItemFromObjectCaseSensitive((cJSON *)obj, key);
    return 0;
}

int stdjson_obj_size(void *obj) {
    if (!obj) return 0;
    return cJSON_GetArraySize((cJSON *)obj);
}

/* Walk the linked list of children to reach index idx. */
static cJSON *obj_child_at(cJSON *obj, int idx) {
    if (!obj || idx < 0) return NULL;
    cJSON *child = obj->child;
    int i = 0;
    while (child && i < idx) {
        child = child->next;
        i++;
    }
    return child;
}

const char *stdjson_obj_key_at(void *obj, int idx) {
    cJSON *child = obj_child_at((cJSON *)obj, idx);
    if (!child) return NULL;
    return child->string; /* cJSON stores the key in the `string` field */
}

void *stdjson_obj_val_at(void *obj, int idx) {
    return obj_child_at((cJSON *)obj, idx);
}

int stdjson_obj_has(void *obj, const char *key) {
    if (!obj || !key) return 0;
    return cJSON_HasObjectItem((cJSON *)obj, key) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Array operations
 * --------------------------------------------------------------------------- */

int stdjson_array_len(void *arr) {
    if (!arr) return 0;
    return cJSON_GetArraySize((cJSON *)arr);
}

void *stdjson_array_get(void *arr, int idx) {
    if (!arr || idx < 0) return NULL;
    return cJSON_GetArrayItem((cJSON *)arr, idx);
}

int stdjson_array_push_item(void *arr, void *item) {
    if (!arr || !item) return -1;
    if (!cJSON_AddItemToArray((cJSON *)arr, (cJSON *)item)) return -1;
    return 0;
}

int stdjson_array_push_str(void *arr, const char *s) {
    if (!arr) return -1;
    void *item = stdjson_make_string(s);
    if (!item) return -1;
    return stdjson_array_push_item(arr, item);
}

int stdjson_array_push_num(void *arr, double n) {
    if (!arr) return -1;
    void *item = stdjson_make_number(n);
    if (!item) return -1;
    return stdjson_array_push_item(arr, item);
}

int stdjson_array_push_bool(void *arr, int b) {
    if (!arr) return -1;
    void *item = stdjson_make_bool(b);
    if (!item) return -1;
    return stdjson_array_push_item(arr, item);
}

int stdjson_array_push_null(void *arr) {
    if (!arr) return -1;
    void *item = stdjson_make_null();
    if (!item) return -1;
    return stdjson_array_push_item(arr, item);
}

int stdjson_array_remove(void *arr, int idx) {
    if (!arr || idx < 0) return -1;
    if (idx >= cJSON_GetArraySize((cJSON *)arr)) return -1;
    cJSON_DeleteItemFromArray((cJSON *)arr, idx);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Deep clone
 * --------------------------------------------------------------------------- */

void *stdjson_clone(void *val) {
    if (!val) return NULL;
    return cJSON_Duplicate((cJSON *)val, 1 /* deep */);
}
