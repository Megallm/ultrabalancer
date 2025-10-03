#include "stats/stats.h"
#include "core/proxy.h"
#include "health/health.h"
#include "utils/log.h"
#include "utils/buffer.h"
#include <stdio.h>
#include <string.h>

// Field names for CSV/JSON output
static const char *field_names[STAT_PX_MAX] = {
    "req_rate", "req_rate_max", "req_tot",
    "conn_rate", "conn_rate_max", "conn_tot",
    "conn_cur", "conn_max", "sess_cur", "sess_max",
    "sess_limit", "sess_tot", "bytes_in", "bytes_out",
    "denied_req", "denied_resp", "failed_req", "failed_hchk",
    "status", "weight", "act", "bck", "chkdown", "lastchg",
    "downtime", "qcur", "qmax", "qlimit", "throttle",
    "rate", "rate_max", "check_status", "check_code",
    "check_duration", "hrsp_1xx", "hrsp_2xx", "hrsp_3xx",
    "hrsp_4xx", "hrsp_5xx", "hrsp_other", "cache_hits",
    "cache_misses", "comp_in", "comp_out", "comp_byp",
    "comp_rsp", "lastsess", "qtime", "ctime", "rtime", "ttime"
};

int stats_fill_fe_stats(struct proxy *px, field_t *stats, int len) {
    if (len < STAT_PX_MAX) return -1;

    int i = 0;

    // Connection stats
    stats[i++].u.u64 = atomic_load(&px->fe_counters[0]);  // req_rate
    stats[i++].u.u64 = atomic_load(&px->fe_counters[1]);  // req_rate_max
    stats[i++].u.u64 = atomic_load(&px->fe_counters[2]);  // req_tot
    stats[i++].u.u32 = atomic_load(&px->fe_counters[3]);  // conn_rate
    stats[i++].u.u32 = atomic_load(&px->fe_counters[4]);  // conn_rate_max
    stats[i++].u.u64 = atomic_load(&px->fe_counters[5]);  // conn_tot
    stats[i++].u.u32 = atomic_load(&px->fe_counters[6]);  // conn_cur
    stats[i++].u.u32 = atomic_load(&px->fe_counters[7]);  // conn_max

    // Session stats
    stats[i++].u.u32 = atomic_load(&px->fe_counters[8]);  // sess_cur
    stats[i++].u.u32 = atomic_load(&px->fe_counters[9]);  // sess_max
    stats[i++].u.u32 = px->maxconn;                       // sess_limit
    stats[i++].u.u64 = atomic_load(&px->fe_counters[10]); // sess_tot

    // Bytes
    stats[i++].u.u64 = atomic_load(&px->fe_counters[11]); // bytes_in
    stats[i++].u.u64 = atomic_load(&px->fe_counters[12]); // bytes_out

    // Errors
    stats[i++].u.u64 = atomic_load(&px->fe_counters[13]); // denied_req
    stats[i++].u.u64 = atomic_load(&px->fe_counters[14]); // denied_resp
    stats[i++].u.u64 = atomic_load(&px->fe_counters[15]); // failed_req

    // Status
    stats[i++].u.str = (px->state & PR_FL_READY) ? "OPEN" : "STOP";

    // HTTP response codes
    stats[i++].u.u64 = atomic_load(&px->fe_counters[16]); // hrsp_1xx
    stats[i++].u.u64 = atomic_load(&px->fe_counters[17]); // hrsp_2xx
    stats[i++].u.u64 = atomic_load(&px->fe_counters[18]); // hrsp_3xx
    stats[i++].u.u64 = atomic_load(&px->fe_counters[19]); // hrsp_4xx
    stats[i++].u.u64 = atomic_load(&px->fe_counters[20]); // hrsp_5xx
    stats[i++].u.u64 = atomic_load(&px->fe_counters[21]); // hrsp_other

    return i;
}

int stats_fill_be_stats(struct proxy *px, field_t *stats, int len) {
    if (len < STAT_PX_MAX) return -1;

    int i = 0;

    // Similar to frontend but from backend counters
    stats[i++].u.u64 = atomic_load(&px->be_counters[0]);  // req_rate
    stats[i++].u.u64 = atomic_load(&px->be_counters[1]);  // req_rate_max
    stats[i++].u.u64 = atomic_load(&px->be_counters[2]);  // req_tot

    // Calculate backend-specific stats
    int act = 0, bck = 0;
    server_t *srv;
    for (srv = px->servers; srv; srv = srv->next) {
        if (srv->cur_state == SRV_RUNNING) {
            if (srv->flags & SRV_BACKUP) bck++;
            else act++;
        }
    }

    stats[i++].u.u32 = act;  // active servers
    stats[i++].u.u32 = bck;  // backup servers

    return i;
}

