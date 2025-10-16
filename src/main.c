#include <sys/mman.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include "core/lb_types.h"
#include "core/common.h"
#include "core/lb_network.h"
#include "config/config.h"

static loadbalancer_t* global_lb = NULL;

static loadbalancer_t* main_lb_create(uint16_t port, lb_algorithm_t algorithm);
static void main_lb_destroy(loadbalancer_t* lb);
static int main_lb_start(loadbalancer_t* lb);
static void main_lb_stop(loadbalancer_t* lb);
static int main_lb_add_backend(loadbalancer_t* lb, const char* host, uint16_t port, uint32_t weight);

extern void* health_check_thread(void* arg);
extern void* stats_thread(void* arg);

extern struct proxy *proxies_list;
extern struct global global;
extern time_t start_time;
extern uint32_t total_connections;
extern volatile unsigned int now_ms;

static void signal_handler(int sig) {
    if (global_lb) {
        printf("\nShutting down...\n");
        main_lb_stop(global_lb);
        main_lb_destroy(global_lb);
        exit(0);
    }
}

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  -c, --config FILE        Configuration file (.cfg or .yaml)\n");
    printf("  -p, --port PORT          Listen port (default: 8080)\n");
    printf("  -a, --algorithm ALGO     Load balancing algorithm:\n");
    printf("                           round-robin (default)\n");
    printf("                           least-conn\n");
    printf("                           ip-hash\n");
    printf("                           weighted\n");
    printf("                           response-time\n");
    printf("  -b, --backend HOST:PORT  Add backend server (can specify multiple)\n");
    printf("  -w, --workers NUM        Number of worker threads (default: CPU*2)\n");
    printf("  --health-check-enabled   Enable health checks (default: true)\n");
    printf("  --no-health-check        Disable health checks\n");
    printf("  --health-check-interval  Health check interval in ms (default: 5000)\n");
    printf("  --health-check-fails     Failed checks before marking down (default: 3)\n");
    printf("  -h, --help              Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s -c config/ultrabalancer.yaml\n", prog);
    printf("  %s -p 8080 -a least-conn -b 127.0.0.1:8001 -b 127.0.0.1:8002\n", prog);
    printf("  %s -p 8080 --no-health-check -b 127.0.0.1:8001\n", prog);
}

static loadbalancer_t* main_lb_create(uint16_t port, lb_algorithm_t algorithm) {
    loadbalancer_t* lb = calloc(1, sizeof(loadbalancer_t));
    if (!lb) return NULL;

    lb->port = port;
    lb->algorithm = algorithm;
    lb->running = false;
    lb->worker_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;

    pthread_spin_init(&lb->conn_pool_lock, PTHREAD_PROCESS_PRIVATE);

    lb->config.connect_timeout_ms = 5000;
    lb->config.read_timeout_ms = 30000;
    lb->config.write_timeout_ms = 30000;
    lb->config.keepalive_timeout_ms = 60000;
    lb->config.health_check_interval_ms = 5000;
    lb->config.max_connections = MAX_CONNECTIONS;
    lb->config.health_check_fail_threshold = 3;
    lb->config.tcp_nodelay = true;
    lb->config.so_reuseport = true;
    lb->config.defer_accept = true;
    lb->config.health_check_enabled = true;

    lb->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (lb->epfd < 0) {
        free(lb);
        return NULL;
    }

    // Initialize memory pool
    size_t pool_size = 256 * 1024 * 1024; // 256MB
    lb->memory_pool = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (lb->memory_pool == MAP_FAILED) {
        close(lb->epfd);
        free(lb);
        return NULL;
    }

    // Initialize cleanup queue
    lb->cleanup_queue = calloc(1, sizeof(cleanup_queue_t));
    if (!lb->cleanup_queue) {
        munmap(lb->memory_pool, pool_size);
        close(lb->epfd);
        free(lb);
        return NULL;
    }

    // Initialize mutex for cleanup queue
    if (pthread_mutex_init(&lb->cleanup_queue->lock, NULL) != 0) {
        free(lb->cleanup_queue);
        munmap(lb->memory_pool, pool_size);
        close(lb->epfd);
        free(lb);
        return NULL;
    }

    atomic_store(&lb->cleanup_queue->head, 0);
    atomic_store(&lb->cleanup_queue->tail, 0);

    return lb;
}

