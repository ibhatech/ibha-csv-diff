# ibha-csvdiff-core
#
# Targets:
#   make            native static library, CLI, tests, benchmark
#   make test       build and run the unit tests
#   make bench      build and run the throughput benchmark
#   make wasm       freestanding wasm32 modules (needs a wasm capable clang, see below)
#   make wasm-check the same, then run them under node
#   make fuzz       libFuzzer targets, built with FUZZ_CC (default clang).
#                   Run them with scripts-and-commands/run_libfuzzer.py
#   make asan       tests under AddressSanitizer and UBSan
#   make clean
#
# The wasm target needs a clang built with the WebAssembly backend plus wasm-ld.
# Apple's bundled clang has neither. Point WASM_CC and WASM_LD at a toolchain that
# does, for example:
#
#   brew install llvm
#   make wasm WASM_CC=/opt/homebrew/opt/llvm/bin/clang \
#             WASM_LD=/opt/homebrew/opt/llvm/bin/wasm-ld
#
# or install wasi-sdk and point at its bin directory. `make wasm` fails with a
# clear message rather than a confusing compiler error when neither is present.

CC      ?= cc
AR      ?= ar
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
           -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
OPT     ?= -O3
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) -fno-strict-aliasing -Iinclude
LDFLAGS ?=

BUILD   := build
LIB     := $(BUILD)/libibha_csvdiff.a
CLI     := $(BUILD)/ibha-csvdiff
TESTBIN := $(BUILD)/test_core
BENCHBIN:= $(BUILD)/bench_parse

TEST_SRC := tests/test_core.c tests/test_hash.c tests/test_parse.c \
            tests/test_schema.c tests/test_normalize.c tests/test_match.c \
            tests/test_diff.c tests/test_validate.c tests/test_columns.c \
            tests/test_segment.c \
            tests/test_emit.c tests/test_property.c

# The property tests read the generated corpus, so `make test` builds it rather
# than quietly skipping those assertions when it is absent.
FIXTURES := fixtures/generated/tiny_source.csv

CORE_SRC := src/arena.c src/ctx.c src/reader.c src/ingest.c src/hash.c \
            src/parse.c src/schema.c src/normalize.c src/match.c src/diff.c \
            src/validate.c src/segment.c src/emit.c src/sys_libc.c
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC))

# Sources for the wasm build: same core, different system layer.
WASM_SRC := src/arena.c src/ctx.c src/reader.c src/ingest.c src/hash.c \
            src/parse.c src/schema.c src/normalize.c src/match.c src/diff.c \
            src/validate.c src/segment.c src/emit.c src/sys_wasm.c

WASM_CC ?= clang
WASM_LD ?= wasm-ld

# libFuzzer is a clang runtime and GCC has no equivalent, so the fuzz targets get
# their own compiler rather than forcing CC to clang for the whole build. Every
# other target stays on CC, which is what the warning-free claim is measured
# against.
FUZZ_CC ?= clang
WASM_CFLAGS := $(CSTD) $(WARN) -O3 -flto -Iinclude \
               --target=wasm32 -nostdlib -ffreestanding \
               -fvisibility=hidden -fno-builtin
# Passed through the driver, so each flag carries its own -Wl, prefix.
# --stack-first puts the shadow stack below static data so a stack overflow traps
# on a guard page rather than silently corrupting the data segment.
WASM_LDFLAGS := -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
                -Wl,--initial-memory=1048576 -Wl,--max-memory=2147483648 \
                -Wl,--stack-first -Wl,--gc-sections -Wl,-O2

.PHONY: all test bench wasm wasm-check fuzz fuzz-native fuzz-native-notrap asan ubsan \
        valgrind clean fixtures check-wasm-toolchain check-sanitizers

all: $(LIB) $(CLI) $(TESTBIN) $(BENCHBIN)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/ibha_csvdiff.h src/internal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $(CORE_OBJ)

