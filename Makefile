CXX ?= c++
PWP_CPPFLAGS := -Iinclude
PWP_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -pthread

BIN_DIR := bin
BUILD_DIR := build
TRACKER := $(BIN_DIR)/pwp-tracker
PEER := $(BIN_DIR)/pwp-peer

.PHONY: all clean format test test-e2e

all: $(TRACKER) $(PEER)

$(TRACKER): src/tracker.cpp include/pwp/protocol.hpp | $(BIN_DIR)
	$(CXX) $(PWP_CPPFLAGS) $(CPPFLAGS) $(PWP_CXXFLAGS) $(CXXFLAGS) $< -o $@

$(PEER): src/peer.cpp include/pwp/protocol.hpp include/pwp/files.hpp | $(BIN_DIR)
	$(CXX) $(PWP_CPPFLAGS) $(CPPFLAGS) $(PWP_CXXFLAGS) $(CXXFLAGS) $< -o $@

$(BUILD_DIR)/protocol-tests: tests/protocol_tests.cpp include/pwp/protocol.hpp include/pwp/files.hpp | $(BUILD_DIR)
	$(CXX) $(PWP_CPPFLAGS) $(CPPFLAGS) $(PWP_CXXFLAGS) $(CXXFLAGS) $< -o $@

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

test: $(BUILD_DIR)/protocol-tests
	./$(BUILD_DIR)/protocol-tests

test-e2e: all
	bash tests/e2e.sh

format:
	clang-format -i src/*.cpp include/pwp/*.hpp tests/*.cpp

clean:
	rm -rf $(BIN_DIR) $(BUILD_DIR)
