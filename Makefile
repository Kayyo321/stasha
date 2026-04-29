CC         = clang
CXX        = clang++
LLVM_BUILD = build/llvm
LLVM_CFG   = $(LLVM_BUILD)/bin/llvm-config

# Flags resolved at recipe time (recursive =) so llvm-config is found after build
LLVM_CFLAGS  = $(shell $(LLVM_CFG) --cflags  2>/dev/null)
LLVM_LDFLAGS = $(shell $(LLVM_CFG) --ldflags --libs core analysis native \
               lto passes option codegen bitwriter debuginfodwarf \
               objcarcopts textapi object --system-libs 2>/dev/null) \
               -lLLVMDTLTO

# If LLD libraries are available, enable embedded LLD support
LLD_LIBS     = $(wildcard $(LLVM_BUILD)/lib/liblldCommon.a)
ifneq ($(LLD_LIBS),)
  LLD_CFLAGS  = -DSTASHA_HAS_LLD -Iextlib/llvm/lld/include
  LLD_LDLIBS  = -llldMachO -llldELF -llldCommon -llldCOFF -llldMinGW -llldWasm
else
  LLD_CFLAGS  =
  LLD_LDLIBS  =
endif

CFLAGS   = -Wall -Wextra -std=c2x -Isrc $(LLVM_CFLAGS)
CXXFLAGS = -Wall -Wextra -std=c++17 -Isrc $(LLVM_CFLAGS) $(LLD_CFLAGS)
LDFLAGS  = $(LLVM_LDFLAGS) $(LLD_LDLIBS) -lc++

# ── Extlib C flags (no -Wall spam from third-party code) ─────────────────────
EXTLIB_CFLAGS = -std=c11 -O2 -fPIC

# ── cJSON (single-file C library) ─────────────────────────────────────────────
CJSON_SRC = extlib/cjson/cJSON.c
CJSON_OBJ = build/obj/extlib/cjson.o

# ── JSON wrapper ───────────────────────────────────────────────────────────────
JSON_WRAP_SRC = std/json/json_wrapper.c
JSON_WRAP_OBJ = build/obj/std/json/json_wrapper.o

# ── Mongoose (single-file C library) ──────────────────────────────────────────
MONGOOSE_SRC = extlib/mongoose/mongoose.c
MONGOOSE_OBJ = build/obj/extlib/mongoose.o

# ── HTTP wrapper ───────────────────────────────────────────────────────────────
HTTP_WRAP_SRC = std/http/http_wrapper.c
HTTP_WRAP_OBJ = build/obj/std/http/http_wrapper.o

# ── cl_args runtime ─────────────────────────────────────────────────────────
CLARGS_RT_SRC = std/cl_args/cl_args_rt.c
CLARGS_RT_OBJ = build/obj/std/cl_args/cl_args_rt.o

SRCS = src/main.c                       \
       src/common/common.c               \
       src/lexer/lexer.c                 \
       src/ast/ast.c                     \
       src/parser/parser.c               \
       src/preprocessor/preprocessor.c  \
       src/tooling/editor.c             \
       src/codegen/codegen.c

OBJS = $(patsubst src/%.c,build/obj/%.o,$(SRCS))
LINKER_OBJ = build/obj/linker/linker.o

TARGET = bin/stasha

# ── Thread runtime ─────────────────────────────────────────────────────────
THREAD_RUNTIME_SRC = src/runtime/thread_runtime.c
THREAD_RUNTIME_OBJ = build/obj/runtime/thread_runtime.o
THREAD_RUNTIME_LIB = bin/thread_runtime.a

# ── Zone runtime ────────────────────────────────────────────────────────────
ZONE_RUNTIME_SRC = src/runtime/zone_runtime.c
ZONE_RUNTIME_OBJ = build/obj/runtime/zone_runtime.o
ZONE_RUNTIME_LIB = bin/zone_runtime.a

