#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "linker.h"
}

extern "C" result_t link_object(const char *obj_path, const char *output_path) {
    char cmd[4096];

#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd),
             "clang \"%s\" -o \"%s\" 2>&1",
             obj_path, output_path);
#else
    snprintf(cmd, sizeof(cmd),
             "clang \"%s\" -o \"%s\" -lm 2>&1",
             obj_path, output_path);
#endif

    int ret = system(cmd);
    if (ret != 0)
        log_err("linker: clang exited with code %d", ret);

    return ret == 0 ? Ok : Err;
}
