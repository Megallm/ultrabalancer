#include "core/common.h"
#include <time.h>

struct global global = {
    .maxconn = 100000,
    .nbproc = 1,
    .nbthread = 8,
    .daemon = 0,
    .debug = 0
};

time_t start_time;
volatile unsigned int now_ms = 0;
uint32_t total_connections = 0;
