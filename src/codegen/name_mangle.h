#ifndef NameMangleH
#define NameMangleH

#include <stddef.h>

/*
 * Compile-time symbol mangling for Stasha's module namespace system.
 *
 * Rules:
 *   module path separator '.' → "__" (double underscore)
 *
 *   mangle_fn("net.socket", "write", ...)      → "net__socket__write"
 *   mangle_method("geom", "Vec2", "len", ...)  → "geom__Vec2__len"
 *   mangle_global("config", "max_conns", ...)  → "config__max_conns"
 *   mangle_type("geom", "Vec2", ...)           → "geom__Vec2"
 *
 *   mangle_fn(NULL, "write", ...)              → "write"   (root/C, no mangling)
 *   mangle_fn("", "write", ...)               → "write"   (no mangling)
 *
 * All functions write into the caller-supplied buf[buf_size] and return buf.
 * C extern symbols (is_c_extern=true) must always be called with module=NULL.
 */

/* "a.b.c" → "a__b__c". Returns out. */
char *mangle_module_prefix(const char *module, char *out, size_t out_size);

/* fn in module → module__fn */
char *mangle_fn(const char *module, const char *fn,
                char *out, size_t out_size);

/* method → module__Type__method */
char *mangle_method(const char *module, const char *type_name,
                    const char *method, char *out, size_t out_size);

/* global variable → module__name */
char *mangle_global(const char *module, const char *name,
                    char *out, size_t out_size);

/* type (struct/enum) → module__Name */
char *mangle_type(const char *module, const char *name,
                  char *out, size_t out_size);

#endif
