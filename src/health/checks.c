#include "health/health.h"
#include "core/proxy.h"
#include "utils/log.h"
#include "utils/buffer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>

// Check type names for logging
static const char *check_type_names[] = {
    "TCP", "HTTP", "HTTPS", "SMTP", "LDAP", "MYSQL",
    "PGSQL", "REDIS", "SSL", "EXTERNAL", "AGENT"
};

check_t* check_new(check_type_t type) {
    check_t *check = calloc(1, sizeof(*check));
    if (!check) return NULL;

    check->type = type;
    check->state = CHK_ST_INIT;
    check->status = HCHK_STATUS_UNKNOWN;

    // Default intervals
    check->interval.inter = 2000;
    check->interval.fastinter = 1000;
    check->interval.downinter = 5000;
    check->interval.timeout = 5000;
    check->interval.rise = 3;
    check->interval.fall = 3;

    check->conn.fd = -1;
    check->conn.buf = buffer_new(8192);

    return check;
}

int check_tcp(check_t *check) {
    struct server *srv = check->server;

    // Create socket
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
        return -1;
    }

    // Set socket options
    int val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));

    struct timeval tv;
    tv.tv_sec = check->interval.timeout / 1000;
    tv.tv_usec = (check->interval.timeout % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Connect to backend
    struct sockaddr_in *sin = (struct sockaddr_in *)&srv->addr;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(check->port ? check->port : srv->port),
        .sin_addr = sin->sin_addr
    };

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
            return -1;
        }
    }

    check->conn.fd = fd;

    // Send check data if configured
    if (check->tcp.send_string && check->tcp.send_len > 0) {
        ssize_t sent = send(fd, check->tcp.send_string, check->tcp.send_len, MSG_NOSIGNAL);
        if (sent < 0) {
            close(fd);
            check->conn.fd = -1;
            set_server_check_status(check, HCHK_STATUS_L4CON, "Send failed");
            return -1;
        }
    }

    // Receive and check response
    if (check->tcp.expect_string || check->tcp.expect_regex) {
        char buffer[4096];
        ssize_t received = recv(fd, buffer, sizeof(buffer) - 1, 0);

        if (received <= 0) {
            close(fd);
            check->conn.fd = -1;
            set_server_check_status(check, HCHK_STATUS_L4TOUT, "No response");
            return -1;
        }

        buffer[received] = '\0';

        // Check expected response
        if (check->tcp.expect_string) {
            if (!strstr(buffer, check->tcp.expect_string)) {
                close(fd);
                check->conn.fd = -1;
                set_server_check_status(check, HCHK_STATUS_L7RSP, "Unexpected response");
                return -1;
            }
        }

        if (check->tcp.expect_regex) {
            int ovector[30];
            int ret = pcre_exec(check->tcp.expect_regex, NULL, buffer, received,
                               0, 0, ovector, 30);
            if (ret < 0) {
                close(fd);
                check->conn.fd = -1;
                set_server_check_status(check, HCHK_STATUS_L7RSP, "Regex mismatch");
                return -1;
            }
        }
    }

    close(fd);
    check->conn.fd = -1;

    set_server_check_status(check, HCHK_STATUS_L4OK, "TCP check passed");
    return 0;
}

int check_http(check_t *check) {
    struct server *srv = check->server;

    // First establish TCP connection
    if (check_tcp(check) < 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
        return -1;
    }

    struct sockaddr_in *sin = (struct sockaddr_in *)&srv->addr;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(check->port ? check->port : srv->port),
        .sin_addr = sin->sin_addr
    };

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS && errno != EISCONN) {
            close(fd);
            set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
            return -1;
        }
    }

    // Build HTTP request
    char request[1024];
    int len = snprintf(request, sizeof(request),
                      "%s %s HTTP/1.%d\r\n"
                      "Host: %s\r\n"
                      "User-Agent: UltraBalancer/1.0\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      check->http.method ? check->http.method : "OPTIONS",
                      check->http.uri ? check->http.uri : "/",
                      check->http.version ? check->http.version : 1,
                      check->http.host ? check->http.host : "localhost");

    if (send(fd, request, len, MSG_NOSIGNAL) < 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6RSP, "Failed to send request");
        return -1;
    }

    // Read response
    char response[4096];
    ssize_t received = recv(fd, response, sizeof(response) - 1, 0);

    if (received <= 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6TOUT, "No HTTP response");
        return -1;
    }

    response[received] = '\0';

    // Parse status code
    int status_code = 0;
    if (sscanf(response, "HTTP/%*d.%*d %d", &status_code) != 1) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L7RSP, "Invalid HTTP response");
        return -1;
    }

    // Check expected status
    if (check->tcp.expect_status > 0) {
        if (status_code != check->tcp.expect_status) {
            close(fd);
            char msg[64];
            snprintf(msg, sizeof(msg), "Status %d != %d", status_code, check->tcp.expect_status);
            set_server_check_status(check, HCHK_STATUS_L7STS, msg);
            return -1;
        }
    } else {
        // Default: accept 2xx and 3xx
        if (status_code < 200 || status_code >= 400) {
            close(fd);
            char msg[64];
            snprintf(msg, sizeof(msg), "HTTP status %d", status_code);
            set_server_check_status(check, HCHK_STATUS_L7STS, msg);
            return -1;
        }
    }

    close(fd);
    set_server_check_status(check, HCHK_STATUS_L7OK, "HTTP check passed");
    return 0;
}