$(CLI): cli/main.c $(LIB)
	$(CC) $(CFLAGS) cli/main.c $(LIB) -o $@ $(LDFLAGS)

$(TESTBIN): $(TEST_SRC) $(CORE_SRC) tests/tap.h tests/suites.h | $(BUILD)
	$(CC) $(CFLAGS) $(TEST_SRC) $(CORE_SRC) -o $@ $(LDFLAGS)

$(BENCHBIN): bench/bench_parse.c $(LIB)
	$(CC) $(CFLAGS) bench/bench_parse.c $(LIB) -o $@ $(LDFLAGS)

$(FIXTURES):
	@python3 fixtures/gen_fixtures.py

test: $(TESTBIN) $(FIXTURES)
	@$(TESTBIN) fixtures/generated

bench: $(BENCHBIN) $(FIXTURES)
	@$(BENCHBIN) --mb 15 --repeat 5

# ---------------------------------------------------------------------- wasm --

check-wasm-toolchain:
	@$(WASM_CC) --target=wasm32 -x c -c /dev/null -o /dev/null 2>/dev/null \
	  || { echo ""; \
	       echo "error: '$(WASM_CC)' cannot target wasm32."; \
	       echo "       Apple's bundled clang has no WebAssembly backend."; \
	       echo "       Install one and point at it, for example:"; \
	       echo "         brew install llvm"; \
	       echo "         make wasm WASM_CC=/opt/homebrew/opt/llvm/bin/clang \\"; \
	       echo "                   WASM_LD=/opt/homebrew/opt/llvm/bin/wasm-ld"; \
	       echo ""; exit 1; }
	@command -v $(WASM_LD) >/dev/null 2>&1 \
	  || { echo "error: wasm-ld not found at '$(WASM_LD)'. Set WASM_LD."; exit 1; }

# Two modules, per spec 13.11: SIMD is the expected runtime path on desktop
# browsers, and the scalar build is retained as the differential oracle for
# testing the SIMD parser rather than as a build we expect to ship.
wasm: check-wasm-toolchain $(BUILD)/ibha_csvdiff.wasm $(BUILD)/ibha_csvdiff.simd.wasm \
      $(BUILD)/abi_offsets.wasm

# Instantiates both modules and drives a parse, a diff and a cursor drain inside
# them. A module that links and exports nothing looks exactly like one that works
# until something tries to drive it, so building is not the same as working.
wasm-check: wasm
	@command -v node >/dev/null 2>&1 \
	  || { echo "error: node is not installed, and the wasm check runs under it."; exit 1; }
	@node ../scripts-and-commands/wasm_smoke.mjs $(BUILD)/ibha_csvdiff.wasm
	@node ../scripts-and-commands/wasm_smoke.mjs $(BUILD)/ibha_csvdiff.simd.wasm

$(BUILD)/ibha_csvdiff.wasm: $(WASM_SRC) include/ibha_csvdiff.h src/internal.h | $(BUILD)
	$(WASM_CC) $(WASM_CFLAGS) $(WASM_SRC) -o $@ $(WASM_LDFLAGS)
	@ls -l $@ | awk '{print "  scalar wasm: " $$5 " bytes"}'

$(BUILD)/ibha_csvdiff.simd.wasm: $(WASM_SRC) include/ibha_csvdiff.h src/internal.h | $(BUILD)
	$(WASM_CC) $(WASM_CFLAGS) -msimd128 $(WASM_SRC) -o $@ $(WASM_LDFLAGS)
	@ls -l $@ | awk '{print "  simd wasm:   " $$5 " bytes"}'

# Reports the wasm32 layout of every public struct, so the JS binding's offsets
# are generated by the compiler that builds the module rather than written out by
# hand. It links against no engine code: offsetof needs none.
$(BUILD)/abi_offsets.wasm: tools/abi_offsets.c include/ibha_csvdiff.h | $(BUILD)
	$(WASM_CC) $(WASM_CFLAGS) tools/abi_offsets.c -o $@ $(WASM_LDFLAGS)

