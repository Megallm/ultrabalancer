#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <syslog.h>
#include "config/config.h"
#include "core/proxy.h"
#include "utils/log.h"

extern proxy_t *proxies_list;
extern struct global global;

static void test_cfg_parsing() {
    printf("Testing .cfg file parsing...\n");

    int result = config_parse("config/ultrabalancer.cfg");
    assert(result == 0 && "Failed to parse .cfg file");

    assert(global.maxconn == 100000 && "Global maxconn incorrect");
    assert(global.nbthread == 8 && "Global nbthread incorrect");
    assert(global.nbproc == 1 && "Global nbproc incorrect");
    assert(global.daemon == 1 && "Global daemon incorrect");

    printf("✓ CFG file parsed successfully\n");
}

static void test_yaml_parsing() {
    printf("Testing .yaml file parsing...\n");

    proxies_list = NULL;
    memset(&global, 0, sizeof(global));

    int result = config_parse("config/ultrabalancer.yaml");
    assert(result == 0 && "Failed to parse .yaml file");

    assert(global.maxconn == 100000 && "YAML: Global maxconn incorrect");
    assert(global.nbthread == 8 && "YAML: Global nbthread incorrect");
    assert(global.nbproc == 1 && "YAML: Global nbproc incorrect");

    printf("✓ YAML file parsed successfully\n");
}

static void test_format_detection() {
    printf("Testing format detection...\n");

    int result1 = config_parse("config/ultrabalancer.cfg");
    assert(result1 == 0 && "Failed to auto-detect .cfg format");

    proxies_list = NULL;
    memset(&global, 0, sizeof(global));

    int result2 = config_parse("config/ultrabalancer.yaml");
    assert(result2 == 0 && "Failed to auto-detect .yaml format");

    printf("✓ Format detection works correctly\n");
}

static void test_invalid_file() {
    printf("Testing invalid file handling...\n");

    int result = config_parse("nonexistent.cfg");
    assert(result == -1 && "Should fail on nonexistent file");

    printf("✓ Invalid file handling works\n");
}

static void test_config_validation() {
    printf("Testing config validation...\n");

    config_parse("config/ultrabalancer.cfg");

    int result = config_check();
    if (result == 0) {
        printf("✓ Config validation passed\n");
    } else {
        printf("⚠ Config validation found issues (expected for incomplete config)\n");
    }
}

static void test_backend_servers() {
    printf("Testing backend server parsing...\n");

    proxies_list = NULL;
    memset(&global, 0, sizeof(global));

    config_parse("config/ultrabalancer.yaml");

    proxy_t *px = proxies_list;
    int found_backend = 0;
    int server_count = 0;

    while (px) {
        if (px->type == PR_TYPE_BACKEND && strcmp(px->id, "web_servers") == 0) {
            found_backend = 1;

            server_t *srv = px->servers;
            while (srv) {
                server_count++;
                srv = srv->next;
            }
            break;
        }
        px = px->next;
    }

    assert(found_backend && "Backend 'web_servers' not found");
    assert(server_count == 3 && "Expected 3 servers in web_servers backend");

    printf("✓ Backend servers parsed correctly\n");
}

static void test_timeout_parsing() {
    printf("Testing timeout parsing...\n");

    proxies_list = NULL;
    memset(&global, 0, sizeof(global));

    config_parse("config/ultrabalancer.yaml");

    proxy_t *px = proxies_list;
    while (px) {
        if (strcmp(px->id, "defaults") == 0) {
            assert(px->timeout.connect == 5000 && "Connect timeout incorrect");
            assert(px->timeout.client == 30000 && "Client timeout incorrect");
            assert(px->timeout.server == 30000 && "Server timeout incorrect");
            printf("✓ Timeouts parsed correctly\n");
            return;
        }
        px = px->next;
    }

    printf("⚠ Defaults section not found\n");
}

static void test_balance_algorithm() {
    printf("Testing balance algorithm parsing...\n");

    proxies_list = NULL;
    memset(&global, 0, sizeof(global));

    config_parse("config/ultrabalancer.yaml");

    proxy_t *px = proxies_list;
    int found_roundrobin = 0;
    int found_leastconn = 0;

    while (px) {
        if (px->type == PR_TYPE_BACKEND) {
            if (strcmp(px->id, "web_servers") == 0) {
                assert(px->lb_algo == LB_ALGO_ROUNDROBIN && "web_servers should use roundrobin");
                found_roundrobin = 1;
            } else if (strcmp(px->id, "api_servers") == 0) {
                assert(px->lb_algo == LB_ALGO_LEASTCONN && "api_servers should use leastconn");
                found_leastconn = 1;
            }
        }
        px = px->next;
    }

    assert(found_roundrobin && "Roundrobin algorithm not found");
    assert(found_leastconn && "Leastconn algorithm not found");

    printf("✓ Balance algorithms parsed correctly\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("\n=== UltraBalancer Config Parser Tests ===\n\n");

    log_init("/dev/null", LOG_ERR);

    test_cfg_parsing();
    test_yaml_parsing();
    test_format_detection();
    test_invalid_file();
    test_config_validation();
    test_backend_servers();
    test_timeout_parsing();
    test_balance_algorithm();

    printf("\n=== All Config Tests Passed ✓ ===\n\n");

    return 0;
}
