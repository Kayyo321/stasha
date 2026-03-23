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

.PHONY: all stdlib clean clean-stdlib clean-llvm llvm openssl clean-openssl

all: $(TARGET)

# Build every .sts under stsstdlib/ into a .a alongside the source,
# then install the .a and .sts files into bin/stdlib/ so that
# `libimp "name" from std;` can find them at runtime.
stdlib: $(TARGET) $(STDLIB_LIBS)
	@mkdir -p bin/stdlib
	@for s in $(STDLIB_SRCS); do \
	    a="$$(dirname $$s)/lib$$(basename $${s%.sts}).a"; \
	    cp "$$a" bin/stdlib/; \
	    cp "$$s" bin/stdlib/; \
	done
	@echo "stdlib installed -> bin/stdlib/"

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

$(TARGET): $(OBJS) $(LINKER_OBJ) | bin
	$(CC) -o $@ $(OBJS) $(LINKER_OBJ) $(LDFLAGS)

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
