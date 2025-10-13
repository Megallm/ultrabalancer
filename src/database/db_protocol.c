#include "database/db_protocol.h"
#include <string.h>
#include <ctype.h>

db_protocol_type_t db_protocol_detect(const uint8_t* data, size_t length) {
    if (length < 4) return DB_PROTOCOL_UNKNOWN;

    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00) {
        return DB_PROTOCOL_POSTGRESQL;
    }

    if (length >= 5 && data[0] < 0x20 && (data[4] == 0x0a || data[4] == 0x09)) {
        return DB_PROTOCOL_MYSQL;
    }

    if (data[0] == '*' || data[0] == '+' || data[0] == '-' ||
        data[0] == ':' || data[0] == '$') {
        return DB_PROTOCOL_REDIS;
    }

    return DB_PROTOCOL_UNKNOWN;
}

static bool is_select_query(const char* query, size_t length) {
    if (length < 6) return false;
    const char* p = query;
    while (p < query + length && isspace(*p)) p++;
    return (length >= 6 &&
            (p[0] == 'S' || p[0] == 's') &&
            (p[1] == 'E' || p[1] == 'e') &&
            (p[2] == 'L' || p[2] == 'l') &&
            (p[3] == 'E' || p[3] == 'e') &&
            (p[4] == 'C' || p[4] == 'c') &&
            (p[5] == 'T' || p[5] == 't'));
}

static bool is_show_query(const char* query, size_t length) {
    if (length < 4) return false;
    const char* p = query;
    while (p < query + length && isspace(*p)) p++;
    return (length >= 4 &&
            (p[0] == 'S' || p[0] == 's') &&
            (p[1] == 'H' || p[1] == 'h') &&
            (p[2] == 'O' || p[2] == 'o') &&
            (p[3] == 'W' || p[3] == 'w'));
}

static bool is_write_query(const char* query, size_t length) {
    if (length < 6) return false;
    const char* p = query;
    while (p < query + length && isspace(*p)) p++;

    if (length >= 6) {
        if ((p[0] == 'I' || p[0] == 'i') &&
            (p[1] == 'N' || p[1] == 'n') &&
            (p[2] == 'S' || p[2] == 's') &&
            (p[3] == 'E' || p[3] == 'e') &&
            (p[4] == 'R' || p[4] == 'r') &&
            (p[5] == 'T' || p[5] == 't')) return true;

        if ((p[0] == 'U' || p[0] == 'u') &&
            (p[1] == 'P' || p[1] == 'p') &&
            (p[2] == 'D' || p[2] == 'd') &&
            (p[3] == 'A' || p[3] == 'a') &&
            (p[4] == 'T' || p[4] == 't') &&
            (p[5] == 'E' || p[5] == 'e')) return true;

        if ((p[0] == 'D' || p[0] == 'd') &&
            (p[1] == 'E' || p[1] == 'e') &&
            (p[2] == 'L' || p[2] == 'l') &&
            (p[3] == 'E' || p[3] == 'e') &&
            (p[4] == 'T' || p[4] == 't') &&
            (p[5] == 'E' || p[5] == 'e')) return true;
    }

    return false;
}

static bool is_transaction_begin(const char* query, size_t length) {
    if (length < 5) return false;
    const char* p = query;
    while (p < query + length && isspace(*p)) p++;

    if (length >= 5 &&
        (p[0] == 'B' || p[0] == 'b') &&
        (p[1] == 'E' || p[1] == 'e') &&
        (p[2] == 'G' || p[2] == 'g') &&
        (p[3] == 'I' || p[3] == 'i') &&
        (p[4] == 'N' || p[4] == 'n')) return true;

    if (length >= 5 &&
        (p[0] == 'S' || p[0] == 's') &&
        (p[1] == 'T' || p[1] == 't') &&
        (p[2] == 'A' || p[2] == 'a') &&
        (p[3] == 'R' || p[3] == 'r') &&
        (p[4] == 'T' || p[4] == 't')) return true;

    return false;
}

static bool is_transaction_end(const char* query, size_t length) {
    if (length < 6) return false;
    const char* p = query;
    while (p < query + length && isspace(*p)) p++;

    if (length >= 6 &&
        (p[0] == 'C' || p[0] == 'c') &&
        (p[1] == 'O' || p[1] == 'o') &&
        (p[2] == 'M' || p[2] == 'm') &&
        (p[3] == 'M' || p[3] == 'm') &&
        (p[4] == 'I' || p[4] == 'i') &&
        (p[5] == 'T' || p[5] == 't')) return true;

    if (length >= 8 &&
        (p[0] == 'R' || p[0] == 'r') &&
        (p[1] == 'O' || p[1] == 'o') &&
        (p[2] == 'L' || p[2] == 'l') &&
        (p[3] == 'L' || p[3] == 'l') &&
        (p[4] == 'B' || p[4] == 'b') &&
        (p[5] == 'A' || p[5] == 'a') &&
        (p[6] == 'C' || p[6] == 'c') &&
        (p[7] == 'K' || p[7] == 'k')) return true;

    return false;
}

static bool is_session_var(const char* query, size_t length) {
    if (length < 3) return false;
    const char* p = query;
    while (p < query + length && isspace(*p)) p++;
    return (length >= 3 &&
            (p[0] == 'S' || p[0] == 's') &&
            (p[1] == 'E' || p[1] == 'e') &&
            (p[2] == 'T' || p[2] == 't'));
}

