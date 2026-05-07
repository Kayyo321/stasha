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

static boolean_t log_stderr_enabled = True;

void quit(result_t res) {
    res = scan_and_deallocate() == Ok ? res : Err;

    if (log_file)
        close_logger();

    if (log_stderr_enabled) {
        if (error_cnt > 0) {
            fprintf(stderr, "error: aborting due to %lu previous error%s",
                    (unsigned long)error_cnt, error_cnt == 1 ? "" : "s");
            if (warn_cnt > 0)
                fprintf(stderr, "; %lu warning%s emitted",
                        (unsigned long)warn_cnt, warn_cnt == 1 ? "" : "s");
            fprintf(stderr, "\n");
        } else if (warn_cnt > 0) {
            fprintf(stderr, "%lu warning%s emitted\n",
                    (unsigned long)warn_cnt, warn_cnt == 1 ? "" : "s");
        }
    }
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
    if (log_stderr_enabled) {
        fputs(msg_buffer, stderr); fputc('\n', stderr);
    }

    fflush(log_file);
    if (log_stderr_enabled)
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

void log_set_stderr_enabled(boolean_t enabled) {
    log_stderr_enabled = enabled;
}

boolean_t log_get_stderr_enabled(void) {
    return log_stderr_enabled;
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
