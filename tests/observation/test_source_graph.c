#define _GNU_SOURCE
#include "../../src/common/observation_sources.h"
#include <assert.h>

/* Pure filesystem/encoding controls. No compiler or generated image launch. */
static unsigned checks;
#define CHECK(x) do {++checks;assert(x);}while(0)
static char error[256];
static void put(const char *path,const void *data,size_t n) {
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(data,1,n,f)==n);CHECK(!fclose(f));
}
static void put_text(const char *path,const char *text){put(path,text,strlen(text));}
static void begin(sts_observation_graph *g,const char *root){CHECK(!sts_observation_graph_begin(g,root,error,sizeof(error)));CHECK(!sts_observation_graph_declare(g,0,"root",error,sizeof(error)));}
static uint32_t add(sts_observation_graph *g,const char *name){uint32_t id=UINT32_MAX;CHECK(!sts_observation_graph_add(g,name,1,1,&id,error,sizeof(error)));CHECK(!sts_observation_graph_declare(g,id,name,error,sizeof(error)));return id;}

/* Frozen ORACLES-R1 config bytes, not derived from the encoder under test. */
static void configuration_v4_vectors(void) {
    const struct {uint32_t enabled,warn,has_max,has_min,minimum;uint64_t maximum;size_t size;const char *digest,*hex;} vectors[]={
        {3,1,1,1,100,33,559,"58bb952baae4cc65014e7f4f34d8bf387ac1076d8e17849c92326b1e4adc133a","000000000000001c7374617368612d6f62736572766174696f6e2d636f6e6669672d7634000000040000000400000000000000336e61746976652d73657175656e7469616c2d6469726563742d736f757263652d706f736974696f6e616c2d736c6963652d7634000000020000000200000000000000286469726563742d736f757263652d706f736974696f6e616c2d736c6963652d626f756e64732d7634000000000000001c7265717565737465642d656e7472792d6469726563746f72792d7631000000000000000972656a6563742d76310000000000000020676c6f62616c2d756e697175652d6578706f727465642d7461726765742d7631000000000000003b636c6f7365642d6469726563742d736f757263652d7072696d69746976652d736c6963652d636f756e742d6f722d706f736974696f6e616c2d76310000001a00000100000000ff01000000000f42400000007f000000ff000001ff000001ff000003ff00000fff0000100000010000000100000000100000010000000100000000400000200000020000000200000000010000000100000001000000000400000100000001000000000000000000187838365f36342d756e6b6e6f776e2d6c696e75782d676e75ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa000000020000000000000003000000010000000100000000000000210000000100000064"},
        {1,1,0,0,0,0,547,"51a891168977452a1a0c681a8242db49365072ad112c678b95f27b3ac9d54e8a","000000000000001c7374617368612d6f62736572766174696f6e2d636f6e6669672d7634000000040000000400000000000000336e61746976652d73657175656e7469616c2d6469726563742d736f757263652d706f736974696f6e616c2d736c6963652d7634000000020000000200000000000000286469726563742d736f757263652d706f736974696f6e616c2d736c6963652d626f756e64732d7634000000000000001c7265717565737465642d656e7472792d6469726563746f72792d7631000000000000000972656a6563742d76310000000000000020676c6f62616c2d756e697175652d6578706f727465642d7461726765742d7631000000000000003b636c6f7365642d6469726563742d736f757263652d7072696d69746976652d736c6963652d636f756e742d6f722d706f736974696f6e616c2d76310000001a00000100000000ff01000000000f42400000007f000000ff000001ff000001ff000003ff00000fff0000100000010000000100000000100000010000000100000000400000200000020000000200000000010000000100000001000000000400000100000001000000000000000000187838365f36342d756e6b6e6f776e2d6c696e75782d676e75ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa000000020000000000000001000000010000000000000000"},
        {1,0,1,0,0,33,555,"ef7f22b16ccb7903b58f4e78120c6a11294e625350be4bc041a0c53b23c03b03","000000000000001c7374617368612d6f62736572766174696f6e2d636f6e6669672d7634000000040000000400000000000000336e61746976652d73657175656e7469616c2d6469726563742d736f757263652d706f736974696f6e616c2d736c6963652d7634000000020000000200000000000000286469726563742d736f757263652d706f736974696f6e616c2d736c6963652d626f756e64732d7634000000000000001c7265717565737465642d656e7472792d6469726563746f72792d7631000000000000000972656a6563742d76310000000000000020676c6f62616c2d756e697175652d6578706f727465642d7461726765742d7631000000000000003b636c6f7365642d6469726563742d736f757263652d7072696d69746976652d736c6963652d636f756e742d6f722d706f736974696f6e616c2d76310000001a00000100000000ff01000000000f42400000007f000000ff000001ff000001ff000003ff00000fff0000100000010000000100000000100000010000000100000000400000200000020000000200000000010000000100000001000000000400000100000001000000000000000000187838365f36342d756e6b6e6f776e2d6c696e75782d676e75ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0000000200000000000000010000000000000001000000000000002100000000"},
        {2,0,0,1,100,0,551,"32854b486f2f05a9f62f5bd0dd119c5bc3ee28a3c989c6d7f4d903ce53693ea6","000000000000001c7374617368612d6f62736572766174696f6e2d636f6e6669672d7634000000040000000400000000000000336e61746976652d73657175656e7469616c2d6469726563742d736f757263652d706f736974696f6e616c2d736c6963652d7634000000020000000200000000000000286469726563742d736f757263652d706f736974696f6e616c2d736c6963652d626f756e64732d7634000000000000001c7265717565737465642d656e7472792d6469726563746f72792d7631000000000000000972656a6563742d76310000000000000020676c6f62616c2d756e697175652d6578706f727465642d7461726765742d7631000000000000003b636c6f7365642d6469726563742d736f757263652d7072696d69746976652d736c6963652d636f756e742d6f722d706f736974696f6e616c2d76310000001a00000100000000ff01000000000f42400000007f000000ff000001ff000001ff000003ff00000fff0000100000010000000100000000100000010000000100000000400000200000020000000200000000010000000100000001000000000400000100000001000000000000000000187838365f36342d756e6b6e6f776e2d6c696e75782d676e75ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa00000002000000000000000200000000000000000000000100000064"},
        {3,1,1,1,90,8,559,"32266849eed635082fdc9753111d97ff4bbeaaf9d95f07cf66c5d31fc90b1654","000000000000001c7374617368612d6f62736572766174696f6e2d636f6e6669672d7634000000040000000400000000000000336e61746976652d73657175656e7469616c2d6469726563742d736f757263652d706f736974696f6e616c2d736c6963652d7634000000020000000200000000000000286469726563742d736f757263652d706f736974696f6e616c2d736c6963652d626f756e64732d7634000000000000001c7265717565737465642d656e7472792d6469726563746f72792d7631000000000000000972656a6563742d76310000000000000020676c6f62616c2d756e697175652d6578706f727465642d7461726765742d7631000000000000003b636c6f7365642d6469726563742d736f757263652d7072696d69746976652d736c6963652d636f756e742d6f722d706f736974696f6e616c2d76310000001a00000100000000ff01000000000f42400000007f000000ff000001ff000001ff000003ff00000fff0000100000010000000100000000100000010000000100000000400000200000020000000200000000010000000100000001000000000400000100000001000000000000000000187838365f36342d756e6b6e6f776e2d6c696e75782d676e75ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa00000002000000000000000300000001000000010000000000000008000000010000005a"},
    };
    for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i){
        sts_observation_config c={.compiler_id="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            .enabled_mask=vectors[i].enabled,.warn_memleak=vectors[i].warn,.has_max_bytes=vectors[i].has_max,
            .max_bytes=vectors[i].maximum,.has_min_coverage=vectors[i].has_min,.min_coverage=vectors[i].minimum};
        unsigned char bytes[559];sts_observation_encoder e;char digest[129];
        sts_observation_encode_init(&e,sizeof(bytes));e.capture=bytes;e.capture_capacity=sizeof(bytes);
        CHECK(!sts_observation_config_encode(&c,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","x86_64-unknown-linux-gnu",2,0,&e));
        CHECK(e.size==vectors[i].size);sts_observation_digest_finish(&e.hash,digest);CHECK(!strcmp(digest,vectors[i].digest));
        CHECK(strlen(vectors[i].hex)==2*e.size);
        for(size_t j=0;j<e.size;++j)CHECK(bytes[j]==(unsigned char)((sts_observation_digest_hex_value(vectors[i].hex[2*j])<<4)|sts_observation_digest_hex_value(vectors[i].hex[2*j+1])));
    }
}

static void encoding_vectors(void) {
    sts_observation_encoder e;unsigned char capture[128];
    sts_observation_encode_init(&e,128);e.capture=capture;e.capture_capacity=128;
    sts_observation_encode_u32(&e,0x01020304);sts_observation_encode_u64(&e,UINT64_C(0x05060708090a0b0c));
    sts_observation_encode_bytes(&e,"\xff\0",2);sts_observation_encode_optional_u32(&e,0,123);sts_observation_encode_optional_u32(&e,1,7);
    static const unsigned char expected[]={1,2,3,4,5,6,7,8,9,10,11,12,0,0,0,0,0,0,0,2,255,0,0,0,0,0,0,0,0,1,0,0,0,7};
    CHECK(!e.failed&&e.size==sizeof(expected)&&!memcmp(capture,expected,sizeof(expected)));
    sts_observation_encode_init(&e,3);sts_observation_encode_u32(&e,1);CHECK(e.failed&&e.size==0);
    sts_observation_encode_init(&e,SIZE_MAX);e.size=SIZE_MAX;sts_observation_encode_u32(&e,1);CHECK(e.failed);
    sts_observation_encode_init(&e,64);sts_observation_encode_digest(&e,"ABC");CHECK(e.failed);
    /* Actual bounded production encoder, fed small repeated blocks to avoid
     * allocating a cap-sized synthetic string or native allocation probe. */
    unsigned char block[4096]={0};sts_observation_encode_init(&e,STS_OBS_MANIFEST_LIMIT);
    while(e.size<STS_OBS_MANIFEST_LIMIT-4u){size_t left=STS_OBS_MANIFEST_LIMIT-4u-e.size;
        sts_observation_encode_raw(&e,block,left<sizeof(block)?left:sizeof(block));CHECK(!e.failed);}
    sts_observation_encode_u32(&e,0);CHECK(!e.failed&&e.size==STS_OBS_MANIFEST_LIMIT);
    sts_observation_encode_raw(&e,block,1);CHECK(e.failed&&e.size==STS_OBS_MANIFEST_LIMIT);


    /* Independent review vector, frozen literal bytes/identities. */
    sts_observation_graph g={.invocation_directory="/invoke",.original_root_spelling="entry/root.sts",.file_count=2,.edge_count=1};
    g.files[0]=(sts_observation_source){.file_id=0,.role=0,.lookup_path="/invoke/entry/root.sts",.canonical_path="/target/root.sts",.declared_module="root",.byte_length=189,.physical_line_count=11,.content_id="d96b7a0bcd97e1e90a2420e768d5348fa4b063cde8e35be3a5603f0bbc2db61d"};
    g.files[1]=(sts_observation_source){.file_id=1,.role=1,.lookup_path="/invoke/entry/raw\xff.sts",.canonical_path="/target/raw\xff.sts",.requested_module="raw",.declared_module="raw",.byte_length=9,.physical_line_count=2,.content_id="22bc4a57004bf5e6f2139832a8c037573189d82916d5f69ca90ad2ff7011dd28"};
    g.edges[0]=(sts_observation_edge){.child_file_id=1,.line=2,.col=1,.requested_module="raw",.lookup_path="/invoke/entry/raw\xff.sts"};
    sts_observation_encode_init(&e,4096);CHECK(!sts_observation_graph_encode(&g,&e));char digest[129];sts_observation_digest_finish(&e.hash,digest);
    CHECK(e.size==544);CHECK(!strcmp(digest,"6134956c38d24a31674d40d1d65308619b177cb3168a23bfa059d097267d126a"));
    g.files[1].declared_module=NULL;CHECK(sts_observation_graph_freeze(&g,error,sizeof(error))!=0);CHECK(g.failed&&!g.frozen&&!g.input_graph_id[0]);

    /* R2 vectors were independently frozen before implementation. */
    CHECK(!strcmp(STS_OBS_ADMISSION_POLICY,"closed-direct-source-primitive-slice-count-or-positional-v1"));
    CHECK(!strcmp(STS_OBS_RESOURCE_POLICY,"direct-source-positional-slice-bounds-v4"));
    CHECK(!strcmp(STS_OBS_GRAPH_PROFILE_V2,"native-sequential-direct-source-v2"));
    sts_observation_config kat={.warn_memleak=1,.has_max_bytes=1,.max_bytes=8,.has_min_coverage=1,.min_coverage=90,.enabled_mask=3,.compiler_id="dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
    const char *runtime="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    unsigned char config_bytes[559];
    sts_observation_encode_init(&e,sizeof(config_bytes));e.capture=config_bytes;e.capture_capacity=sizeof(config_bytes);
    CHECK(!sts_observation_config_encode(&kat,runtime,"x86_64-unknown-linux-gnu",2,0,&e));
    CHECK(e.size==559);sts_observation_digest_finish(&e.hash,digest);
    CHECK(!strcmp(digest,"32266849eed635082fdc9753111d97ff4bbeaaf9d95f07cf66c5d31fc90b1654"));
    CHECK(!sts_observation_config_digest(&kat,runtime,"x86_64-unknown-linux-gnu",2,0));CHECK(!strcmp(kat.config_id,digest));
    /* Each independent policy and each new limit must actually be encoded.
     * Locate exact bytes, mutate only that field, and compare to literal KAT. */
    const char *policies[]={STS_OBS_RESOURCE_POLICY,STS_OBS_LOOKUP_POLICY,STS_OBS_DUPLICATE_POLICY,STS_OBS_ALIAS_POLICY,STS_OBS_ADMISSION_POLICY};
    for(unsigned i=0;i<5;++i){
        unsigned char mutated[sizeof(config_bytes)];memcpy(mutated,config_bytes,sizeof(mutated));
        unsigned char *at=memmem(mutated,sizeof(mutated),policies[i],strlen(policies[i]));CHECK(at);*at^=1;
        char altered[129];sts_observation_digest(mutated,sizeof(mutated),altered);CHECK(strcmp(altered,digest));
    }
    const unsigned char count_and_limits[]={0,0,0,26,0,0,1,0,0,0,0,255};
    unsigned char *limit_start=memmem(config_bytes,sizeof(config_bytes),count_and_limits,sizeof(count_and_limits));CHECK(limit_start);
    for(unsigned i=20;i<26;++i){
        unsigned char mutated[sizeof(config_bytes)];memcpy(mutated,config_bytes,sizeof(mutated));
        size_t offset=(size_t)(limit_start-config_bytes)+4+4*i;
        CHECK(offset+4<=sizeof(mutated));mutated[offset+3]^=1;
        char altered[129];sts_observation_digest(mutated,sizeof(mutated),altered);CHECK(strcmp(altered,digest));
    }
    kat.has_max_bytes=0;kat.has_min_coverage=0;kat.enabled_mask=1;
    sts_observation_encode_init(&e,STS_OBS_METADATA_LIMIT);
    CHECK(!sts_observation_config_encode(&kat,runtime,"x86_64-unknown-linux-gnu",2,0,&e));CHECK(e.size==547);
    sts_observation_digest_finish(&e.hash,digest);CHECK(!strcmp(digest,"51a891168977452a1a0c681a8242db49365072ad112c678b95f27b3ac9d54e8a"));
    CHECK(!sts_observation_config_digest(&kat,runtime,"x86_64-unknown-linux-gnu",2,0));CHECK(!strcmp(kat.config_id,digest));
    CHECK(sts_observation_config_digest(&kat,"bad","x86_64-unknown-linux-gnu",2,0)!=0);
    CHECK(sts_observation_config_digest(&kat,runtime,"x86_64-unknown-linux-gnu",4,0)!=0);
    kat.has_max_bytes=1;kat.has_min_coverage=1;kat.enabled_mask=3;
    sts_observation_encode_init(&e,558);CHECK(sts_observation_config_encode(&kat,runtime,"x86_64-unknown-linux-gnu",2,0,&e)!=0);
    sts_observation_config c={.input_graph_id="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",.config_id="202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"};
    uint32_t masks[]={0,2,1,2,4,4,4},lines[]={2},line_files[]={0};
    sts_observation_file_manifest files[]={{.physical_line_count=6,.content_id="a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5",.reason_masks=masks}};
    sts_observation_declaration decls[]={
        {.kind=1,.line=1,.col=1,.end_line=3,.end_col=1,.module_name="root",.name="f",.selected=1},
        {.decl_id=1,.ordinal_in_file=1,.kind=58,.line=4,.col=1,.end_line=6,.end_col=1,.module_name="root",.name="",.selected=1}};
    sts_observation_test tests[]={{.decl_id=1,.line=4,.col=1,.name="t"}};
    sts_observation_anchor anchors[]={
        {.line=2,.col=1,.end_line=2,.end_col=80,.kind=1,.has_coverage_id=1},
        {.anchor_id=1,.decl_id=1,.line=5,.col=1,.end_line=5,.end_col=80,.kind=3,.exclusion_reason=2}};
    sts_observation_site sites[]={
        {.line=2,.col=1,.end_line=2,.end_col=10,.kind=1,.owner_test=UINT32_MAX},
        {.line=2,.col=12,.end_line=2,.end_col=22,.kind=2,.owner_test=UINT32_MAX},
        {.line=2,.col=24,.end_line=2,.end_col=30,.kind=3,.owner_test=UINT32_MAX},
        {.line=2,.col=32,.end_line=2,.end_col=50,.kind=4,.owner_test=UINT32_MAX,.slice_element_kind=5,.slice_abi_stride=4,.slice_count_arity=2},
        {.decl_id=1,.line=5,.col=1,.end_line=5,.end_col=20,.kind=4,.owner_test=0,.slice_element_kind=9,.slice_abi_stride=1,.slice_count_arity=1},
        {.decl_id=1,.line=5,.col=22,.end_line=5,.end_col=30,.kind=5,.owner_test=0}};
    sts_observation_manifest m={.file_count=1,.files=files,.decl_count=2,.declarations=decls,.test_count=1,.tests=tests,
        .anchor_count=2,.anchors=anchors,.site_count=6,.sites=sites,.line_count=1,.lines=lines,.line_file_ids=line_files};
    CHECK(!sts_observation_manifest_digest(&c,&m));CHECK(m.serialized_size==795);
    CHECK(!strcmp(m.map_id,"3813ba52b6d33367990b5cbd004a984ec1c6f53e22e21a8b6d5f259d0957f12c"));
    uint32_t *fields[]={&sites[3].slice_element_kind,&sites[3].slice_abi_stride,&sites[3].slice_count_arity,&sites[3].slice_initializer_item_count};
    for(unsigned i=0;i<4;++i){uint32_t save=*fields[i];++*fields[i];CHECK(!sts_observation_manifest_digest(&c,&m));
        CHECK(strcmp(m.map_id,"3813ba52b6d33367990b5cbd004a984ec1c6f53e22e21a8b6d5f259d0957f12c"));*fields[i]=save;}
    const char record[]="allocations 2 27\nallocation 1 1 7 0 4294967295 0\nallocation 2 2 20 3 4294967295 0\nEND\n";
    CHECK(sizeof(record)-1==86);sts_observation_digest(record,sizeof(record)-1,digest);
    CHECK(!strcmp(digest,"14074eb03ad114dead37a8c2b9edfd4676485af698b86f605e6c303ce6ebf30c"));
    char wrong_domain[sizeof(record)];memcpy(wrong_domain,record,sizeof(record));strstr(wrong_domain,"allocation 2 2")[13]='1';
    sts_observation_digest(wrong_domain,sizeof(wrong_domain)-1,digest);CHECK(strcmp(digest,"14074eb03ad114dead37a8c2b9edfd4676485af698b86f605e6c303ce6ebf30c"));
    m.file_count=STS_OBS_FILE_LIMIT+1;CHECK(sts_observation_manifest_digest(&c,&m)!=0);

}
static void module_bounds(void) {
    CHECK(sts_observation_module_valid("a.b_9"));CHECK(!sts_observation_module_valid("a..b"));CHECK(!sts_observation_module_valid("a."));
    CHECK(!sts_observation_module_valid("9a"));CHECK(!sts_observation_module_valid("a.\xff"));
    char module[257];memset(module,'a',sizeof(module));module[127]='.';module[255]=0;CHECK(sts_observation_module_valid(module));
    module[255]='a';module[256]=0;CHECK(!sts_observation_module_valid(module));module[127]='a';module[128]=0;CHECK(!sts_observation_module_valid(module));
    char directory[4096];memset(directory,'a',4095);directory[0]='/';directory[4095]=0;CHECK(!sts_observation_anchor_path(directory,"a"));
    char *p=sts_observation_anchor_path("/a","../b");CHECK(p&&!strcmp(p,"/a/../b"));free(p);
}
static void filesystem_controls(void) {
    char cwd[4096];CHECK(getcwd(cwd,sizeof(cwd)));char temp[]="/tmp/stasha-source-graph-unit-XXXXXX";CHECK(mkdtemp(temp));CHECK(!chdir(temp));
    CHECK(!mkdir("requested",0700));CHECK(!mkdir("target",0700));CHECK(!mkdir("requested/util",0700));CHECK(!mkdir("target/util",0700));
    put_text("target/root.sts","mod root;\n");put_text("target/other.sts","mod root;\n");put_text("requested/util/calc.sts","mod util.calc;\n");put_text("target/util/calc.sts","WRONG TARGET\n");
    CHECK(!symlink("../target/root.sts","requested/root.sts"));sts_observation_graph g;
    begin(&g,"requested/root.sts");CHECK(add(&g,"util.calc")==1);CHECK(g.file_count==2&&g.edge_count==1);
    CHECK(strstr(g.files[0].canonical_path,"/target/root.sts"));CHECK(strstr(g.files[1].lookup_path,"/requested/util/calc.sts"));
    CHECK(!strcmp(g.files[1].bytes,"mod util.calc;\n"));CHECK(g.physical_line_count==4);CHECK(!sts_observation_graph_freeze(&g,error,sizeof(error)));
    CHECK(!sts_observation_graph_revalidate(&g,error,sizeof(error)));
    CHECK(!unlink("requested/root.sts"));CHECK(!symlink("../target/other.sts","requested/root.sts"));
    CHECK(sts_observation_graph_revalidate(&g,error,sizeof(error))!=0);CHECK(!unlink("requested/root.sts"));CHECK(!symlink("../target/root.sts","requested/root.sts"));
    put_text("requested/util/calc.sts","changed bytes\n");CHECK(sts_observation_graph_revalidate(&g,error,sizeof(error))!=0);
    put_text("requested/util/calc.sts","mod util.calc;\n");CHECK(!sts_observation_graph_revalidate(&g,error,sizeof(error)));
    put_text("requested/util/replacement.sts","mod util.calc;\n");CHECK(!rename("requested/util/replacement.sts","requested/util/calc.sts"));
    CHECK(sts_observation_graph_revalidate(&g,error,sizeof(error))!=0);sts_observation_graph_free(&g);
    uint32_t id;
    begin(&g,"requested/root.sts");add(&g,"util.calc");CHECK(sts_observation_graph_add(&g,"util.calc",1,1,&id,error,sizeof(error))!=0);CHECK(g.failed&&!g.frozen);CHECK(sts_observation_graph_freeze(&g,error,sizeof(error))!=0);sts_observation_graph_free(&g);
    CHECK(!symlink("util/calc.sts","requested/same.sts"));begin(&g,"requested/root.sts");add(&g,"util.calc");CHECK(sts_observation_graph_add(&g,"same",1,1,&id,error,sizeof(error))!=0);CHECK(strstr(error,"canonical"));sts_observation_graph_free(&g);
    CHECK(!symlink("../target/root.sts","requested/self.sts"));begin(&g,"requested/root.sts");CHECK(sts_observation_graph_add(&g,"self",1,1,&id,error,sizeof(error))!=0);CHECK(strstr(error,"self-import"));sts_observation_graph_free(&g);
    begin(&g,"requested/root.sts");add(&g,"util.calc");CHECK(sts_observation_graph_add(&g,"other.calc",1,1,&id,error,sizeof(error))!=0);CHECK(strstr(error,"alias"));sts_observation_graph_free(&g);
    begin(&g,"requested/root.sts");CHECK(!sts_observation_graph_add(&g,"util.calc",1,1,&id,error,sizeof(error)));CHECK(sts_observation_graph_declare(&g,id,"wrong",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    /* Distinct canonical hardlink paths are not coalesced by inode or digest. */
    CHECK(!link("requested/util/calc.sts","requested/hard.sts"));begin(&g,"requested/root.sts");add(&g,"util.calc");add(&g,"hard");CHECK(g.file_count==3);CHECK(!strcmp(g.files[1].content_id,g.files[2].content_id));sts_observation_graph_free(&g);
    /* Linux literal-backslash entry uses the ordinary original-spelling rule. */
    CHECK(!mkdir("back",0700));put_text("back\\root.sts","mod root;");put_text("back/leaf.sts","mod leaf;");
    begin(&g,"back\\root.sts");add(&g,"leaf");CHECK(strstr(g.files[1].lookup_path,"/back/leaf.sts"));sts_observation_graph_free(&g);
    /* One LF adds one physical line, including the final empty line. */
    put_text("empty.sts","");begin(&g,"empty.sts");CHECK(g.files[0].physical_line_count==1&&g.files[0].byte_length==0);sts_observation_graph_free(&g);
    put("nul.sts","a\0b",3);CHECK(sts_observation_graph_begin(&g,"nul.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    CHECK(!mkfifo("pipe.sts",0600));CHECK(sts_observation_graph_begin(&g,"pipe.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    CHECK(sts_observation_graph_begin(&g,"requested",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    char long_root[513];memset(long_root,'a',512);long_root[512]=0;CHECK(sts_observation_graph_begin(&g,long_root,error,sizeof(error))!=0);sts_observation_graph_free(&g);
    begin(&g,"empty.sts");g.file_count=STS_OBS_FILE_LIMIT;CHECK(sts_observation_graph_add(&g,"leaf",1,1,&id,error,sizeof(error))!=0);g.file_count=1;sts_observation_graph_free(&g);
    /* Exact-read capacity checks include bytes/lines before any parser access. */
    char *large=malloc(STS_OBS_SOURCE_LIMIT+1);CHECK(large);memset(large,'x',STS_OBS_SOURCE_LIMIT+1);put("large.sts",large,STS_OBS_SOURCE_LIMIT+1);
    CHECK(sts_observation_graph_begin(&g,"large.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    put("large.sts",large,STS_OBS_SOURCE_LIMIT);begin(&g,"large.sts");CHECK(g.byte_length==STS_OBS_SOURCE_LIMIT);CHECK(sts_observation_graph_add(&g,"empty",1,1,&id,error,sizeof(error))==0);CHECK(g.file_count==2);sts_observation_graph_free(&g);
    memset(large,'\n',STS_OBS_SOURCE_LINES);put("large.sts",large,STS_OBS_SOURCE_LINES);CHECK(sts_observation_graph_begin(&g,"large.sts",error,sizeof(error))!=0);sts_observation_graph_free(&g);
    put("large.sts",large,STS_OBS_SOURCE_LINES-1);begin(&g,"large.sts");CHECK(g.physical_line_count==STS_OBS_SOURCE_LINES);CHECK(sts_observation_graph_add(&g,"empty",1,1,&id,error,sizeof(error))!=0);sts_observation_graph_free(&g);free(large);
    const char *paths[]={"target/root.sts","target/other.sts","requested/root.sts","requested/util/calc.sts","target/util/calc.sts","requested/same.sts","requested/self.sts","requested/hard.sts","back\\root.sts","back/leaf.sts","empty.sts","nul.sts","pipe.sts","large.sts"};
    for(size_t i=0;i<sizeof(paths)/sizeof(paths[0]);++i)CHECK(!unlink(paths[i]));
    CHECK(!rmdir("requested/util"));CHECK(!rmdir("target/util"));CHECK(!rmdir("requested"));CHECK(!rmdir("target"));CHECK(!rmdir("back"));CHECK(!chdir(cwd));CHECK(!rmdir(temp));
}
int main(void){configuration_v4_vectors();encoding_vectors();module_bounds();filesystem_controls();printf("observation source graph: %u pure checks passed; no compiler or generated image executed\n",checks);return 0;}
