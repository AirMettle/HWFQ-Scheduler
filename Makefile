# H-WFQ Library Makefile
# Supports debug and release builds, tests, and examples

# ============================================================================
# Configuration
# ============================================================================

# Compiler
CC = gcc

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests
EXAMPLE_DIR = examples
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
LIB_DIR = $(BUILD_DIR)/lib
BIN_DIR = $(BUILD_DIR)/bin

# Library name
LIB_NAME = libhwfq.a

# Source files
SRC_FILES = $(SRC_DIR)/hwfq_config.c \
            $(SRC_DIR)/hwfq_memory.c \
            $(SRC_DIR)/hwfq_group_scheduler.c \
            $(SRC_DIR)/hwfq_group_calendar.c \
            $(SRC_DIR)/hwfq_flow_trie.c \
            $(SRC_DIR)/hwfq_chunked_entries.c \
            $(SRC_DIR)/hwfq_scheduler.c

# Object files
OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC_FILES))

# Test files
TEST_FILES = $(TEST_DIR)/test_config.c \
             $(TEST_DIR)/test_group_scheduler.c \
             $(TEST_DIR)/test_fairness.c \
             $(TEST_DIR)/test_hierarchical.c \
             $(TEST_DIR)/test_statistics.c \
             $(TEST_DIR)/test_performance.c \
             $(TEST_DIR)/test_stress.c \
             $(TEST_DIR)/test_fairness_extended.c \
             $(TEST_DIR)/test_edge_cases.c \
             $(TEST_DIR)/test_scale.c \
             $(TEST_DIR)/test_skew_progressive.c \
             $(TEST_DIR)/test_data_export.c \
             $(TEST_DIR)/test_concurrency.c \
             $(TEST_DIR)/test_group_scheduler_concurrency.c \
             $(TEST_DIR)/test_flow_trie.c \
             $(TEST_DIR)/test_chunked_entries.c \
             $(TEST_DIR)/test_calendar.c \
             $(TEST_DIR)/test_hierarchical_fairness_at_scale.c
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(BIN_DIR)/%,$(TEST_FILES))

# Example files
EXAMPLE_FILES = $(EXAMPLE_DIR)/basic_config.c \
                $(EXAMPLE_DIR)/custom_allocator.c \
                $(EXAMPLE_DIR)/async_worker.c
EXAMPLE_BINS = $(patsubst $(EXAMPLE_DIR)/%.c,$(BIN_DIR)/%,$(EXAMPLE_FILES))

# Compiler flags
CFLAGS_COMMON = -std=c11 -I$(INCLUDE_DIR) -I$(SRC_DIR) -Wall -Wextra -Wpedantic
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0 -DDEBUG
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O3 -DNDEBUG

# Linker flags
LDFLAGS = -L$(LIB_DIR) -lhwfq -lpthread -lm

# Default to release build
CFLAGS = $(CFLAGS_RELEASE)

# Build mode (debug or release)
MODE ?= release

ifeq ($(MODE),debug)
    CFLAGS = $(CFLAGS_DEBUG)
endif

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean test examples help debug release format tsan asan

# Default target
all: $(LIB_DIR)/$(LIB_NAME)

# Help target
help:
	@echo "H-WFQ Library Makefile"
	@echo ""
	@echo "Usage:"
	@echo "  make [target] [MODE=debug|release]"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the library (default: debug mode)"
	@echo "  test       - Build and run tests"
	@echo "  examples   - Build example programs"
	@echo "  debug      - Build library in debug mode"
	@echo "  release    - Build library in release mode"
	@echo "  tsan       - Build and run tests with ThreadSanitizer (race detection)"
	@echo "  asan       - Build and run tests with AddressSanitizer (memory leak detection)"
	@echo "  format     - Format all C source files using clang-format"
	@echo "  clean      - Remove all build artifacts"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build debug library"
	@echo "  make MODE=release       # Build release library"
	@echo "  make test               # Build and run tests"
	@echo "  make examples           # Build examples"
	@echo "  make clean              # Clean build artifacts"

# ============================================================================
# Library Build
# ============================================================================

# Build library
$(LIB_DIR)/$(LIB_NAME): $(OBJ_FILES) | $(LIB_DIR)
	@echo "Creating static library: $@"
	ar rcs $@ $^
	@echo "Library built successfully!"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# ============================================================================
# Tests
# ============================================================================

# Build and run tests
test: $(LIB_DIR)/$(LIB_NAME) $(TEST_BINS)
	@echo ""
	@echo "Running tests..."
	@echo "================"
	@for test in $(TEST_BINS); do \
		echo ""; \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done
	@echo ""
	@echo "All tests passed!"

# Test common object file
TEST_COMMON_OBJ = $(OBJ_DIR)/test_common.o

$(OBJ_DIR)/test_common.o: $(TEST_DIR)/test_common.c $(TEST_DIR)/test_common.h | $(OBJ_DIR)
	@echo "Compiling: $<"
	$(CC) $(CFLAGS) -I$(TEST_DIR) -c $< -o $@

# Build test binaries
$(BIN_DIR)/test_%: $(TEST_DIR)/test_%.c $(LIB_DIR)/$(LIB_NAME) $(TEST_COMMON_OBJ) | $(BIN_DIR)
	@echo "Building test: $@"
	$(CC) $(CFLAGS) -I$(TEST_DIR) $< $(TEST_COMMON_OBJ) -o $@ $(LDFLAGS)

# ============================================================================
# Examples
# ============================================================================

