/* ── name_mangle.c ─────────────────────────────────────────────────────────
 * Compile-time symbol mangling for Stasha's module namespace system.
 * Included directly into codegen.c (unity build), so no separate .o needed.
 * ────────────────────────────────────────────────────────────────────────── */

#include "name_mangle.h"
#include <string.h>
#include <stdio.h>

/*
 * Convert dotted module path to double-underscore prefix.
 * "net.socket" → "net__socket"
 * "" or NULL   → ""
 */
char *mangle_module_prefix(const char *module, char *out, size_t out_size) {
    if (!out || out_size == 0) return out;
    if (!module || module[0] == '\0') { out[0] = '\0'; return out; }

    size_t i = 0, j = 0;
    while (module[i] && j + 3 < out_size) {
        if (module[i] == '.') {
            out[j++] = '_';
            out[j++] = '_';
        } else {
            out[j++] = module[i];
        }
        i++;
    }
    out[j] = '\0';
    return out;
}

char *mangle_fn(const char *module, const char *fn,
                char *out, size_t out_size) {
    if (!module || module[0] == '\0') {
        snprintf(out, out_size, "%s", fn ? fn : "");
        return out;
    }
    char prefix[512];
    mangle_module_prefix(module, prefix, sizeof(prefix));
    snprintf(out, out_size, "%s__%s", prefix, fn ? fn : "");
    return out;
}

char *mangle_method(const char *module, const char *type_name,
                    const char *method, char *out, size_t out_size) {
    if (!module || module[0] == '\0') {
        /* root module: preserve legacy "Type.method" format for symtab compat */
        snprintf(out, out_size, "%s.%s",
                 type_name ? type_name : "", method ? method : "");
        return out;
    }
    char prefix[512];
    mangle_module_prefix(module, prefix, sizeof(prefix));
    snprintf(out, out_size, "%s__%s__%s", prefix,
             type_name ? type_name : "", method ? method : "");
    return out;
}

char *mangle_global(const char *module, const char *name,
                    char *out, size_t out_size) {
    return mangle_fn(module, name, out, out_size);
}

char *mangle_type(const char *module, const char *name,
                  char *out, size_t out_size) {
    return mangle_fn(module, name, out, out_size);
}