# ── Thread test programs ────────────────────────────────────────────────────
THREAD_TEST_SRCS = examples/thread_basic.sts    \
                   examples/thread_return.sts   \
                   examples/thread_many_jobs.sts \
                   examples/thread_stress.sts   \
                   examples/future_wait.sts

# ── Standard library ──────────────────────────────────────────────────────
STDLIB_SRCS_ALL := $(shell find stsstdlib -name '*.sts' 2>/dev/null)

# Modules that have custom bundled-archive rules (exclude from default foreach).
STDLIB_BUNDLED := stsstdlib/serial/json.sts stsstdlib/net/http.sts stsstdlib/random/complex_rng.sts stsstdlib/sys/cl_args.sts

# Files for the default compile-only rule.
STDLIB_SRCS := $(filter-out $(STDLIB_BUNDLED),$(STDLIB_SRCS_ALL))

# All .a targets (used by 'stdlib' phony target to know what to build).
STDLIB_LIBS := $(foreach s,$(STDLIB_SRCS_ALL),$(dir $(s))lib$(notdir $(basename $(s))).a)

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# ── OpenSSL (static libcrypto) ────────────────────────────────────────────
OPENSSL_SRC   = extlib/openssl
OPENSSL_BUILD = build/openssl
OPENSSL_LIB   = $(OPENSSL_BUILD)/lib/libcrypto.a

ifeq ($(UNAME_S),Darwin)
  ifeq ($(UNAME_M),arm64)
    OPENSSL_TARGET = darwin64-arm64-cc
  else
    OPENSSL_TARGET = darwin64-x86_64-cc
  endif
else
  ifeq ($(UNAME_M),aarch64)
    OPENSSL_TARGET = linux-aarch64
  else
    OPENSSL_TARGET = linux-x86_64
  endif
endif

.PHONY: all stdlib stdlib-test thread-runtime zone-runtime clean clean-stdlib clean-llvm llvm openssl clean-openssl test-threads test-cinterop clean-cinterop

all: $(TARGET) thread-runtime zone-runtime

# Build every .sts under stsstdlib/ into a .a alongside the source,
# then install the .a and .sts files into bin/stdlib/, then run all tests.
stdlib: $(TARGET) $(STDLIB_LIBS) stsstdlib/serial/libjson.a stsstdlib/net/libhttp.a stsstdlib/sys/libcl_args.a stdlib-test
	@mkdir -p bin/stdlib
	@for s in $(STDLIB_SRCS); do \
	    a="$$(dirname $$s)/lib$$(basename $${s%.sts}).a"; \
	    cp "$$a" bin/stdlib/; \
	    cp "$$s" bin/stdlib/; \
	done
	@cp stsstdlib/serial/libjson.a bin/stdlib/
	@cp stsstdlib/serial/json.sts  bin/stdlib/
	@cp stsstdlib/net/libhttp.a    bin/stdlib/
	@cp stsstdlib/net/http.sts     bin/stdlib/
	@cp stsstdlib/sys/libcl_args.a bin/stdlib/
	@cp stsstdlib/sys/cl_args.sts  bin/stdlib/
	@echo "stdlib installed -> bin/stdlib/"

# Modules that require platform-specific external libs not available everywhere.
# These are compiled into .a files but skipped during 'make stdlib-test'.
STDLIB_TEST_SKIP =

# Bundled modules need their fat archive passed via -l during test.
# They are excluded from the generic loop and tested separately below.
STDLIB_TEST_BUNDLED_JSON      = stsstdlib/serial/json.sts
STDLIB_TEST_BUNDLED_HTTP      = stsstdlib/net/http.sts
STDLIB_TEST_BUNDLED_CRNG      = stsstdlib/random/complex_rng.sts
STDLIB_TEST_BUNDLED_CLARGS    = stsstdlib/sys/cl_args.sts
STDLIB_BUNDLED_CRNG_LIB       = stsstdlib/random/libcomplex_rng.a

