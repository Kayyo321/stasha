#ifndef STASHA_TARGET_H
#define STASHA_TARGET_H

/* Target properties must never be inferred from compiler-host preprocessor
 * macros. The triple is normalized by LLVM before this descriptor is built. */
#include <string.h>
#include <stddef.h>

typedef enum { StsFormatUnknown, StsFormatELF, StsFormatMachO, StsFormatCOFF } sts_object_format;
typedef struct {
    char triple[256];
    char arch[32];
    char os[32];
    unsigned pointer_bits;
    unsigned c_long_bits;
    sts_object_format format;
} sts_target_spec;

static int sts_target_init(sts_target_spec *out, const char *normalized_triple) {
    memset(out, 0, sizeof(*out));
    if (!normalized_triple || strlen(normalized_triple) >= sizeof(out->triple)) return -1;
    strcpy(out->triple, normalized_triple);
    const char *dash = strchr(normalized_triple, '-');
    size_t arch_len = dash ? (size_t)(dash - normalized_triple) : 0;
    if (!arch_len || arch_len >= sizeof(out->arch)) return -1;
    memcpy(out->arch, normalized_triple, arch_len);
    if (strcmp(out->arch, "arm64") == 0) strcpy(out->arch, "aarch64");
    if (strcmp(out->arch, "amd64") == 0) strcpy(out->arch, "x86_64");
    if (strcmp(out->arch, "x86_64") == 0 || strcmp(out->arch, "aarch64") == 0)
        out->pointer_bits = 64;
    else if (strcmp(out->arch, "i386") == 0 || strcmp(out->arch, "i486") == 0 ||
             strcmp(out->arch, "i586") == 0 || strcmp(out->arch, "i686") == 0)
        out->pointer_bits = 32;
    else return -1; /* No guessed pointer width for unavailable backends. */
    const char *vendor_end = strchr(dash + 1, '-');
    if (!vendor_end) return -1;
    const char *os = vendor_end + 1;
    if (strncmp(os, "linux", 5) == 0) strcpy(out->os, "linux");
    else if (strncmp(os, "darwin", 6) == 0 || strncmp(os, "macos", 5) == 0)
        strcpy(out->os, "macos");
    else if (strncmp(os, "windows", 7) == 0 || strncmp(os, "win32", 5) == 0)
        strcpy(out->os, "windows");
    else if (strncmp(os, "freebsd", 7) == 0) strcpy(out->os, "freebsd");
    else if (strncmp(os, "none", 4) == 0 || strncmp(os, "unknown", 7) == 0)
        strcpy(out->os, "none");
    else return -1;
    out->format = strcmp(out->os, "windows") == 0 ? StsFormatCOFF :
                  strcmp(out->os, "macos") == 0 ? StsFormatMachO : StsFormatELF;
    /* LLVM permits an explicit object-format suffix on the environment.
     * Admit only the OS's supported format; never label Mach-O/COFF as ELF. */
    const char *environment = strchr(os, '-');
    if (environment) {
        const char *formats[] = {"xcoff", "coff", "elf", "goff", "macho", "wasm", "spirv"};
        sts_object_format kinds[] = {StsFormatUnknown, StsFormatCOFF, StsFormatELF,
            StsFormatUnknown, StsFormatMachO, StsFormatUnknown, StsFormatUnknown};
        size_t n = strlen(environment);
        for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
            size_t f = strlen(formats[i]);
            if (n >= f && !strcmp(environment + n - f, formats[i])) {
                if (kinds[i] != out->format) return -1;
                break;
            }
        }
    }
    out->c_long_bits = out->format == StsFormatCOFF ? 32 : out->pointer_bits;
    /* x32 is ILP32 even though its instruction set is x86-64. */
    if (strstr(normalized_triple, "gnux32")) out->pointer_bits = out->c_long_bits = 32;
    return 0;
}

static int sts_target_arch_matches(const sts_target_spec *target, const char *arch) {
    if (!strcmp(target->arch, arch)) return 1;
    if ((!strcmp(arch, "amd64") || !strcmp(arch, "x64")) && !strcmp(target->arch, "x86_64")) return 1;
    if (!strcmp(arch, "arm64") && !strcmp(target->arch, "aarch64")) return 1;
    if (!strcmp(arch, "x86") && target->pointer_bits == 32 && target->arch[0] == 'i') return 1;
    return 0;
}
static int sts_target_os_matches(const sts_target_spec *target, const char *os) {
    return !strcmp(target->os, os) || (!strcmp(os, "darwin") && !strcmp(target->os, "macos"));
}
#endif