static void main_lb_destroy(loadbalancer_t* lb) {
    if (!lb) return;

    for (uint32_t i = 0; i < lb->backend_count; i++) {
        if (lb->backends[i]) {
            pthread_spin_destroy(&lb->backends[i]->lock);
            free(lb->backends[i]);
        }
    }

    if (lb->memory_pool && lb->memory_pool != MAP_FAILED) {
        munmap(lb->memory_pool, 256 * 1024 * 1024);
    }

    if (lb->epfd >= 0) close(lb->epfd);
    pthread_spin_destroy(&lb->conn_pool_lock);

    // Cleanup queue with mutex destruction
    if (lb->cleanup_queue) {
        pthread_mutex_destroy(&lb->cleanup_queue->lock);
        free(lb->cleanup_queue);
        lb->cleanup_queue = NULL;
    }

    // Cleanup listen wrapper
    if (lb->listen_wrapper) {
        free(lb->listen_wrapper);
        lb->listen_wrapper = NULL;
    }

    // Cleanup workers
    if (lb->workers) {
        free(lb->workers);
        lb->workers = NULL;
    }

    free(lb);
}

static int main_lb_add_backend(loadbalancer_t* lb, const char* host, uint16_t port, uint32_t weight) {
    if (lb->backend_count >= MAX_BACKENDS) return -1;

    backend_t* backend = calloc(1, sizeof(backend_t));
    if (!backend) return -1;

    strncpy(backend->host, host, sizeof(backend->host) - 1);
    backend->port = port;
    backend->weight = weight ? weight : 1;
    backend->state = BACKEND_DOWN;
    backend->sockfd = -1;

    pthread_spin_init(&backend->lock, PTHREAD_PROCESS_PRIVATE);

    lb->backends[lb->backend_count++] = backend;

    return 0;
}

// Forward declaration - defined in lb_core.c
extern int create_listen_socket(uint16_t port, bool reuseport);

static backend_t* main_lb_select_backend(loadbalancer_t* lb, struct sockaddr_in* client_addr) {
    backend_t* selected = NULL;
    uint32_t min_conns = UINT32_MAX;

    switch (lb->algorithm) {
        case LB_ALGO_ROUNDROBIN: {
            uint32_t attempts = 0;
            while (attempts < lb->backend_count) {
                uint32_t idx = atomic_fetch_add(&lb->round_robin_idx, 1) % lb->backend_count;
                backend_t* b = lb->backends[idx];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    return b;
                }
                attempts++;
            }
            break;
        }

        case LB_ALGO_LEASTCONN: {
            for (uint32_t i = 0; i < lb->backend_count; i++) {
                backend_t* b = lb->backends[i];
                if (b && atomic_load(&b->state) == BACKEND_UP) {
                    uint32_t conns = atomic_load(&b->active_conns);
                    if (conns < min_conns) {
                        min_conns = conns;
                        selected = b;
                    }
                }
            }
            break;
        }

        default:
            // Default to round-robin
            return lb->backends[lb->round_robin_idx++ % lb->backend_count];
    }

    return selected;
}

// Old worker_thread removed - using worker_thread_v2 from lb_net.c

