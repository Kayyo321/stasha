/* Source snippets have owned lifetimes independent of parser/import buffers. */
#include "../../src/common/common.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } checks++; } while (0)
int main(void) {
    unsigned checks=0;
    diag_set_render_enabled(False);
    char filename[]="temporary-import.sts";
    char source[]="original line\n";
    diag_register_source(filename,source);diag_set_file(filename);
    const char *stable_file=diag_get_file();
    memset(source,'x',sizeof(source)-1);memset(filename,'y',sizeof(filename)-1);
    CHECK(!strcmp(g_source,"original line\n"));
    CHECK(!strcmp(stable_file,"temporary-import.sts"));
    diag_begin_error("pending coordinates");diag_span(SRC_LOC(1,1,3),True,"pending label");
    diag_register_source(stable_file,"replacement line\n");
    CHECK(!strcmp(g_source,"replacement line\n"));
    diag_finish();CHECK(diag_get_captured_count()==1);
    CHECK(!strcmp(diag_get_captured(0)->filename,stable_file));
    CHECK(diag_get_captured(0)->diag.labels[0].loc.line==1);
    diag_register_source(stable_file,g_source);CHECK(!strcmp(g_source,"replacement line\n"));
    char borrowed[]="standalone source\n";diag_set_source(borrowed);memset(borrowed,'z',sizeof(borrowed)-1);
    CHECK(!strcmp(g_source,"standalone source\n"));
    for (unsigned i=0;i<70;i++) {
        char file[64],text[64];snprintf(file,sizeof(file),"file-%u.sts",i);snprintf(text,sizeof(text),"line-%u\n",i);
        diag_register_source(file,text);diag_set_file(file);CHECK(!strcmp(g_source,text));
    }
    diag_set_file("not-registered.sts");CHECK(g_source==NULL);
    diag_clear_sources();diag_clear_sources();CHECK(!g_source && !g_source_registry);
    CHECK(!strcmp(stable_file,"temporary-import.sts"));
    CHECK(!strcmp(diag_get_captured(0)->filename,stable_file));
    diag_set_file(stable_file);CHECK(!g_source);
    diag_register_source(stable_file,"new compilation\n");CHECK(!strcmp(g_source,"new compilation\n"));
    diag_clear_sources();diag_clear_sources();CHECK(!g_source_registry);
    /* Exit fallback is deliberately left a live entry to reclaim. */
    diag_register_source("exit.sts","exit fallback\n");
    printf("PASS: %u owned diagnostic source checks\n",checks);
    return 0;
}