# Build example programs
examples: $(LIB_DIR)/$(LIB_NAME) $(EXAMPLE_BINS)
	@echo ""
	@echo "Examples built successfully!"
	@echo "Run examples from $(BIN_DIR)/"

# Build example binaries
$(BIN_DIR)/%: $(EXAMPLE_DIR)/%.c $(LIB_DIR)/$(LIB_NAME) | $(BIN_DIR)
	@echo "Building example: $@"
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# ============================================================================
# Build Modes
# ============================================================================

# Debug build
debug:
	@$(MAKE) MODE=debug all

# Release build
release:
	@$(MAKE) MODE=release all

# ============================================================================
# ThreadSanitizer Build (Race Detection)
# ============================================================================

# Build and run tests with ThreadSanitizer enabled
# Usage: make tsan
tsan: CFLAGS += -fsanitize=thread -fno-omit-frame-pointer
tsan: LDFLAGS += -fsanitize=thread
tsan: clean all test

# ============================================================================
# AddressSanitizer Build (Memory Leak Detection)
# ============================================================================

# Build and run tests with AddressSanitizer enabled
# Detects: buffer overflows, use-after-free, double-free
# On Linux, also enables leak sanitizer for memory leak detection
# Usage: make asan
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer
ifeq ($(shell uname),Linux)
    ASAN_FLAGS += -fsanitize=leak
endif

asan: CFLAGS += $(ASAN_FLAGS)
asan: LDFLAGS += $(ASAN_FLAGS)
asan: clean all test

# ============================================================================
# Directory Creation
# ============================================================================

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(LIB_DIR):
	@mkdir -p $(LIB_DIR)

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# ============================================================================
# Code Formatting
# ============================================================================

# Format all C source files using clang-format
format:
	@echo "Formatting C source files..."
	@clang-format -i $(SRC_DIR)/*.c $(SRC_DIR)/*.h
	@clang-format -i $(INCLUDE_DIR)/*.h
	@clang-format -i $(TEST_DIR)/*.c $(TEST_DIR)/*.h
	@clang-format -i $(EXAMPLE_DIR)/*.c
	@echo "Formatting complete!"

# ============================================================================
# Cleanup
# ============================================================================

clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)
	@echo "Clean complete!"

# ============================================================================
# Dependencies
# ============================================================================

# Header dependencies
$(OBJ_DIR)/hwfq_config.o: $(SRC_DIR)/hwfq_config.c $(INCLUDE_DIR)/hwfq.h $(SRC_DIR)/hwfq_internal.h
$(OBJ_DIR)/hwfq_memory.o: $(SRC_DIR)/hwfq_memory.c $(INCLUDE_DIR)/hwfq.h $(SRC_DIR)/hwfq_internal.h
$(OBJ_DIR)/hwfq_group_scheduler.o: $(SRC_DIR)/hwfq_group_scheduler.c $(INCLUDE_DIR)/hwfq.h $(SRC_DIR)/hwfq_group_scheduler.h $(SRC_DIR)/hwfq_group_scheduler_internal.h
$(OBJ_DIR)/hwfq_group_calendar.o: $(SRC_DIR)/hwfq_group_calendar.c $(SRC_DIR)/hwfq_group_scheduler.h $(SRC_DIR)/hwfq_group_scheduler_internal.h
$(OBJ_DIR)/hwfq_flow_trie.o: $(SRC_DIR)/hwfq_flow_trie.c $(SRC_DIR)/hwfq_flow_trie.h $(INCLUDE_DIR)/hwfq.h
$(OBJ_DIR)/hwfq_chunked_entries.o: $(SRC_DIR)/hwfq_chunked_entries.c $(SRC_DIR)/hwfq_chunked_entries.h $(SRC_DIR)/hwfq_group_scheduler_internal.h
$(OBJ_DIR)/hwfq_scheduler.o: $(SRC_DIR)/hwfq_scheduler.c $(INCLUDE_DIR)/hwfq.h $(SRC_DIR)/hwfq_internal.h $(SRC_DIR)/hwfq_group_scheduler.h

$(BIN_DIR)/test_config: $(TEST_DIR)/test_config.c $(INCLUDE_DIR)/hwfq.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_group_scheduler: $(TEST_DIR)/test_group_scheduler.c $(INCLUDE_DIR)/hwfq.h $(SRC_DIR)/hwfq_group_scheduler.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_fairness: $(TEST_DIR)/test_fairness.c $(INCLUDE_DIR)/hwfq.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_hierarchical: $(TEST_DIR)/test_hierarchical.c $(INCLUDE_DIR)/hwfq.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_group_scheduler_concurrency: $(TEST_DIR)/test_group_scheduler_concurrency.c $(INCLUDE_DIR)/hwfq.h $(SRC_DIR)/hwfq_group_scheduler.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/basic_config: $(EXAMPLE_DIR)/basic_config.c $(INCLUDE_DIR)/hwfq.h
$(BIN_DIR)/custom_allocator: $(EXAMPLE_DIR)/custom_allocator.c $(INCLUDE_DIR)/hwfq.h
$(BIN_DIR)/async_worker: $(EXAMPLE_DIR)/async_worker.c $(INCLUDE_DIR)/hwfq.h
$(BIN_DIR)/test_flow_trie: $(TEST_DIR)/test_flow_trie.c $(SRC_DIR)/hwfq_flow_trie.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_chunked_entries: $(TEST_DIR)/test_chunked_entries.c $(SRC_DIR)/hwfq_chunked_entries.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_calendar: $(TEST_DIR)/test_calendar.c $(SRC_DIR)/hwfq_group_scheduler_internal.h $(TEST_DIR)/test_common.h
