/* Production graph helpers and repeated codegen in one process. No invalid
 * source is executed. LLVM is linked only for codegen/IR verification. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n", __LINE__, #x); return 1; } checks++; } while (0)
static unsigned checks;
static node_t *parse_source(const char *file, const char *source) {
    restore_diagnostic_counts(0,0); diag_clear_captured(); diag_set_render_enabled(False);
    diag_register_source(file,source); diag_set_file(file); diag_set_source(source);
    return parse(source);
}
static void init_graph(cg_t *cg, node_t *ast) {
    memset(cg,0,sizeof(*cg)); cg->ast = cg->root_ast = ast;
    sts_target_init(&cg->target_spec,"x86_64-unknown-linux-gnu");
    validate_interface_declarations(cg);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *diamond = "mod graph; type A: interface { a(void): i32; };"
        "type B: interface.[A] { b(void): i32; };"
        "type C: interface.[A] { c(void): i32; };"
        "type D: interface.[B,C] { d(void): i32; };";
    node_t *ast = parse_source("diamond.sts",diamond);
    CHECK(ast && !get_error_count());
    cg_t cg; init_graph(&cg,ast); CHECK(!get_error_count());
    usize_t index = find_interface_index(&cg,"D"); CHECK(index != (usize_t)-1);
    iface_reg_t *d = &cg.interfaces[index];
    CHECK(d->graph_state == IfaceGraphReady && d->group_count == 4 && d->path_depth == 3);
    CHECK(!d->signatures_ready);
    for (usize_t i=0;i<4;i++) {
        CHECK(d->groups[i].origin_count == 1 && d->groups[i].signature_pending);
        CHECK(d->groups[i].name[0] == "abcd"[i]);
    }
    iface_preflight(&cg); CHECK(!get_error_count()); CHECK(d->signatures_ready);
    char *names[4] = {0}; type_info_t returns[4];
    CHECK(collect_all_iface_methods(&cg,"D",names,returns,4) == 4);
    CHECK(collect_all_iface_methods(&cg,"D",names,returns,3) == 0);
    CHECK(get_error_count() == 1);
    restore_diagnostic_counts(0,0);
    char late_name[512]; memset(late_name,'S',sizeof(late_name)-1); late_name[511]=0;
    CHECK(!ensure_vtable(&cg,late_name,"D").success);
    CHECK(get_error_count() == 1); /* guarded before LLVM/name formatting */
    iface_free_contracts(&cg); free_interface_registry(&cg);
    CHECK(!cg.interfaces && !cg.iface_count && !cg.iface_cap && !cg.ifaces_heap.pointer);
    /* Same AST, fresh context: no static memoization or invalid collector state. */
    restore_diagnostic_counts(0,0); init_graph(&cg,ast); CHECK(!get_error_count());
    CHECK(cg.interfaces[find_interface_index(&cg,"D")].group_count == 4);
    iface_free_contracts(&cg); free_interface_registry(&cg); ast_free_all();

    ast = parse_source("origins.sts","mod origins; type A: interface { m(void): i32; };"
        "type B: interface { m(void): u64; }; type C: interface.[A,B] { m(void): f64; };");
    CHECK(ast && !get_error_count()); init_graph(&cg,ast); CHECK(!get_error_count());
    d = &cg.interfaces[find_interface_index(&cg,"C")];
    CHECK(d->group_count == 1 && d->groups[0].origin_count == 3);
    CHECK(d->groups[0].origins[0]->as.fn_decl.return_types[0].base == TypeI32);
    CHECK(d->groups[0].origins[1]->as.fn_decl.return_types[0].base == TypeU64);
    CHECK(d->groups[0].origins[2]->as.fn_decl.return_types[0].base == TypeF64);
    CHECK(collect_all_iface_methods(&cg,"C",names,returns,4) == 0);
    CHECK(get_error_count() == 1);
    restore_diagnostic_counts(0,0);
    iface_preflight(&cg); CHECK(get_error_count() == 4); CHECK(!d->signatures_ready);
    iface_free_contracts(&cg); free_interface_registry(&cg); ast_free_all();

    /* A new module reuses bare names with a different topology and slot order. */
    for (int pass=0;pass<4;pass++) {
        const char *source = pass % 2
            ? "mod second; type A: interface { z(void): i32; }; fn f(stack A v): i32 { ret v.z(); }"
            : "mod first; type A: interface { x(void): i32; y(void): i32; }; fn f(stack A v): i32 { ret v.y(); }";
        ast = parse_source(pass % 2 ? "second.sts" : "first.sts",source);
        CHECK(ast && !get_error_count());
        CHECK(codegen(ast,argv[1],False,"x86_64-unknown-linux-gnu",
                      pass % 2 ? "second.sts" : "first.sts",False,0,True) == Ok);
        CHECK(!get_error_count());
        FILE *file = fopen(argv[1],"rb"); CHECK(file);
        char ir[32768]; usize_t n=fread(ir,1,sizeof(ir)-1,file); ir[n]=0; fclose(file);
        CHECK(strstr(ir,pass%2 ? "getelementptr ptr, ptr %iface_vt, i64 0" : "getelementptr ptr, ptr %iface_vt, i64 1") ||
              strstr(ir,pass%2 ? "getelementptr ptr, ptr %iface_vt, i32 0" : "getelementptr ptr, ptr %iface_vt, i32 1"));
        ast_free_all();
    }
    remove(argv[1]);
    printf("PASS: %u graph/lifetime checks (four codegen invocations in one process)\n",checks);
    return 0;
}
