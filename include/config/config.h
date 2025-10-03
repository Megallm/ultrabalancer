#ifndef CONFIG_CONFIG_H
#define CONFIG_CONFIG_H

#include <stdint.h>

struct proxy;
struct server;

typedef struct config_section {
    const char *name;
    int (*parse)(const char **args, int line);
} config_section_t;

int config_parse_file(const char *filename);
int config_parse_line(char *line, int line_num);
int config_check_global();
int config_check_proxy(struct proxy *px);
int config_check();

// Helper functions
struct proxy* proxy_find_by_name(const char *name);
int parse_stick_table(struct proxy *px, const char **args);
int parse_stick_rule(struct proxy *px, const char **args);

void config_init();

#endif