# ------------------------------------------------------------- correctness ----

# The sanitizer runtime is a separate package on some distributions, and a
# missing one surfaces as a link error naming a file nobody has heard of. Check
# once, and say what to install and what to run in the meantime.
check-sanitizers:
	@echo 'int main(void){return 0;}' | $(CC) $(CSTD) -fsanitize=address,undefined \
	  -x c - -o /dev/null 2>/dev/null \
	  || { echo ""; \
	       echo "error: '$(CC)' cannot link the AddressSanitizer runtime."; \
	       echo "       On Fedora and RHEL it is a separate package:"; \
	       echo "         sudo dnf install libasan libubsan"; \
	       echo "       On Debian and Ubuntu it ships with gcc."; \
	       echo "       Meanwhile, and these need nothing extra:"; \
	       echo "         make ubsan      undefined behaviour, trapping, no runtime"; \
	       echo "         make valgrind   memory safety under memcheck"; \
	       echo ""; exit 1; }

# AddressSanitizer plus UBSan on the unit tests. Cheap, and per spec section 10
# this is a large part of why C is acceptable for this engine.
#
# detect_stack_use_after_return is not on by default and is switched on here
# deliberately: the parser borrows rather than copies, so a test that hands it a
# stack buffer builds an index into a dead frame. That reads correctly at -O3 and
# wrongly under a sanitizer, which is the worst way to find out.
asan: check-sanitizers $(FIXTURES) | $(BUILD)
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude $(TEST_SRC) $(CORE_SRC) -o $(BUILD)/test_core_asan
	@ASAN_OPTIONS=detect_stack_use_after_return=1 $(BUILD)/test_core_asan fixtures/generated

# UndefinedBehaviorSanitizer in trapping mode. It needs no runtime library at
# all, because a violation becomes an illegal instruction rather than a
# diagnostic: harder to read when it fires, identical in what it detects, and it
# runs on a machine where libubsan is not installed.
ubsan: $(FIXTURES) | $(BUILD)
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error \
	  -fno-sanitize-recover=all -fno-omit-frame-pointer \
	  -Iinclude $(TEST_SRC) $(CORE_SRC) -o $(BUILD)/test_core_ubsan
	@$(BUILD)/test_core_ubsan fixtures/generated

# Memory safety without a sanitizer runtime. Coarser than ASan inside an arena
# block, which memcheck cannot see into because the whole block is one live
# allocation, and still what catches a use after free of a context or a read past
# the end of an index array.
valgrind: $(FIXTURES) | $(BUILD)
	@command -v valgrind >/dev/null 2>&1 || { echo "error: valgrind is not installed."; exit 1; }
	$(CC) $(CSTD) $(WARN) -O1 -g -Iinclude $(TEST_SRC) $(CORE_SRC) -o $(BUILD)/test_core_vg
	@valgrind -q --error-exitcode=9 --leak-check=no $(BUILD)/test_core_vg fixtures/generated

