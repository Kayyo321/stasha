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
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(coff)

extern "C" {
#include "linker.h"
}

#ifdef _WIN32
#include <string>
/* Append `/libpath:<dir>` entries from the MSVC `LIB` env var (set by
   vcvarsall.bat / Developer Command Prompt). Strings are stored in a
   thread-local arena because args is std::vector<const char *>. */
static std::vector<std::string>& msvc_libpath_storage(void) {
    static thread_local std::vector<std::string> storage;
    return storage;
}
static void push_msvc_libpaths(std::vector<const char *> &args) {
    auto &storage = msvc_libpath_storage();
    storage.clear();
    const char *lib_env = getenv("LIB");
    if (!lib_env || !*lib_env) {
        log_warn("linker: LIB env var is empty — MSVC CRT libs will not be "
                 "found. Run from a Developer Command Prompt or call "
                 "vcvarsall.bat first.");
        return;
    }
    std::string s(lib_env);
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(';', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) {
            std::string entry = "/libpath:" + s.substr(start, end - start);
            storage.push_back(std::move(entry));
        }
        start = end + 1;
    }
    for (auto &e : storage) args.push_back(e.c_str());
}
static const char *coff_machine(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "/machine:arm64";
#else
    return "/machine:x64";
#endif
}
#endif

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
    args.push_back("-lc++");
    args.push_back(obj_path);
    /* custom .a libraries from `lib "name" from "path"` declarations */
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
#elif defined(_WIN32)
    args.push_back("lld-link");
    args.push_back("/subsystem:console");
    args.push_back(coff_machine());
    args.push_back("/nologo");
    push_msvc_libpaths(args);
    /* Default to dynamic UCRT — matches what clang-cl picks. */
    args.push_back("ucrt.lib");
    args.push_back("vcruntime.lib");
    args.push_back("msvcrt.lib");
    args.push_back("kernel32.lib");
    args.push_back("user32.lib");
    args.push_back("dbghelp.lib");      /* SEH crash handler symbolisation */
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    static std::string out_arg;
    out_arg = std::string("/out:") + output_path;
    args.push_back(out_arg.c_str());
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
    /* System lib search paths — Ubuntu/Debian multiarch + traditional. */
#if defined(__aarch64__) || defined(__arm64__)
    args.push_back("-L/usr/lib/aarch64-linux-gnu");
    args.push_back("-L/lib/aarch64-linux-gnu");
#else
    args.push_back("-L/usr/lib/x86_64-linux-gnu");
    args.push_back("-L/lib/x86_64-linux-gnu");
#endif
    args.push_back("-L/usr/lib");
    args.push_back("-L/lib");
    args.push_back("-lc");
    args.push_back("-lm");
    args.push_back("-lpthread");  /* thread runtime requires pthreads on Linux */
    args.push_back("-dynamic-linker");
#if defined(__aarch64__) || defined(__arm64__)
    args.push_back("/lib/ld-linux-aarch64.so.1");
#else
    args.push_back("/lib64/ld-linux-x86-64.so.2");
#endif
#endif

    llvm::raw_null_ostream null_os;
    lld::Result res = lld::lldMain(args, null_os, llvm::errs(),
                                    {{lld::Gnu,     &lld::elf::link},
                                     {lld::Darwin,  &lld::macho::link},
                                     {lld::WinLink, &lld::coff::link}});
    if (res.retCode != 0) {
        log_err("linker: LLD failed with code %d", res.retCode);
        return Err;
    }
    return Ok;
}

extern "C" result_t link_object_freestanding(const char *obj_path,
                                              const char *output_path,
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
    /* Freestanding: no -lSystem; allow undefined symbols so the caller can
       provide them later via another link step. */
    args.push_back("-undefined");
    args.push_back("dynamic_lookup");
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
#elif defined(_WIN32)
    args.push_back("lld-link");
    args.push_back("/subsystem:console");
    args.push_back(coff_machine());
    args.push_back("/nologo");
    args.push_back("/force:unresolved");
    push_msvc_libpaths(args);
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    static std::string fs_out_arg;
    fs_out_arg = std::string("/out:") + output_path;
    args.push_back(fs_out_arg.c_str());
#else
    args.push_back("ld.lld");
    args.push_back("--no-dynamic-linker");
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
#endif

    llvm::raw_null_ostream null_os;
    lld::Result res = lld::lldMain(args, null_os, llvm::errs(),
                                    {{lld::Gnu,     &lld::elf::link},
                                     {lld::Darwin,  &lld::macho::link},
                                     {lld::WinLink, &lld::coff::link}});
    if (res.retCode != 0) {
        log_err("linker: LLD failed (freestanding) with code %d", res.retCode);
        return Err;
    }
    return Ok;
}

extern "C" result_t archive_object(const char *obj_path, const char *output_path) {
    using namespace llvm;
    using namespace llvm::object;

    auto member_or_err = NewArchiveMember::getFile(obj_path, /*Deterministic=*/true);
    if (!member_or_err) {
        llvm::errs() << "archiver: " << toString(member_or_err.takeError()) << "\n";
        return Err;
    }

    std::vector<NewArchiveMember> members;
    members.push_back(std::move(*member_or_err));

    llvm::Error err = writeArchive(output_path, members,
                                   SymtabWritingMode::NormalSymtab,
                                   Archive::K_GNU,
                                   /*Deterministic=*/true,
                                   /*Thin=*/false);
    if (err) {
        llvm::errs() << "archiver: " << toString(std::move(err)) << "\n";
        return Err;
    }
    return Ok;
}

extern "C" result_t link_dynamic(const char *obj_path, const char *output_path,
                                  const char **extra_libs) {
    std::vector<const char *> args;

#ifdef __APPLE__
    args.push_back("ld64.lld");
    args.push_back("-dylib");
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
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
#elif defined(_WIN32)
    args.push_back("lld-link");
    args.push_back("/dll");
    args.push_back(coff_machine());
    args.push_back("/nologo");
    push_msvc_libpaths(args);
    args.push_back("ucrt.lib");
    args.push_back("vcruntime.lib");
    args.push_back("msvcrt.lib");
    args.push_back("kernel32.lib");
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    static std::string dyn_out_arg;
    dyn_out_arg = std::string("/out:") + output_path;
    args.push_back(dyn_out_arg.c_str());
#else
    args.push_back("ld.lld");
    args.push_back("-shared");
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++)
            args.push_back(*p);
    }
    args.push_back("-o");
    args.push_back(output_path);
    args.push_back("-lc");
    args.push_back("-lm");
#endif

    llvm::raw_null_ostream null_os;
    lld::Result res = lld::lldMain(args, null_os, llvm::errs(),
                                    {{lld::Gnu,     &lld::elf::link},
                                     {lld::Darwin,  &lld::macho::link},
                                     {lld::WinLink, &lld::coff::link}});
    if (res.retCode != 0) {
        log_err("linker: LLD failed (dynamic) with code %d", res.retCode);
        return Err;
    }
    return Ok;
}
