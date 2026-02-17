SUITE := ghoti.io
PROJECT := image

BUILD ?= release
BRANCH := -dev
# If BUILD is debug, append -debug
ifeq ($(BUILD),debug)
    BRANCH := $(BRANCH)-debug
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ENV_VARS :=

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	# Additional Linux-specific variables
	PKG_CONFIG_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	# Additional macOS-specific variables
	PKG_CONFIG_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	BUILD := mac/$(BUILD)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PKG_CONFIG_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	# Windows paths for .pc so gcc invoked by mingw can resolve -I/-L (cygpath for MSYS2)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PKG_CONFIG_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	# Windows paths for .pc so gcc invoked by mingw can resolve -I/-L (cygpath for MSYS2)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))

endif


CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g
CC := cc
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c17 -O0 -g
# Library-specific compile flags (export symbols on Windows, PIC on Linux)
# GIMG_BUILD enables DLL export on Windows (checked by GIMG_API macro)
# GIMG_TEST_BUILD enables export of internal functions for testing (checked by GIMG_INTERNAL_API macro)
LIB_CFLAGS := $(CFLAGS) -DGIMG_BUILD -DGIMG_TEST_BUILD
LDFLAGS := -L /usr/lib -lstdc++ -lm
BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps


# Add OS-specific flags
ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC

else ifeq ($(UNAME_S), Darwin)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows

else
	$(error Unsupported OS: $(UNAME_S))

endif

# The standard include directories for the project.
INCLUDE := -I include/ -I $(GEN_DIR)/
# ghoti.io-compress (required for PNG codec). Prefer pkg-config; fallback to sibling.
COMPRESS_PC ?= ghoti.io-compress
COMPRESS_CFLAGS := $(shell pkg-config --cflags $(COMPRESS_PC) 2>/dev/null)
COMPRESS_LIBS := $(shell pkg-config --libs $(COMPRESS_PC) 2>/dev/null)
# Use sibling path when pkg-config failed (empty) or returned unsubstituted placeholder.
COMPRESS_PLACEHOLDER := (
COMPRESS_NEED_FALLBACK := $(or $(findstring $(COMPRESS_PLACEHOLDER),$(COMPRESS_CFLAGS)),$(if $(COMPRESS_CFLAGS),,y))
ifneq ($(COMPRESS_NEED_FALLBACK),)
COMPRESS_CFLAGS := -I../compress/include
COMPRESS_LIBS := -L../compress/build/$(BUILD)/apps -lghoti.io-compress$(BRANCH)
# Let linker resolve image .so's dependency on compress when linking tests.
LDFLAGS += -Wl,-rpath-link,../compress/build/$(BUILD)/apps
endif
INCLUDE += $(COMPRESS_CFLAGS)

# Automatically collect all .c source files under the src directory.
SOURCES := $(shell find src -type f -name '*.c')

# Convert each source file path to an object file path.
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))


TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --cflags gtest`

# Valgrind flags (exclude "still reachable" as it's not a leak)
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1

####################################################################
# Test discovery
####################################################################

# Optional shared test helper (if present). If you don't use it, you can omit it.
TEST_HELPER_SRC := $(wildcard tests/test_helpers.cpp)
TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))


IMAGELIBRARY := -L $(APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)

# Single shell: discover test sources and compute executable name for each (path|name per line).
# test.cpp -> testImage; test_foo.cpp -> testFoo. Avoids hundreds of $(call test-name) / CreateProcess on Windows.
TEST_PAIRS := $(shell find tests -type f -name 'test*.cpp' 2>/dev/null | sort | grep -v test_helpers | while read f; do \
	if [ "$$f" = "tests/test.cpp" ]; then echo "$$f|testImage"; \
	else echo "$$f|$$(basename "$$f" .cpp | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/')"; fi; done)
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))

# Generate list of test executables (no $(call test-name) - use precomputed TEST_NAMES)
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# Automatically collect all example .c files under examples directories.
EXAMPLE_SOURCES := $(shell find examples -type f -name '*.c' 2>/dev/null)

# Convert each example source file path to an executable path.
EXAMPLES := $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(EXAMPLE_SOURCES))


all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build shared + static libraries

####################################################################
# Dependency Inclusion
####################################################################

# PNG test helper (shared by test_png_decode and test_png_encode).
PNG_TEST_UTILS_OBJ := $(OBJ_DIR)/tests/png_test_utils.o
# JPEG test helper (shared by test_jpeg_load, optional for test_jpeg_encode).
JPEG_TEST_UTILS_OBJ := $(OBJ_DIR)/tests/jpeg_test_utils.o

# Explicit list of dependency files (no wildcard: same set on all platforms, faster make startup).
TEST_DEPFILES := $(foreach pair,$(TEST_PAIRS),$(OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_HELPER_OBJ:.o=.d) $(TEST_DEPFILES) $(PNG_TEST_UTILS_OBJ:.o=.d) $(JPEG_TEST_UTILS_OBJ:.o=.d)
-include $(DEPFILES)


####################################################################
# Object Files
####################################################################

# Pattern rule for C source files: compile .c files to .o files, generating dependency files.
$(OBJ_DIR)/%.o: src/%.c
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for C++ source files (if any):
$(OBJ_DIR)/%.o: src/%.cpp
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@


####################################################################
# Shared Library
####################################################################

$(APP_DIR)/$(TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Compiling Image Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(COMPRESS_LIBS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

####################################################################
# Static Library
####################################################################

$(APP_DIR)/$(STATIC_TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Archiving Image Static Library ###\n"
	@mkdir -p $(@D)
	ar rcs $@ $^

####################################################################
# Unit Tests
####################################################################

# Test helper object (compiled once, linked into all tests) (only if present)
$(TEST_HELPER_OBJ): $(TEST_HELPER_SRC)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for compiling test source files to object files
# This allows tests to be compiled separately from linking
$(OBJ_DIR)/tests/%.o: tests/%.cpp
	@printf "\n### Compiling Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@
# Tests in tests/unit/ (object still under tests/ so executable name matches)
$(OBJ_DIR)/tests/%.o: tests/unit/%.cpp
	@printf "\n### Compiling Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Test in tests/codec/png/ (object name from basename for link)
$(OBJ_DIR)/tests/test_png_chunk.o: tests/codec/png/test_png_chunk.cpp
	@printf "\n### Compiling Test Object: test_png_chunk ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/test_jpeg_load.o: tests/codec/jpeg/test_jpeg_load.cpp
	@printf "\n### Compiling Test Object: test_jpeg_load ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests/codec/jpeg -DGIMG_TEST_DATA_JPEG=\"$(TEST_DATA_JPEG)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/test_jpeg_encode.o: tests/codec/jpeg/test_jpeg_encode.cpp
	@printf "\n### Compiling Test Object: test_jpeg_encode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Wno-missing-field-initializers $(INCLUDE) -Itests/codec/jpeg -DGIMG_TEST_DATA_JPEG=\"$(TEST_DATA_JPEG)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Project root for test data (run make from repo root so CURDIR is correct).
# Test binary may run from build/.../apps/; paths are compile-time absolute so data/out are found.
IMAGE_ROOT := $(CURDIR)
# Test data path for PNG tests (reference files from tests/data/png/generate.py).
TEST_DATA_PNG := $(IMAGE_ROOT)/tests/data/png
# Test data path for JPEG tests (optional cmyk_sample.jpg etc.).
TEST_DATA_JPEG := $(IMAGE_ROOT)/tests/data/jpeg
# Output directory for PNG encode test output (add to .gitignore); verifier reads this.
TEST_OUT_PNG := $(IMAGE_ROOT)/tests/out/png
# Output directory for JPEG encode test output; verifier reads this.
TEST_OUT_JPEG := $(IMAGE_ROOT)/tests/out/jpeg
$(OBJ_DIR)/tests/test_png_decode.o: tests/codec/png/test_png_decode.cpp
	@printf "\n### Compiling Test Object: test_png_decode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests/codec/png -DGIMG_TEST_DATA_PNG=\"$(TEST_DATA_PNG)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/test_png_encode.o: tests/codec/png/test_png_encode.cpp
	@printf "\n### Compiling Test Object: test_png_encode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -Wno-missing-field-initializers $(INCLUDE) -Itests/codec/png -DGIMG_TEST_DATA_PNG=\"$(TEST_DATA_PNG)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(PNG_TEST_UTILS_OBJ): tests/codec/png/png_test_utils.cpp
	@printf "\n### Compiling Test Helper: png_test_utils ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests/codec/png -DGIMG_TEST_DATA_PNG=\"$(TEST_DATA_PNG)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(JPEG_TEST_UTILS_OBJ): tests/codec/jpeg/jpeg_test_utils.cpp
	@printf "\n### Compiling Test Helper: jpeg_test_utils ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests/codec/jpeg -DGIMG_TEST_DATA_JPEG=\"$(TEST_DATA_JPEG)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for building test executables. Args: $1 = source path, $2 = executable name (from TEST_PAIRS).
# Tests are compiled to .o files first, then linked separately (relink only when library changes).
define test-executable-rule
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(APP_DIR)/$2$(EXE_EXTENSION): \
		$$(TEST_OBJ_$1) \
		$(TEST_HELPER_OBJ) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Linking %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) -o $$@ $$(TEST_OBJ_$1) $$(TEST_HELPER_OBJ) $$(LDFLAGS) $$(TESTFLAGS) $(IMAGELIBRARY)
endef

# testPng_decode, testPng_encode, test_jpeg_load use explicit rules (link test utils).
TEST_PAIRS_OTHER := $(filter-out tests/codec/png/test_png_decode.cpp|testPng_decode tests/codec/png/test_png_encode.cpp|testPng_encode tests/codec/jpeg/test_jpeg_load.cpp|testJpeg_load tests/codec/jpeg/test_jpeg_encode.cpp|testJpeg_encode,$(TEST_PAIRS))
# Generate build rules for all other tests (one pair = source|name)
$(foreach pair,$(TEST_PAIRS_OTHER),$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

# JPEG load test links jpeg_test_utils (load_jpeg_file, raster_pixel_hash, rasters_equal).
$(APP_DIR)/testJpeg_load$(EXE_EXTENSION): $(OBJ_DIR)/tests/test_jpeg_load.o $(TEST_HELPER_OBJ) $(JPEG_TEST_UTILS_OBJ) | $(APP_DIR)/$(TARGET)
	@printf "\n### Linking testJpeg_load Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/tests/test_jpeg_load.o $(TEST_HELPER_OBJ) $(JPEG_TEST_UTILS_OBJ) $(LDFLAGS) $(TESTFLAGS) $(IMAGELIBRARY)

# JPEG encode test links jpeg_test_utils (load_jpeg_file, raster_pixel_hash for round-trip test).
$(APP_DIR)/testJpeg_encode$(EXE_EXTENSION): $(OBJ_DIR)/tests/test_jpeg_encode.o $(TEST_HELPER_OBJ) $(JPEG_TEST_UTILS_OBJ) | $(APP_DIR)/$(TARGET)
	@printf "\n### Linking testJpeg_encode Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/tests/test_jpeg_encode.o $(TEST_HELPER_OBJ) $(JPEG_TEST_UTILS_OBJ) $(LDFLAGS) $(TESTFLAGS) $(IMAGELIBRARY)

# Dump JPEG raster to stdout (for compare_pillow_ours.py).
$(OBJ_DIR)/tests/dump_jpeg_raster.o: tests/codec/jpeg/dump_jpeg_raster.cpp
	@printf "\n### Compiling dump_jpeg_raster ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@
$(APP_DIR)/dump_jpeg_raster$(EXE_EXTENSION): $(OBJ_DIR)/tests/dump_jpeg_raster.o | $(APP_DIR)/$(TARGET)
	@printf "\n### Linking dump_jpeg_raster ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/tests/dump_jpeg_raster.o $(LDFLAGS) $(IMAGELIBRARY)

# Dump JPEG file structure: segments in order with offset, size, hex dump (no library dependency).
$(OBJ_DIR)/tests/dump_jpeg_structure.o: tests/codec/jpeg/dump_jpeg_structure.cpp
	@printf "\n### Compiling dump_jpeg_structure ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@
$(APP_DIR)/dump_jpeg_structure$(EXE_EXTENSION): $(OBJ_DIR)/tests/dump_jpeg_structure.o
	@printf "\n### Linking dump_jpeg_structure ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/tests/dump_jpeg_structure.o $(LDFLAGS)

jpeg-dump-structure: $(APP_DIR)/dump_jpeg_structure$(EXE_EXTENSION) ## Build dump_jpeg_structure; run: build/.../dump_jpeg_structure <file.jpg>

# libjpeg-based oracle tools live in third_party/jpeg-oracle (optional; not required for make test).
# See third_party/jpeg-oracle/README.md. Tests that use the oracle skip when it is not present.

# IJG v10 (Independent JPEG Group reference, third_party/jpeg-10). Decode precision 8-12 only;
# rejects 16-bit and extended DHT (242 AC symbols). See tests/data/jpeg/README.md.
jpeg-ijg10-build: ## Build IJG v10 (configure + make) in third_party/jpeg-10. Requires source from ijg.org (jpegsrc.v10.tar.gz).
	@IJG=third_party/jpeg-10; \
	if [ ! -d "$$IJG" ]; then \
		echo "Extract IJG v10 first: cd third_party && curl -sL -o jpegsrc.v10.tar.gz https://ijg.org/files/jpegsrc.v10.tar.gz && tar -xzf jpegsrc.v10.tar.gz"; \
		exit 1; \
	fi; \
	if [ ! -f "$$IJG/Makefile" ]; then \
		(cd $$IJG && ./configure --prefix=$$(pwd)/build); \
	fi; \
	$(MAKE) -C $$IJG
	@echo "IJG v10 built. Run third_party/jpeg-10/djpeg for decode (8-12 bit only; not 16-bit oracle)."

# PNG tests link the shared png_test_utils helper.
$(APP_DIR)/testPng_decode$(EXE_EXTENSION): $(OBJ_DIR)/tests/test_png_decode.o $(TEST_HELPER_OBJ) $(PNG_TEST_UTILS_OBJ) | $(APP_DIR)/$(TARGET)
	@printf "\n### Linking testPng_decode Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/tests/test_png_decode.o $(TEST_HELPER_OBJ) $(PNG_TEST_UTILS_OBJ) $(LDFLAGS) $(TESTFLAGS) $(IMAGELIBRARY)

$(APP_DIR)/testPng_encode$(EXE_EXTENSION): $(OBJ_DIR)/tests/test_png_encode.o $(TEST_HELPER_OBJ) $(PNG_TEST_UTILS_OBJ) | $(APP_DIR)/$(TARGET)
	@printf "\n### Linking testPng_encode Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/tests/test_png_encode.o $(TEST_HELPER_OBJ) $(PNG_TEST_UTILS_OBJ) $(LDFLAGS) $(TESTFLAGS) $(IMAGELIBRARY)

####################################################################
# Examples
####################################################################

# Pattern rule for example executables
$(APP_DIR)/examples/%$(EXE_EXTENSION): examples/%.c $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(IMAGELIBRARY)

####################################################################
# Commands
####################################################################

# General commands
.PHONY: clean clean-test-out cloc docs docs-pdf examples jpeg-ijg10-build
# Release build commands
.PHONY: all install test test-quiet test-asan test-ubsan test-valgrind test-valgrind-quiet test-verify-png test-verify-jpeg test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug


watch: ## Watch the file directory for changes and compile the target
	@while true; do \
		make --no-print-directory all; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

test-watch: ## Watch the file directory for changes and run the unit tests
	@while true; do \
		make --no-print-directory all; \
		make --no-print-directory test; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

examples: ## Build all examples
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Examples built       ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "Examples are available in: $(APP_DIR)/examples/\n"
	@printf "\n"
	@printf "\033[0;33mTo run examples:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export LD_LIBRARY_PATH=\"$(APP_DIR):$$LD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/example\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export DYLD_LIBRARY_PATH=\"$(APP_DIR):$$DYLD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/example\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): The DLL must be in the same directory or in PATH.\n"
	@printf "  Option 1 - Run from the library directory:\n"
	@printf "    cd $(APP_DIR)\n"
	@printf "    ./examples/example$(EXE_EXTENSION)\n"
	@printf "  Option 2 - Add library directory to PATH:\n"
	@printf "    export PATH=\"$(APP_DIR):$$PATH\"\n"
	@printf "    $(APP_DIR)/examples/example$(EXE_EXTENSION)\n"
	@printf "  Option 3 - Copy DLL to example directories:\n"
	@printf "    cp $(APP_DIR)/$(TARGET) $(APP_DIR)/examples/\n"
	@printf "    Then run: $(APP_DIR)/examples/example$(EXE_EXTENSION)\n"
endif
	@printf "\n"

# So tests can load image lib and its dependency (e.g. compress for PNG).
TEST_LD_PATH := $(APP_DIR):../compress/build/$(BUILD)/apps

test: ## Make and run the Unit tests, then verify PNG and JPEG output with PIL
test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
	@mkdir -p $(TEST_OUT_PNG) $(TEST_OUT_JPEG)
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		GIMG_IMAGE_ROOT="$(IMAGE_ROOT)" LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1; \
	done
	@printf "\033[0;30;43m\n############################\n### Verifying PNG output (PIL) ###\n############################\033[0m\n\n"; \
	python3 $(CURDIR)/tests/data/png/verify_png_output.py $(TEST_OUT_PNG) && \
	printf "\033[0;32mPNG output verification passed.\033[0m\n"; \
	printf "\033[0;30;43m\n############################\n### Verifying JPEG output (PIL) ###\n############################\033[0m\n\n"; \
	python3 $(CURDIR)/tests/data/jpeg/verify_jpeg_output.py $(TEST_OUT_JPEG) && \
	printf "\033[0;32mJPEG output verification passed.\033[0m\n"

test-quiet: ## Run tests with minimal output (one line per test suite)
test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
	@mkdir -p $(TEST_OUT_PNG) $(TEST_OUT_JPEG)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;36m%-30s %8s %10s %s\033[0m\n" "Test Suite" "Tests" "Time" "Status"; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(GIMG_IMAGE_ROOT="$(IMAGE_ROOT)" LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
		python3 $(CURDIR)/tests/data/png/verify_png_output.py $(TEST_OUT_PNG) && \
		python3 $(CURDIR)/tests/data/jpeg/verify_jpeg_output.py $(TEST_OUT_JPEG) && \
		printf "\033[0;32mPNG and JPEG output verification passed.\033[0m\n"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi

test-verify-png: ## Run only PNG output verification (run 'make test' for full test + verify)
	@mkdir -p $(TEST_OUT_PNG)
	@python3 $(CURDIR)/tests/data/png/verify_png_output.py $(TEST_OUT_PNG) && \
		printf "\033[0;32mPNG output verification passed.\033[0m\n"

test-verify-jpeg: ## Run only JPEG output verification (run 'make test' for full test + verify)
	@mkdir -p $(TEST_OUT_JPEG)
	@python3 $(CURDIR)/tests/data/jpeg/verify_jpeg_output.py $(TEST_OUT_JPEG) && \
		printf "\033[0;32mJPEG output verification passed.\033[0m\n"

test-valgrind: ## Run all tests under valgrind (Linux only)
test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests under Valgrind ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1; \
	done
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

# test-valgrind-quiet: PASS only when BOTH (1) all tests pass AND (2) no leaks.
# So "FAIL" here can mean test assertion failures (e.g. decode returns error 5)
# even when Valgrind reports 0 errors and 0 leaks. Use test-valgrind-noleak to
# pass when Valgrind is clean regardless of test results.
test-valgrind-quiet: ## Run tests under valgrind with minimal output (Linux only)
test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;35m%-30s %8s %10s %s\033[0m\n" "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -c "are definitely lost\|are indirectly lost\|are possibly lost" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=0; \
			if [ $$has_leak -gt 0 ]; then \
				status_msg="LEAK"; \
			else \
				status_msg="FAIL"; \
			fi; \
			total_failed=$$((total_failed + 1)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31m%s\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms" "$$status_msg"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d suites)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

####################################################################
# Sanitizer builds (ASan + UBSan): separate build dir, run test suite
####################################################################
ASAN_UBSAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps

ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))
ASAN_TARGET := $(BASE_NAME_PREFIX)-asan.$(LIB_EXTENSION)
ASAN_IMAGELIBRARY := -L $(ASAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-asan

ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGIMG_BUILD -DGIMG_TEST_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

$(ASAN_OBJ_DIR)/%.o: src/%.c
	@printf "\n### Compiling (ASan+UBSan): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -o $@

$(ASAN_APP_DIR)/$(ASAN_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Linking ASan+UBSan Image Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -shared -o $@ $^ $(ASAN_LDFLAGS) $(COMPRESS_LIBS)

# ASan test helper (optional)
ASAN_TEST_HELPER_OBJ := $(patsubst $(OBJ_DIR)/%,$(ASAN_OBJ_DIR)/%,$(TEST_HELPER_OBJ))
ifneq ($(TEST_HELPER_SRC),)
$(ASAN_TEST_HELPER_OBJ): $(TEST_HELPER_SRC)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -c $< -o $@
endif

# ASan test objects: generic and PNG-specific
$(ASAN_OBJ_DIR)/tests/%.o: tests/%.cpp
	@printf "\n### Compiling ASan Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -c $< -o $@

$(ASAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp
	@printf "\n### Compiling ASan Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -c $< -o $@

$(ASAN_OBJ_DIR)/tests/test_png_chunk.o: tests/codec/png/test_png_chunk.cpp
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -c $< -o $@

$(ASAN_OBJ_DIR)/tests/test_png_decode.o: tests/codec/png/test_png_decode.cpp
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests/codec/png -DGIMG_TEST_DATA_PNG=\"$(TEST_DATA_PNG)\" -c $< -o $@

$(ASAN_OBJ_DIR)/tests/test_png_encode.o: tests/codec/png/test_png_encode.cpp
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -Wno-missing-field-initializers $(INCLUDE) -Itests/codec/png -DGIMG_TEST_DATA_PNG=\"$(TEST_DATA_PNG)\" -c $< -o $@

$(ASAN_OBJ_DIR)/tests/png_test_utils.o: tests/codec/png/png_test_utils.cpp
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests/codec/png -DGIMG_TEST_DATA_PNG=\"$(TEST_DATA_PNG)\" -c $< -o $@

define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): \
		$$(ASAN_TEST_OBJ_$1) \
		$(ASAN_TEST_HELPER_OBJ) \
		| $(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Linking ASan %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $$(ASAN_TEST_HELPER_OBJ) $$(ASAN_LDFLAGS) $$(TESTFLAGS) $$(ASAN_IMAGELIBRARY)
endef

$(foreach pair,$(TEST_PAIRS_OTHER),$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

$(ASAN_APP_DIR)/testPng_decode$(EXE_EXTENSION): $(ASAN_OBJ_DIR)/tests/test_png_decode.o $(ASAN_TEST_HELPER_OBJ) $(ASAN_OBJ_DIR)/tests/png_test_utils.o | $(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Linking ASan testPng_decode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $@ $(ASAN_OBJ_DIR)/tests/test_png_decode.o $(ASAN_TEST_HELPER_OBJ) $(ASAN_OBJ_DIR)/tests/png_test_utils.o $(ASAN_LDFLAGS) $(TESTFLAGS) $(ASAN_IMAGELIBRARY)

$(ASAN_APP_DIR)/testPng_encode$(EXE_EXTENSION): $(ASAN_OBJ_DIR)/tests/test_png_encode.o $(ASAN_TEST_HELPER_OBJ) $(ASAN_OBJ_DIR)/tests/png_test_utils.o | $(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Linking ASan testPng_encode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $@ $(ASAN_OBJ_DIR)/tests/test_png_encode.o $(ASAN_TEST_HELPER_OBJ) $(ASAN_OBJ_DIR)/tests/png_test_utils.o $(ASAN_LDFLAGS) $(TESTFLAGS) $(ASAN_IMAGELIBRARY)

$(ASAN_OBJ_DIR)/tests/test_jpeg_encode.o: tests/codec/jpeg/test_jpeg_encode.cpp
	@printf "\n### Compiling ASan Test: test_jpeg_encode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -Wno-missing-field-initializers $(INCLUDE) -Itests/codec/jpeg -DGIMG_TEST_DATA_JPEG=\"$(TEST_DATA_JPEG)\" -c $< -o $@

$(ASAN_OBJ_DIR)/tests/jpeg_test_utils.o: tests/codec/jpeg/jpeg_test_utils.cpp
	@printf "\n### Compiling ASan Test Helper: jpeg_test_utils ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests/codec/jpeg -DGIMG_TEST_DATA_JPEG=\"$(TEST_DATA_JPEG)\" -c $< -o $@

$(ASAN_APP_DIR)/testJpeg_encode$(EXE_EXTENSION): $(ASAN_OBJ_DIR)/tests/test_jpeg_encode.o $(ASAN_TEST_HELPER_OBJ) $(ASAN_OBJ_DIR)/tests/jpeg_test_utils.o | $(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Linking ASan testJpeg_encode ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $@ $(ASAN_OBJ_DIR)/tests/test_jpeg_encode.o $(ASAN_TEST_HELPER_OBJ) $(ASAN_OBJ_DIR)/tests/jpeg_test_utils.o $(ASAN_LDFLAGS) $(TESTFLAGS) $(ASAN_IMAGELIBRARY)

ASAN_TEST_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

test-asan: ## Run all tests with AddressSanitizer + UndefinedBehaviorSanitizer (Linux only)
test-asan: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m\n"
	@printf "###########################################\n"
	@printf "### Running tests with ASan + UBSan    ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n### Running %s (ASan+UBSan) ###\033[0m\n\n" "$$test_name"; \
		GIMG_IMAGE_ROOT="$(IMAGE_ROOT)" LD_LIBRARY_PATH="$(ASAN_APP_DIR):$(TEST_LD_PATH)" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\nAll tests passed with ASan + UBSan.\033[0m\n"
else
	@printf "\033[0;31mSanitizer builds are currently only supported on Linux.\033[0m\n"
	@exit 1
endif

test-ubsan: ## Alias for test-asan (ASan+UBSan run together)
test-ubsan: test-asan

clean: ## Remove all contents of the build directories.
	-@rm -rvf $(BUILD_DIR)

clean-test-out: ## Remove test output (tests/out/jpeg, tests/out/png). Run 'make test' to regenerate.
	-@rm -rf $(TEST_OUT_JPEG) $(TEST_OUT_PNG)
	@mkdir -p $(TEST_OUT_JPEG) $(TEST_OUT_PNG)
	@echo "Test output dirs cleared. Run 'make test' to regenerate."

# Files will be as follows:
# /usr/local/lib/(SUITE)/
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR).(MINOR)
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR) link to previous
#   lib(SUITE)-(PROJECT)(BRANCH).so link to previous
# /etc/ld.so.conf.d/(SUITE)-(PROJECT)(BRANCH).conf will point to /usr/local/lib/(SUITE)
# /usr/local/include/(SUITE)/(PROJECT)(BRANCH)
#   *.h copied from ./include/(PROJECT)
# /usr/local/share/pkgconfig
#   (SUITE)-(PROJECT)(BRANCH).pc created

install: ## Install the library globally, requires sudo
	# Installing the shared library.
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
# Install the .so file
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	# Installing the ld configuration file.
	@echo "/usr/local/lib/$(SUITE)" > /etc/ld.so.conf.d/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
# The .dll file and the .dll.a file
	@mkdir -p $(BIN_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)
endif
	# Installing the headers.
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d $(GEN_DIR) ] && [ -n "$$(ls -A $(GEN_DIR) 2>/dev/null)" ]; then \
		mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT); \
		cp $(GEN_DIR)/*.h $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT)/; \
	fi
	# Installing the pkg-config files.
	@mkdir -p $(PKG_CONFIG_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g' > $(PKG_CONFIG_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@ldconfig >> /dev/null 2>&1
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the globally-installed files.  Requires sudo.
	# Deleting the shared library.
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	# Deleting the ld configuration file.
	@rm -f /etc/ld.so.conf.d/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	# Deleting the headers.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	# Deleting the pkg-config files.
	@rm -f $(PKG_CONFIG_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	# Cleaning up (potentially) no longer needed directories.
	@rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_PATH)/$(SUITE)
	@rmdir --ignore-fail-on-non-empty $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@ldconfig >> /dev/null 2>&1
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build the project in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library globally, requires sudo
	make install BUILD=debug

uninstall-debug: ## Delete the DEBUG globally-installed files.  Requires sudo.
	make uninstall BUILD=debug

test-debug: ## Make and run the Unit tests in DEBUG mode
	make test BUILD=debug

test-valgrind-debug: ## Run all tests under valgrind in DEBUG mode (Linux only)
	make test-valgrind BUILD=debug

watch-debug: ## Watch the file directory for changes and compile the target in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch the file directory for changes and run the unit tests in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate the documentation in the ./docs subdirectory
	doxygen

docs-pdf: docs ## Generate the documentation as a pdf, at ./docs/(SUITE)-(PROJECT)(BRANCH).pdf
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

####################################################################
# Fuzz target (libFuzzer): PNG/APNG load and decode
####################################################################
FUZZ_CXX ?= clang++
FUZZ_FLAGS := -fsanitize=fuzzer -g -O2
# Check if clang++ is available for fuzz
FUZZ_CXX_OK := $(shell which $(FUZZ_CXX) 2>/dev/null)

fuzz-png: $(APP_DIR)/$(TARGET) ## Build libFuzzer harness for PNG/APNG (requires clang++)
	@if [ -z "$(FUZZ_CXX_OK)" ]; then \
		echo "fuzz-png requires $(FUZZ_CXX); install clang or set FUZZ_CXX"; exit 1; \
	fi
	@mkdir -p $(OBJ_DIR) $(APP_DIR)
	$(FUZZ_CXX) $(CXXFLAGS) $(INCLUDE) $(FUZZ_FLAGS) -c tests/fuzz/fuzz_png_load.cpp -o $(OBJ_DIR)/fuzz_png_load.o
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $(APP_DIR)/fuzz_png_load$(EXE_EXTENSION) $(OBJ_DIR)/fuzz_png_load.o $(LDFLAGS) $(IMAGELIBRARY) $(COMPRESS_LIBS)
	@echo "Fuzz harness: $(APP_DIR)/fuzz_png_load$(EXE_EXTENSION). Run with corpus: LD_LIBRARY_PATH=\"$(TEST_LD_PATH)\" $(APP_DIR)/fuzz_png_load tests/fuzz/corpus"

fuzz-png-encode: $(APP_DIR)/$(TARGET) ## Build libFuzzer harness for PNG round-trip load->save->load (requires clang++)
	@if [ -z "$(FUZZ_CXX_OK)" ]; then \
		echo "fuzz-png-encode requires $(FUZZ_CXX); install clang or set FUZZ_CXX"; exit 1; \
	fi
	@mkdir -p $(OBJ_DIR) $(APP_DIR)
	$(FUZZ_CXX) $(CXXFLAGS) $(INCLUDE) $(FUZZ_FLAGS) -c tests/fuzz/fuzz_png_encode.cpp -o $(OBJ_DIR)/fuzz_png_encode.o
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $(APP_DIR)/fuzz_png_encode$(EXE_EXTENSION) $(OBJ_DIR)/fuzz_png_encode.o $(LDFLAGS) $(IMAGELIBRARY) $(COMPRESS_LIBS)
	@echo "Fuzz harness: $(APP_DIR)/fuzz_png_encode$(EXE_EXTENSION). Run with corpus: LD_LIBRARY_PATH=\"$(TEST_LD_PATH)\" $(APP_DIR)/fuzz_png_encode tests/fuzz/corpus"

fuzz-jpeg: $(APP_DIR)/$(TARGET) ## Build libFuzzer harness for JPEG load/decode (requires clang++)
	@if [ -z "$(FUZZ_CXX_OK)" ]; then \
		echo "fuzz-jpeg requires $(FUZZ_CXX); install clang or set FUZZ_CXX"; exit 1; \
	fi
	@mkdir -p $(OBJ_DIR) $(APP_DIR)
	$(FUZZ_CXX) $(CXXFLAGS) $(INCLUDE) $(FUZZ_FLAGS) -c tests/fuzz/fuzz_jpeg_load.cpp -o $(OBJ_DIR)/fuzz_jpeg_load.o
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $(APP_DIR)/fuzz_jpeg_load$(EXE_EXTENSION) $(OBJ_DIR)/fuzz_jpeg_load.o $(LDFLAGS) $(IMAGELIBRARY) $(COMPRESS_LIBS)
	@echo "Fuzz harness: $(APP_DIR)/fuzz_jpeg_load$(EXE_EXTENSION). Run with corpus: LD_LIBRARY_PATH=\"$(TEST_LD_PATH)\" $(APP_DIR)/fuzz_jpeg_load tests/fuzz/corpus"

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\\([^:]*\\):.*## \\(.*\\)/\\1:\\2/' | awk -F: '{printf "%-15s %s\n", $$1, $$2}' | sed "s/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g"
