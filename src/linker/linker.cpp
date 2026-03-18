#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>
#include <cstdio>

#ifdef __APPLE__
LLD_HAS_DRIVER(macho)
#else
LLD_HAS_DRIVER(elf)
#endif

extern "C" {
#include "linker.h"
}

#ifdef __APPLE__

static std::string get_sdk_path() {
    FILE *fp = popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            std::string path(buf);
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            if (!path.empty()) return path;
        } else {
            pclose(fp);
        }
    }
    return "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
}

extern "C" result_t link_object(const char *obj_path, const char *output_path) {
    std::string sdk = get_sdk_path();
    std::string lib_path = sdk + "/usr/lib";

    std::vector<const char *> args;
    args.push_back("ld64.lld");

#if defined(__aarch64__) || defined(__arm64__)
    args.push_back("-arch");
    args.push_back("arm64");
#else
    args.push_back("-arch");
    args.push_back("x86_64");
#endif

    args.push_back("-platform_version");
    args.push_back("macos");
    args.push_back("11.0.0");
    args.push_back("11.0.0");

    args.push_back("-syslibroot");
    args.push_back(sdk.c_str());

    args.push_back("-L");
    args.push_back(lib_path.c_str());

    args.push_back("-lSystem");

    args.push_back(obj_path);
    args.push_back("-o");
    args.push_back(output_path);

    std::string err_str;
    llvm::raw_string_ostream err_os(err_str);

    lld::Result result = lld::lldMain(
        args, llvm::outs(), err_os,
        {{lld::Darwin, &lld::macho::link}});

    if (result.retCode != 0)
        log_err("lld: %s", err_str.c_str());

    return result.retCode == 0 ? Ok : Err;
}

#else /* Linux */

#include <unistd.h>

static std::string find_crt(const char *name) {
    static const char *search[] = {
        "/usr/lib/x86_64-linux-gnu/",
        "/usr/lib64/",
        "/usr/lib/aarch64-linux-gnu/",
        "/usr/lib/",
        nullptr,
    };
    for (int i = 0; search[i]; i++) {
        std::string path = std::string(search[i]) + name;
        if (access(path.c_str(), F_OK) == 0) return path;
    }
    return name;
}

extern "C" result_t link_object(const char *obj_path, const char *output_path) {
    std::string crt1 = find_crt("crt1.o");
    std::string crti = find_crt("crti.o");
    std::string crtn = find_crt("crtn.o");
    std::string lib_dir = crt1.substr(0, crt1.rfind('/'));

    std::vector<const char *> args;
    args.push_back("ld.lld");

    args.push_back("--eh-frame-hdr");

#if defined(__aarch64__) || defined(__arm64__)
    args.push_back("-dynamic-linker");
    args.push_back("/lib/ld-linux-aarch64.so.1");
#else
    args.push_back("-dynamic-linker");
    args.push_back("/lib64/ld-linux-x86-64.so.2");
#endif

    args.push_back(crt1.c_str());
    args.push_back(crti.c_str());

    std::string L_flag = "-L" + lib_dir;
    args.push_back(L_flag.c_str());

    args.push_back(obj_path);

    args.push_back("-lc");
    args.push_back("-lm");

    args.push_back(crtn.c_str());

    args.push_back("-o");
    args.push_back(output_path);

    std::string err_str;
    llvm::raw_string_ostream err_os(err_str);

    lld::Result result = lld::lldMain(
        args, llvm::outs(), err_os,
        {{lld::Gnu, &lld::elf::link}});

    if (result.retCode != 0)
        log_err("lld: %s", err_str.c_str());

    return result.retCode == 0 ? Ok : Err;
}

#endif
