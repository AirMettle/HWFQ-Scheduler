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
            $(SRC_DIR)/hwfq_group_calendar.c

# Object files
OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC_FILES))

# Test files
TEST_FILES = $(TEST_DIR)/test_config.c \
             $(TEST_DIR)/test_group_scheduler.c
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(BIN_DIR)/%,$(TEST_FILES))

# Example files
EXAMPLE_FILES = $(EXAMPLE_DIR)/basic_config.c \
                $(EXAMPLE_DIR)/custom_allocator.c
EXAMPLE_BINS = $(patsubst $(EXAMPLE_DIR)/%.c,$(BIN_DIR)/%,$(EXAMPLE_FILES))

# Compiler flags
CFLAGS_COMMON = -std=c11 -I$(INCLUDE_DIR) -I$(SRC_DIR) -Wall -Wextra -Wpedantic
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0 -DDEBUG
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -DNDEBUG

# Linker flags
LDFLAGS = -L$(LIB_DIR) -lhwfq -lpthread

# Default to debug build
CFLAGS = $(CFLAGS_DEBUG)

# Build mode (debug or release)
MODE ?= debug

ifeq ($(MODE),release)
    CFLAGS = $(CFLAGS_RELEASE)
endif

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean test examples help debug release format

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

# Build test binaries
$(BIN_DIR)/test_%: $(TEST_DIR)/test_%.c $(LIB_DIR)/$(LIB_NAME) | $(BIN_DIR)
	@echo "Building test: $@"
	$(CC) $(CFLAGS) -I$(TEST_DIR) $< -o $@ $(LDFLAGS)

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
$(OBJ_DIR)/hwfq_group_scheduler.o: $(SRC_DIR)/hwfq_group_scheduler.c $(INCLUDE_DIR)/hwfq.h $(INCLUDE_DIR)/hwfq_group_scheduler.h $(SRC_DIR)/hwfq_group_scheduler_internal.h
$(OBJ_DIR)/hwfq_group_calendar.o: $(SRC_DIR)/hwfq_group_calendar.c $(INCLUDE_DIR)/hwfq_group_scheduler.h $(SRC_DIR)/hwfq_group_scheduler_internal.h

$(BIN_DIR)/test_config: $(TEST_DIR)/test_config.c $(INCLUDE_DIR)/hwfq.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/test_group_scheduler: $(TEST_DIR)/test_group_scheduler.c $(INCLUDE_DIR)/hwfq.h $(INCLUDE_DIR)/hwfq_group_scheduler.h $(TEST_DIR)/test_common.h
$(BIN_DIR)/basic_config: $(EXAMPLE_DIR)/basic_config.c $(INCLUDE_DIR)/hwfq.h
$(BIN_DIR)/custom_allocator: $(EXAMPLE_DIR)/custom_allocator.c $(INCLUDE_DIR)/hwfq.h