static int main_lb_start(loadbalancer_t* lb) {
    if (!lb || lb->running) return -1;

    lb->listen_fd = create_listen_socket(lb->port, lb->config.so_reuseport);
    if (lb->listen_fd < 0) {
        perror("Failed to create listen socket");
        return -1;
    }

    // Allocate and initialize listen socket wrapper
    lb->listen_wrapper = (epoll_data_wrapper_t*)malloc(sizeof(epoll_data_wrapper_t));
    if (!lb->listen_wrapper) {
        perror("Failed to allocate listen wrapper");
        close(lb->listen_fd);
        return -1;
    }

    memset(lb->listen_wrapper, 0, sizeof(epoll_data_wrapper_t));
    lb->listen_wrapper->type = SOCKET_TYPE_LISTEN;
    lb->listen_wrapper->fd = lb->listen_fd;
    lb->listen_wrapper->conn = NULL;

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.ptr = lb->listen_wrapper
    };

    if (epoll_ctl(lb->epfd, EPOLL_CTL_ADD, lb->listen_fd, &ev) < 0) {
        free(lb->listen_wrapper);
        lb->listen_wrapper = NULL;
        close(lb->listen_fd);
        return -1;
    }

    lb->running = true;

    // Start all backends as UP for testing
    for (uint32_t i = 0; i < lb->backend_count; i++) {
        lb->backends[i]->state = BACKEND_UP;
    }

    lb->workers = calloc(lb->worker_threads, sizeof(pthread_t));
    if (!lb->workers) {
        close(lb->listen_fd);
        return -1;
    }

    for (uint32_t i = 0; i < lb->worker_threads; i++) {
        if (pthread_create(&lb->workers[i], NULL, worker_thread_v2, lb) != 0) {
            lb->running = false;
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(lb->workers[j], NULL);
            }
            free(lb->workers);
            lb->workers = NULL;
            close(lb->listen_fd);
            return -1;
        }
    }

    pthread_t health_thread;
    if (pthread_create(&health_thread, NULL, health_check_thread, lb) == 0) {
        pthread_detach(health_thread);
    }

    pthread_t stats_tid;
    if (pthread_create(&stats_tid, NULL, stats_thread, lb) == 0) {
        pthread_detach(stats_tid);
    }

    printf("Load balancer started on port %u with %u workers\n", lb->port, lb->worker_threads);
    printf("Algorithm: ");
    switch (lb->algorithm) {
        case LB_ALGO_ROUNDROBIN: printf("Round Robin\n"); break;
        case LB_ALGO_LEASTCONN: printf("Least Connections\n"); break;
        case LB_ALGO_SOURCE: printf("IP Hash\n"); break;
        case LB_ALGO_STICKY: printf("Weighted\n"); break;
        default: printf("Unknown\n");
    }
    printf("\nHealth checks enabled (interval: %ums)\n", lb->config.health_check_interval_ms);
    printf("Statistics will be printed every 5 seconds\n\n");

    return 0;
}

static void main_lb_stop(loadbalancer_t* lb) {
    if (!lb || !lb->running) return;

    lb->running = false;

    if (lb->workers) {
        for (uint32_t i = 0; i < lb->worker_threads; i++) {
            pthread_join(lb->workers[i], NULL);
        }
    }

    if (lb->listen_fd >= 0) {
        close(lb->listen_fd);
    }

    printf("Load balancer stopped\n");
}

