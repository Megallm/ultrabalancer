#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "config/config.h"
#include "core/proxy.h"
#include "utils/log.h"
#include <syslog.h>

extern proxy_t *proxies_list;
extern struct global global;

static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("   ✗ FAILED: %s\n", msg); \
        test_failed++; \
        return -1; \
    } \
    test_passed++; \
} while(0)

static void reset_global() {
    memset(&global, 0, sizeof(global));
    proxies_list = NULL;
}

static int test_invalid_file() {
    printf("\n1. Testing invalid file handling...\n");
    int result = config_parse("nonexistent.cfg");
    TEST_ASSERT(result == -1, "Should reject nonexistent file");
    printf("   ✓ Invalid file correctly rejected\n");
    return 0;
}

static int test_yaml_basic_parsing() {
    printf("\n2. Testing basic YAML parsing...\n");
    reset_global();

    int result = config_parse("config/ultrabalancer.yaml");
    TEST_ASSERT(result == 0, "YAML file should parse successfully");
    printf("   ✓ YAML file parsed without errors\n");
    return 0;
}

static int test_yaml_global_section() {
    printf("\n3. Testing YAML global section...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    TEST_ASSERT(global.maxconn == 100000, "Global maxconn should be 100000");
    printf("   ✓ maxconn = %d\n", global.maxconn);

    TEST_ASSERT(global.nbthread == 8, "Global nbthread should be 8");
    printf("   ✓ nbthread = %d\n", global.nbthread);

    TEST_ASSERT(global.nbproc == 1, "Global nbproc should be 1");
    printf("   ✓ nbproc = %d\n", global.nbproc);

    TEST_ASSERT(global.daemon == 1, "Global daemon should be true");
    printf("   ✓ daemon = %d\n", global.daemon);

    if (global.pidfile) {
        TEST_ASSERT(strcmp(global.pidfile, "/var/run/ultrabalancer.pid") == 0,
                    "pidfile should be /var/run/ultrabalancer.pid");
        printf("   ✓ pidfile = %s\n", global.pidfile);
    }

    if (global.stats_socket) {
        TEST_ASSERT(strcmp(global.stats_socket, "/var/run/ultrabalancer.sock") == 0,
                    "stats_socket should be /var/run/ultrabalancer.sock");
        printf("   ✓ stats_socket = %s\n", global.stats_socket);
    }

    return 0;
}

static int test_yaml_defaults_section() {
    printf("\n4. Testing YAML defaults section...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    proxy_t *defaults = NULL;
    for (proxy_t *px = proxies_list; px; px = px->next) {
        if (strcmp(px->id, "defaults") == 0) {
            defaults = px;
            break;
        }
    }

    TEST_ASSERT(defaults != NULL, "Defaults proxy should exist");
    printf("   ✓ Defaults section found\n");

    TEST_ASSERT(defaults->mode == PR_MODE_HTTP, "Default mode should be HTTP");
    printf("   ✓ mode = http\n");

    TEST_ASSERT(defaults->timeout.connect == 5000, "Connect timeout should be 5000ms");
    printf("   ✓ timeout.connect = %ums\n", defaults->timeout.connect);

    TEST_ASSERT(defaults->timeout.client == 30000, "Client timeout should be 30000ms");
    printf("   ✓ timeout.client = %ums\n", defaults->timeout.client);

    TEST_ASSERT(defaults->timeout.server == 30000, "Server timeout should be 30000ms");
    printf("   ✓ timeout.server = %ums\n", defaults->timeout.server);

    TEST_ASSERT(defaults->timeout.check == 2000, "Check timeout should be 2000ms");
    printf("   ✓ timeout.check = %ums\n", defaults->timeout.check);

    TEST_ASSERT(defaults->retries == 3, "Retries should be 3");
    printf("   ✓ retries = %d\n", defaults->retries);

    TEST_ASSERT(defaults->maxconn == 50000, "Maxconn should be 50000");
    printf("   ✓ maxconn = %d\n", defaults->maxconn);

    return 0;
}

static int test_yaml_frontend_section() {
    printf("\n5. Testing YAML frontend section...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    proxy_t *frontend = NULL;
    for (proxy_t *px = proxies_list; px; px = px->next) {
        if (strcmp(px->id, "web_frontend") == 0) {
            frontend = px;
            break;
        }
    }

    TEST_ASSERT(frontend != NULL, "Frontend 'web_frontend' should exist");
    printf("   ✓ Frontend 'web_frontend' found\n");

    TEST_ASSERT(frontend->type == PR_TYPE_FRONTEND, "Should be frontend type");
    printf("   ✓ type = frontend\n");

    int listener_count = 0;
    for (listener_t *l = frontend->listeners; l; l = l->next) {
        listener_count++;
    }
    TEST_ASSERT(listener_count == 2, "Should have 2 listeners (port 80 and 443)");
    printf("   ✓ listeners = %d\n", listener_count);

    return 0;
}

static int test_yaml_backend_section() {
    printf("\n6. Testing YAML backend section...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    proxy_t *backend = NULL;
    for (proxy_t *px = proxies_list; px; px = px->next) {
        if (strcmp(px->id, "web_servers") == 0) {
            backend = px;
            break;
        }
    }

    TEST_ASSERT(backend != NULL, "Backend 'web_servers' should exist");
    printf("   ✓ Backend 'web_servers' found\n");

    TEST_ASSERT(backend->type == PR_TYPE_BACKEND, "Should be backend type");
    printf("   ✓ type = backend\n");

    TEST_ASSERT(backend->lb_algo == LB_ALGO_ROUNDROBIN, "Should use roundrobin algorithm");
    printf("   ✓ balance = roundrobin\n");

    int server_count = 0;
    for (server_t *srv = backend->servers; srv; srv = srv->next) {
        server_count++;
        TEST_ASSERT(srv->weight == 100 || srv->weight == 50, "Server weight should be 100 or 50");
        TEST_ASSERT(srv->check != NULL, "Server should have health check enabled");
        printf("   ✓ server %s: weight=%d, check=%s\n",
               srv->id, srv->weight, srv->check ? "enabled" : "disabled");
    }

    TEST_ASSERT(server_count == 3, "Should have 3 servers");
    printf("   ✓ Total servers = %d\n", server_count);

    return 0;
}

static int test_yaml_multiple_backends() {
    printf("\n7. Testing multiple YAML backends...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    int backend_count = 0;
    const char *expected_backends[] = {"web_servers", "api_servers", "static_servers"};

    for (proxy_t *px = proxies_list; px; px = px->next) {
        if (px->type == PR_TYPE_BACKEND) {
            backend_count++;
            printf("   ✓ Found backend: %s\n", px->id);
        }
    }

    TEST_ASSERT(backend_count >= 3, "Should have at least 3 backends");
    printf("   ✓ Total backends = %d\n", backend_count);

    return 0;
}

static int test_balance_algorithms() {
    printf("\n8. Testing balance algorithm parsing...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    proxy_t *api_backend = NULL;
    for (proxy_t *px = proxies_list; px; px = px->next) {
        if (strcmp(px->id, "api_servers") == 0) {
            api_backend = px;
            break;
        }
    }

    TEST_ASSERT(api_backend != NULL, "Backend 'api_servers' should exist");
    TEST_ASSERT(api_backend->lb_algo == LB_ALGO_LEASTCONN,
                "api_servers should use leastconn algorithm");
    printf("   ✓ api_servers uses leastconn algorithm\n");

    return 0;
}

static int test_format_detection() {
    printf("\n9. Testing automatic format detection...\n");

    reset_global();
    int yaml_result = config_parse("config/ultrabalancer.yaml");
    TEST_ASSERT(yaml_result == 0, ".yaml extension should trigger YAML parser");
    printf("   ✓ .yaml extension detected\n");

    reset_global();
    int yml_result = config_parse("config/ultrabalancer.yaml");
    TEST_ASSERT(yml_result == 0, ".yml extension should trigger YAML parser");
    printf("   ✓ .yml extension handled\n");

    return 0;
}

static int test_server_properties() {
    printf("\n10. Testing server property parsing...\n");
    reset_global();

    config_parse("config/ultrabalancer.yaml");

    proxy_t *backend = NULL;
    for (proxy_t *px = proxies_list; px; px = px->next) {
        if (strcmp(px->id, "web_servers") == 0) {
            backend = px;
            break;
        }
    }

    TEST_ASSERT(backend != NULL, "Backend should exist");

    for (server_t *srv = backend->servers; srv; srv = srv->next) {
        TEST_ASSERT(srv->id != NULL, "Server should have name");
        TEST_ASSERT(srv->port > 0, "Server should have valid port");
        TEST_ASSERT(srv->weight > 0, "Server should have positive weight");

        printf("   ✓ Server %s: port=%d, weight=%d, check=%s\n",
               srv->id, srv->port, srv->weight, srv->check ? "yes" : "no");
    }

    return 0;
}

int main() {
    printf("\n=== UltraBalancer Configuration Parser Comprehensive Test Suite ===\n");

    log_init("/dev/null", LOG_ERR);

    test_invalid_file();
    test_yaml_basic_parsing();
    test_yaml_global_section();
    test_yaml_defaults_section();
    test_yaml_frontend_section();
    test_yaml_backend_section();
    test_yaml_multiple_backends();
    test_balance_algorithms();
    test_format_detection();
    test_server_properties();

    printf("\n=== Test Summary ===\n");
    printf("Tests Passed: %d\n", test_passed);
    printf("Tests Failed: %d\n", test_failed);
    printf("Total Tests:  %d\n", test_passed + test_failed);

    if (test_failed == 0) {
        printf("\n✓ All tests PASSED!\n\n");
        return 0;
    } else {
        printf("\n✗ Some tests FAILED!\n\n");
        return 1;
    }
}
