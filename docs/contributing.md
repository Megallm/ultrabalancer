# 🤝 Contributing to UltraBalancer

## Table of Contents
1. [Welcome](#welcome)
2. [Development Environment](#development-environment)
3. [Code Style](#code-style)
4. [Development Workflow](#development-workflow)
5. [Testing](#testing)
6. [Documentation](#documentation)
7. [Roadmap](#roadmap)
8. [Community](#community)

## Welcome

Thank you for considering contributing to UltraBalancer! This high-performance load balancer is built with modern C23 and C++23, and we welcome contributions of all kinds.

### Ways to Contribute

- 🐛 Report bugs
- 💡 Suggest features
- 📝 Improve documentation
- 🔧 Submit pull requests
- 🎨 Improve UI/UX
- 🌐 Add translations
- ⚡ Optimize performance
- 🧪 Write tests

## Development Environment

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install -y \
    build-essential \
    gcc-13 \
    g++-13 \
    cmake \
    ninja-build \
    pkg-config \
    libssl-dev \
    libpcre3-dev \
    zlib1g-dev \
    libbrotli-dev \
    libjemalloc-dev \
    liburing-dev \
    libnuma-dev \
    clang-format \
    clang-tidy \
    valgrind \
    gdb

# Fedora/RHEL
sudo dnf install -y \
    gcc \
    gcc-c++ \
    cmake \
    ninja-build \
    openssl-devel \
    pcre-devel \
    zlib-devel \
    brotli-devel \
    jemalloc-devel \
    liburing-devel \
    numactl-devel \
    clang-tools-extra \
    valgrind \
    gdb
```

### Setting Up Development Environment

```bash
# Clone repository
git clone https://github.com/Bas3line/ultrabalancer.git
cd ultrabalancer

# Create development branch
git checkout -b feature/your-feature

# Set up git hooks
cp scripts/pre-commit .git/hooks/
chmod +x .git/hooks/pre-commit

# Build with debug symbols
make debug

# Run tests
make test

# Run with debug configuration
./bin/ultrabalancer -d -f configs/debug.yaml
```

### IDE Setup

#### VS Code

`.vscode/settings.json`:
```json
{
    "C_Cpp.default.cStandard": "c23",
    "C_Cpp.default.cppStandard": "c++23",
    "C_Cpp.default.compilerPath": "/usr/bin/gcc-13",
    "C_Cpp.clang_format_path": "/usr/bin/clang-format",
    "editor.formatOnSave": true,
    "files.associations": {
        "*.h": "c",
        "*.hpp": "cpp"
    }
}
```

#### CLion

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.25)
project(UltraBalancer)

set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_COMPILER gcc-13)
set(CMAKE_CXX_COMPILER g++-13)

add_compile_options(-Wall -Wextra -O3 -march=native)
```

## Code Style

### C23 Style Guide

```c
// File header
/*
 * filename.c - Brief description
 *
 * Copyright (C) 2024 UltraBalancer Project
 * Licensed under MIT License
 */

#include <system_headers.h>
#include "project_headers.h"

// Constants
#define MAX_BUFFER_SIZE 8192
static const int DEFAULT_TIMEOUT = 30;

// Type definitions
typedef struct {
    int fd;
    _Atomic(uint32_t) refcount;
    alignas(64) char buffer[MAX_BUFFER_SIZE];
} connection_t;

// Function declarations with attributes
[[nodiscard]] static int process_request(connection_t *conn) [[gnu::hot]];

// Implementation
int process_request(connection_t *conn) {
    if (unlikely(!conn)) {
        return -EINVAL;
    }

    // C23 features
    typeof(conn->fd) fd = conn->fd;
    _BitInt(128) large_counter = 0;

    // Pattern matching with _Generic
    #define handle_type(x) _Generic((x), \
        int: handle_int, \
        char*: handle_string, \
        default: handle_default)(x)

    return 0;
}
```

### C++23 Style Guide

```cpp
// File header
/*
 * filename.cpp - Brief description
 *
 * Copyright (C) 2024 UltraBalancer Project
 * Licensed under MIT License
 */

#pragma once

#include <system_headers>
#include <ranges>
#include <expected>
#include <format>

#include "project_headers.hpp"

namespace ultrabalancer {

// Concepts
template<typename T>
concept Hashable = requires(T t) {
    { std::hash<T>{}(t) } -> std::convertible_to<size_t>;
};

// Modern class with C++23 features
class [[nodiscard]] Connection final {
public:
    // Deducing this
    template<typename Self>
    auto get_fd(this Self&& self) {
        return std::forward<Self>(self).fd_;
    }

    // Explicit object parameter
    void process(this Connection& self) {
        self.process_impl();
    }

    // std::expected for error handling
    [[nodiscard]] std::expected<Response, Error>
    send_request(const Request& req) noexcept;

private:
    int fd_{-1};
    std::atomic<uint32_t> refcount_{0};

    void process_impl();
};

// Coroutines
Task<void> async_process() {
    auto conn = co_await acquire_connection();
    auto result = co_await conn.send_async();
    co_return process_result(result);
}

// Ranges and views
auto filter_healthy_servers(auto&& servers) {
    return servers
        | std::views::filter(&Server::is_healthy)
        | std::views::take(10);
}

} // namespace ultrabalancer
```

### Formatting Rules

```bash
# Format C code
clang-format -i src/**/*.c include/**/*.h

# Format C++ code
clang-format -i src/**/*.cpp include/**/*.hpp

# Check style
clang-tidy src/**/*.c src/**/*.cpp -- -I./include
```

`.clang-format`:
```yaml
BasedOnStyle: LLVM
IndentWidth: 4
ColumnLimit: 100
AlignAfterOpenBracket: Align
AllowShortFunctionsOnASingleLine: None
AlwaysBreakTemplateDeclarations: Yes
BreakBeforeBraces: Attach
IndentCaseLabels: true
PointerAlignment: Left
```

## Development Workflow

### Branch Strategy

```
main
├── develop
│   ├── feature/connection-pool
│   ├── feature/metrics-api
│   └── feature/http3-support
├── release/1.1.0
└── hotfix/critical-memory-leak
```

### Commit Messages

Follow conventional commits:

```
type(scope): subject

body

footer
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `perf`: Performance improvement
- `refactor`: Code refactoring
- `test`: Tests
- `docs`: Documentation
- `style`: Code style
- `chore`: Maintenance

Examples:
```bash
git commit -m "feat(router): add weighted round-robin algorithm"
git commit -m "fix(memory): resolve connection pool leak"
git commit -m "perf(network): optimize zero-copy with io_uring"
git commit -m "docs(api): update REST API documentation"
```

### Pull Request Process

1. **Fork and Clone**
```bash
gh repo fork ultrabalancer/ultrabalancer --clone
```

2. **Create Feature Branch**
```bash
git checkout -b feature/amazing-feature
```

3. **Make Changes**
```bash
# Code, test, document
make test
make benchmark
```

4. **Submit PR**
```bash
gh pr create --title "Add amazing feature" \
             --body "Description of changes"
```

### Code Review Checklist

- [ ] Code follows style guide
- [ ] Tests pass
- [ ] Documentation updated
- [ ] No memory leaks (Valgrind clean)
- [ ] Performance impact assessed
- [ ] Security implications considered
- [ ] Backwards compatibility maintained

## Testing

### Unit Tests (C23)

```c
// test/test_connection.c
#include "test_framework.h"
#include "core/connection.h"

TEST_CASE("connection_create") {
    connection_t* conn = connection_create();

    ASSERT_NOT_NULL(conn);
    ASSERT_EQ(conn->state, CONN_STATE_INIT);
    ASSERT_EQ(atomic_load(&conn->refcount), 1);

    connection_destroy(conn);
}

TEST_CASE("connection_pool") {
    pool_t* pool = pool_create(100);

    SECTION("acquire_and_release") {
        connection_t* conn = pool_acquire(pool);
        ASSERT_NOT_NULL(conn);

        pool_release(pool, conn);
        ASSERT_EQ(pool_size(pool), 1);
    }

    pool_destroy(pool);
}
```

### Unit Tests (C++23)

```cpp
// test/test_router.cpp
#include <catch2/catch_test_macros.hpp>
#include "core/router.hpp"

using namespace ultrabalancer;

TEST_CASE("RequestRouter", "[router]") {
    RequestRouter router;

    SECTION("exact match routing") {
        auto route = std::make_shared<Route>("test");
        route->add_rule(RouteRule::exact("/api/users"));
        route->add_target(std::make_shared<RouteTarget>("backend1"));

        router.add_route(route);

        auto target = router.route("GET", "/api/users", {});
        REQUIRE(target);
        REQUIRE(target->get_backend() == "backend1");
    }

    SECTION("regex routing") {
        auto route = std::make_shared<Route>("regex");
        route->add_rule(RouteRule::regex(R"(^/api/v\d+/.*$)"));

        router.add_route(route);

        REQUIRE(router.route("GET", "/api/v1/users", {}));
        REQUIRE(router.route("GET", "/api/v2/posts", {}));
        REQUIRE_FALSE(router.route("GET", "/api/users", {}));
    }
}
```

### Integration Tests

```bash
# test/integration/test_load_balancing.sh
#!/bin/bash

# Start test backends
docker-compose up -d backend1 backend2 backend3

# Start UltraBalancer
./bin/ultrabalancer -f test/configs/integration.yaml &
UB_PID=$!

# Wait for startup
sleep 5

# Test round-robin distribution
for i in {1..100}; do
    curl -s http://localhost/test | grep -o "backend[0-9]"
done | sort | uniq -c

# Cleanup
kill $UB_PID
docker-compose down
```

### Performance Tests

```cpp
// test/bench/bench_router.cpp
#include <benchmark/benchmark.h>
#include "core/router.hpp"

static void BM_RouterExactMatch(benchmark::State& state) {
    RequestRouter router;
    // Setup routes...

    for (auto _ : state) {
        auto target = router.route("GET", "/api/users", {});
        benchmark::DoNotOptimize(target);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RouterExactMatch);

static void BM_RouterRegexMatch(benchmark::State& state) {
    // Similar setup...
}
BENCHMARK(BM_RouterRegexMatch);

BENCHMARK_MAIN();
```

### Fuzz Testing

```cpp
// test/fuzz/fuzz_parser.cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 10 || size > 10000) return 0;

    // Create HTTP request from fuzz data
    std::string request(reinterpret_cast<const char*>(data), size);

    try {
        HTTPParser parser;
        parser.parse(request);
    } catch (...) {
        // Ignore exceptions, we're looking for crashes
    }

    return 0;
}
```

## Documentation

### Code Documentation

```c
/**
 * @brief Acquire connection from pool
 *
 * @param pool Connection pool instance
 * @param timeout_ms Timeout in milliseconds (0 = infinite)
 *
 * @return Connection pointer on success, NULL on failure
 *
 * @note Thread-safe
 * @since 1.0.0
 */
connection_t* pool_acquire(pool_t* pool, int timeout_ms);
```

### API Documentation

```yaml
# docs/api/endpoints.yaml
/api/v1/backends/{name}/servers:
  post:
    summary: Add server to backend
    parameters:
      - name: name
        in: path
        required: true
        schema:
          type: string
    requestBody:
      required: true
      content:
        application/json:
          schema:
            $ref: '#/components/schemas/Server'
    responses:
      201:
        description: Server added successfully
```

## Roadmap

### Version 1.1 (Q1 2024)
- [x] C23/C++23 migration
- [ ] io_uring support
- [ ] HTTP/3 (QUIC)
- [ ] WebAssembly plugins
- [ ] Kubernetes operator

### Version 1.2 (Q2 2024)
- [ ] eBPF integration
- [ ] Service mesh mode
- [ ] Distributed configuration
- [ ] Machine learning routing
- [ ] GraphQL support

### Version 2.0 (Q3 2024)
- [ ] Multi-node clustering
- [ ] Global load balancing
- [ ] Edge computing support
- [ ] Serverless integration
- [ ] Advanced observability

### Feature Requests

Current high-priority features:

1. **gRPC Load Balancing**
   - Client-side load balancing
   - Health checking
   - Circuit breaking

2. **Advanced Caching**
   - Redis integration
   - Content-aware caching
   - Edge caching

3. **Security Features**
   - WAF integration
   - DDoS protection
   - Rate limiting per user

## Community

### Communication Channels

- **GitHub Discussions**: Technical discussions
- **Discord**: Real-time chat
- **Twitter**: @UltraBalancer
- **Blog**: blog.ultrabalancer.io

### Code of Conduct

We are committed to providing a friendly, safe, and welcoming environment.

- Be respectful and inclusive
- Welcome newcomers
- Focus on constructive criticism
- No harassment or discrimination

### Contributors

Special thanks to all contributors!

```markdown
<!-- ALL-CONTRIBUTORS-LIST:START -->
| [<img src="https://github.com/user1.png" width="100px;"/><br /><sub><b>User 1</b></sub>](https://github.com/user1)<br />[💻](# "Code") [📖](# "Documentation") | [<img src="https://github.com/user2.png" width="100px;"/><br /><sub><b>User 2</b></sub>](https://github.com/user2)<br />[🐛](# "Bug reports") [🔧](# "Tests") |
| :---: | :---: |
<!-- ALL-CONTRIBUTORS-LIST:END -->
```

### Getting Help

- Read the [documentation](https://docs.ultrabalancer.io)
- Search [existing issues](https://github.com/Bas3line/ultrabalancer/issues)
- Ask in [Discord](https://discord.gg/ultrabalancer)
- Stack Overflow tag: `ultrabalancer`

### License

UltraBalancer is MIT licensed. Contributions are also licensed under MIT unless explicitly stated otherwise.