int main(int argc, char** argv) {
    const char* config_file = NULL;
    uint16_t port = 8080;
    lb_algorithm_t algorithm = LB_ALGO_ROUNDROBIN;
    struct {
        char host[256];
        uint16_t port;
        uint32_t weight;
    } backends[MAX_BACKENDS];
    int backend_count = 0;
    uint32_t workers = 0;
    bool health_check_enabled = true;
    uint32_t health_check_interval = 5000;
    uint32_t health_check_fails = 3;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"algorithm", required_argument, 0, 'a'},
        {"backend", required_argument, 0, 'b'},
        {"workers", required_argument, 0, 'w'},
        {"health-check-enabled", no_argument, 0, 1001},
        {"no-health-check", no_argument, 0, 1002},
        {"health-check-interval", required_argument, 0, 1003},
        {"health-check-fails", required_argument, 0, 1004},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:a:b:w:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;

            case 'p':
                port = atoi(optarg);
                break;

            case 'a':
                if (strcmp(optarg, "round-robin") == 0) {
                    algorithm = LB_ALGO_ROUNDROBIN;
                } else if (strcmp(optarg, "least-conn") == 0) {
                    algorithm = LB_ALGO_LEASTCONN;
                } else if (strcmp(optarg, "ip-hash") == 0) {
                    algorithm = LB_ALGO_SOURCE;
                } else if (strcmp(optarg, "weighted") == 0 || strcmp(optarg, "weighted-rr") == 0) {
                    algorithm = LB_ALGO_STICKY;
                } else if (strcmp(optarg, "response-time") == 0) {
                    algorithm = LB_ALGO_RANDOM;
                } else {
                    fprintf(stderr, "Unknown algorithm: %s\n", optarg);
                    exit(1);
                }
                break;

            case 'b': {
                if (backend_count >= MAX_BACKENDS) {
                    fprintf(stderr, "Too many backends (max %d)\n", MAX_BACKENDS);
                    exit(1);
                }

                char* colon = strchr(optarg, ':');
                if (!colon) {
                    fprintf(stderr, "Invalid backend format: %s (expected HOST:PORT)\n", optarg);
                    exit(1);
                }

                *colon = '\0';
                strncpy(backends[backend_count].host, optarg, 255);
                backends[backend_count].port = atoi(colon + 1);
                backends[backend_count].weight = 1;

                char* at = strchr(colon + 1, '@');
                if (at) {
                    backends[backend_count].weight = atoi(at + 1);
                }

                backend_count++;
                break;
            }

            case 'w':
                workers = atoi(optarg);
                break;

            case 1001:
                health_check_enabled = true;
                break;

            case 1002:
                health_check_enabled = false;
                break;

            case 1003:
                health_check_interval = atoi(optarg);
                break;

            case 1004:
                health_check_fails = atoi(optarg);
                break;

            case 'h':
                print_usage(argv[0]);
                exit(0);

            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    if (config_file) {
        printf("Loading configuration from: %s\n", config_file);
        if (config_parse(config_file) < 0) {
            fprintf(stderr, "Failed to parse config file: %s\n", config_file);
            exit(1);
        }

        if (config_check() < 0) {
            fprintf(stderr, "Configuration validation failed\n");
            exit(1);
        }

        printf("Configuration loaded successfully\n");
        exit(0);
    }

    // default backend if none specified
    
    if (backend_count == 0) {
        strncpy(backends[0].host, "127.0.0.1", sizeof(backends[0].host) - 1);
        backends[0].port = 8001;
        backends[0].weight = 1;

        strncpy(backends[1].host, "127.0.0.1", sizeof(backends[1].host) - 1);
        backends[1].port = 8002;
        backends[1].weight = 1;

        strncpy(backends[2].host, "127.0.0.1", sizeof(backends[2].host) - 1);
        backends[2].port = 8003;
        backends[2].weight = 1;

        backend_count = 3;
        printf("No backends specified, using defaults: 127.0.0.1:8001-8003\n");
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    start_time = time(NULL);

    global_lb = main_lb_create(port, algorithm);
    if (!global_lb) {
        fprintf(stderr, "Failed to create load balancer\n");
        exit(1);
    }

    if (workers > 0) {
        global_lb->worker_threads = workers;
    }

    // Apply health check configuration from CLI
    global_lb->config.health_check_enabled = health_check_enabled;
    global_lb->config.health_check_interval_ms = health_check_interval;
    global_lb->config.health_check_fail_threshold = health_check_fails;

    printf("Health check: %s (interval: %ums, fail threshold: %u)\n",
           health_check_enabled ? "enabled" : "disabled",
           health_check_interval, health_check_fails);

    for (int i = 0; i < backend_count; i++) {
        if (main_lb_add_backend(global_lb, backends[i].host, backends[i].port,
                          backends[i].weight) < 0) {
            fprintf(stderr, "Failed to add backend %s:%u\n",
                   backends[i].host, backends[i].port);
        } else {
            printf("Added backend: %s:%u (weight: %u)\n",
                   backends[i].host, backends[i].port, backends[i].weight);
        }
    }

    if (main_lb_start(global_lb) < 0) {
        fprintf(stderr, "Failed to start load balancer\n");
        main_lb_destroy(global_lb);
        exit(1);
    }

    while (global_lb->running) {
        sleep(1);
        now_ms += 1000; // Update time
    }

    main_lb_destroy(global_lb);
    return 0;
}