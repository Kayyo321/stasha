/* Pure source facts: original declaration/nominal identity, shape and access.
 * Invalid source is queried only, never emitted or executed. */
#define _GNU_SOURCE
#include "../../src/codegen/codegen.c"
#include "parser/parser.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}checks++;}while(0)
static unsigned checks;
static node_t *source(const char *text){restore_diagnostic_counts(0,0);diag_clear_captured();diag_set_render_enabled(False);diag_register_source("source-fact.sts",text);diag_set_file("source-fact.sts");diag_set_source(text);return parse(text);}
static node_t *named(node_t *ast,const char *name){for(usize_t i=0;i<ast->as.module.decls.count;i++){node_t *d=ast->as.module.decls.items[i];if(d->kind==NodeVarDecl&&!strcmp(d->as.var_decl.name,name))return d;}return Null;}
static void bind(cg_t *cg,node_t *d){iface_source_record_t *r=iface_source_record_var(cg,d);symtab_add(&cg->locals,d->as.var_decl.name,Null,Null,d->as.var_decl.type,0);cg->locals.entries[cg->locals.count-1].iface_source=r;}
int main(void){
 for(unsigned repeat=0;repeat<4;repeat++){
  cg_t cg={0};cg.ast=cg.root_ast=source("mod facts; type E: enum { A,B }; type F: enum { A,B }; type EA: E; type S: struct { ext i32 v; }; type Callback: fn*(stack i32): i32; type H: struct { ext Callback callback; }; stack H h; stack EA e=E.B; stack F f=F.B; stack i32 *r array[3]; stack i32 n=41; const i32 c=41; stack i32 *w p=nil; stack i32 *rw q=nil;");CHECK(cg.ast&&!get_error_count());sts_target_init(&cg.target_spec,"x86_64-unknown-linux-gnu");
  cg.ctx=LLVMContextCreate();cg.module=LLVMModuleCreateWithNameInContext("facts",cg.ctx);cg.builder=LLVMCreateBuilderInContext(cg.ctx);LLVMTypeRef ft=LLVMFunctionType(LLVMVoidTypeInContext(cg.ctx),Null,0,0);cg.current_fn=LLVMAddFunction(cg.module,"scope",ft);LLVMBasicBlockRef bb=LLVMAppendBasicBlockInContext(cg.ctx,cg.current_fn,"entry");LLVMPositionBuilderAtEnd(cg.builder,bb);
  for(usize_t i=0;i<cg.ast->as.module.decls.count;i++){node_t *d=cg.ast->as.module.decls.items[i];if(d->kind==NodeVarDecl)bind(&cg,d);}
  node_t e={.kind=NodeIdentExpr,.as.ident={.name="e"}},f={.kind=NodeIdentExpr,.as.ident={.name="f"}};
  symbol_t *es=iface_source_lookup(&cg,"e");iface_source_record_t *identity=es->iface_source;type_info_t before=identity->decl->as.var_decl.type;usize_t count=cg.locals.count;
  iface_source_fact_t ef=iface_source_expr_at(&cg,&e,NO_TYPE,0),ff=iface_source_expr_at(&cg,&f,NO_TYPE,0);
  CHECK(ef.status==IfaceSourceKnown&&ff.status==IfaceSourceKnown);CHECK(ef.contract.nominal!=ff.contract.nominal);CHECK(!es->used);CHECK(cg.locals.count==count);CHECK(LLVMGetFirstInstruction(bb)==Null);CHECK(LLVMGetInsertBlock(cg.builder)==bb);CHECK(!memcmp(&before,&identity->decl->as.var_decl.type,sizeof(before)));
  for(unsigned i=0;i<600;i++)symtab_add(&cg.locals,"growth",Null,Null,NO_TYPE,0);
  CHECK(iface_source_lookup(&cg,"e")->iface_source==identity);CHECK(iface_source_expr_at(&cg,&e,NO_TYPE,0).contract.nominal==ef.contract.nominal);
  node_t index={.kind=NodeIntLitExpr,.as.int_lit={.value=0}},array={.kind=NodeIdentExpr,.as.ident={.name="array"}},element={.kind=NodeIndexExpr,.as.index_expr={.object=&array,.index=&index}};
  iface_source_fact_t af=iface_source_expr_at(&cg,&array,NO_TYPE,0),elem=iface_source_expr_at(&cg,&element,NO_TYPE,0);
  CHECK(af.dimensions==1&&af.contract.type.ptr_depth==1);CHECK(elem.dimensions==0&&elem.contract.type.ptr_depth==1);
  node_t n={.kind=NodeIdentExpr,.as.ident={.name="n"}},c={.kind=NodeIdentExpr,.as.ident={.name="c"}},addr={.kind=NodeAddrOf,.as.addr_of={.operand=&n}};
  iface_source_fact_t address=iface_source_expr_at(&cg,&addr,NO_TYPE,0);CHECK(address.contract.type.ptr_perm==(PtrReadWrite|PtrArith));CHECK(address.contract.domain==StorageStack);addr.as.addr_of.operand=&c;address=iface_source_expr_at(&cg,&addr,NO_TYPE,0);CHECK(address.contract.type.ptr_perm==(PtrRead|PtrArith));
  node_t p={.kind=NodeIdentExpr,.as.ident={.name="p"}},deref={.kind=NodeUnaryPrefixExpr,.as.unary={.op=TokStar,.operand=&p}};
  iface_source_fact_t write=iface_source_expr_at(&cg,&deref,NO_TYPE,0);CHECK(write.status==IfaceSourceKnown&&write.access==PtrWrite);CHECK(!iface_source_primitive(write));addr.as.addr_of.operand=&deref;address=iface_source_expr_at(&cg,&addr,NO_TYPE,0);CHECK(address.contract.type.ptr_perm==PtrWrite);
  const char *why=Null;iface_type_t from=address.contract,to=from;to.type.ptr_perm=PtrRead;to.type.ptr_perms[0]=PtrRead;CHECK(!iface_source_pointer_compatible(from,to,&why));
  node_t unknown={.kind=NodeFutureOp};CHECK(iface_source_expr_at(&cg,&unknown,NO_TYPE,0).status==IfaceSourceUnknown);CHECK(iface_source_expr_at(&cg,&n,NO_TYPE,257).status==IfaceSourceUnknown);
  /* Unsupported source metadata is a quiet Unknown, even through aliases and
   * members on ordinary calls. Signature construction below still diagnoses. */
  usize_t errors=get_error_count();
  node_t h={.kind=NodeIdentExpr,.as.ident={.name="h"}},member={.kind=NodeMemberExpr,.as.member_expr={.object=&h,.field="callback"}};
  iface_source_fact_t unsupported=iface_source_expr_at(&cg,&member,NO_TYPE,0);
  CHECK(unsupported.status==IfaceSourceUnknown);CHECK(unsupported.reason&&strstr(unsupported.reason,"callback/closure"));CHECK(get_error_count()==errors);CHECK(!iface_source_lookup(&cg,"h")->used);
  int unsupported_types[]={TypeFnPtr,TypeClosure,TypeError,TypeFuture,TypeStream,TypeVa,TypeZone,TypeInfer};
  for(usize_t i=0;i<sizeof(unsupported_types)/sizeof(*unsupported_types);i++){
    unsupported=iface_source_from_type(&cg,(type_info_t){.base=unsupported_types[i]},StorageDefault,Null);
    CHECK(unsupported.status==IfaceSourceUnknown&&unsupported.reason);CHECK(get_error_count()==errors);
  }
  node_t *seen[IfaceMaxMetadataDepth];iface_type_t concrete;type_info_t st={.base=TypeUser,.user_name="S"};CHECK(iface_resolve_type(&cg,st,False,True,False,seen,0,&concrete));CHECK(concrete.nominal);CHECK(!iface_canonical_type(&cg,st,False,False,seen,0,&concrete));
  iface_source_free(&cg);symtab_free(&cg.locals);iface_free_contracts(&cg);LLVMDisposeBuilder(cg.builder);LLVMDisposeModule(cg.module);LLVMContextDispose(cg.ctx);ast_free_all();
 }
 printf("PASS: %u source fact/purity/stable-record checks\n",checks);return 0;
}
