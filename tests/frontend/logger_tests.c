/* Run in a fresh working directory; compile with ASan+UBSan to guard severity
 * marker termination as well as exact message formatting. */
#define CommonAllowStdlibAllocators
#include "common/common.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    log_set_stderr_enabled(False);
    if (open_logger() != Ok) return 1;
    log_msg("plain %d", 7);
    log_warn("warning %d", 11);
    log_err("error %d", 13);
    char long_message[4096];
    memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    log_err("%s", long_message);
    if (close_logger() != Ok) return 2;

    FILE *file = fopen("logs/log0-last.log", "r");
    if (!file) return 3;
    char line[8192];
    int plain = 0, warning = 0, error = 0, truncated = 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "] plain 7\n")) plain += line[0] == '[';
        if (strstr(line, "] warning 11\n")) warning += strncmp(line, "(!!) [", 6) == 0;
        if (strstr(line, "] error 13\n")) error += strncmp(line, "(EE) [", 6) == 0;
        if (strstr(line, "] xxxxx")) {
            size_t size = strlen(line);
            truncated += strncmp(line, "(EE) [", 6) == 0 &&
                         size == 1048 && line[size - 1] == '\n';
        }
    }
    int failed = ferror(file);
    fclose(file);
    if (failed || plain != 1 || warning != 1 || error != 1 || truncated != 1) return 4;
    puts("logger severity and bounded formatting: PASS");
    return 0;
}
