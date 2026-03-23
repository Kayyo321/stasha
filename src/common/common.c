#define CommonAllowStdlibAllocators
#include "common.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum {
    LogMaxIndex = 9,
    LogDeleteIndex = 10,
};

static const char *LogDirectory = "logs";

enum {
    LogFlagNone = 0,
    LogFlagWarn = 1 << 0,
    LogFlagError = 1 << 1,
};

typedef struct heap_node_t {
    heap_t heap;
    struct heap_node_t *next;
    struct heap_node_t **prev_next;
} heap_list_t;

// Extern refs
void *const Null = 0;
const heap_t NullHeap = {.pointer = Null, .size = 0, .priv = Null};

static FILE *log_file = Null;
static char current_log_path[PATH_MAX] = {0};

static usize_t error_cnt = 0;
static usize_t warn_cnt = 0;

static heap_list_t *heap_list_head = Null;
static heap_list_t *heap_list_last = Null;

#include "logger.c"
#include "heap.c"