int check_mysql(check_t *check) {
    struct server *srv = check->server;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
        return -1;
    }

    struct sockaddr_in *sin = (struct sockaddr_in *)&srv->addr;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(check->port ? check->port : 3306),
        .sin_addr = sin->sin_addr
    };

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
        return -1;
    }

    // Read MySQL handshake packet
    uint8_t packet[256];
    ssize_t received = recv(fd, packet, sizeof(packet), 0);

    if (received < 4) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6RSP, "Invalid MySQL handshake");
        return -1;
    }

    // Check packet header
    uint32_t packet_len = packet[0] | (packet[1] << 8) | (packet[2] << 16);
    uint8_t packet_num = packet[3];

    if (packet_len < 4 || packet_num != 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6RSP, "Invalid MySQL packet");
        return -1;
    }

    // Check protocol version (packet[4])
    if (packet[4] != 10 && packet[4] != 9) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6RSP, "Unsupported MySQL version");
        return -1;
    }

    close(fd);
    set_server_check_status(check, HCHK_STATUS_L6OK, "MySQL check passed");
    return 0;
}

int check_redis(check_t *check) {
    struct server *srv = check->server;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
        return -1;
    }

    struct sockaddr_in *sin = (struct sockaddr_in *)&srv->addr;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(check->port ? check->port : 6379),
        .sin_addr = sin->sin_addr
    };

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L4CON, strerror(errno));
        return -1;
    }

    // Send PING command
    const char *ping_cmd = "*1\r\n$4\r\nPING\r\n";
    if (send(fd, ping_cmd, strlen(ping_cmd), MSG_NOSIGNAL) < 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6RSP, "Failed to send PING");
        return -1;
    }

    // Expect "+PONG\r\n"
    char response[32];
    ssize_t received = recv(fd, response, sizeof(response) - 1, 0);

    if (received < 7 || strncmp(response, "+PONG\r\n", 7) != 0) {
        close(fd);
        set_server_check_status(check, HCHK_STATUS_L6RSP, "Invalid PONG response");
        return -1;
    }

    close(fd);
    set_server_check_status(check, HCHK_STATUS_L6OK, "Redis check passed");
    return 0;
}

void set_server_check_status(check_t *check, check_status_t status, const char *desc) {
    check->status = status;

    if (desc) {
        strncpy(check->desc, desc, sizeof(check->desc) - 1);
        check->desc[sizeof(check->desc) - 1] = '\0';
    }

    struct server *srv = check->server;
    if (!srv) return;

    time_t now = time(NULL);

    // Update server state based on check result
    switch (status) {
        case HCHK_STATUS_L4OK:
        case HCHK_STATUS_L6OK:
        case HCHK_STATUS_L7OK:
        case HCHK_STATUS_L7OKC:
            check->consecutive_success++;
            check->consecutive_errors = 0;

            if (check->consecutive_success >= check->interval.rise) {
                if (srv->cur_state != SRV_RUNNING) {
                    srv->cur_state = SRV_RUNNING;
                    srv->last_change = now;
                    log_info("Server %s:%d is UP", srv->hostname, srv->port);
                }
            }
            break;

        default:
            check->consecutive_errors++;
            check->consecutive_success = 0;

            if (check->consecutive_errors >= check->interval.fall) {
                if (srv->cur_state == SRV_RUNNING) {
                    srv->cur_state = SRV_MAINTAIN;
                    srv->last_change = now;
                    log_warning("Server %s:%d is DOWN: %s", srv->hostname, srv->port, desc);
                }
            }
            break;
    }

    check->last_check = now;
}

struct task* process_check(struct task *t, void *context, unsigned int state) {
    check_t *check = context;
    int ret = -1;

    check->start_time = time(NULL);

    // Execute check based on type
    switch (check->type) {
        case HCHK_TYPE_TCP:
            ret = check_tcp(check);
            break;
        case HCHK_TYPE_HTTP:
            ret = check_http(check);
            break;
        case HCHK_TYPE_HTTPS:
            ret = check_https(check);
            break;
        case HCHK_TYPE_MYSQL:
            ret = check_mysql(check);
            break;
        case HCHK_TYPE_REDIS:
            ret = check_redis(check);
            break;
        default:
            ret = check_tcp(check);
    }

    check->duration = (time(NULL) - check->start_time) * 1000;

    // Schedule next check
    uint32_t interval;
    if (check->server->cur_state == SRV_RUNNING) {
        interval = check->interval.inter;
    } else if (check->consecutive_errors == 0) {
        interval = check->interval.fastinter;
    } else {
        interval = check->interval.downinter;
    }

    t->expire = tick_add(now_ms, interval);

    return t;
}

int start_health_check(struct server *srv) {
    if (!srv->check) {
        srv->check = check_new(HCHK_TYPE_TCP);
        if (!srv->check) return -1;
    }

    srv->check->server = srv;
    srv->check->state = CHK_ST_ENABLED;

    // Create task for periodic checks
    srv->check->task = task_new();
    if (!srv->check->task) return -1;

    srv->check->task->process = process_check;
    srv->check->task->context = srv->check;
    srv->check->task->expire = tick_add(now_ms, srv->check->interval.inter);

    task_queue(srv->check->task);

    log_debug("Started health check for %s:%d", srv->hostname, srv->port);
    return 0;
}