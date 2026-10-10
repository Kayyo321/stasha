/* Direct immutable local-plan tests. Synthetic invalid trees are queried only,
 * never emitted or executed. No child process or shared compiler object writes. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}checks++;}while(0)
static unsigned checks;
static const char *prefix="mod locals; type I: interface { m(void): i32; }; type S: struct.[I] { ext i32 n; ext fn m(void): i32 { ret this.n; } }; type T: struct { ext i32 n; }; type H: struct { ext S value; ext i32 flag; }; fn consume(stack I value): i32 { ret value.m(); } ";
static node_t *unit_named(cg_t *cg,const char *name) {
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){node_t *d=cg->ast->as.module.decls.items[i];if(d->kind==NodeFnDecl&&!strcmp(d->as.fn_decl.name,name))return d;}return Null;
}
static node_t *unit_init(cg_t *cg,const char *body) {
    memset(cg,0,sizeof(*cg));restore_diagnostic_counts(0,0);diag_clear_captured();diag_set_render_enabled(False);
    size_t bytes=strlen(prefix)+strlen(body)+1;heap_t text=allocate(bytes,1);snprintf(text.pointer,bytes,"%s%s",prefix,body);
    diag_register_source("local-unit.sts",text.pointer);diag_set_file("local-unit.sts");diag_set_source(text.pointer);
    cg->ast=cg->root_ast=parse(text.pointer);deallocate(text);
    if(!cg->ast||get_error_count())return Null;
    sts_target_init(&cg->target_spec,"x86_64-unknown-linux-gnu");
    cg->ctx=LLVMContextCreate();cg->module=LLVMModuleCreateWithNameInContext("locals",cg->ctx);cg->builder=LLVMCreateBuilderInContext(cg->ctx);
    cg->di_data_layout=LLVMCreateTargetData("e-p:64:64-i64:64-n8:16:32:64-S128");LLVMSetModuleDataLayout(cg->module,cg->di_data_layout);
    validate_interface_declarations(cg);iface_preflight(cg);
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){node_t *d=cg->ast->as.module.decls.items[i];
        if(d->kind==NodeTypeDecl&&d->as.type_decl.decl_kind==TypeDeclStruct){
            register_struct(cg,d->as.type_decl.name,LLVMStructCreateNamed(cg->ctx,d->as.type_decl.name),False);
            find_struct(cg,d->as.type_decl.name)->layout_decl=d;
        }
    }
    for(usize_t i=0;i<cg->struct_count;i++)cg_layout_struct(cg,cg->structs[i].name);
    LLVMTypeRef ft=LLVMFunctionType(LLVMInt32TypeInContext(cg->ctx),Null,0,0);
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeFnDecl)continue;
        LLVMValueRef f=LLVMAddFunction(cg->module,d->as.fn_decl.name,ft);iface_register_native(cg,d,Null,f,False,False);
        symtab_add(&cg->globals,d->as.fn_decl.name,f,ft,scalar_type(TypeI32),0);
    }
    node_t *owner=unit_named(cg,"main");cg->current_fn=symtab_lookup(&cg->globals,"main")->value;
    LLVMBasicBlockRef block=LLVMAppendBasicBlockInContext(cg->ctx,cg->current_fn,"entry");LLVMPositionBuilderAtEnd(cg->builder,block);
    return owner;
}
static void unit_cleanup(cg_t *cg) {
    iface_local_end(cg,Null);iface_source_free(cg);iface_free_contracts(cg);free_interface_registry(cg);
    symtab_free(&cg->locals);symtab_free(&cg->globals);
    for(usize_t i=0;i<cg->struct_count;i++)if(cg->structs[i].fields_heap.pointer)deallocate(cg->structs[i].fields_heap);
    if(cg->structs_heap.pointer)deallocate(cg->structs_heap);
    LLVMDisposeBuilder(cg->builder);LLVMDisposeModule(cg->module);LLVMDisposeTargetData(cg->di_data_layout);LLVMContextDispose(cg->ctx);ast_free_all();
}
static iface_local_node_t *unit_sink(iface_local_plan_t *p) {for(iface_local_node_t *r=p->nodes;r;r=r->next)if(r->sink)return r;return Null;}
int main(void) {
    struct {const char *body;iface_local_state_t state;} cases[]={
      {"fn main(void): i32 { stack S a=.{.n=41}; stack S b=a; a=b; b=a; ret consume(b); }",LocalClosed},
      {"fn main(void): i32 { stack S a; stack S b=a; a=b; ret consume(b); }",LocalUnsupported},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack H h=.{.value=a}; stack S b=h.value; ret consume(b); }",LocalClosed},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack H h=.{.value=a}; stack i32 *rw p=&h.flag; stack S b=h.value; ret consume(b); }",LocalUnsupported},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack S *rw p=&a; stack T t=.{.n=7}; a=t; ret consume(a); }",LocalMismatch},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack S values[2]=.{a,a}; stack S b=values[1]; ret consume(b); }",LocalClosed},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack S values[2]=.{a,a,a}; stack S b=values[1]; ret consume(b); }",LocalMismatch},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack S values[2]=.{a}; stack S b=values[0]; ret consume(b); }",LocalUnsupported},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack S values[2]=.{a,a}; stack S *rw p=values; stack S b=values[1]; ret consume(b); }",LocalUnsupported},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack T t=.{.n=7}; stack S b=(t=a); ret consume(b); }",LocalMismatch},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack T t=.{.n=7}; if (false) { a=t; } ret consume(a); }",LocalMismatch},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack T t=.{.n=7}; for (stack i32 i=0;i<2;i++) { a=t; } ret consume(a); }",LocalMismatch},
      {"fn main(void): i32 { stack S a=.{.n=41}; { stack T a=.{.n=7}; } ret consume(a); }",LocalClosed},
      {"fn main(void): i32 { stack S a=.{.n=41}; { stack S a=a; ret consume(a); } ret 0; }",LocalClosed},
      {"fn main(void): i32 { stack T t=.{.n=7}; stack S good=.{.n=41}; stack S [a,b]=t,good,good; ret consume(a); }",LocalMismatch},
      {"fn main(void): i32 { stack T t=.{.n=7}; stack S good=.{.n=41}; stack S [a,b,c]=t,good; ret consume(a); }",LocalMismatch},
      {"fn pair(void): [S,i32] {stack S a=.{.n=41};ret a,0;} fn main(void): i32 {stack let [a,n]=pair();stack T t=.{.n=7};a=t;ret a.I.m()+n;}",LocalMismatch},
      {"fn main(void): i32 { stack S a=.{.n=41}; stack i32 matrix[2][2]; stack i32 n=matrix[0][0]; ret consume(a); }",LocalClosed},
    };
    for(unsigned repeat=0;repeat<3;repeat++)for(usize_t i=0;i<sizeof(cases)/sizeof(*cases);i++){
        cg_t cg;node_t *owner=unit_init(&cg,cases[i].body);CHECK(owner&&!get_error_count());
        char *before=LLVMPrintModuleToString(cg.module);usize_t locals=cg.locals.count,globals=cg.globals.count;
        heap_t globals_snapshot=allocate(globals,sizeof(symbol_t));symbol_t *saved_globals=globals_snapshot.pointer;memcpy(saved_globals,cg.globals.entries,globals*sizeof(*saved_globals));
        iface_local_plan_t *previous=iface_local_begin(&cg,owner,owner->as.fn_decl.body);CHECK(!previous);
        iface_local_plan_t *p=cg.iface_local_plan;iface_local_node_t *sink=unit_sink(p);CHECK(p&&p->frozen&&!p->building&&sink&&sink->proof);
        if(sink->proof->state!=cases[i].state)fprintf(stderr,"case %zu state %d expected %d reason %s\n",i,sink->proof->state,cases[i].state,sink->proof->reason?sink->proof->reason:"none");
        CHECK(sink->proof->state==cases[i].state);CHECK(p->rejected==(cases[i].state==LocalMismatch));
        CHECK(cg.locals.count==locals&&cg.globals.count==globals);CHECK(!memcmp(saved_globals,cg.globals.entries,globals*sizeof(*saved_globals)));deallocate(globals_snapshot);CHECK(!LLVMGetFirstInstruction(LLVMGetInsertBlock(cg.builder)));
        char *after=LLVMPrintModuleToString(cg.module);CHECK(!strcmp(before,after));LLVMDisposeMessage(before);LLVMDisposeMessage(after);
        node_t duplicate=*sink->ast;CHECK(!iface_local_node(p,&duplicate,False));
        LLVMValueRef saved=cg.current_fn;cg.current_fn=symtab_lookup(&cg.globals,"consume")->value;CHECK(!iface_local_current(&cg));cg.current_fn=saved;
        unit_cleanup(&cg);
    }
    /* An exhausted inventory that never reached its sink cannot fall back. */
    {cg_t cg;node_t *owner=unit_init(&cg,"fn main(void): i32 { stack S a=.{.n=41}; ret consume(a); }");CHECK(owner&&!get_error_count());
        iface_local_begin(&cg,owner,owner->as.fn_decl.body);iface_local_plan_t *p=cg.iface_local_plan;iface_local_node_t *sink=unit_sink(p);CHECK(sink);
        p->incomplete=True;p->reason="deterministic unit budget exhaustion";node_t missed=*sink->ast;symbol_t sym=sink->path.root->symbol;
        CHECK(iface_local_eligibility(&cg,&missed,&sym)==-1);CHECK(get_error_count()>0);unit_cleanup(&cg);}
    /* Exhaust traversal before a later sink, rather than merely setting a flag. */
    {cg_t cg;node_t *owner=unit_init(&cg,"fn main(void): i32 { stack S a=.{.n=41}; ret consume(a); }");CHECK(owner&&!get_error_count());
        iface_local_plan_t *p=iface_local_zero(sizeof(*p));p->cg=&cg;p->context=cg.current_fn;p->owner=owner;p->body=owner->as.fn_decl.body;
        p->building=True;p->buckets=iface_local_zero(IfaceLocalBuckets*sizeof(*p->buckets));p->node_count=IfaceLocalMaxNodes-2;cg.iface_local_plan=p;
        iface_local_visit(p,p->body,0);p->building=False;p->frozen=True;CHECK(p->incomplete&&!p->proof_count);
        node_t *ret=p->body->as.block.stmts.items[1],*call=ret->as.ret_stmt.values.items[0],*use=call->as.call.args.items[0];
        CHECK(!iface_local_node(p,use,False));symbol_t actual=p->bindings->symbol;
        CHECK(iface_local_eligibility(&cg,use,&actual)==-1);CHECK(get_error_count()>0);unit_cleanup(&cg);}
    printf("PASS: %u immutable local binding/effect/transfer/purity checks\n",checks);return 0;
}
