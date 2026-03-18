#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef STASHA_HAS_LLD
#include <vector>
#include <string>
#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(elf)
#endif

extern "C" {
#include "linker.h"
}

#ifdef __APPLE__
static const char *get_sdk_path(void) {
    static char sdk_path[1024] = {};
    if (sdk_path[0] != '\0') return sdk_path;

    FILE *p = popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (p) {
        if (fgets(sdk_path, sizeof(sdk_path), p)) {
            size_t len = strlen(sdk_path);
            if (len > 0 && sdk_path[len - 1] == '\n')
                sdk_path[len - 1] = '\0';
        }
        pclose(p);
    }
    if (sdk_path[0] == '\0')
        strcpy(sdk_path, "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk");
    return sdk_path;
}
#endif

#ifdef STASHA_HAS_LLD
static result_t try_lld(const char *obj_path, const char *output_path) {
    std::vector<const char *> args;

#ifdef __APPLE__
    args.push_back("ld64.lld");
    args.push_back("-arch");
  #if defined(__aarch64__) || defined(__arm64__)
    args.push_back("arm64");
  #else
    args.push_back("x86_64");
  #endif
    args.push_back("-platform_version");
    args.push_back("macos");
    args.push_back("13.0");
    args.push_back("13.0");
    args.push_back("-syslibroot");
    args.push_back(get_sdk_path());
    args.push_back("-lSystem");
    args.push_back(obj_path);
    args.push_back("-o");
    args.push_back(output_path);

    llvm::raw_null_ostream null_os;
    lld::Result res = lld::lldMain(args, null_os, llvm::errs(),
                                    {{lld::Gnu, &lld::elf::link},
                                     {lld::Darwin, &lld::macho::link}});
    return res.retCode == 0 ? Ok : Err;
#else
    args.push_back("ld.lld");
    args.push_back(obj_path);
    args.push_back("-o");
    args.push_back(output_path);
    args.push_back("-lc");
    args.push_back("-lm");
    args.push_back("-dynamic-linker");
    args.push_back("/lib64/ld-linux-x86-64.so.2");

    llvm::raw_null_ostream null_os;
    lld::Result res = lld::lldMain(args, null_os, llvm::errs(),
                                    {{lld::Gnu, &lld::elf::link},
                                     {lld::Darwin, &lld::macho::link}});
    return res.retCode == 0 ? Ok : Err;
#endif
}
#endif /* STASHA_HAS_LLD */

extern "C" result_t link_object(const char *obj_path, const char *output_path) {
#ifdef STASHA_HAS_LLD
    if (try_lld(obj_path, output_path) == Ok)
        return Ok;
    log_warn("linker: LLD failed, falling back to clang");
#endif

    /* fallback: use clang as linker driver */
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
