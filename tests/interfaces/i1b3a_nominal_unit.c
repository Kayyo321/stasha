/* I1b.3a pure raw views and exact physical mapping guards. Malformed AST/LLVM
 * controls below are queried only: no invalid tree is emitted or executed. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}checks++;}while(0)
static unsigned checks;
static node_t *named(cg_t *cg,node_kind_t kind,const char *name) {
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=kind)continue;
        const char *n=kind==NodeFnDecl?d->as.fn_decl.name:kind==NodeVarDecl?d->as.var_decl.name:d->as.type_decl.name;
        if(!strcmp(n,name))return d;
    }return Null;
}
static void clear_errors(void){restore_diagnostic_counts(0,0);diag_clear_captured();}
static boolean_t last_error(const char *text){
    const captured_diag_t *d=diag_get_captured(diag_get_captured_count()-1);
    return d&&strstr(d->diag.message,text)!=Null;
}
static const char *program=
 "mod nominal; type I: interface { get(void): i32; }; "
 "type S: struct.[I] { ext i32 n; ext fn get(void): i32 { ret this.n; } }; "
 "type T: struct { ext i32 n; }; type Alias: S; "
 "type G: @comptime[U] struct { ext U n; }; "
 "type G_G_i32: struct.[I] { ext i32 n; ext fn get(void): i32 { ret this.n; } }; "
 "stack S s; stack T t; stack Alias a; stack G.[i32] g; stack G_G_i32 spelling; "
 "stack let inferred=t; stack S *rw pointer; stack S array[2]; "
 "fn take(stack S value): i32 { ret 0; } fn make(void): T { ret T {.n=7}; } "
 "fn owner(void): S { take(t); ret t; }";
static node_t *setup(cg_t *cg) {
    memset(cg,0,sizeof(*cg));clear_errors();diag_set_render_enabled(False);
    diag_register_source("nominal-unit.sts",program);diag_set_file("nominal-unit.sts");diag_set_source(program);
    cg->ast=cg->root_ast=parse(program);if(!cg->ast||get_error_count())return Null;
    sts_target_init(&cg->target_spec,"x86_64-unknown-linux-gnu");
    cg->ctx=LLVMContextCreate();cg->module=LLVMModuleCreateWithNameInContext("nominal",cg->ctx);cg->builder=LLVMCreateBuilderInContext(cg->ctx);
    validate_interface_declarations(cg);
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeTypeDecl)continue;
        if(d->as.type_decl.decl_kind==TypeDeclAlias)register_alias(cg,d->as.type_decl.name,d->as.type_decl.alias_type);
        if(d->as.type_decl.decl_kind!=TypeDeclStruct||d->as.type_decl.type_param_count)continue;
        LLVMTypeRef type=LLVMStructCreateNamed(cg->ctx,d->as.type_decl.name),field=LLVMInt32TypeInContext(cg->ctx);
        LLVMStructSetBody(type,&field,1,False);register_struct(cg,d->as.type_decl.name,type,False);
        find_struct(cg,d->as.type_decl.name)->layout_decl=d;
    }
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeFnDecl)continue;
        LLVMTypeRef args[8];for(usize_t j=0;j<d->as.fn_decl.params.count;j++)args[j]=get_llvm_type(cg,d->as.fn_decl.params.items[j]->as.var_decl.type);
        LLVMTypeRef type=LLVMFunctionType(get_llvm_type(cg,d->as.fn_decl.return_types[0]),args,d->as.fn_decl.params.count,False);
        LLVMValueRef fn=LLVMAddFunction(cg->module,d->as.fn_decl.name,type);iface_register_native(cg,d,Null,fn,False,Null);
        symtab_add(&cg->globals,d->as.fn_decl.name,fn,Null,NO_TYPE,0);
        scalar_register_signature(cg,&cg->globals.entries[cg->globals.count-1],d);
    }
    node_t *owner=named(cg,NodeFnDecl,"owner");cg->current_fn=symtab_lookup(&cg->globals,"owner")->value;
    cg->current_return_types=owner->as.fn_decl.return_types;cg->current_return_count=1;
    LLVMPositionBuilderAtEnd(cg->builder,LLVMAppendBasicBlockInContext(cg->ctx,cg->current_fn,"entry"));
    for(usize_t i=0;i<cg->ast->as.module.decls.count;i++){
        node_t *d=cg->ast->as.module.decls.items[i];if(d->kind!=NodeVarDecl)continue;
        iface_source_record_t *r=iface_source_record_var(cg,d);
        symtab_add(&cg->locals,d->as.var_decl.name,Null,Null,d->as.var_decl.type,0);
        cg->locals.entries[cg->locals.count-1].iface_source=r;
    }
    iface_local_begin(cg,owner,owner->as.fn_decl.body);return owner;
}
static void cleanup(cg_t *cg){
    iface_local_end(cg,Null);iface_source_free(cg);iface_free_contracts(cg);free_interface_registry(cg);
    symtab_free(&cg->locals);symtab_free(&cg->globals);
    if(cg->structs_heap.pointer)deallocate(cg->structs_heap);if(cg->aliases_heap.pointer)deallocate(cg->aliases_heap);
    LLVMDisposeBuilder(cg->builder);LLVMDisposeModule(cg->module);LLVMContextDispose(cg->ctx);ast_free_all();
}
int main(void){
 for(unsigned repeat=0;repeat<4;repeat++){
    cg_t cg;node_t *owner=setup(&cg);CHECK(owner&&!get_error_count());
    node_t *S=named(&cg,NodeTypeDecl,"S"),*T=named(&cg,NodeTypeDecl,"T");const char *mapping=Null;
    node_t s={.kind=NodeIdentExpr,.as.ident={.name="s"}},t=s,a=s,g=s,spelling=s,inferred=s,unknown={.kind=NodeTernaryExpr};
    t.as.ident.name="t";a.as.ident.name="a";g.as.ident.name="g";spelling.as.ident.name="spelling";inferred.as.ident.name="inferred";
    symbol_t *symbol=iface_source_lookup(&cg,"s");symbol->iface_excluded_origin=IfaceOriginGeneric|IfaceOriginUnresolved;
    iface_source_record_t *record=symbol->iface_source;node_t before=*record->decl;
    char *ir=LLVMPrintModuleToString(cg.module);usize_t count=cg.locals.count;
    CHECK(iface_immediate_compare(&cg,&s,S,&mapping)==IfaceImmediateDeclaredNominalEqual);
    CHECK(iface_immediate_compare(&cg,&a,S,&mapping)==IfaceImmediateDeclaredNominalEqual);
    CHECK(iface_immediate_compare(&cg,&t,S,&mapping)==IfaceImmediateDefiniteNominalMismatch);
    CHECK(iface_immediate_compare(&cg,&g,S,&mapping)==IfaceImmediateDefiniteNominalMismatch);
    CHECK(iface_immediate_compare(&cg,&spelling,named(&cg,NodeTypeDecl,"G_G_i32"),&mapping)==IfaceImmediateDeclaredNominalEqual);
    CHECK(iface_immediate_compare(&cg,&g,named(&cg,NodeTypeDecl,"G_G_i32"),&mapping)==IfaceImmediateDefiniteNominalMismatch);
    CHECK(iface_immediate_compare(&cg,&inferred,S,&mapping)==IfaceImmediateDefiniteNominalMismatch);
    CHECK(iface_immediate_compare(&cg,&unknown,S,&mapping)==IfaceImmediateNoEvidence);
    CHECK(iface_immediate_compare(&cg,&unknown,Null,&mapping)==IfaceImmediateNotApplicable);
    CHECK(!mapping&&!get_error_count()&&!symbol->used&&symbol->iface_excluded_origin==3);
    CHECK(cg.locals.count==count&&!memcmp(&before,record->decl,sizeof(before)));
    char *after=LLVMPrintModuleToString(cg.module);CHECK(!strcmp(ir,after));LLVMDisposeMessage(after);LLVMDisposeMessage(ir);
    for(unsigned i=0;i<600;i++)symtab_add(&cg.locals,"growth",Null,Null,NO_TYPE,0);
    CHECK(iface_source_lookup(&cg,"s")->iface_source==record);
    CHECK(iface_immediate_compare(&cg,&s,S,&mapping)==IfaceImmediateDeclaredNominalEqual);
    symbol=iface_source_lookup(&cg,"inferred");iface_source_fact_t saved_fact=symbol->iface_source->fact;
    symbol->iface_source->fact=(iface_source_fact_t){.status=IfaceSourceExcluded,.contract.type={.base=TypeUser,.user_name="S"},.origin=IfaceOriginGeneric};
    CHECK(iface_immediate_compare(&cg,&inferred,S,&mapping)==IfaceImmediateDeclaredNominalEqual);
    symbol->iface_source->fact.status=IfaceSourceUnknown;
    CHECK(iface_immediate_compare(&cg,&inferred,S,&mapping)==IfaceImmediateNoEvidence);
    symbol->iface_source->fact=saved_fact;
    node_t ptr=s;ptr.as.ident.name="pointer";CHECK(iface_immediate_compare(&cg,&ptr,S,&mapping)==IfaceImmediateNoEvidence);
    ptr.as.ident.name="array";CHECK(iface_immediate_compare(&cg,&ptr,S,&mapping)==IfaceImmediateNoEvidence);
    cg.generic_n=1;cg.generic_params[0]="T";CHECK(iface_immediate_compare(&cg,&t,S,&mapping)==IfaceImmediateNoEvidence);cg.generic_n=0;
    type_info_t missing={.base=TypeUser,.user_name="Missing"};CHECK(!iface_immediate_type(&cg,missing).decl);
    node_t *call=owner->as.fn_decl.body->as.block.stmts.items[0]->as.expr_stmt.expr;
    node_t *ret=owner->as.fn_decl.body->as.block.stmts.items[1];
    symbol_t selected=*symtab_lookup(&cg.globals,"take");node_t *actual=call->as.call.args.items[0];
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("definite nominal mismatch"));clear_errors();
    CHECK(!iface_immediate_return(&cg,ret));CHECK(last_error("definite nominal mismatch"));clear_errors();
    CHECK(!iface_source_lookup(&cg,"t")->used);CHECK(!LLVMGetFirstInstruction(LLVMGetInsertBlock(cg.builder)));
    call->as.call.args.items[0]=&s;CHECK(iface_immediate_arguments(&cg,call,&selected,False));
    usize_t argc=call->as.call.args.count;call->as.call.args.count=0;
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("arity or identity"));clear_errors();call->as.call.args.count=argc;
    LLVMSetFunctionCallConv(selected.value,LLVMFastCallConv);
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("arity or identity"));clear_errors();LLVMSetFunctionCallConv(selected.value,LLVMCCallConv);
    unsigned attr_kind=LLVMGetEnumAttributeKindForName("inreg",5);
    LLVMAddAttributeAtIndex(selected.value,1,LLVMCreateEnumAttribute(cg.ctx,attr_kind,0));
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("formal/native ordinal"));clear_errors();LLVMRemoveEnumAttributeAtIndex(selected.value,1,attr_kind);
    LLVMAddAttributeAtIndex(cg.current_fn,LLVMAttributeReturnIndex,LLVMCreateEnumAttribute(cg.ctx,attr_kind,0));
    CHECK(!iface_immediate_return(&cg,ret));CHECK(last_error("body/result native"));clear_errors();LLVMRemoveEnumAttributeAtIndex(cg.current_fn,LLVMAttributeReturnIndex,attr_kind);
    type_info_t old_formal=selected.scalar_signature->params[0].type;selected.scalar_signature->params[0].type=(type_info_t){.base=TypeUser,.user_name="T"};
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("formal/native ordinal"));clear_errors();selected.scalar_signature->params[0].type=old_formal;
    selected.requires_receiver=True;CHECK(!iface_immediate_arguments(&cg,call,&selected,False));clear_errors();selected.requires_receiver=False;
    node_t synthetic=*call;CHECK(iface_immediate_arguments(&cg,&synthetic,&selected,False));
    cg.iface_local_plan->incomplete=True;CHECK(!iface_immediate_arguments(&cg,&synthetic,&selected,False));CHECK(last_error("correspondence is incomplete"));clear_errors();cg.iface_local_plan->incomplete=False;
    iface_local_node_t *ret_identity=iface_local_node(cg.iface_local_plan,ret,False);ret_identity->inline_carrier=1;CHECK(iface_immediate_return(&cg,ret));ret_identity->inline_carrier=0;
    type_info_t *saved_types=cg.current_return_types;type_info_t copied_type=*saved_types;cg.current_return_types=&copied_type;
    CHECK(!iface_immediate_return(&cg,ret));CHECK(last_error("body/result native"));clear_errors();cg.current_return_types=saved_types;
    node_t *saved_owner=cg.iface_local_plan->owner;cg.iface_local_plan->owner=Null;
    CHECK(!iface_immediate_return(&cg,ret));CHECK(last_error("body/result native"));clear_errors();
    node_t stale_owner=*owner;stale_owner.as.fn_decl.return_count=2;cg.iface_local_plan->owner=&stale_owner;
    CHECK(!iface_immediate_return(&cg,ret));CHECK(last_error("body/result native"));clear_errors();cg.iface_local_plan->owner=saved_owner;
    LLVMValueRef context=cg.current_fn;cg.current_fn=selected.value;CHECK(iface_immediate_return(&cg,ret));cg.current_fn=context;
    node_t *saved_body=cg.iface_local_plan->body;cg.iface_local_plan->body=ret;
    CHECK(!iface_immediate_return(&cg,ret));clear_errors();cg.iface_local_plan->body=saved_body;
    node_t source_call={.kind=NodeCallExpr,.as.call={.callee="make"}};
    CHECK(iface_immediate_compare(&cg,&source_call,S,&mapping)==IfaceImmediateDefiniteNominalMismatch&&!mapping);
    symbol_t *make=symtab_lookup(&cg.globals,"make");type_info_t old_result=make->scalar_signature->ret_type;make->scalar_signature->ret_type=(type_info_t){.base=TypeUser,.user_name="S"};
    CHECK(iface_immediate_compare(&cg,&source_call,S,&mapping)==IfaceImmediateNoEvidence&&mapping);mapping=Null;make->scalar_signature->ret_type=old_result;
    iface_native_t *take_native=iface_source_native(&cg,&selected);LLVMValueRef saved_function=take_native->function;
    LLVMTypeRef wrong_type=LLVMFunctionType(LLVMInt32TypeInContext(cg.ctx),(LLVMTypeRef[]){LLVMInt32TypeInContext(cg.ctx)},1,False);
    take_native->function=selected.value=LLVMAddFunction(cg.module,"wrong_physical",wrong_type);
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("formal/native ordinal"));clear_errors();take_native->function=selected.value=saved_function;
    iface_register_native(&cg,take_native->decl,Null,selected.value,False,Null);
    CHECK(!iface_immediate_arguments(&cg,call,&selected,False));CHECK(last_error("arity or identity"));clear_errors();
    call->as.call.args.items[0]=actual;CHECK(iface_immediate_type(&cg,(type_info_t){.base=TypeUser,.user_name="T"}).decl==T);
    cleanup(&cg);
 }
 printf("PASS: %u immediate nominal/purity/mapping checks\n",checks);return 0;
}