# Run 'stasha test' on every stdlib source file.
# Prints a pass/fail summary and exits non-zero if any test fails.
stdlib-test: $(TARGET) stsstdlib/serial/libjson.a stsstdlib/net/libhttp.a stsstdlib/sys/libcl_args.a $(STDLIB_BUNDLED_CRNG_LIB)
	@echo ""
	@echo "=== stdlib tests ==="
	@pass=0; fail=0; skip=0; \
	for f in $(STDLIB_SRCS); do \
	    skip_this=0; \
	    for s in $(STDLIB_TEST_SKIP); do \
	        if [ "$$f" = "$$s" ]; then skip_this=1; fi; \
	    done; \
	    if [ $$skip_this -eq 1 ]; then \
	        printf "  %-55s" "$$f ..."; echo "SKIP"; skip=$$((skip+1)); continue; \
	    fi; \
	    printf "  %-55s" "$$f ..."; \
	    out=$$($(TARGET) test "$$f" 2>&1); \
	    code=$$?; \
	    if [ $$code -eq 0 ]; then \
	        echo "PASS"; pass=$$((pass+1)); \
	    else \
	        echo "FAIL"; fail=$$((fail+1)); \
	        echo "$$out" | grep -E "^error:|FAIL|failed" | head -3 | sed 's/^/      /'; \
	    fi; \
	done; \
	printf "  %-55s" "$(STDLIB_TEST_BUNDLED_JSON) ..."; \
	out=$$($(TARGET) test "$(STDLIB_TEST_BUNDLED_JSON)" -l stsstdlib/serial/libjson.a 2>&1); \
	code=$$?; \
	if [ $$code -eq 0 ]; then \
	    echo "PASS"; pass=$$((pass+1)); \
	else \
	    echo "FAIL"; fail=$$((fail+1)); \
	    echo "$$out" | grep -E "^error:|FAIL|failed" | head -3 | sed 's/^/      /'; \
	fi; \
	printf "  %-55s" "$(STDLIB_TEST_BUNDLED_HTTP) ..."; \
	out=$$($(TARGET) test "$(STDLIB_TEST_BUNDLED_HTTP)" -l stsstdlib/net/libhttp.a 2>&1); \
	code=$$?; \
	if [ $$code -eq 0 ]; then \
	    echo "PASS"; pass=$$((pass+1)); \
	else \
	    echo "FAIL"; fail=$$((fail+1)); \
	    echo "$$out" | grep -E "^error:|FAIL|failed" | head -3 | sed 's/^/      /'; \
	fi; \
	printf "  %-55s" "$(STDLIB_TEST_BUNDLED_CRNG) ..."; \
	out=$$($(TARGET) test "$(STDLIB_TEST_BUNDLED_CRNG)" -l $(STDLIB_BUNDLED_CRNG_LIB) 2>&1); \
	code=$$?; \
	if [ $$code -eq 0 ]; then \
	    echo "PASS"; pass=$$((pass+1)); \
	else \
	    echo "FAIL"; fail=$$((fail+1)); \
	    echo "$$out" | grep -E "^error:|FAIL|failed" | head -3 | sed 's/^/      /'; \
	fi; \
	printf "  %-55s" "$(STDLIB_TEST_BUNDLED_CLARGS) ..."; \
	out=$$($(TARGET) test "$(STDLIB_TEST_BUNDLED_CLARGS)" -l stsstdlib/sys/libcl_args.a 2>&1); \
	code=$$?; \
	if [ $$code -eq 0 ]; then \
	    echo "PASS"; pass=$$((pass+1)); \
	else \
	    echo "FAIL"; fail=$$((fail+1)); \
	    echo "$$out" | grep -E "^error:|FAIL|failed" | head -3 | sed 's/^/      /'; \
	fi; \
	echo ""; \
	echo "  Passed: $$pass   Failed: $$fail   Skipped: $$skip   Total: $$((pass+fail+skip))"; \
	echo ""; \
	if [ $$fail -gt 0 ]; then \
	    echo "=== STDLIB TESTS FAILED ==="; exit 1; \
	else \
	    echo "=== All stdlib tests passed ==="; \
	fi

# ── Extlib C object rules ──────────────────────────────────────────────────────

