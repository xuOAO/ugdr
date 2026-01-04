# Root Makefile

# Configuration
PROJECT_ROOT ?= $(shell pwd)
BUILD_DIR ?= $(PROJECT_ROOT)/build
OBJ_DIR ?= $(BUILD_DIR)/obj
BIN_DIR ?= $(BUILD_DIR)/bin

# Export directories
export PROJECT_ROOT BUILD_DIR OBJ_DIR BIN_DIR

# Compilers
CXX := g++
NVCC := nvcc

# Global Flags
# -fPIC is important for shared libraries, adding it globally here for simplicity
# though strictly speaking only libugdr needs it, but it doesn't hurt daemon much usually.
# However, for strictness, I'll add -fPIC in sub-makefiles where needed or globally if lazy.
# Given requirements, libugdr needs -fPIC. Common objects linked to libugdr also need -fPIC.
# Common objects linked to daemon (executable) don't strictly need it but it allows sharing objects if we want to be fancy.
# To avoid recompiling common objects twice (once for lib, once for daemon), we can compile them with -fPIC always.
CXXFLAGS := -std=c++20 -Wall -Wextra
NVCCFLAGS := -std=c++20 -ccbin $(CXX)

# Include Paths
INC_FLAGS := -I$(PROJECT_ROOT)/include -I$(PROJECT_ROOT)/src

# Debug/Release
MODE ?= release
ifeq ($(MODE), debug)
    CXXFLAGS += -g -O0
    NVCCFLAGS += -g -O0
else
    CXXFLAGS += -O3
    NVCCFLAGS += -O3
endif

# Export Compiler Flags
export CXX NVCC CXXFLAGS NVCCFLAGS INC_FLAGS MODE

.PHONY: all clean libugdr daemon tests debug release dirs help

all: libugdr daemon tests

dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)

libugdr: dirs
	@echo "=== Building libugdr ==="
	@$(MAKE) -C src/libugdr

daemon: dirs
	@echo "=== Building ugdr-daemon ==="
	@$(MAKE) -C src/daemon

tests: libugdr daemon dirs
	@echo "=== Building tests ==="
	@$(MAKE) -C tests

clean:
	@echo "=== Cleaning ==="
	@rm -rf $(BUILD_DIR)

debug:
	@$(MAKE) MODE=debug all

release:
	@$(MAKE) MODE=release all

help:
	@echo "UGDR Build System"
	@echo "Targets:"
	@echo "  all       : Build everything (default release)"
	@echo "  debug     : Build everything in debug mode"
	@echo "  release   : Build everything in release mode"
	@echo "  libugdr   : Build shared library"
	@echo "  daemon    : Build daemon executable"
	@echo "  tests     : Build test executables"
	@echo "  clean     : Remove build artifacts"
