#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LLD is required — build with 'make llvm' if this fails */
#ifndef STASHA_HAS_LLD
#error "Stasha requires the bundled LLD linker. Run 'make llvm' to build LLVM and LLD."
#endif

#include <vector>
#include <string>
#if defined(__linux__)
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <llvm/TargetParser/Host.h>
#endif
#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Support/Error.h>
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(coff)

extern "C" {
#include "linker.h"
}

extern "C" result_t audit_kernel_object(const char *obj_path) {
    auto binary = llvm::object::createBinary(obj_path);
    if (!binary) {
        log_err("kernel object audit: %s", llvm::toString(binary.takeError()).c_str());
        return Err;
    }
    auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->getBinary());
    if (!object || !object->isELF() || object->getArch() != llvm::Triple::x86_64 ||
        object->getBytesInAddress() != 8) {
        log_err("kernel object audit: expected a 64-bit x86 ELF object");
        return Err;
    }
    bool failed = false;
    for (const auto &symbol : object->symbols()) {
        auto flags = symbol.getFlags();
        auto name = symbol.getName();
        if (!flags || !name) {
            if (!flags) llvm::consumeError(flags.takeError());
            if (!name) llvm::consumeError(name.takeError());
            log_err("kernel object audit: cannot read symbol metadata");
            failed = true;
            continue;
        }
        if (name->empty()) continue;
        if (!(*flags & llvm::object::SymbolRef::SF_Undefined)) {
            if (*name == "__stasha_panic" || *name == "memcpy" || *name == "memmove" || *name == "memset") {
                log_err("kernel object audit: reserved runtime hook definition '%s'", name->str().c_str());
                failed = true;
            }
            continue;
        }
        if (*name != "__stasha_panic" && *name != "memcpy" && *name != "memmove" && *name != "memset") {
            log_err("kernel object audit: unqualified runtime import '%s'", name->str().c_str());
            failed = true;
        }
    }
    for (const auto &section : object->sections()) {
        auto name = section.getName();
        if (!name) {
            llvm::consumeError(name.takeError());
            log_err("kernel object audit: cannot read section metadata");
            failed = true;
            continue;
        }
        if (!section.getSize()) continue;
        llvm::object::ELFSectionRef elf_section(section);
        uint32_t type = elf_section.getType();
        if ((elf_section.getFlags() & llvm::ELF::SHF_TLS) ||
            type == llvm::ELF::SHT_PREINIT_ARRAY || type == llvm::ELF::SHT_INIT_ARRAY ||
            type == llvm::ELF::SHT_FINI_ARRAY || name->starts_with(".preinit_array") ||
            name->starts_with(".tdata") || name->starts_with(".tbss") ||
            name->starts_with(".init_array") || name->starts_with(".fini_array") ||
            name->starts_with(".ctors") || name->starts_with(".dtors") ||
            name->starts_with(".eh_frame") || name->starts_with(".gcc_except_table")) {
            log_err("kernel object audit: forbidden hosted section '%s'", name->str().c_str());
            failed = true;
        }
    }
    return failed ? Err : Ok;
}

static bool g_linker_debug_mode = false;