int stats_fill_sv_stats(struct proxy *px, struct server *sv, field_t *stats, int len) {
    if (len < STAT_PX_MAX) return -1;

    int i = 0;

    // Server-specific stats
    stats[i++].u.u32 = atomic_load(&sv->cur_conns);      // cur_conns
    stats[i++].u.u32 = atomic_load(&sv->max_conns);      // max_conns
    stats[i++].u.u64 = atomic_load(&sv->cum_conns);      // total_conns

    // Status
    const char *status = "UNKNOWN";
    switch (sv->cur_state) {
        case SRV_RUNNING: status = "UP"; break;
        case SRV_BACKUP:  status = "BACKUP"; break;
        case SRV_DRAIN:   status = "DRAIN"; break;
        case SRV_MAINTAIN: status = "MAINT"; break;
    }
    stats[i++].u.str = status;

    // Weight
    stats[i++].u.u32 = sv->weight;

    // Check status
    if (sv->check) {
        stats[i++].u.str = get_check_status_string(sv->check->status);
        stats[i++].u.s32 = sv->check->code;
        stats[i++].u.u32 = sv->check->duration;
    }

    // Last change
    stats[i++].u.u64 = sv->last_change;

    // Queue
    stats[i++].u.u32 = atomic_load(&sv->counters[0]);  // qcur
    stats[i++].u.u32 = atomic_load(&sv->counters[1]);  // qmax

    // Response times
    stats[i++].u.u64 = atomic_load(&sv->counters[10]); // qtime
    stats[i++].u.u64 = atomic_load(&sv->counters[11]); // ctime
    stats[i++].u.u64 = atomic_load(&sv->counters[12]); // rtime
    stats[i++].u.u64 = atomic_load(&sv->counters[13]); // ttime

    return i;
}

// Output stats in CSV format
int stats_dump_csv_header(struct channel *chn) {
    const char *header =
        "# pxname,svname,qcur,qmax,scur,smax,slim,stot,bin,bout,"
        "dreq,dresp,ereq,econ,eresp,wretr,wredis,status,weight,"
        "act,bck,chkfail,chkdown,lastchg,downtime,qlimit,pid,iid,"
        "sid,throttle,lbtot,tracked,type,rate,rate_lim,rate_max,"
        "check_status,check_code,check_duration,hrsp_1xx,hrsp_2xx,"
        "hrsp_3xx,hrsp_4xx,hrsp_5xx,hrsp_other,hanafail,req_rate,"
        "req_rate_max,req_tot,cli_abrt,srv_abrt,comp_in,comp_out,"
        "comp_byp,comp_rsp,lastsess,last_chk,last_agt,qtime,ctime,"
        "rtime,ttime,agent_status,agent_code,agent_duration\n";

    return buffer_put(&chn->buf, header, strlen(header));
}

