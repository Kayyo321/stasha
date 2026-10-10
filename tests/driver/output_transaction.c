#define _GNU_SOURCE
#include "common/output_transaction.h"
#include <assert.h>

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
int main(void) {
    char dir[] = "/tmp/stasha-output-test-XXXXXX";
    assert(mkdtemp(dir));
    char input[512], output[512], alias[512];
    snprintf(input, sizeof(input), "%s/input.sts", dir);
    snprintf(output, sizeof(output), "%s/result", dir);
    snprintf(alias, sizeof(alias), "%s/alias", dir);
    write_text(input, "source");
    write_text(output, "stale");
    sts_output_transaction tx;
    assert(sts_output_begin(&tx, input, output) == 0);
    assert(access(output, F_OK) != 0);
    write_text(tx.staged, "partial");
    assert(sts_output_finish(&tx, 0) == 0);
    assert(access(output, F_OK) != 0);
    assert(access(tx.directory, F_OK) != 0);
    assert(sts_output_begin(&tx, input, output) == 0);
    write_text(tx.staged, "fresh");
    assert(sts_output_finish(&tx, 1) == 0);
    FILE *f = fopen(output, "r"); assert(f);
    char buf[16]; assert(fgets(buf, sizeof(buf), f)); fclose(f);
    assert(strcmp(buf, "fresh") == 0);
    assert(sts_output_begin(&tx, input, input) != 0);
    assert(link(input, alias) == 0);
    assert(sts_output_begin(&tx, input, alias) != 0);
    assert(sts_output_begin(&tx, input, dir) != 0);
    remove(alias);
    assert(symlink(input, alias) == 0);
    assert(sts_output_begin(&tx, input, alias) != 0);
    remove(alias);
    assert(sts_output_begin(&tx, input, output) == 0);
    /* A linker that says success without producing a file is still failure. */
    assert(sts_output_finish(&tx, 1) != 0);
    assert(access(output, F_OK) != 0);
    /* Unrelated cleanup failures must not turn a published result into a
     * failed build with a live destination. Report a cleanup warning instead. */
    assert(sts_output_begin(&tx, input, output) == 0);
    write_text(tx.staged, "fresh");
    char sidecar[600]; snprintf(sidecar, sizeof(sidecar), "%s/sidecar", tx.directory);
    write_text(sidecar, "leftover");
    assert(sts_output_finish(&tx, 1) == 1);
    assert(access(output, F_OK) == 0);
    remove(sidecar); assert(rmdir(tx.directory) == 0);
    remove(output);
    remove(input); assert(rmdir(dir) == 0);
    puts("output transaction: 9 checks passed");
}
