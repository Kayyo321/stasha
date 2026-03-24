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

SRCS = src/main.c         \
       src/common/common.c \
       src/lexer/lexer.c   \
       src/ast/ast.c       \
       src/parser/parser.c \
       src/codegen/codegen.c

OBJS = $(patsubst src/%.c,build/obj/%.o,$(SRCS))
LINKER_OBJ = build/obj/linker/linker.o

TARGET = bin/stasha

# ── Thread runtime ─────────────────────────────────────────────────────────
THREAD_RUNTIME_SRC = src/runtime/thread_runtime.c
THREAD_RUNTIME_OBJ = build/obj/runtime/thread_runtime.o
THREAD_RUNTIME_LIB = bin/thread_runtime.a

# ── Thread test programs ────────────────────────────────────────────────────
THREAD_TEST_SRCS = examples/thread_basic.sts    \
                   examples/thread_return.sts   \
                   examples/thread_many_jobs.sts \
                   examples/thread_stress.sts   \
                   examples/future_wait.sts

# ── Standard library ──────────────────────────────────────────────────────
STDLIB_SRCS := $(shell find stsstdlib -name '*.sts' 2>/dev/null)
STDLIB_LIBS := $(foreach s,$(STDLIB_SRCS),$(dir $(s))lib$(notdir $(basename $(s))).a)

UNAME_S := $(shell uname -s)

# ── OpenSSL (static libcrypto) ────────────────────────────────────────────
OPENSSL_SRC   = extlib/extlib/openssl
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

.PHONY: all stdlib stdlib-test thread-runtime clean clean-stdlib clean-llvm llvm openssl clean-openssl test-threads

all: $(TARGET) thread-runtime

# Build every .sts under stsstdlib/ into a .a alongside the source,
# then install the .a and .sts files into bin/stdlib/, then run all tests.
stdlib: $(TARGET) $(STDLIB_LIBS) stdlib-test
	@mkdir -p bin/stdlib
	@for s in $(STDLIB_SRCS); do \
	    a="$$(dirname $$s)/lib$$(basename $${s%.sts}).a"; \
	    cp "$$a" bin/stdlib/; \
	    cp "$$s" bin/stdlib/; \
	done
	@echo "stdlib installed -> bin/stdlib/"

# Modules that require platform-specific external libs not available everywhere.
# These are compiled into .a files but skipped during 'make stdlib-test'.
STDLIB_TEST_SKIP = stsstdlib/random/complex_rng.sts

# Run 'stasha test' on every stdlib source file.
# Prints a pass/fail summary and exits non-zero if any test fails.
stdlib-test: $(TARGET)
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
	echo ""; \
	echo "  Passed: $$pass   Failed: $$fail   Skipped: $$skip   Total: $$((pass+fail+skip))"; \
	echo ""; \
	if [ $$fail -gt 0 ]; then \
	    echo "=== STDLIB TESTS FAILED ==="; exit 1; \
	else \
	    echo "=== All stdlib tests passed ==="; \
	fi

# Generated rule: stsstdlib/<cat>/lib<name>.a  ←  stsstdlib/<cat>/<name>.sts
define stdlib-rule
$(dir $(1))lib$(notdir $(basename $(1))).a: $(1) $(TARGET)
	@mkdir -p $$(dir $$@)
	$(TARGET) lib $$< -o $$@
endef
$(foreach s,$(STDLIB_SRCS),$(eval $(call stdlib-rule,$(s))))

clean-stdlib:
	find stsstdlib -name '*.a' -delete 2>/dev/null; true
	rm -rf bin/stdlib

thread-runtime: $(THREAD_RUNTIME_LIB)

$(THREAD_RUNTIME_OBJ): $(THREAD_RUNTIME_SRC) src/runtime/thread_runtime.h
	@mkdir -p $(dir $@)
	$(CC) -std=c2x -O2 -Wall -c -o $@ $<

$(THREAD_RUNTIME_LIB): $(THREAD_RUNTIME_OBJ) | bin
	ar rcs $@ $<

$(TARGET): $(OBJS) $(LINKER_OBJ) | bin
	$(CC) -o $@ $(OBJS) $(LINKER_OBJ) $(LDFLAGS)

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