$(CJSON_OBJ): $(CJSON_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(EXTLIB_CFLAGS) -Iextlib/cjson -c -o $@ $<

$(JSON_WRAP_OBJ): $(JSON_WRAP_SRC) std/json/json_wrapper.h extlib/cjson/cJSON.h
	@mkdir -p $(dir $@)
	$(CC) $(EXTLIB_CFLAGS) -Iextlib/cjson -c -o $@ $<

$(MONGOOSE_OBJ): $(MONGOOSE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(EXTLIB_CFLAGS) -Iextlib/mongoose -DMG_ENABLE_LINES=1 -c -o $@ $<

$(HTTP_WRAP_OBJ): $(HTTP_WRAP_SRC) std/http/http_wrapper.h extlib/mongoose/mongoose.h
	@mkdir -p $(dir $@)
	$(CC) $(EXTLIB_CFLAGS) -Iextlib/mongoose -c -o $@ $<

$(CLARGS_RT_OBJ): $(CLARGS_RT_SRC) std/cl_args/cl_args_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(EXTLIB_CFLAGS) -c -o $@ $<

# ── Generated rule: stsstdlib/<cat>/lib<name>.a ← stsstdlib/<cat>/<name>.sts ─
#
# Default: compile the .sts into a .a.  JSON and HTTP are overridden below to
# also bundle cJSON/mongoose + their C wrappers into the same archive so users
# only need a single libjson.a / libhttp.a with no extra dependencies.

define stdlib-rule
$(dir $(1))lib$(notdir $(basename $(1))).a: $(1) $(TARGET)
	@mkdir -p $$(dir $$@)
	$(TARGET) lib $$< -o $$@
endef
$(foreach s,$(STDLIB_SRCS),$(eval $(call stdlib-rule,$(s))))

# ── JSON: bundle cJSON + wrapper into the archive ─────────────────────────────

stsstdlib/serial/libjson.a: stsstdlib/serial/json.sts $(TARGET) \
                              $(CJSON_OBJ) $(JSON_WRAP_OBJ)
	@mkdir -p stsstdlib/serial
	$(TARGET) lib stsstdlib/serial/json.sts -o $@
	ar q $@ $(CJSON_OBJ) $(JSON_WRAP_OBJ)
	ranlib $@

# ── HTTP: bundle mongoose + wrapper into the archive ──────────────────────────

stsstdlib/net/libhttp.a: stsstdlib/net/http.sts $(TARGET) \
                           $(MONGOOSE_OBJ) $(HTTP_WRAP_OBJ)
	@mkdir -p stsstdlib/net
	$(TARGET) lib stsstdlib/net/http.sts -o $@
	ar q $@ $(MONGOOSE_OBJ) $(HTTP_WRAP_OBJ)
	ranlib $@

# ── cl_args: bundle C runtime into the archive ───────────────────────────────

stsstdlib/sys/libcl_args.a: stsstdlib/sys/cl_args.sts $(TARGET) $(CLARGS_RT_OBJ)
	@mkdir -p stsstdlib/sys
	$(TARGET) lib stsstdlib/sys/cl_args.sts -o $@
	ar q $@ $(CLARGS_RT_OBJ)
	ranlib $@

# ── complex_rng: bundle OpenSSL libcrypto into the archive ────────────────────

$(STDLIB_BUNDLED_CRNG_LIB): stsstdlib/random/complex_rng.sts $(TARGET) $(OPENSSL_LIB)
	@mkdir -p stsstdlib/random
	$(TARGET) lib stsstdlib/random/complex_rng.sts -o $@
	ranlib $@

clean-stdlib:
	find stsstdlib -name '*.a' -delete 2>/dev/null; true
	rm -rf bin/stdlib

thread-runtime: $(THREAD_RUNTIME_LIB)

$(THREAD_RUNTIME_OBJ): $(THREAD_RUNTIME_SRC) src/runtime/thread_runtime.h
	@mkdir -p $(dir $@)
	$(CC) -std=c2x -O2 -Wall -c -o $@ $<

$(THREAD_RUNTIME_LIB): $(THREAD_RUNTIME_OBJ) | bin
	ar rcs $@ $<

zone-runtime: $(ZONE_RUNTIME_LIB)

$(ZONE_RUNTIME_OBJ): $(ZONE_RUNTIME_SRC) src/runtime/zone_runtime.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -c -o $@ $<

$(ZONE_RUNTIME_LIB): $(ZONE_RUNTIME_OBJ) | bin
	ar rcs $@ $<

$(TARGET): $(OBJS) $(LINKER_OBJ) | bin
	$(CC) -o $@ $(OBJS) $(LINKER_OBJ) $(LDFLAGS)

# ── C interop test suite ───────────────────────────────────────────────────
# Compiles every .c under tests/cinterop/support/ into a single static archive
# linked against the .sts tests via `stasha -l`.  The shell driver runs each
# matrix row, then the LLVM smoke capstone, then the negative tests.

CINTEROP_DIR        = tests/cinterop
CINTEROP_BUILD      = $(CINTEROP_DIR)/build
CINTEROP_SUPPORT_C  = $(wildcard $(CINTEROP_DIR)/support/*.c)
CINTEROP_SUPPORT_O  = $(patsubst $(CINTEROP_DIR)/support/%.c,$(CINTEROP_BUILD)/%.o,$(CINTEROP_SUPPORT_C))
CINTEROP_SUPPORT_LIB = $(CINTEROP_BUILD)/libcinterop_support.a

$(CINTEROP_BUILD)/%.o: $(CINTEROP_DIR)/support/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O0 -Wall -fPIC -c -o $@ $<

$(CINTEROP_SUPPORT_LIB): $(CINTEROP_SUPPORT_O)
	@mkdir -p $(dir $@)
	@if [ -n "$(CINTEROP_SUPPORT_O)" ]; then \
	    ar rcs $@ $(CINTEROP_SUPPORT_O); \
	    ranlib $@; \
	else \
	    rm -f $@; \
	fi

test-cinterop: $(TARGET) $(CINTEROP_SUPPORT_LIB)
	@bash $(CINTEROP_DIR)/run_cinterop_tests.sh $(TARGET)

# LLVM C API capstone — needs the in-tree LLVM build (`make llvm`).  Uses
# llvm-config to assemble the libfile list and drives stasha to compile +
# link the .sts directly.  Also verifies the produced binary emits a
# valid LLVM IR module to stdout.
LLVM_SMOKE_SRC  = $(CINTEROP_DIR)/llvm_smoke.sts
LLVM_SMOKE_BIN  = $(CINTEROP_BUILD)/llvm_smoke

test-llvm-smoke: $(TARGET) $(LLVM_SMOKE_SRC) | $(LLVM_CFG)
	@mkdir -p $(CINTEROP_BUILD)
	@echo "=== LLVM C API capstone ==="
	@LIBS=$$($(LLVM_CFG) --libfiles core analysis native lto passes option codegen bitwriter debuginfodwarf objcarcopts textapi object 2>/dev/null); \
	LARGS=""; for L in $$LIBS; do LARGS="$$LARGS -l $$L"; done; \
	OUT=$$($(TARGET) build $(LLVM_SMOKE_SRC) -o $(LLVM_SMOKE_BIN) $$LARGS 2>&1); \
	if [ ! -x $(LLVM_SMOKE_BIN) ]; then \
	    echo "  FAIL llvm_smoke (compile/link):"; echo "$$OUT" | grep -E '^error:|ld64.lld' | head -5; exit 1; \
	fi; \
	OUT=$$($(LLVM_SMOKE_BIN) 2>&1); \
	if echo "$$OUT" | grep -qE 'ModuleID|target triple'; then \
	    echo "  PASS llvm_smoke"; \
	    echo "$$OUT" | head -3 | sed 's/^/    | /'; \
	else \
	    echo "  FAIL llvm_smoke (no IR in stdout):"; echo "$$OUT" | head -5; exit 1; \
	fi

clean-cinterop:
	rm -rf $(CINTEROP_BUILD)

# Run all thread-related test programs via 'stasha test'
test-threads: $(TARGET) $(THREAD_RUNTIME_LIB)
	@echo "=== Thread system tests ==="
	@fail=0; \
	for f in $(THREAD_TEST_SRCS); do \
	    echo "--- $$f ---"; \
	    if ! $(TARGET) test "$$f"; then \
	        echo "FAIL: $$f"; fail=1; \
	    fi; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== SOME TESTS FAILED ==="; exit 1; fi; \
	echo "=== All thread tests passed ==="

HDRS := $(shell find src -name '*.h')

build/obj/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LINKER_OBJ): src/linker/linker.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

bin:
	@mkdir -p bin

# ── LLVM + LLD build (one-time) ──────────────────────────────
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
  LLVM_TARGETS = AArch64
else
  LLVM_TARGETS = X86
endif

LLD_LIB = $(LLVM_BUILD)/lib/liblldCommon.a

llvm: $(LLD_LIB)

$(LLD_LIB): $(LLVM_CFG)
	cmake -S extlib/llvm/llvm -B $(LLVM_BUILD)            \
	      -DCMAKE_BUILD_TYPE=Release                       \
	      -DLLVM_ENABLE_PROJECTS="lld"                     \
	      -DLLVM_TARGETS_TO_BUILD="$(LLVM_TARGETS)"       \
	      -DLLVM_BUILD_TOOLS=OFF                           \
	      -DLLVM_BUILD_UTILS=OFF                           \
	      -DLLVM_BUILD_EXAMPLES=OFF                        \
	      -DLLVM_INCLUDE_TESTS=OFF                         \
	      -DLLVM_INCLUDE_BENCHMARKS=OFF                    \
	      -DLLVM_ENABLE_ZLIB=OFF                           \
	      -DLLVM_ENABLE_TERMINFO=OFF                       \
	      -DLLVM_ENABLE_ZSTD=OFF
	cmake --build $(LLVM_BUILD) -- -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc)

$(LLVM_CFG):
	cmake -S extlib/llvm/llvm -B $(LLVM_BUILD)            \
	      -DCMAKE_BUILD_TYPE=Release                       \
	      -DLLVM_ENABLE_PROJECTS="lld"                     \
	      -DLLVM_TARGETS_TO_BUILD="$(LLVM_TARGETS)"       \
	      -DLLVM_BUILD_TOOLS=OFF                           \
	      -DLLVM_BUILD_UTILS=OFF                           \
	      -DLLVM_BUILD_EXAMPLES=OFF                        \
	      -DLLVM_INCLUDE_TESTS=OFF                         \
	      -DLLVM_INCLUDE_BENCHMARKS=OFF                    \
	      -DLLVM_ENABLE_ZLIB=OFF                           \
	      -DLLVM_ENABLE_TERMINFO=OFF                       \
	      -DLLVM_ENABLE_ZSTD=OFF
	cmake --build $(LLVM_BUILD) -- -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc)

# ── OpenSSL build ─────────────────────────────────────────────
openssl: $(OPENSSL_LIB)

$(OPENSSL_LIB): $(OPENSSL_SRC)/Configure
	cd $(OPENSSL_SRC) && ./Configure \
	    --prefix=$(abspath $(OPENSSL_BUILD)) \
	    --openssldir=$(abspath $(OPENSSL_BUILD))/ssl \
	    $(OPENSSL_TARGET) \
	    no-shared no-tests no-docs
	$(MAKE) -C $(OPENSSL_SRC) build_libs
	$(MAKE) -C $(OPENSSL_SRC) install_dev

clean-openssl:
	$(MAKE) -C $(OPENSSL_SRC) clean 2>/dev/null; true
	rm -rf $(OPENSSL_BUILD)

# ── housekeeping ───────────────────────────────────────────────
clean:
	rm -rf build/obj bin

clean-llvm:
	rm -rf build/llvm

distclean: clean
	rm -rf build
