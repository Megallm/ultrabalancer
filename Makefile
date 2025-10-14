# UltraBalancer Makefile
# High-Performance Load Balancer

CC = gcc
CXX = g++
VERSION = 1.0.0
TARGET = ultrabalancer

# Compiler flags
CFLAGS = -std=c2x -D_GNU_SOURCE -DVERSION=\"$(VERSION)\"
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -O3 -march=native -mtune=native -flto -fomit-frame-pointer
CFLAGS += -pthread -fno-strict-aliasing -fwrapv
CFLAGS += -DUSE_EPOLL -DUSE_SPLICE -DUSE_ACCEPT4
CFLAGS += -I./include

CXXFLAGS = -std=c++23 -D_GNU_SOURCE -DVERSION=\"$(VERSION)\"
CXXFLAGS += -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -O3 -march=native -mtune=native -flto -fomit-frame-pointer
CXXFLAGS += -pthread
CXXFLAGS += -I./include

# Debug flags
DEBUG_FLAGS = -g3 -O0 -DDEBUG -fsanitize=address -fsanitize=undefined

# Libraries
LIBS = -lpthread -lm -lrt -ldl -lstdc++
LIBS += -lssl -lcrypto
LIBS += -lpcre -lz
LIBS += -lbrotlienc -lbrotlidec
LIBS += -lyaml

# Optional features
ifdef USE_SYSTEMD
    CFLAGS += -DUSE_SYSTEMD
    LIBS += -lsystemd
endif

ifdef USE_PCRE2
    CFLAGS += -DUSE_PCRE2
    LIBS += -lpcre2-8
endif

# Source directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN_DIR = bin

