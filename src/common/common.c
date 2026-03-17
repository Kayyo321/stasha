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

static int create_log_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static result_t ensure_log_directory(void) {
    struct stat path_stat = {0};
    if (stat(LogDirectory, &path_stat) == 0) {
        if (S_ISDIR(path_stat.st_mode))
            return Ok;

        fprintf(stderr, "Log path '%s' exists but is not a directory\n", LogDirectory);
        return Err;
    }

    if (create_log_directory(LogDirectory) != 0) {
        fprintf(stderr, "Failed to create log directory '%s': %s\n", LogDirectory, strerror(errno));
        return Err;
    }

    return Ok;
}

static void format_timestamp(time_t value, char *buffer, usize_t buffer_size) {
    struct tm tm_info = {0};
#ifdef _WIN32
    if (localtime_s(&tm_info, &value) != 0) {
#else
    if (!localtime_r(&value, &tm_info)) {
#endif
        if (snprintf(buffer, buffer_size, "unknown") >= (int)buffer_size)
            buffer[0] = '\0';
        return;
    }

    strftime(buffer, buffer_size, "%Y-%m-%d_%H-%M-%S", &tm_info);
}

static result_t build_log_path(int index, const char *timestamp, char *out_path, usize_t out_size) {
    if (!out_path || out_size == 0)
        return Err;

    if (index == 0) {
        if (snprintf(out_path, out_size, "%s/log0-last.log", LogDirectory) >= (int)out_size)
            return Err;
        return Ok;
    }

    if (!timestamp || timestamp[0] == '\0')
        return Err;

    if (snprintf(out_path, out_size, "%s/log%d-%s.log", LogDirectory, index, timestamp) >= (int)out_size)
        return Err;

    return Ok;
}

static boolean_t try_extract_timestamp_from_name(const char *file_name, int index, char *out_timestamp, usize_t out_size) {
    if (!file_name || !out_timestamp || out_size == 0 || index <= 0)
        return False;

    char prefix[32] = {0};
    if (snprintf(prefix, sizeof(prefix), "log%d-", index) >= (int)sizeof(prefix))
        return False;

    const usize_t prefix_len = strlen(prefix);
    const usize_t name_len = strlen(file_name);
    if (name_len <= prefix_len + 4)
        return False;

    if (strncmp(file_name, prefix, prefix_len) != 0)
        return False;

    if (strcmp(file_name + name_len - 4, ".log") != 0)
        return False;

    const usize_t ts_len = name_len - prefix_len - 4;
    if (ts_len + 1 > out_size)
        return False;

    memcpy(out_timestamp, file_name + prefix_len, ts_len);
    out_timestamp[ts_len] = '\0';
    return True;
}

static boolean_t find_log_file_for_index(int index, char *out_path, usize_t out_size, char *out_name, usize_t out_name_size) {
    if (!out_path || out_size == 0)
        return False;

    DIR *directory = opendir(LogDirectory);
    if (!directory)
        return False;

    char best_match_name[PATH_MAX] = {0};
    char exact_name[32] = {0};
    if (index == 0)
        snprintf(exact_name, sizeof(exact_name), "log0-last.log");

    struct dirent *entry = Null;
    while ((entry = readdir(directory)) != Null) {
        if (entry->d_name[0] == '.')
            continue;

        if (index == 0) {
            if (strcmp(entry->d_name, "log0") == 0 || strcmp(entry->d_name, "log0.log") == 0) {
                snprintf(best_match_name, sizeof(best_match_name), "%s", entry->d_name);
                continue;
            }

            if (strcmp(entry->d_name, exact_name) == 0) {
                snprintf(best_match_name, sizeof(best_match_name), "%s", entry->d_name);
                break;
            }

            if (strncmp(entry->d_name, "log0-", 5) == 0) {
                snprintf(best_match_name, sizeof(best_match_name), "%s", entry->d_name);
                continue;
            }

            continue;
        }

        char prefix[32] = {0};
        if (snprintf(prefix, sizeof(prefix), "log%d", index) >= (int)sizeof(prefix))
            continue;

        const usize_t prefix_len = strlen(prefix);
        if (strncmp(entry->d_name, prefix, prefix_len) != 0)
            continue;

        const char next_char = entry->d_name[prefix_len];
        if (next_char != '\0' && next_char != '-' && next_char != '.')
            continue;

        snprintf(best_match_name, sizeof(best_match_name), "%s", entry->d_name);
        if (next_char == '-')
            break;
    }

    boolean_t found = False;
    if (best_match_name[0] != '\0') {
        if (snprintf(out_path, out_size, "%s/%s", LogDirectory, best_match_name) < (int)out_size) {
            if (out_name && out_name_size > 0)
                snprintf(out_name, out_name_size, "%s", best_match_name);
            found = True;
        }
    }

    closedir(directory);
    return found;
}

static result_t rotate_logs(void) {
    for (int index = LogMaxIndex; index >= 0; --index) {
        char source_path[PATH_MAX] = {0};
        char source_name[PATH_MAX] = {0};
        if (!find_log_file_for_index(index, source_path, sizeof(source_path), source_name, sizeof(source_name)))
            continue;

        if (index + 1 >= LogDeleteIndex) {
            if (remove(source_path) != 0 && errno != ENOENT) {
                fprintf(stderr, "Failed to remove old log '%s': %s\n", source_path, strerror(errno));
                return Err;
            }
            continue;
        }

        char timestamp[32] = {0};
        if (index == 0) {
            struct stat source_stat = {0};
            if (stat(source_path, &source_stat) != 0) {
                fprintf(stderr, "Failed to stat log '%s': %s\n", source_path, strerror(errno));
                return Err;
            }

            format_timestamp(source_stat.st_mtime, timestamp, sizeof(timestamp));
        } else {
            if (!try_extract_timestamp_from_name(source_name, index, timestamp, sizeof(timestamp))) {
                struct stat source_stat = {0};
                if (stat(source_path, &source_stat) != 0) {
                    fprintf(stderr, "Failed to stat log '%s': %s\n", source_path, strerror(errno));
                    return Err;
                }

                format_timestamp(source_stat.st_mtime, timestamp, sizeof(timestamp));
            }
        }

        char destination_path[PATH_MAX] = {0};
        if (build_log_path(index + 1, timestamp, destination_path, sizeof(destination_path)) != Ok) {
            fprintf(stderr, "Failed to build rotated log path for index %d\n", index + 1);
            return Err;
        }

        if (remove(destination_path) != 0 && errno != ENOENT) {
            fprintf(stderr, "Failed to remove previous rotated log '%s': %s\n", destination_path, strerror(errno));
            return Err;
        }

        if (rename(source_path, destination_path) != 0) {
            fprintf(stderr, "Failed to rotate log '%s' -> '%s': %s\n", source_path, destination_path, strerror(errno));
            return Err;
        }
    }

    return Ok;
}

static result_t prepare_log_path(void) {
    if (ensure_log_directory() != Ok)
        return Err;

    if (rotate_logs() != Ok)
        return Err;

    if (build_log_path(0, Null, current_log_path, sizeof(current_log_path)) != Ok) {
        fprintf(stderr, "Log path is too long\n");
        return Err;
    }

    if (remove(current_log_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "Failed to remove previous log '%s': %s\n", current_log_path, strerror(errno));
        return Err;
    }

    return Ok;
}

static const char *log_path(void) {
    return current_log_path;
}

static void timestamp(char *buffer, size_t buf_size) {
    time_t now = time(Null);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, buf_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

void quit(result_t res) {
    if (log_file)
        close_logger();

    res = scan_and_deallocate() == Ok ? res : Err;

    fprintf(stderr, "warn count: %lu, error count: %lu\n", warn_cnt, error_cnt);
    fprintf(stderr, "exited with code %d\n", res);
    exit(res);
}

result_t open_logger(void) {
    if (prepare_log_path() != Ok)
        return Err;

    log_file = fopen(log_path(), "a");
    if (!log_file) {
        fprintf(stderr, "Failed to open log file: %s\n", log_path());
        return Err;
    }

    //Insert a header to the log file, including the time it was opened
    char time_buffer[20];
    timestamp(time_buffer, sizeof(time_buffer));
    fprintf(log_file, "=== Log opened at %s ===\n", time_buffer);

    return Ok;
}

result_t close_logger(void) {
    if (!log_file) 
        return Err;

    //Insert a footer to the log file, including the time it was closed
    char time_buffer[20];
    timestamp(time_buffer, sizeof(time_buffer));
    fprintf(log_file, "=== Log closed at %s ===\n", time_buffer);

    fclose(log_file);
    log_file = Null;

    return Ok;
}

static result_t log_message(const char *fmt, va_list args, int log_flags) {
    if (!log_file) 
        return Err;

    // convert message to string and prepend timestamp
    char time_buffer[20];
    timestamp(time_buffer, sizeof(time_buffer));

    char footer[4];
    if (log_flags & LogFlagError) {
        memmove(footer, "(EE)", 4);
    } else if (log_flags & LogFlagWarn) {
        memmove(footer, "(!!)", 4);
    } else {
        footer[0] = '\0';
    }

    char msg_buffer[1024 + 20 + 4];
    vsnprintf(msg_buffer, sizeof(msg_buffer), fmt, args);

    // add timestamp inbetween footer (if any) and message


    if (footer[0] != '\0') {
        char temp_buffer[sizeof(msg_buffer)];
        snprintf(temp_buffer, sizeof(temp_buffer), "%s [%s] %s", footer, time_buffer, msg_buffer);
        memmove(msg_buffer, temp_buffer, sizeof(msg_buffer));
    } else {
        char temp_buffer[sizeof(msg_buffer)];
        snprintf(temp_buffer, sizeof(temp_buffer), "[%s] %s", time_buffer, msg_buffer);
        memmove(msg_buffer, temp_buffer, sizeof(msg_buffer));
    }

    fputs(msg_buffer, log_file); fputc('\n', log_file);
    fputs(msg_buffer, stderr); fputc('\n', stderr);

    fflush(log_file);
    fflush(stderr);

    error_cnt += (log_flags & LogFlagError);
    warn_cnt += (log_flags & LogFlagWarn);

    return Ok;
}

void log_msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    log_message(fmt, args, LogFlagNone);
    
    va_end(args);
}

void log_warn(const char *fmt, ...){
    va_list args;
    va_start(args, fmt);

    log_message(fmt, args, LogFlagWarn);
    
    va_end(args);
}

void log_err(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    log_message(fmt, args, LogFlagError);
    
    va_end(args);
}

usize_t get_error_count(void){
    return error_cnt;
}

usize_t get_warn_count(void){
    return warn_cnt;
}

#ifdef Testing
void restore_diagnostic_counts(usize_t warn_count, usize_t error_count) {
    warn_cnt = warn_count;
    error_cnt = error_count;
}
#endif // Testing

static void ensure_heap_list_integrity(void) {
    if (!heap_list_head) {
        heap_list_head = (heap_list_t *)malloc(sizeof(heap_list_t));
        if (!heap_list_head) {
            log_err("Failed to allocate memory for heap list head.");
            quit(Err);
        }
        heap_list_head->heap = NullHeap;
        heap_list_head->next = Null;
    }

    // configure last pointer if it's not set
    if (!heap_list_last) {
        heap_list_t *current = heap_list_head;
        while (current->next) {
            current = current->next;
        }
        heap_list_last = current;
    }
}

static heap_list_t *get_new_node(void) {
    ensure_heap_list_integrity();

    // change the heap_last to point to a new node, then move the heap_last to the new nodes next pointer
    heap_list_t *new_node = (heap_list_t *)malloc(sizeof(heap_list_t));
    if (!new_node) {
        log_err("Failed to allocate memory for new heap node.");
        quit(Err);
    }
    new_node->heap = NullHeap;
    new_node->next = Null;
    new_node->prev_next = &heap_list_last->next;

    heap_list_last->next = new_node;
    heap_list_last = new_node;

    return new_node;
}

heap_t allocate(usize_t count, usize_t bytes) {
    heap_list_t *node = get_new_node();

    const usize_t total_size = count * bytes;

    node->heap.pointer = malloc(total_size);
    node->heap.size = total_size;
    if (!node->heap.pointer) {
        log_err("Failed to allocate memory: requested %lu bytes.", total_size);
        quit(Err);
    }

    node->heap.priv = node; // private data is a pointer to the node itself, so we can find it when we need to reallocate or deallocate

    return node->heap;
}

heap_t reallocate(heap_t heap, usize_t new_size) {
    if (!heap.pointer) {
        log_err("Cannot reallocate a null pointer.");
        quit(Err);
    }

    heap_list_t *node = (heap_list_t *)heap.priv;
    if (!node) {
        log_err("Invalid heap provided for reallocation.");
        quit(Err);
    }

    void *new_ptr = realloc(node->heap.pointer, new_size);
    if (!new_ptr) {
        log_err("Failed to reallocate memory: requested %lu bytes.", new_size);
        quit(Err);
    }

    node->heap.pointer = new_ptr;
    node->heap.size = new_size;

    return node->heap;
}

static void wipe_memory(void *pointer, usize_t size) {
    #if !defined(Debug) && !defined(Testing)
        memset(pointer, 0, size); // 0 out all allocations on release builds
    #endif
}

void deallocate(heap_t heap) {
    // deallocate the heap and remove it from the list
    if (!heap.pointer) {
        log_err("Cannot deallocate a null pointer.");
        quit(Err);
    }

    heap_list_t *node = (heap_list_t *)heap.priv;
    if (!node) {
        log_err("Invalid heap provided for deallocation.");
        quit(Err);
    }

    wipe_memory(node->heap.pointer, node->heap.size);

    free(node->heap.pointer);
    node->heap.pointer = Null;
    node->heap.size = 0;
    node->heap.priv = Null;

    if (!node->prev_next || *node->prev_next != node) {
        log_err("Heap list corruption detected during deallocation.");
        quit(Err);
    }

    *node->prev_next = node->next;
    if (node->next) {
        node->next->prev_next = node->prev_next;
    } else if (node->prev_next == &heap_list_head->next) {
        heap_list_last = heap_list_head;
    } else {
        heap_list_t *prev = (heap_list_t *)((char *)node->prev_next - offsetof(heap_list_t, next));
        heap_list_last = prev;
    }

    free(node);
}

result_t scan_and_deallocate(void) {
    ensure_heap_list_integrity();

    heap_list_t *current = heap_list_head->next;
    usize_t reclaimed_bytes = 0;

    while (current) {
        heap_list_t *next = current->next;
        if (current->heap.pointer) {
            log_warn("Memory leak detected: %lu bytes at %p", current->heap.size, current->heap.pointer);
            reclaimed_bytes += current->heap.size;
            deallocate(current->heap);
        }
        current = next;
    }

    return (reclaimed_bytes > 0) ? Err : Ok;
}