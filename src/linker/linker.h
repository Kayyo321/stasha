#ifndef LinkerH
#define LinkerH

#include "../common/common.h"

/*
 * extra_libs: NULL-terminated array of .a library paths to link in addition
 * to the object file (from `lib "name" from "path"` declarations).
 * May be NULL if there are no custom libraries.
 */
result_t link_object(const char *obj_path, const char *output_path,
                     const char **extra_libs);

/*
 * Freestanding link: skips -lSystem / -lc / -lm / -lpthread.  Intended for
 * @[[freestanding]] modules that must not depend on the host libc.  Only the
 * object file and caller-supplied libraries are passed to LLD.
 */
result_t link_object_freestanding(const char *obj_path, const char *output_path,
                                   const char **extra_libs);

/*
 * Bundle a single object file into a static archive (.a).
 * Uses the bundled LLVM archive writer — no external 'ar' required.
 */
result_t archive_object(const char *obj_path, const char *output_path);

/*
 * Link an object file into a dynamic library (.so / .dylib / .dll).
 * extra_libs: same semantics as link_object.
 */
result_t link_dynamic(const char *obj_path, const char *output_path,
                      const char **extra_libs);

#endif
