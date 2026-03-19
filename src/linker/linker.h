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
 * Bundle a single object file into a static archive (.a).
 * Uses the bundled LLVM archive writer — no external 'ar' required.
 */
result_t archive_object(const char *obj_path, const char *output_path);

#endif
