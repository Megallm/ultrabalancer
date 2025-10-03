cmake_minimum_required(VERSION 3.10)
project(UltraBalancer VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(ENABLE_SSL "Enable SSL/TLS support" ON)
option(ENABLE_PCRE "Enable PCRE for regex" ON)
option(ENABLE_ZLIB "Enable zlib compression" ON)
option(ENABLE_BROTLI "Enable brotli compression" ON)
option(ENABLE_JEMALLOC "Use jemalloc" ON)
option(BUILD_TESTS "Build tests" ON)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O3 -march=native -mtune=native")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fomit-frame-pointer -flto -fno-strict-aliasing")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

if(ENABLE_SSL)
    find_package(OpenSSL REQUIRED)
    add_definitions(-DUSE_SSL)
endif()

if(ENABLE_PCRE)
    pkg_check_modules(PCRE REQUIRED libpcre)
    add_definitions(-DUSE_PCRE)
endif()

if(ENABLE_ZLIB)
    find_package(ZLIB REQUIRED)
    add_definitions(-DUSE_ZLIB)
endif()

if(ENABLE_BROTLI)
    pkg_check_modules(BROTLI libbrotlienc libbrotlidec)
    if(BROTLI_FOUND)
        add_definitions(-DUSE_BROTLI)
    endif()
endif()

if(ENABLE_JEMALLOC)
    find_library(JEMALLOC_LIB jemalloc)
    if(JEMALLOC_LIB)
        add_definitions(-DUSE_JEMALLOC)
    endif()
endif()

include_directories(include)

set(CORE_SOURCES
    src/main.c
    src/core/core.c
    src/core/backend.c
    src/core/frontend.c
    src/core/proxy.c
    src/core/listener.c
    src/core/server.c
)

set(NETWORK_SOURCES
    src/network/network.c
    src/network/lb_network.c
    src/network/socket.c
    src/network/epoll.c
    src/network/connection.c
    src/network/buffer.c
    src/network/stream.c
)

set(HTTP_SOURCES
    src/http/http_parser.c
    src/http/http_request.c
    src/http/http_response.c
    src/http/http_headers.c
    src/http/http2.c
    src/http/websocket.c
    src/http/chunked.c
)

set(SSL_SOURCES
    src/ssl/ssl_ctx.c
    src/ssl/ssl_sock.c
    src/ssl/ssl_certs.c
    src/ssl/ssl_sni.c
    src/ssl/ssl_alpn.c
)

set(HEALTH_SOURCES
    src/health/health.c
    src/health/checks.c
    src/health/tcp_check.c
    src/health/http_check.c
    src/health/ssl_check.c
)

set(ACL_SOURCES
    src/acl/acl.c
    src/acl/pattern.c
    src/acl/regex.c
    src/acl/rules.c
    src/acl/actions.c
)

set(CACHE_SOURCES
    src/cache/cache.c
    src/cache/lru.c
    src/cache/compression.c
    src/cache/store.c
)

set(STATS_SOURCES
    src/stats/stats.c
    src/stats/metrics.c
    src/stats/prometheus.c
    src/stats/json_stats.c
    src/stats/html_stats.c
)

set(UTILS_SOURCES
    src/utils/memory.c
    src/utils/hash.c
    src/utils/time.c
    src/utils/log.c
    src/utils/queue.c
    src/utils/rbtree.c
    src/utils/pool.c
)

set(CONFIG_SOURCES
    src/config/config.c
    src/config/parser.c
    src/config/lexer.c
    src/config/validator.c
)

set(ALL_SOURCES
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${HTTP_SOURCES}
    ${HEALTH_SOURCES}
    ${ACL_SOURCES}
    ${CACHE_SOURCES}
    ${STATS_SOURCES}
    ${UTILS_SOURCES}
    ${CONFIG_SOURCES}
)

if(ENABLE_SSL)
    set(ALL_SOURCES ${ALL_SOURCES} ${SSL_SOURCES})
endif()

add_executable(ultrabalancer ${ALL_SOURCES})

target_link_libraries(ultrabalancer
    Threads::Threads
    m
    rt
    dl
)

if(ENABLE_SSL)
    target_link_libraries(ultrabalancer OpenSSL::SSL OpenSSL::Crypto)
endif()

if(ENABLE_PCRE)
    target_link_libraries(ultrabalancer ${PCRE_LIBRARIES})
    target_include_directories(ultrabalancer PUBLIC ${PCRE_INCLUDE_DIRS})
endif()

if(ENABLE_ZLIB)
    target_link_libraries(ultrabalancer ZLIB::ZLIB)
endif()

if(BROTLI_FOUND)
    target_link_libraries(ultrabalancer ${BROTLI_LIBRARIES})
    target_include_directories(ultrabalancer PUBLIC ${BROTLI_INCLUDE_DIRS})
endif()

if(JEMALLOC_LIB)
    target_link_libraries(ultrabalancer ${JEMALLOC_LIB})
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS ultrabalancer DESTINATION bin)
install(DIRECTORY config/ DESTINATION /etc/ultrabalancer)
install(FILES docs/man/ultrabalancer.1 DESTINATION share/man/man1)