extern "C" void linker_set_debug_mode(boolean_t on) {
    g_linker_debug_mode = (on == True);
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

#if defined(__linux__)
/* Query the native C toolchain for its startup/runtime paths, without a shell.
 * These are host tools, not a substitute sysroot for a foreign target. */
static bool linux_cc_query(const char *cc, const std::string &option,
                           std::string &output) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execlp(cc, cc, option.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); return false; }
    output.clear();
    char buffer[512];
    bool read_ok = true;
    for (;;) {
        ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { read_ok = false; break; }
        if (!count) break;
        if (output.size() + (size_t)count >= PATH_MAX) {
            read_ok = false;
            kill(child, SIGKILL);
            break;
        }
        output.append(buffer, (size_t)count);
    }
    close(fds[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           !output.empty() && output.find('\n') == std::string::npos;
}

static bool linux_cc_file(const char *cc, const char *name, std::string &path) {
    std::string found;
    if (!linux_cc_query(cc, std::string("-print-file-name=") + name, found) || found == name) {
        log_err("native Linux startup: '%s' cannot locate %s", cc, name);
        return false;
    }
    char absolute[PATH_MAX];
    struct stat st;
    if (!realpath(found.c_str(), absolute) || stat(absolute, &st) != 0 ||
        !S_ISREG(st.st_mode) || access(absolute, R_OK) != 0) {
        log_err("native Linux startup: unreadable %s reported by '%s'", name, cc);
        return false;
    }
    path = absolute;
    return true;
}

struct LinuxHostedRuntime {
    std::string crt1, crti, crtbegin, crtend, crtn;
    std::string libc, libm, libgcc, libgcc_s, loader;
    std::string system_search, compiler_search;
    const char *emulation = nullptr;
};

static bool linux_hosted_runtime(const char *target_triple, const char *obj_path,
                                 LinuxHostedRuntime &rt) {
    llvm::Triple target(target_triple ? target_triple : "");
    llvm::Triple host(llvm::sys::getProcessTriple());
    if (!target.isOSLinux() || !host.isOSLinux() || target.getArch() != host.getArch() ||
        (target.getArch() != llvm::Triple::x86_64 && target.getArch() != llvm::Triple::aarch64) ||
        target.getEnvironment() != llvm::Triple::GNU || !target.isOSBinFormatELF()) {
        log_err("hosted Linux linking supports only the native 64-bit GNU/Linux ABI; use obj/ir for foreign target '%s'", target_triple ? target_triple : "");
        return false;
    }
    auto binary = llvm::object::createBinary(obj_path);
    if (!binary) {
        log_err("hosted Linux linking: %s", llvm::toString(binary.takeError()).c_str());
        return false;
    }
    auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->getBinary());
    if (!object || !object->isELF() || !object->isRelocatableObject() ||
        object->getArch() != target.getArch() || object->getBytesInAddress() != 8) {
        log_err("hosted Linux linking: object does not match the native target ABI");
        return false;
    }
    const char *cc = getenv("CC");
    if (!cc || !cc[0]) cc = "cc";
    std::string cc_machine;
    if (!linux_cc_query(cc, "-dumpmachine", cc_machine)) {
        log_err("hosted Linux linking requires a working native C compiler; set CC to its executable path");
        return false;
    }
    llvm::Triple cc_target(llvm::Triple::normalize(cc_machine));
    if (!cc_target.isOSLinux() || cc_target.getArch() != target.getArch() ||
        cc_target.getEnvironment() != target.getEnvironment()) {
        log_err("hosted Linux linking: CC target '%s' does not match '%s'", cc_machine.c_str(), target_triple);
        return false;
    }
    const char *loader = target.getArch() == llvm::Triple::x86_64
        ? "ld-linux-x86-64.so.2" : "ld-linux-aarch64.so.1";
    rt.emulation = target.getArch() == llvm::Triple::x86_64 ? "elf_x86_64" : "aarch64linux";
    if (!linux_cc_file(cc, "crt1.o", rt.crt1) || !linux_cc_file(cc, "crti.o", rt.crti) ||
        !linux_cc_file(cc, "crtbegin.o", rt.crtbegin) || !linux_cc_file(cc, "crtend.o", rt.crtend) ||
        !linux_cc_file(cc, "crtn.o", rt.crtn) || !linux_cc_file(cc, "libc.so", rt.libc) ||
        !linux_cc_file(cc, "libm.so", rt.libm) || !linux_cc_file(cc, "libgcc.a", rt.libgcc) ||
        !linux_cc_file(cc, "libgcc_s.so", rt.libgcc_s) || !linux_cc_file(cc, loader, rt.loader)) return false;
    rt.system_search = "-L" + rt.libc.substr(0, rt.libc.find_last_of('/'));
    rt.compiler_search = "-L" + rt.libgcc.substr(0, rt.libgcc.find_last_of('/'));
    return true;
}
#endif

extern "C" result_t link_object(const char *obj_path, const char *output_path,
                                 const char **extra_libs, const char *target_triple) {
    std::vector<const char *> args;
#if !defined(__linux__)
    (void)target_triple;
#endif

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
    static std::string pdb_arg;
    if (g_linker_debug_mode) {
        args.push_back("/debug");
        pdb_arg = std::string("/pdb:") + output_path + ".pdb";
        args.push_back(pdb_arg.c_str());
    }
#elif defined(__linux__)
    LinuxHostedRuntime runtime;
    if (!linux_hosted_runtime(target_triple, obj_path, runtime)) return Err;
    args.push_back("ld.lld");
    args.push_back("--fatal-warnings");
    args.push_back("--eh-frame-hdr");
    args.push_back("-m");
    args.push_back(runtime.emulation);
    args.push_back("-e");
    args.push_back("_start");
    args.push_back("-dynamic-linker");
    args.push_back(runtime.loader.c_str());
    args.push_back(runtime.system_search.c_str());
    args.push_back(runtime.compiler_search.c_str());
    args.push_back(runtime.crt1.c_str());
    args.push_back(runtime.crti.c_str());
    args.push_back(runtime.crtbegin.c_str());
    args.push_back(obj_path);
    if (extra_libs) {
        for (const char **p = extra_libs; *p; p++) args.push_back(*p);
    }
    args.push_back(runtime.libm.c_str());
    args.push_back("-lpthread");
    args.push_back("-ldl");
    args.push_back("--start-group");
    args.push_back(runtime.libgcc.c_str());
    args.push_back("--as-needed");
    args.push_back(runtime.libgcc_s.c_str());
    args.push_back("--no-as-needed");
    args.push_back(runtime.libc.c_str());
    args.push_back("--end-group");
    args.push_back(runtime.crtend.c_str());
    args.push_back(runtime.crtn.c_str());
    args.push_back("-o");
    args.push_back(output_path);
#else
    (void)target_triple;
    log_err("hosted executable linking is not supported on this host; emit an object instead");
    return Err;
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
    static std::string dyn_pdb_arg;
    if (g_linker_debug_mode) {
        args.push_back("/debug");
        dyn_pdb_arg = std::string("/pdb:") + output_path + ".pdb";
        args.push_back(dyn_pdb_arg.c_str());
    }
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