# Source files
CORE_SRCS = $(wildcard $(SRC_DIR)/core/*.c)
CORE_CXX_SRCS = $(wildcard $(SRC_DIR)/core/*.cpp)
NETWORK_SRCS = $(filter-out $(SRC_DIR)/network/lb_network.c, $(wildcard $(SRC_DIR)/network/*.c))
HTTP_SRCS = $(wildcard $(SRC_DIR)/http/*.c)
SSL_SRCS = $(wildcard $(SRC_DIR)/ssl/*.c)
HEALTH_SRCS = $(wildcard $(SRC_DIR)/health/*.c)
ACL_SRCS = $(wildcard $(SRC_DIR)/acl/*.c)
CACHE_SRCS = $(wildcard $(SRC_DIR)/cache/*.c)
STATS_SRCS = $(wildcard $(SRC_DIR)/stats/*.c)
STATS_CXX_SRCS = $(wildcard $(SRC_DIR)/stats/*.cpp)
UTILS_SRCS = $(wildcard $(SRC_DIR)/utils/*.c)
CONFIG_SRCS = $(wildcard $(SRC_DIR)/config/*.c)
DATABASE_SRCS = $(filter-out $(SRC_DIR)/database/db_pool.c, $(wildcard $(SRC_DIR)/database/*.c))
DATABASE_CXX_SRCS = $(wildcard $(SRC_DIR)/database/*.cpp)

ALL_SRCS = $(SRC_DIR)/main.c $(CORE_SRCS) $(NETWORK_SRCS) $(HTTP_SRCS) \
           $(SSL_SRCS) $(HEALTH_SRCS) $(ACL_SRCS) $(CACHE_SRCS) \
           $(STATS_SRCS) $(UTILS_SRCS) $(CONFIG_SRCS) $(DATABASE_SRCS)

ALL_CXX_SRCS = $(CORE_CXX_SRCS) $(STATS_CXX_SRCS) $(DATABASE_CXX_SRCS)

# Object files
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(ALL_SRCS))
CXX_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(ALL_CXX_SRCS))

# Create directories
$(shell mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/network $(OBJ_DIR)/http \
                 $(OBJ_DIR)/ssl $(OBJ_DIR)/health $(OBJ_DIR)/acl \
                 $(OBJ_DIR)/cache $(OBJ_DIR)/stats $(OBJ_DIR)/utils \
                 $(OBJ_DIR)/config $(OBJ_DIR)/database $(BIN_DIR))

# Default target
all: $(BIN_DIR)/$(TARGET)

# Build target (alias for all)
build: all

# Build target
$(BIN_DIR)/$(TARGET): $(OBJS) $(CXX_OBJS)
	@echo "Linking $@..."
	@$(CXX) $(CXXFLAGS) $(OBJS) $(CXX_OBJS) -o $@ $(LIBS)
	@echo "Build complete: $@"

# Compile C source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<..."
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile C++ source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Debug build
debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean $(BIN_DIR)/$(TARGET)

# Static analysis
analyze:
	@echo "Running static analysis..."
	@cppcheck --enable=all --suppress=missingIncludeSystem \
	          --error-exitcode=1 --inline-suppr $(SRC_DIR)
	@scan-build --status-bugs make

# Performance profiling build
profile: CFLAGS += -pg -fprofile-arcs -ftest-coverage
profile: LIBS += -lgcov
profile: $(BIN_DIR)/$(TARGET)

# Install
install: $(BIN_DIR)/$(TARGET)
	@echo "Installing $(TARGET)..."
	@install -D -m 755 $(BIN_DIR)/$(TARGET) /usr/local/bin/$(TARGET)
	@install -D -m 644 config/ultrabalancer.cfg /etc/ultrabalancer/ultrabalancer.cfg
	@install -D -m 644 scripts/ultrabalancer.service /etc/systemd/system/ultrabalancer.service
	@echo "Installation complete"

# Uninstall
uninstall:
	@echo "Uninstalling $(TARGET)..."
	@rm -f /usr/local/bin/$(TARGET)
	@rm -rf /etc/ultrabalancer
	@rm -f /etc/systemd/system/ultrabalancer.service
	@echo "Uninstallation complete"

# Clean
clean:
	@echo "Cleaning..."
	@rm -rf $(OBJ_DIR) $(BIN_DIR)

# Test
test: $(BIN_DIR)/$(TARGET)
	@echo "Running tests..."
	@$(MAKE) -C tests

# Benchmark
benchmark: $(BIN_DIR)/$(TARGET)
	@echo "Running benchmarks..."
	@./scripts/benchmark.sh

# Format code
format:
	@echo "Formatting code..."
	@find $(SRC_DIR) $(INC_DIR) -name "*.c" -o -name "*.h" | \
	 xargs clang-format -i -style=file

# Generate documentation
docs:
	@echo "Generating documentation..."
	@doxygen docs/Doxyfile

# Check dependencies
check-deps:
	@echo "Checking dependencies..."
	@./scripts/check-deps.sh

# Package
package:
	@echo "Creating package..."
	@tar czf ultrabalancer-$(VERSION).tar.gz \
	     --exclude=obj --exclude=bin --exclude=.git \
	     --exclude=*.tar.gz .

.PHONY: all build clean debug analyze profile install uninstall test benchmark \
        format docs check-deps package

# Help
help:
	@echo "UltraBalancer Makefile targets:"
	@echo "  all       - Build the load balancer (default)"
	@echo "  debug     - Build with debug symbols and sanitizers"
	@echo "  analyze   - Run static code analysis"
	@echo "  profile   - Build with profiling support"
	@echo "  install   - Install the load balancer"
	@echo "  uninstall - Uninstall the load balancer"
	@echo "  clean     - Remove build artifacts"
	@echo "  test      - Run tests"
	@echo "  benchmark - Run performance benchmarks"
	@echo "  format    - Format source code"
	@echo "  docs      - Generate documentation"
	@echo "  package   - Create distribution package"
	@echo ""
	@echo "Options:"
	@echo "  USE_SYSTEMD=1 - Enable systemd integration"
	@echo "  USE_PCRE2=1   - Use PCRE2 instead of PCRE"