// Output stats in JSON format
int stats_dump_json_to_buffer(struct stream *s, struct channel *res) {
    struct proxy *px;
    char json_buf[65536];
    int len = 0;

    len += snprintf(json_buf + len, sizeof(json_buf) - len, "{\n");
    len += snprintf(json_buf + len, sizeof(json_buf) - len, "  \"proxies\": [\n");

    int first_px = 1;
    for (px = proxies_list; px; px = px->next) {
        if (!first_px) {
            len += snprintf(json_buf + len, sizeof(json_buf) - len, ",\n");
        }
        first_px = 0;

        len += snprintf(json_buf + len, sizeof(json_buf) - len,
                       "    {\n"
                       "      \"name\": \"%s\",\n"
                       "      \"type\": \"%s\",\n"
                       "      \"status\": \"%s\",\n",
                       px->id,
                       px->mode == PR_MODE_TCP ? "tcp" : "http",
                       (px->state & PR_FL_READY) ? "UP" : "DOWN");

        // Add frontend stats
        if (px->type == PR_TYPE_FRONTEND || px->type == PR_TYPE_LISTEN) {
            len += snprintf(json_buf + len, sizeof(json_buf) - len,
                           "      \"frontend\": {\n"
                           "        \"connections\": %lu,\n"
                           "        \"sessions\": %lu,\n"
                           "        \"bytes_in\": %lu,\n"
                           "        \"bytes_out\": %lu,\n"
                           "        \"denied_requests\": %lu,\n"
                           "        \"errors\": %lu\n"
                           "      }",
                           atomic_load(&px->fe_counters[5]),
                           atomic_load(&px->fe_counters[10]),
                           atomic_load(&px->fe_counters[11]),
                           atomic_load(&px->fe_counters[12]),
                           atomic_load(&px->fe_counters[13]),
                           atomic_load(&px->fe_counters[15]));
        }

        // Add backend stats
        if (px->type == PR_TYPE_BACKEND || px->type == PR_TYPE_LISTEN) {
            if (px->type == PR_TYPE_LISTEN) {
                len += snprintf(json_buf + len, sizeof(json_buf) - len, ",\n");
            }

            len += snprintf(json_buf + len, sizeof(json_buf) - len,
                           "      \"backend\": {\n"
                           "        \"servers\": [\n");

            server_t *srv;
            int first_srv = 1;
            for (srv = px->servers; srv; srv = srv->next) {
                if (!first_srv) {
                    len += snprintf(json_buf + len, sizeof(json_buf) - len, ",\n");
                }
                first_srv = 0;

                const char *status = "UNKNOWN";
                switch (srv->cur_state) {
                    case SRV_RUNNING: status = "UP"; break;
                    case SRV_BACKUP:  status = "BACKUP"; break;
                    case SRV_DRAIN:   status = "DRAIN"; break;
                    case SRV_MAINTAIN: status = "MAINT"; break;
                }

                len += snprintf(json_buf + len, sizeof(json_buf) - len,
                               "          {\n"
                               "            \"name\": \"%s\",\n"
                               "            \"address\": \"%s:%d\",\n"
                               "            \"status\": \"%s\",\n"
                               "            \"weight\": %u,\n"
                               "            \"active_connections\": %d,\n"
                               "            \"total_connections\": %lu\n"
                               "          }",
                               srv->id,
                               srv->hostname, srv->port,
                               status,
                               srv->weight,
                               atomic_load(&srv->cur_conns),
                               atomic_load(&srv->cum_conns));
            }

            len += snprintf(json_buf + len, sizeof(json_buf) - len,
                           "\n        ]\n"
                           "      }");
        }

        len += snprintf(json_buf + len, sizeof(json_buf) - len, "\n    }");
    }

    len += snprintf(json_buf + len, sizeof(json_buf) - len,
                   "\n  ],\n"
                   "  \"info\": {\n"
                   "    \"version\": \"%s\",\n"
                   "    \"uptime\": %ld,\n"
                   "    \"max_connections\": %u,\n"
                   "    \"current_connections\": %u\n"
                   "  }\n"
                   "}\n",
                   UB_VERSION,
                   time(NULL) - start_time,
                   global.maxconn,
                   total_connections);

    return buffer_put(&res->buf, json_buf, len);
}