# Four libFuzzer targets. The ingest one stays because it covers the limit and
# retention paths. The parse one is the surface that matters: the state machine
# is the only place where untrusted bytes drive control flow, which spec 2.3
# named as the main reason C is acceptable for this engine.
fuzz: | $(BUILD)
	@$(FUZZ_CC) $(CSTD) -fsanitize=fuzzer -x c -c /dev/null -o /dev/null 2>/dev/null \
	  || { echo ""; \
	       echo "error: '$(FUZZ_CC)' has no libFuzzer runtime."; \
	       echo "       On Fedora, RHEL and Oracle Linux it comes with clang:"; \
	       echo "         sudo dnf install clang"; \
	       echo "       Apple's bundled clang does not ship one, so install a"; \
	       echo "       clang that does and point at it:"; \
	       echo "         brew install llvm"; \
	       echo "         make fuzz FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang"; \
	       echo "       or run the same targets under the built in driver, which"; \
	       echo "       needs no extra toolchain:"; \
	       echo "         make fuzz-native"; \
	       echo ""; exit 1; }
	$(FUZZ_CC) $(CSTD) -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude \
	  tests/fuzz_ingest.c $(CORE_SRC) -o $(BUILD)/fuzz_ingest
	$(FUZZ_CC) $(CSTD) -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude \
	  tests/fuzz_parse.c $(CORE_SRC) -o $(BUILD)/fuzz_parse
	$(FUZZ_CC) $(CSTD) -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude \
	  tests/fuzz_diff.c $(CORE_SRC) -o $(BUILD)/fuzz_diff
	$(FUZZ_CC) $(CSTD) -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude \
	  tests/fuzz_emit.c $(CORE_SRC) -o $(BUILD)/fuzz_emit
	@echo ""
	@echo "built with $(FUZZ_CC). Seed a corpus and run, longest first:"
	@echo "run: $(BUILD)/fuzz_parse  -max_total_time=60 corpus/parse"
	@echo "run: $(BUILD)/fuzz_diff   -max_total_time=60 corpus/diff"
	@echo "run: $(BUILD)/fuzz_emit   -max_total_time=60 corpus/emit"
	@echo "run: $(BUILD)/fuzz_ingest -max_total_time=60 corpus/ingest"
	@echo "or all four with a seeded corpus:"
	@echo "  python3 ../scripts-and-commands/run_libfuzzer.py --seconds 90"

# The same fuzz targets under a self contained driver, so the parser's untrusted
# input surface is exercised on any machine and in CI without a second
# toolchain. Weaker than libFuzzer, which has coverage feedback, and still the
# thing that catches a resumability regression the moment it lands.
FUZZ_ITERS ?= 200000

fuzz-native: check-sanitizers | $(BUILD)
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude tests/fuzz_main.c tests/fuzz_parse.c $(CORE_SRC) -o $(BUILD)/fuzz_parse_native
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude tests/fuzz_main.c tests/fuzz_diff.c $(CORE_SRC) -o $(BUILD)/fuzz_diff_native
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude tests/fuzz_main.c tests/fuzz_ingest.c $(CORE_SRC) -o $(BUILD)/fuzz_ingest_native
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Iinclude tests/fuzz_main.c tests/fuzz_emit.c $(CORE_SRC) -o $(BUILD)/fuzz_emit_native
	@$(BUILD)/fuzz_parse_native $(FUZZ_ITERS)
	@$(BUILD)/fuzz_diff_native $(FUZZ_ITERS)
	@$(BUILD)/fuzz_emit_native $(FUZZ_ITERS)
	@$(BUILD)/fuzz_ingest_native $(FUZZ_ITERS)

# The same three targets with trapping UBSan instead of ASan, for a machine with
# no sanitizer runtime. It still catches every invariant the targets assert, and
# every undefined behaviour, but not a heap overrun.
fuzz-native-notrap: | $(BUILD)
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error \
	  -fno-sanitize-recover=all \
	  -Iinclude tests/fuzz_main.c tests/fuzz_parse.c $(CORE_SRC) -o $(BUILD)/fuzz_parse_notrap
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error \
	  -fno-sanitize-recover=all \
	  -Iinclude tests/fuzz_main.c tests/fuzz_diff.c $(CORE_SRC) -o $(BUILD)/fuzz_diff_notrap
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error \
	  -fno-sanitize-recover=all \
	  -Iinclude tests/fuzz_main.c tests/fuzz_emit.c $(CORE_SRC) -o $(BUILD)/fuzz_emit_notrap
	@$(BUILD)/fuzz_parse_notrap $(FUZZ_ITERS)
	@$(BUILD)/fuzz_diff_notrap $(FUZZ_ITERS)
	@$(BUILD)/fuzz_emit_notrap $(FUZZ_ITERS)

fixtures:
	@python3 fixtures/gen_fixtures.py

clean:
	rm -rf $(BUILD)
