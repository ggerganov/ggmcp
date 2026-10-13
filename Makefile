# ggmcp - convenience wrapper around CMake.
#
# The real build system is CMakeLists.txt; this only saves typing.
#
#   make            configure + build (RelWithDebInfo) into ./build
#   make test       build + run unit and HTTP integration tests
#   make run        build + serve MCP over HTTP on 0.0.0.0:8081 (the default)
#   make stdio      build + serve MCP over stdio
#   make clean      remove build artifacts
#   make distclean  remove build artifacts and this Makefile's caches

CMAKE ?= cmake
BUILD_DIR ?= build
CONFIG ?= RelWithDebInfo
# These only override the binary's own defaults (0.0.0.0:8081); `make run`
# without them is the same as running ./build/ggmcp.
PORT ?= 8081
HOST ?= 0.0.0.0

.PHONY: all configure build test run stdio health clean distclean format-check help

all: build

configure:
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

build: configure
	@$(CMAKE) --build $(BUILD_DIR) -j

test: build
	@$(CMAKE) --build $(BUILD_DIR)
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

run: build
	@$(BUILD_DIR)/ggmcp --host $(HOST) --port $(PORT) --verbose

stdio: build
	@$(BUILD_DIR)/ggmcp --stdio --verbose

# Smoke-tests a running instance; expects `make run` to be running elsewhere.
# Always curls loopback: 0.0.0.0 is a bind address, not a destination.
health:
	@curl -sS http://127.0.0.1:$(PORT)/health | python3 -m json.tool

clean:
	@rm -rf $(BUILD_DIR)

distclean: clean
	@find . -name __pycache__ -type d -prune -exec rm -rf {} +

help:
	@printf '%s\n' "targets: all configure build test run stdio health clean distclean help" \
	              "vars:    HOST=$(HOST) PORT=$(PORT) CONFIG=$(CONFIG) CMAKE=$(CMAKE)"