// Prometheus metrics format
int stats_dump_prometheus(struct stream *s, struct channel *res) {
    struct proxy *px;
    char buf[4096];
    int len;

    // Global metrics
    len = snprintf(buf, sizeof(buf),
                  "# HELP ultrabalancer_up Is the load balancer up\n"
                  "# TYPE ultrabalancer_up gauge\n"
                  "ultrabalancer_up 1\n"
                  "\n"
                  "# HELP ultrabalancer_connections_total Total connections\n"
                  "# TYPE ultrabalancer_connections_total counter\n");

    buffer_put(&res->buf, buf, len);

    // Per-proxy metrics
    for (px = proxies_list; px; px = px->next) {
        len = snprintf(buf, sizeof(buf),
                      "ultrabalancer_frontend_connections_total{proxy=\"%s\"} %lu\n"
                      "ultrabalancer_frontend_bytes_in_total{proxy=\"%s\"} %lu\n"
                      "ultrabalancer_frontend_bytes_out_total{proxy=\"%s\"} %lu\n"
                      "ultrabalancer_frontend_denied_requests_total{proxy=\"%s\"} %lu\n",
                      px->id, atomic_load(&px->fe_counters[5]),
                      px->id, atomic_load(&px->fe_counters[11]),
                      px->id, atomic_load(&px->fe_counters[12]),
                      px->id, atomic_load(&px->fe_counters[13]));

        buffer_put(&res->buf, buf, len);

        // Server metrics
        server_t *srv;
        for (srv = px->servers; srv; srv = srv->next) {
            len = snprintf(buf, sizeof(buf),
                          "ultrabalancer_server_up{proxy=\"%s\",server=\"%s\"} %d\n"
                          "ultrabalancer_server_current_sessions{proxy=\"%s\",server=\"%s\"} %d\n"
                          "ultrabalancer_server_total_sessions{proxy=\"%s\",server=\"%s\"} %lu\n"
                          "ultrabalancer_server_weight{proxy=\"%s\",server=\"%s\"} %u\n",
                          px->id, srv->id, (srv->cur_state == SRV_RUNNING) ? 1 : 0,
                          px->id, srv->id, atomic_load(&srv->cur_conns),
                          px->id, srv->id, atomic_load(&srv->cum_conns),
                          px->id, srv->id, srv->weight);

            buffer_put(&res->buf, buf, len);
        }
    }

    return 0;
}

// HTML stats page
int stats_dump_html_to_buffer(struct stream *s, struct channel *res) {
    const char *html_header =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>UltraBalancer Stats</title>\n"
        "<meta http-equiv=\"refresh\" content=\"10\">\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; margin: 20px; }\n"
        "h1 { color: #333; }\n"
        "table { border-collapse: collapse; width: 100%; margin-bottom: 20px; }\n"
        "th { background-color: #4CAF50; color: white; padding: 8px; text-align: left; }\n"
        "td { padding: 8px; border-bottom: 1px solid #ddd; }\n"
        "tr:hover { background-color: #f5f5f5; }\n"
        ".status-up { color: green; font-weight: bold; }\n"
        ".status-down { color: red; font-weight: bold; }\n"
        ".status-drain { color: orange; font-weight: bold; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>UltraBalancer Statistics</h1>\n";

    buffer_put(&res->buf, html_header, strlen(html_header));

    // Add proxy tables
    struct proxy *px;
    for (px = proxies_list; px; px = px->next) {
        char buf[2048];
        int len = snprintf(buf, sizeof(buf),
                          "<h2>Proxy: %s (%s)</h2>\n"
                          "<table>\n"
                          "<tr><th>Server</th><th>Status</th><th>Weight</th>"
                          "<th>Active</th><th>Total</th><th>Failed</th>"
                          "<th>Response Time</th></tr>\n",
                          px->id,
                          px->mode == PR_MODE_TCP ? "TCP" : "HTTP");

        buffer_put(&res->buf, buf, len);

        // Add server rows
        server_t *srv;
        for (srv = px->servers; srv; srv = srv->next) {
            const char *status_class = "status-down";
            const char *status_text = "DOWN";

            switch (srv->cur_state) {
                case SRV_RUNNING:
                    status_class = "status-up";
                    status_text = "UP";
                    break;
                case SRV_DRAIN:
                    status_class = "status-drain";
                    status_text = "DRAIN";
                    break;
                case SRV_MAINTAIN:
                    status_text = "MAINT";
                    break;
            }

            len = snprintf(buf, sizeof(buf),
                          "<tr>"
                          "<td>%s:%d</td>"
                          "<td class=\"%s\">%s</td>"
                          "<td>%u</td>"
                          "<td>%d</td>"
                          "<td>%lu</td>"
                          "<td>%u</td>"
                          "<td>%u ms</td>"
                          "</tr>\n",
                          srv->hostname, srv->port,
                          status_class, status_text,
                          srv->weight,
                          atomic_load(&srv->cur_conns),
                          atomic_load(&srv->cum_conns),
                          srv->consecutive_errors,
                          srv->check ? srv->check->duration : 0);

            buffer_put(&res->buf, buf, len);
        }

        buffer_put(&res->buf, "</table>\n", 9);
    }

    const char *html_footer = "</body>\n</html>\n";
    buffer_put(&res->buf, html_footer, strlen(html_footer));

    return 0;
}

void stats_init() {
    log_info("Statistics module initialized");
}