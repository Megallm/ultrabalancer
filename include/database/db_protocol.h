#ifndef DB_PROTOCOL_H
#define DB_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DB_PROTOCOL_UNKNOWN = 0,
    DB_PROTOCOL_POSTGRESQL,
    DB_PROTOCOL_MYSQL,
    DB_PROTOCOL_REDIS
} db_protocol_type_t;

typedef enum {
    DB_QUERY_UNKNOWN = 0,
    DB_QUERY_READ,
    DB_QUERY_WRITE,
    DB_QUERY_TRANSACTION_BEGIN,
    DB_QUERY_TRANSACTION_END,
    DB_QUERY_SESSION_VAR
} db_query_type_t;

typedef struct {
    db_protocol_type_t protocol;
    db_query_type_t query_type;
    const char* query_text;
    size_t query_length;
    bool is_transaction;
    bool requires_sticky;
    uint32_t session_id;
} db_query_info_t;

typedef struct {
    db_protocol_type_t type;
    const uint8_t* data;
    size_t length;
    void* parsed_data;
} db_packet_t;

db_protocol_type_t db_protocol_detect(const uint8_t* data, size_t length);

int db_protocol_parse_postgresql(const uint8_t* data, size_t length, db_query_info_t* info);
int db_protocol_parse_mysql(const uint8_t* data, size_t length, db_query_info_t* info);
int db_protocol_parse_redis(const uint8_t* data, size_t length, db_query_info_t* info);

db_query_type_t db_protocol_classify_query(const char* query, size_t length);

bool db_protocol_is_handshake(const uint8_t* data, size_t length, db_protocol_type_t protocol);

#endif
