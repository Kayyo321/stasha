CC         = clang
CXX        = clang++
LLVM_BUILD = build/llvm
LLVM_CFG   = $(LLVM_BUILD)/bin/llvm-config

# Flags resolved at recipe time (recursive =) so llvm-config is found after build
LLVM_CFLAGS  = $(shell $(LLVM_CFG) --cflags  2>/dev/null)
LLVM_LDFLAGS = $(shell $(LLVM_CFG) --ldflags --libs core analysis native \
               lto passes option codegen bitwriter debuginfodwarf \
               objcarcopts textapi --system-libs 2>/dev/null)

CFLAGS   = -Wall -Wextra -std=c2x -Isrc $(LLVM_CFLAGS)
CXXFLAGS = -Wall -Wextra -std=c++17 -Isrc $(LLVM_CFLAGS)
LDFLAGS  = $(LLVM_LDFLAGS) -lc++

SRCS = src/main.c         \
       src/common/common.c \
       src/lexer/lexer.c   \
       src/ast/ast.c       \
       src/parser/parser.c \
       src/codegen/codegen.c

OBJS = $(patsubst src/%.c,build/obj/%.o,$(SRCS))
LINKER_OBJ = build/obj/linker/linker.o

TARGET = bin/stasha

.PHONY: all clean llvm

all: $(TARGET)

$(TARGET): $(OBJS) $(LINKER_OBJ) | bin
	$(CC) -o $@ $(OBJS) $(LINKER_OBJ) $(LDFLAGS)

build/obj/%.o: src/%.c | $(LLVM_CFG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LINKER_OBJ): src/linker/linker.cpp | $(LLVM_CFG)
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

llvm: $(LLVM_CFG)

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

# ── housekeeping ───────────────────────────────────────────────
clean:
	rm -rf build/obj bin

distclean: clean
	rm -rf build
