#ifndef STASHA_OUTPUT_TRANSACTION_H
#define STASHA_OUTPUT_TRANSACTION_H

/* Private sibling directory keeps concurrent invocations and incomplete
 * linker outputs separate. The final name only becomes visible after success.
 * Caller must remove compiler-owned sidecars before aborting the transaction. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct {
    char directory[512];
    char staged[512];
    const char *destination;
} sts_output_transaction;

static int sts_output_same_file(const char *a, const char *b) {
#ifdef _WIN32
    HANDLE ha = CreateFileA(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE hb = CreateFileA(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION ia, ib;
    int same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib) &&
        ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
        ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
#else
    struct stat ia, ib;
    return stat(a, &ia) == 0 && stat(b, &ib) == 0 &&
           ia.st_dev == ib.st_dev && ia.st_ino == ib.st_ino;
#endif
}

static int sts_output_begin(sts_output_transaction *tx, const char *source,
                            const char *destination) {
    memset(tx, 0, sizeof(*tx));
    if (source && sts_output_same_file(source, destination)) {
        errno = EINVAL;
        return -1;
    }
    struct stat output_stat;
    if (stat(destination, &output_stat) == 0 &&
        (output_stat.st_mode & S_IFMT) != S_IFREG) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(tx->directory, sizeof(tx->directory),
                     "%s.stasha-XXXXXX", destination);
    /* Reserve space for every compiler-owned companion path. */
    if (n < 0 || (size_t)n + 32 >= sizeof(tx->staged)) {
        errno = ENAMETOOLONG;
        return -1;
    }
#ifdef _WIN32
    /* _mkdir is the exclusive operation; existing names are never reused. */
    static unsigned long sequence;
    int created = 0;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        snprintf(tx->directory + n - 6, sizeof(tx->directory) - (size_t)n + 6,
                 "%06lx", ((unsigned long)_getpid() * 65537 + ++sequence) & 0xffffff);
        if (_mkdir(tx->directory) == 0) { created = 1; break; }
        if (errno != EEXIST) break;
    }
    if (!created) return -1;
#else
    if (!mkdtemp(tx->directory)) return -1;
#endif
    const char *basename = strrchr(destination, '/');
#ifdef _WIN32
    const char *backslash = strrchr(destination, '\\');
    if (!basename || (backslash && backslash > basename)) basename = backslash;
#endif
    basename = basename ? basename + 1 : destination;
    size_t directory_length = strlen(tx->directory);
    size_t basename_length = strlen(basename);
    if (!basename_length || directory_length + basename_length + 32 >= sizeof(tx->staged)) {
#ifdef _WIN32
        _rmdir(tx->directory);
#else
        rmdir(tx->directory);
#endif
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tx->staged, tx->directory, directory_length);
    tx->staged[directory_length] = '/';
    memcpy(tx->staged + directory_length + 1, basename, basename_length + 1);
    tx->destination = destination;
    /* Invalidate stale results even when parsing subsequently fails. */
    if (remove(destination) != 0 && errno != ENOENT) {
        int saved = errno;
#ifdef _WIN32
        _rmdir(tx->directory);
#else
        rmdir(tx->directory);
#endif
        tx->destination = NULL;
        errno = saved;
        return -1;
    }
    return 0;
}

/* -1: publication failed; 0: complete; 1: artifact state is correct but a
 * temporary directory could not be removed. Cleanup is not a build failure. */
static int sts_output_finish(sts_output_transaction *tx, int success) {
    int result = 0;
    int saved = 0;
    if (success && rename(tx->staged, tx->destination) != 0) {
        result = -1;
        saved = errno;
    }
    if (!success || result != 0) remove(tx->staged);
#ifdef _WIN32
    int cleanup = _rmdir(tx->directory);
#else
    int cleanup = rmdir(tx->directory);
#endif
    if (cleanup != 0 && result == 0) { result = 1; saved = errno; }
    if (result != 0) errno = saved;
    return result;
}
#endif