db_query_type_t db_protocol_classify_query(const char* query, size_t length) {
    if (!query || length == 0) return DB_QUERY_UNKNOWN;

    if (is_transaction_begin(query, length)) return DB_QUERY_TRANSACTION_BEGIN;
    if (is_transaction_end(query, length)) return DB_QUERY_TRANSACTION_END;
    if (is_session_var(query, length)) return DB_QUERY_SESSION_VAR;
    if (is_select_query(query, length) || is_show_query(query, length)) return DB_QUERY_READ;
    if (is_write_query(query, length)) return DB_QUERY_WRITE;

    return DB_QUERY_UNKNOWN;
}

int db_protocol_parse_postgresql(const uint8_t* data, size_t length, db_query_info_t* info) {
    if (!data || !info || length < 5) return -1;

    info->protocol = DB_PROTOCOL_POSTGRESQL;
    info->is_transaction = false;
    info->requires_sticky = false;

    char message_type = data[0];
    uint32_t message_length = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4];

    if (message_type == 'Q' && length >= message_length + 1) {
        info->query_text = (const char*)(data + 5);
        info->query_length = message_length - 4;
        info->query_type = db_protocol_classify_query(info->query_text, info->query_length);

        if (info->query_type == DB_QUERY_TRANSACTION_BEGIN ||
            info->query_type == DB_QUERY_SESSION_VAR) {
            info->requires_sticky = true;
        }

        return 0;
    }

    info->query_type = DB_QUERY_UNKNOWN;
    return 0;
}

int db_protocol_parse_mysql(const uint8_t* data, size_t length, db_query_info_t* info) {
    if (!data || !info || length < 5) return -1;

    info->protocol = DB_PROTOCOL_MYSQL;
    info->is_transaction = false;
    info->requires_sticky = false;

    uint32_t packet_length = data[0] | (data[1] << 8) | (data[2] << 16);
    uint8_t command = data[4];

    if (command == 0x03 && length >= packet_length + 4) {
        info->query_text = (const char*)(data + 5);
        info->query_length = packet_length - 1;
        info->query_type = db_protocol_classify_query(info->query_text, info->query_length);

        if (info->query_type == DB_QUERY_TRANSACTION_BEGIN ||
            info->query_type == DB_QUERY_SESSION_VAR) {
            info->requires_sticky = true;
        }

        return 0;
    }

    info->query_type = DB_QUERY_UNKNOWN;
    return 0;
}

int db_protocol_parse_redis(const uint8_t* data, size_t length, db_query_info_t* info) {
    if (!data || !info || length < 3) return -1;

    info->protocol = DB_PROTOCOL_REDIS;
    info->is_transaction = false;
    info->requires_sticky = false;
    info->query_type = DB_QUERY_READ;

    if (data[0] == '*') {
        const uint8_t* cmd_start = data;
        size_t i = 0;
        while (i < length && data[i] != '\n') i++;
        if (i < length) {
            i++;
            while (i < length && data[i] != '\n') i++;
            if (i < length) {
                i++;
                cmd_start = data + i;
                size_t cmd_len = 0;
                while (i + cmd_len < length && data[i + cmd_len] != '\r') cmd_len++;

                if (cmd_len >= 3) {
                    const char* cmd = (const char*)cmd_start;
                    if ((cmd[0] == 'S' || cmd[0] == 's') &&
                        (cmd[1] == 'E' || cmd[1] == 'e') &&
                        (cmd[2] == 'T' || cmd[2] == 't')) {
                        info->query_type = DB_QUERY_WRITE;
                    } else if ((cmd[0] == 'G' || cmd[0] == 'g') &&
                               (cmd[1] == 'E' || cmd[1] == 'e') &&
                               (cmd[2] == 'T' || cmd[2] == 't')) {
                        info->query_type = DB_QUERY_READ;
                    } else if (cmd_len >= 4 &&
                               (cmd[0] == 'M' || cmd[0] == 'm') &&
                               (cmd[3] == 'T' || cmd[3] == 't')) {
                        info->query_type = DB_QUERY_TRANSACTION_BEGIN;
                        info->requires_sticky = true;
                    } else if (cmd_len >= 4 &&
                               (cmd[0] == 'E' || cmd[0] == 'e') &&
                               (cmd[1] == 'X' || cmd[1] == 'x') &&
                               (cmd[2] == 'E' || cmd[2] == 'e') &&
                               (cmd[3] == 'C' || cmd[3] == 'c')) {
                        info->query_type = DB_QUERY_TRANSACTION_END;
                    }
                }
            }
        }
    }

    return 0;
}

bool db_protocol_is_handshake(const uint8_t* data, size_t length, db_protocol_type_t protocol) {
    if (!data || length < 8) return false;

    switch (protocol) {
        case DB_PROTOCOL_POSTGRESQL:
            return length >= 8 &&
                   data[0] == 0x00 && data[1] == 0x00 &&
                   data[2] == 0x00 && data[3] >= 0x08;

        case DB_PROTOCOL_MYSQL:
            return length >= 5 && data[3] == 0x00 && data[4] == 0x0a;

        case DB_PROTOCOL_REDIS:
            return false;

        default:
            return false;
    }
}
