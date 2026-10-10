/* Promotion-disabled source/ownership/receipt tests. Only valid factory bodies
 * are lowered; malformed receipts below are internal compile-only mutations. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#ifndef FACTORY_UNIT_EXPECT_PROMOTION
#define FACTORY_UNIT_EXPECT_PROMOTION 0
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}++checks;}while(0)
static unsigned checks;
static const char *prefix="mod factoryunit; type I: interface { m(void): i32; }; type S: struct.[I] { ext i32 n; ext fn m(void): i32 { ret this.n; } }; type T: struct { ext i32 n; }; type H: struct { ext S value; ext i32 flag; }; fn consume(stack I i): i32 { ret i.m(); } ";
static const char *factory="int fn make(stack i32 n): H {stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret h;} ";
static const char *consumer="fn main(void): i32 {stack H h=make(17);stack H copy=h;stack let s=copy.value;ret consume(s);}";
static node_t *named(cg_t *cg,const char *name) {
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){node_t *d=cg->ast->as.module.decls.items[i];if(d->kind==NodeFnDecl&&!strcmp(d->as.fn_decl.name,name))return d;}return Null;
}
static int setup(cg_t *cg,const char *body) {
    memset(cg,0,sizeof(*cg));restore_diagnostic_counts(0,0);diag_clear_captured();diag_set_render_enabled(False);
    size_t size=strlen(prefix)+strlen(body)+1;heap_t source_heap=allocate(size,1);char *source=source_heap.pointer;snprintf(source,size,"%s%s",prefix,body);
    diag_register_source("factory-unit.sts",source);diag_set_file("factory-unit.sts");diag_set_source(source);
    cg->ast=cg->root_ast=parse(source);deallocate(source_heap);if(!cg->ast||get_error_count())return 0;
    sts_target_init(&cg->target_spec,"x86_64-unknown-linux-gnu");
    cg->ctx=LLVMContextCreate();cg->module=LLVMModuleCreateWithNameInContext("factoryunit",cg->ctx);cg->builder=LLVMCreateBuilderInContext(cg->ctx);
    cg->di_data_layout=LLVMCreateTargetData("e-p:64:64-i64:64-n8:16:32:64-S128");LLVMSetModuleDataLayout(cg->module,cg->di_data_layout);
    validate_interface_declarations(cg);iface_preflight(cg);
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeTypeDecl)continue;
        if(d->as.type_decl.decl_kind==TypeDeclAlias)register_alias(cg,d->as.type_decl.name,d->as.type_decl.alias_type);
        if(d->as.type_decl.decl_kind==TypeDeclStruct&&!d->as.type_decl.type_param_count){
            register_struct(cg,d->as.type_decl.name,LLVMStructCreateNamed(cg->ctx,d->as.type_decl.name),False);
            find_struct(cg,d->as.type_decl.name)->layout_decl=d;
        }
    }
    for(usize_t i=0;i<cg->struct_count;i++)cg_layout_struct(cg,cg->structs[i].name);
    LLVMTypeRef ptr=LLVMPointerTypeInContext(cg->ctx,0),pair[]={ptr,ptr};
    register_struct(cg,"I",LLVMStructCreateNamed(cg->ctx,"I"),False);struct_reg_t *ir=find_struct(cg,"I");
    LLVMStructSetBody(ir->llvm_type,pair,2,False);ir->is_interface=True;
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeFnDecl)continue;
        LLVMTypeRef params[8];if(d->as.fn_decl.params.count>8)return 0;
        for(usize_t j=0;j<d->as.fn_decl.params.count;j++)params[j]=get_llvm_type(cg,d->as.fn_decl.params.items[j]->as.var_decl.type);
        LLVMTypeRef type=LLVMFunctionType(get_llvm_type(cg,d->as.fn_decl.return_types[0]),params,d->as.fn_decl.params.count,False);
        LLVMValueRef fn=LLVMAddFunction(cg->module,d->as.fn_decl.name,type);
        if(d->as.fn_decl.linkage==LinkageInternal)LLVMSetLinkage(fn,LLVMInternalLinkage);
        iface_register_native(cg,d,Null,fn,False,Null);symtab_add(&cg->globals,d->as.fn_decl.name,fn,type,d->as.fn_decl.return_types[0],0);
        scalar_register_signature(cg,&cg->globals.entries[cg->globals.count-1],d);
    }
    return !get_error_count();
}
static void cleanup(cg_t *cg) {
    iface_local_end(cg,Null);iface_factory_dispose(cg);iface_source_free(cg);iface_free_contracts(cg);free_interface_registry(cg);
    symtab_free(&cg->locals);symtab_free(&cg->globals);
    for(usize_t i=0;i<cg->struct_count;i++)if(cg->structs[i].fields_heap.pointer)deallocate(cg->structs[i].fields_heap);
    if(cg->structs_heap.pointer)deallocate(cg->structs_heap);if(cg->aliases_heap.pointer)deallocate(cg->aliases_heap);
    if(cg->dtor_stack_heap.pointer)deallocate(cg->dtor_stack_heap);
    LLVMDisposeBuilder(cg->builder);LLVMDisposeModule(cg->module);LLVMDisposeTargetData(cg->di_data_layout);LLVMContextDispose(cg->ctx);ast_free_all();
}
static iface_local_plan_t *plan(cg_t *cg,const char *name){for(iface_local_plan_t *p=cg->iface_factories?cg->iface_factories->plans:Null;p;p=p->retained_next)if(!strcmp(p->owner->as.fn_decl.name,name))return p;return Null;}
static iface_local_node_t *sink(iface_local_plan_t *p){for(iface_local_node_t *r=p?p->nodes:Null;r;r=r->next)if(r->upgrade)return r;return Null;}
static void activate(cg_t *cg,iface_local_plan_t *p) {
    cg->current_fn=p->context;cg->current_return_types=p->owner->as.fn_decl.return_types;cg->current_return_count=p->owner->as.fn_decl.return_count;
    cg->current_fn_linkage=p->owner->as.fn_decl.linkage;cg->locals.count=0;cg->current_module_prefix[0]=0;
    LLVMPositionBuilderAtEnd(cg->builder,LLVMAppendBasicBlockInContext(cg->ctx,p->context,"entry"));
    for(usize_t i=0;i<p->owner->as.fn_decl.params.count;i++){
        node_t *d=p->owner->as.fn_decl.params.items[i];LLVMTypeRef type=get_llvm_type(cg,d->as.var_decl.type);
        LLVMValueRef slot=LLVMBuildAlloca(cg->builder,type,d->as.var_decl.name);LLVMBuildStore(cg->builder,LLVMGetParam(p->context,i),slot);
        symtab_add(&cg->locals,d->as.var_decl.name,slot,type,d->as.var_decl.type,0);cg->locals.entries[cg->locals.count-1].iface_source=iface_source_record_var(cg,d);
    }
    iface_local_begin(cg,p->owner,p->body);
}
static void emit_factory(cg_t *cg,iface_local_plan_t *p){activate(cg,p);gen_block(cg,p->body);iface_factory_finalize_body(cg,p->owner);iface_local_end(cg,Null);}
int main(void) {
    CHECK(IFACE_FACTORY_RESULT_PROMOTION==FACTORY_UNIT_EXPECT_PROMOTION);
    char program[32768];snprintf(program,sizeof(program),"%s%s",factory,consumer);
    for(unsigned repeat=0;repeat<4;repeat++){
        cg_t cg;CHECK(setup(&cg,program));char *before=LLVMPrintModuleToString(cg.module);
        usize_t globals=cg.globals.count;heap_t snapshot_heap=allocate(globals,sizeof(symbol_t));symbol_t *snapshot=snapshot_heap.pointer;memcpy(snapshot,cg.globals.entries,globals*sizeof(*snapshot));
        node_t ast_before=*cg.ast;iface_factory_prepare(&cg);
        CHECK(!get_error_count()&&cg.iface_factories&&!cg.iface_query_frame&&!cg.iface_local_plan);
        CHECK(!memcmp(snapshot,cg.globals.entries,globals*sizeof(*snapshot))&&!memcmp(&ast_before,cg.ast,sizeof(ast_before)));deallocate(snapshot_heap);
        char *after=LLVMPrintModuleToString(cg.module);CHECK(!strcmp(before,after));LLVMDisposeMessage(before);LLVMDisposeMessage(after);
        iface_local_plan_t *fp=plan(&cg,"make"),*cp=plan(&cg,"main");iface_local_node_t *use=sink(cp);
        CHECK(fp&&cp&&use&&use->proof&&use->upgrade);
        if(use->upgrade->state!=LocalClosed)fprintf(stderr,"upgrade state=%d reason=%s\n",use->upgrade->state,use->upgrade->reason?use->upgrade->reason:"none");
        CHECK(use->proof->state==LocalDeclared&&use->upgrade->state==LocalClosed&&!cp->rejected&&!fp->rejected);
        CHECK(cg.iface_factories->paths==1&&cg.iface_factories->bodies==1&&cg.iface_factories->request_count==1);
        for(unsigned i=0;i<600;i++)symtab_add(&cg.globals,"growth",Null,Null,NO_TYPE,0);
        CHECK(iface_factory_identity(&cg,fp)&&iface_factory_seal(&cg));
        /* Foreign contexts reuse exact native/AST pointers but cannot observe
         * or invalidate the owning body’s new physical receipts. */
        activate(&cg,fp);
        type_info_t foreign_types[1]={fp->owner->as.fn_decl.return_types[0]};
        type_info_t *types=cg.current_return_types;cg.current_return_types=foreign_types;
        CHECK(!iface_factory_current(&cg));
        for(iface_local_node_t *r=fp->nodes;r;r=r->next)if(r->constructor_needed){
            CHECK(!r->constructor_seen&&!r->physical_failure);
            CHECK(iface_local_constructor(&cg,r->ast,r->ast,iface_immediate_storage(&cg,r->constructor_target.contract.nominal)));
            CHECK(!r->constructor_seen&&!r->physical_failure);
            CHECK(iface_local_constructor(&cg,r->ast,r->ast,LLVMInt32TypeInContext(cg.ctx)));
            CHECK(!r->constructor_seen&&!r->physical_failure);
        }
        for(iface_local_binding_t *b=fp->bindings;b;b=b->next)if(b->local){
            CHECK(iface_local_declaration_storage(&cg,b->decl,LLVMInt32TypeInContext(cg.ctx)));
            CHECK(!b->storage_seen);
        }
        cg.current_return_types=types;iface_local_end(&cg,Null);
        LLVMDeleteBasicBlock(LLVMGetFirstBasicBlock(fp->context));
        if(repeat&1)emit_factory(&cg,fp);
        activate(&cg,cp);
        for(usize_t i=0;i+1<cp->body->as.block.stmts.count;i++)gen_stmt(&cg,cp->body->as.block.stmts.items[i]);
        symbol_t *symbol=iface_source_lookup(&cg,"s");CHECK(symbol&&!get_error_count());
        CHECK(cg.iface_factories->requests->emitted_count==1&&!cg.iface_factories->requests->consumed);
        CHECK(iface_factory_consume(&cg,cp,use,symbol)==FACTORY_UNIT_EXPECT_PROMOTION);
        CHECK(use->consumed==FACTORY_UNIT_EXPECT_PROMOTION);
        CHECK(iface_factory_require(&cg,cp,use,symbol)==1&&use->consumed&&cg.iface_factories->requests->consumed);
        LLVMBuildRet(cg.builder,LLVMConstInt(LLVMInt32TypeInContext(cg.ctx),0,0));iface_factory_finalize_body(&cg,cp->owner);iface_local_end(&cg,Null);
        if(!(repeat&1))emit_factory(&cg,fp);
        CHECK(!get_error_count()&&iface_factory_return_sealed(&cg,fp)&&iface_factory_seal(&cg));
        LLVMValueRef raw=fp->raw_return;fp->raw_return=LLVMConstNull(LLVMTypeOf(raw));CHECK(!iface_factory_return_sealed(&cg,fp));fp->raw_return=raw;
        iface_native_t duplicate=*iface_factory_identity(&cg,fp);duplicate.next=cg.iface_natives;cg.iface_natives=&duplicate;
        CHECK(!iface_factory_seal(&cg));cg.iface_natives=duplicate.next;restore_diagnostic_counts(0,0);diag_clear_captured();
        CHECK(iface_factory_seal(&cg));
        iface_factory_request_t *request=cg.iface_factories->requests;
        LLVMSetInstructionCallConv(request->emitted,LLVMFastCallConv);
        CHECK(!iface_factory_seal(&cg));restore_diagnostic_counts(0,0);diag_clear_captured();
        LLVMSetInstructionCallConv(request->emitted,LLVMCCallConv);CHECK(iface_factory_seal(&cg));
        unsigned attr=LLVMGetEnumAttributeKindForName("noundef",7);
        LLVMAddCallSiteAttribute(request->emitted,1,LLVMCreateEnumAttribute(cg.ctx,attr,0));
        CHECK(!iface_factory_seal(&cg));restore_diagnostic_counts(0,0);diag_clear_captured();
        LLVMRemoveCallSiteEnumAttribute(request->emitted,1,attr);CHECK(iface_factory_seal(&cg));
        LLVMBasicBlockRef extra=LLVMAppendBasicBlockInContext(cg.ctx,fp->context,"unreachable.zero");
        LLVMPositionBuilderAtEnd(cg.builder,extra);LLVMBuildRet(cg.builder,LLVMConstNull(LLVMTypeOf(fp->raw_return)));
        CHECK(!iface_factory_seal(&cg));restore_diagnostic_counts(0,0);diag_clear_captured();LLVMDeleteBasicBlock(extra);
        CHECK(iface_factory_seal(&cg));
        unsigned count=fp->returns_seen;fp->returns_seen=0;CHECK(!iface_factory_return_sealed(&cg,fp));fp->returns_seen=count;
        fp->factory_failure="forced unexpected nested cleanup registration";CHECK(!iface_factory_seal(&cg));
        fp->factory_failure=Null;restore_diagnostic_counts(0,0);diag_clear_captured();CHECK(iface_factory_seal(&cg));
        cleanup(&cg);CHECK(!cg.iface_factories&&!cg.iface_query_frame&&!cg.iface_local_plan);
    }
    struct {const char *factory;iface_local_state_t state;} cases[]={
        {"int fn make(stack i32 n): H {stack T t=.{.n=n};stack S s=t;stack H h=.{.value=s,.flag=5};ret h;}",LocalMismatch},
        {"fn child(stack i32 n): S {stack S s=.{.n=n};ret s;} int fn make(stack i32 n): H {stack S s=child(n);stack H h=.{.value=s,.flag=5};ret h;}",LocalUnsupported},
        {"int fn make(stack i32 n): H {stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};if(n>0){ret h;}else{ret h;}}",LocalUnsupported},
        {"int fn make(stack i32 n): H {stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret;}",LocalUnsupported},
        {"int fn make(stack i32 n): H {stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret h;stack i32 dead=2;}",LocalUnsupported},
        {"type G: @comptime[U] struct {ext U n;}; int fn make(stack i32 n): H {stack G.[i32] unrelated=.{.n=3};stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret h;}",LocalUnsupported},
        {"type E: enum {A,B}; int fn make(stack i32 n): H {stack E unrelated;stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret h;}",LocalUnsupported},
        {"int fn make(stack i32 n): H {stack i32 unrelated[2]=.{1,2};stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret h;}",LocalUnsupported},
        {"int fn make(stack i32 n): H {stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};{stack i32 nested=2;}ret h;}",LocalUnsupported},
        {"fn make(stack i32 n): H {stack S s=.{.n=n};stack H h=.{.value=s,.flag=5};ret h;}",LocalUnsupported},
        {"int fn make(stack i32 n): H {stack T t=.{.n=n};stack S s=t;stack H h=.{.value=s,.flag=5};stack i32 unrelated[2]=.{1,2};ret h;}",LocalMismatch},
        {"int fn make(stack i32 n): H {stack T t=.{.n=n};stack S s=t;stack H h=.{.value=s,.flag=5};s.I.m();ret h;}",LocalMismatch}
    };
    for(unsigned repeat=0;repeat<2;repeat++)for(usize_t i=0;i<sizeof(cases)/sizeof(*cases);i++){
        snprintf(program,sizeof(program),"%s%s",repeat?consumer:cases[i].factory,repeat?cases[i].factory:consumer);
        cg_t cg;CHECK(setup(&cg,program));usize_t before=cg.struct_count;char *ir=LLVMPrintModuleToString(cg.module);
        iface_factory_prepare(&cg);iface_local_plan_t *cp=plan(&cg,"main");iface_local_node_t *use=sink(cp);
        CHECK(cp&&use&&use->upgrade&&!get_error_count()&&!cp->rejected);
        if(use->upgrade->state!=cases[i].state)fprintf(stderr,"source case %zu state %d expected %d reason %s\n",i,use->upgrade->state,cases[i].state,use->upgrade->reason);
        CHECK(use->upgrade->state==cases[i].state&&use->proof->state==LocalDeclared);
        if(cases[i].state==LocalMismatch)CHECK(use->upgrade->reason&&strstr(use->upgrade->reason,"identity does not match"));
        CHECK(cg.struct_count==before&&!use->consumed&&iface_factory_seal(&cg));
        char *after=LLVMPrintModuleToString(cg.module);CHECK(!strcmp(ir,after));LLVMDisposeMessage(ir);LLVMDisposeMessage(after);cleanup(&cg);
    }
    /* Exhaust each new-only allocation/work budget without touching the
     * legacy proof state or diagnostics, then destroy the partial cache. */
    for(unsigned mode=0;mode<6;mode++){
        snprintf(program,sizeof(program),"%s%s",factory,consumer);cg_t cg;CHECK(setup(&cg,program));iface_factory_prepare(&cg);
        iface_factory_state_t *fs=cg.iface_factories;iface_local_plan_t *cp=plan(&cg,"main"),*fp=plan(&cg,"make");
        iface_local_node_t *use=sink(cp);CHECK(fs&&cp&&fp&&use);iface_local_state_t old=use->proof->state;
        iface_factory_request_t *req=fs->requests;node_t *field=fp->return_node?iface_local_node(fp,fp->return_operand,False)->path.root->fact.contract.nominal->as.type_decl.fields.items[1]:Null;
        if(mode==0){fs->request_count=FactoryMaxRequests;CHECK(iface_factory_value(cp,use->upgrade,req->call,req->source,Null,0));}
        if(mode==1){fs->paths=FactoryMaxPaths;CHECK(!iface_factory_summary(fp,&field,1));}
        if(mode==2){fs->bodies=FactoryMaxBodies;fp->factory=False;CHECK(!iface_factory_summary(fp,&field,1));}
        if(mode==3){fs->visits=FactoryMaxVisits;CHECK(!iface_factory_visit(&cg));}
        if(mode==4){fs->transient_nodes=FactoryMaxTransientNodes;fs->discovering=True;node_t dummy={.kind=NodeIntLitExpr};CHECK(!iface_local_node(cp,&dummy,True));fs->discovering=False;}
        if(mode==5){cp->overlay_building=True;cp->check_count=FactoryMaxChecks;
            iface_local_node_t *unrelated=iface_local_node(cp,cp->body,False);iface_local_check(cp,use->upgrade,unrelated,FactoryCheckExpression);
            CHECK(cp->overlay_incomplete&&!cp->incomplete);cp->overlay_building=False;}
        CHECK((mode==5||fs->exhausted)&&!get_error_count()&&!cp->rejected&&use->proof->state==old&&!use->consumed);
        cleanup(&cg);CHECK(!cg.iface_factories&&!cg.iface_query_frame&&!cg.iface_local_plan);
    }
    {snprintf(program,sizeof(program),"%s%s",factory,consumer);size_t len=strlen(program);
        for(unsigned i=0;i<FactoryMaxPlans+1;i++)len+=(size_t)snprintf(program+len,sizeof(program)-len,"fn unused%u(void): i32 {ret %u;} ",i,i);
        cg_t cg;CHECK(setup(&cg,program));iface_factory_prepare(&cg);
        CHECK(cg.iface_factory_exhausted&&!cg.iface_factories&&!get_error_count()&&!cg.iface_query_frame);
        cleanup(&cg);}
    {const char *two="type Pair: struct {ext S first;ext S second;}; int fn make(stack i32 n): Pair {stack S good=.{.n=n};stack T wrong=.{.n=29};stack Pair p=.{.first=good,.second=wrong};ret p;} fn main(void): i32 {stack Pair h=make(17);stack let a=h.first;stack let b=h.second;ret consume(a)+consume(b);}";
        cg_t cg;CHECK(setup(&cg,two));iface_factory_prepare(&cg);iface_local_plan_t *cp=plan(&cg,"main");unsigned closed=0,mismatch=0;
        for(iface_local_node_t *r=cp->nodes;r;r=r->next)if(r->upgrade){
            closed+=r->upgrade->state==LocalClosed;mismatch+=r->upgrade->state==LocalMismatch;CHECK(r->proof->state==LocalDeclared&&!r->consumed);}
        CHECK(closed==1&&mismatch==1&&cg.iface_factories->paths==2&&cg.iface_factories->request_count==2&&!get_error_count());
        CHECK(iface_factory_seal(&cg));cleanup(&cg);
    }
    {const char *direct="int fn make(stack i32 n): S {stack S s=.{.n=n};ret s;} fn main(void): i32 {stack S s=make(17);ret consume(s);}";
        cg_t cg;CHECK(setup(&cg,direct));iface_factory_prepare(&cg);
        CHECK(cg.iface_factories&&!cg.iface_factories->plans&&!cg.iface_factories->requests&&!cg.iface_factories->summaries&&!get_error_count());cleanup(&cg);
    }
    printf("PASS: %u factory result source/lifetime/receipt checks\n",checks);return 0;
}
