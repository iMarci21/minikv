CC ?= gcc

# Flags the project depends on. Always applied, even if CFLAGS is overridden.
REQUIRED_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE
# Tunable flags. Override freely: `make CFLAGS="-O0 -g3"`.
CFLAGS ?= -O2 -g
SANITIZE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
                  -fno-omit-frame-pointer
LDFLAGS ?=
LDLIBS ?=

# Pinned so local and CI formatting are identical (CI installs this exact version).
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_VERSION := 22.1.8

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
# Only files that exist; avoids passing a literal "src/*.h" when there are no headers yet.
FORMAT_FILES := $(SRC) $(HDR) $(wildcard tests/*.c tests/*/*.c tests/*.h tests/*/*.h)

# Each build mode gets its own object directory, so release and sanitizer objects
# can never be mixed and switching modes needs no `make clean`.
RELEASE_DIR := build/release
SANITIZE_DIR := build/sanitize
RELEASE_OBJ := $(SRC:src/%.c=$(RELEASE_DIR)/%.o)
SANITIZE_OBJ := $(SRC:src/%.c=$(SANITIZE_DIR)/%.o)
RELEASE_BIN := $(RELEASE_DIR)/minikv
SANITIZE_BIN := $(SANITIZE_DIR)/minikv

.PHONY: all release sanitize test test-sanitize format format-check clean

all: release

release: $(RELEASE_BIN)

sanitize: $(SANITIZE_BIN)

$(RELEASE_BIN): $(RELEASE_OBJ)
	$(CC) $(REQUIRED_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(SANITIZE_BIN): $(SANITIZE_OBJ)
	$(CC) $(REQUIRED_CFLAGS) $(SANITIZE_FLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# -MMD -MP emit .d files listing each object's header dependencies (included below),
# so editing a header rebuilds every object that includes it.
$(RELEASE_DIR)/%.o: src/%.c | $(RELEASE_DIR)
	$(CC) $(REQUIRED_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(SANITIZE_DIR)/%.o: src/%.c | $(SANITIZE_DIR)
	$(CC) $(REQUIRED_CFLAGS) $(SANITIZE_FLAGS) -MMD -MP -c $< -o $@

$(RELEASE_DIR) $(SANITIZE_DIR):
	mkdir -p $@

test: $(RELEASE_BIN)
	./tests/integration.sh $(RELEASE_BIN)

test-sanitize: $(SANITIZE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	./tests/integration.sh $(SANITIZE_BIN)

format:
	@$(CLANG_FORMAT) --version | grep -q " $(CLANG_FORMAT_VERSION)" || \
		echo "warning: expected clang-format $(CLANG_FORMAT_VERSION), output may differ from CI" >&2
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	@$(CLANG_FORMAT) --version | grep -q " $(CLANG_FORMAT_VERSION)" || \
		echo "warning: expected clang-format $(CLANG_FORMAT_VERSION), output may differ from CI" >&2
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

clean:
	rm -rf build

-include $(RELEASE_OBJ:.o=.d) $(SANITIZE_OBJ:.o=.d)
