#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LLD is required — build with 'make llvm' if this fails */
#ifndef STASHA_HAS_LLD
#error "Stasha requires the bundled LLD linker. Run 'make llvm' to build LLVM and LLD."
#endif

#include <vector>
#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(elf)

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

extern "C" result_t link_object(const char *obj_path, const char *output_path,
                                 const char **extra_libs) {
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
    /* custom .a libraries from `lib "name" from "path"` declarations */
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
#else
    args.push_back("ld.lld");
    args.push_back(obj_path);
    /* custom .a libraries from `lib "name" from "path"` declarations */
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
    args.push_back("-lc");
    args.push_back("-lm");
    args.push_back("-dynamic-linker");
    args.push_back("/lib64/ld-linux-x86-64.so.2");
#endif

    llvm::raw_null_ostream null_os;
    lld::Result res = lld::lldMain(args, null_os, llvm::errs(),
                                    {{lld::Gnu,    &lld::elf::link},
                                     {lld::Darwin, &lld::macho::link}});
    if (res.retCode != 0) {
        log_err("linker: LLD failed with code %d", res.retCode);
        return Err;
    }
    return Ok;
}
