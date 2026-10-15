#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 字段编号同时用作 seen[] 和 field_lines[] 的下标。
 * 新增配置字段时，记得同步更新 field_names[] 和 parse_field()。
 */
enum config_field {
    FIELD_ROLE,
    FIELD_BIND_IP,
    FIELD_SERVICE_PORT,
    FIELD_LOG_LEVEL,
    FIELD_PERSISTENCE_MODE,
    FIELD_REPLICATION_PORT,
    FIELD_PRIMARY_HOST,
    FIELD_PRIMARY_PORT,
    FIELD_COUNT
};

static const char *const field_names[FIELD_COUNT] = {
    "role",
    "bind_ip",
    "service_port",
    "log_level",
    "persistence_mode",
    "replication_port",
    "primary_host",
    "primary_port"
};

/* 把错误位置和原因统一写入调用方提供的缓冲区。 */
static int config_error(
    char *error,
    size_t error_capacity,
    const char *path,
    size_t line_number,
    const char *format,
    ...
)
{
    char detail[256];
    va_list args;

    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);

    if (line_number == 0) {
        snprintf(error, error_capacity, "%s: %s", path, detail);
    } else {
        snprintf(
            error,
            error_capacity,
            "%s:%zu: %s",
            path,
            line_number,
            detail
        );
    }

    return -1;
}

/* 原地去掉字符串首尾的空白字符，不另外分配内存。 */
static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        ++text;
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';

    return text;
}

static int find_field(const char *name)
{
    int field;

    for (field = 0; field < FIELD_COUNT; ++field) {
        if (strcmp(name, field_names[field]) == 0) {
            return field;
        }
    }

    return -1;
}

/*
 * 只接受十进制数字和 1～65535。
 * 先检查每个字符，可以拒绝负数、正号以及 "123abc"。
 */
static int parse_port(const char *text, unsigned short *port)
{
    const unsigned char *cursor = (const unsigned char *)text;
    char *end;
    long number;

    if (*cursor == '\0') {
        return -1;
    }

    while (*cursor != '\0') {
        if (!isdigit(*cursor)) {
            return -1;
        }
        ++cursor;
    }

    errno = 0;
    number = strtol(text, &end, 10);
    if (errno == ERANGE || *end != '\0' ||
        number < 1 || number > 65535) {
        return -1;
    }

    *port = (unsigned short)number;
    return 0;
}

/* 当前配置契约只接受 IPv4 字面量，不进行 DNS 查询。 */
static int valid_ipv4(const char *text)
{
    struct in_addr address;

    return inet_pton(AF_INET, text, &address) == 1;
}

static int parse_field(
    int field,
    const char *value,
    kvs_config_t *parsed,
    const char *path,
    size_t line_number,
    char *error,
    size_t error_capacity
)
{
    switch (field) {
    case FIELD_ROLE:
        if (strcmp(value, "standalone") == 0) {
            parsed->role = KVS_ROLE_STANDALONE;
        } else if (strcmp(value, "primary") == 0) {
            parsed->role = KVS_ROLE_PRIMARY;
        } else if (strcmp(value, "replica") == 0) {
            parsed->role = KVS_ROLE_REPLICA;
        } else {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid role '%s'; expected standalone, primary or replica",
                value
            );
        }
        return 0;

    case FIELD_BIND_IP:
        if (!valid_ipv4(value)) {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid bind_ip '%s'; expected an IPv4 address",
                value
            );
        }
        snprintf(parsed->bind_ip, sizeof(parsed->bind_ip), "%s", value);
        return 0;

    case FIELD_SERVICE_PORT:
        if (parse_port(value, &parsed->service_port) < 0) {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid service_port '%s'; expected 1-65535",
                value
            );
        }
        return 0;

    case FIELD_LOG_LEVEL:
        if (strcmp(value, "error") == 0) {
            parsed->log_level = KVS_LOG_ERROR;
        } else if (strcmp(value, "info") == 0) {
            parsed->log_level = KVS_LOG_INFO;
        } else if (strcmp(value, "debug") == 0) {
            parsed->log_level = KVS_LOG_DEBUG;
        } else {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid log_level '%s'; expected error, info or debug",
                value
            );
        }
        return 0;

    case FIELD_PERSISTENCE_MODE:
        if (strcmp(value, "legacy_hybrid") == 0) {
            parsed->persistence_mode = KVS_PERSISTENCE_LEGACY_HYBRID;
        } else if (strcmp(value, "snapshot") == 0) {
            parsed->persistence_mode = KVS_PERSISTENCE_SNAPSHOT;
        } else if (strcmp(value, "aof") == 0) {
            parsed->persistence_mode = KVS_PERSISTENCE_AOF;
        } else {
            return config_error(
                error, error_capacity, path, line_number,
                "unsupported persistence_mode '%s'; "
                "expected legacy_hybrid, snapshot or aof",
                value
            );
        }
        return 0;

    case FIELD_REPLICATION_PORT:
        if (parse_port(value, &parsed->replication_port) < 0) {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid replication_port '%s'; expected 1-65535",
                value
            );
        }
        return 0;

    case FIELD_PRIMARY_HOST:
        if (!valid_ipv4(value) || strcmp(value, "0.0.0.0") == 0) {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid primary_host '%s'; expected a connectable "
                "IPv4 address",
                value
            );
        }
        snprintf(
            parsed->primary_host,
            sizeof(parsed->primary_host),
            "%s",
            value
        );
        return 0;

    case FIELD_PRIMARY_PORT:
        if (parse_port(value, &parsed->primary_port) < 0) {
            return config_error(
                error, error_capacity, path, line_number,
                "invalid primary_port '%s'; expected 1-65535",
                value
            );
        }
        return 0;

    default:
        return config_error(
            error, error_capacity, path, line_number,
            "internal error: unknown field number %d",
            field
        );
    }
}

/*
 * 字段间的约束需要等整个文件读完才能判断，因为字段顺序是任意的。
 */
static int validate_config(
    const kvs_config_t *parsed,
    const bool seen[FIELD_COUNT],
    const size_t field_lines[FIELD_COUNT],
    const char *path,
    char *error,
    size_t error_capacity
)
{
    if (!seen[FIELD_ROLE]) {
        return config_error(
            error, error_capacity, path, 0,
            "missing required field 'role'"
        );
    }

    if (!seen[FIELD_SERVICE_PORT]) {
        return config_error(
            error, error_capacity, path, 0,
            "missing required field 'service_port'"
        );
    }

    switch (parsed->role) {
    case KVS_ROLE_STANDALONE:
        if (seen[FIELD_REPLICATION_PORT]) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_REPLICATION_PORT],
                "replication_port is only valid for role=primary"
            );
        }
        if (seen[FIELD_PRIMARY_HOST]) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_PRIMARY_HOST],
                "primary_host is only valid for role=replica"
            );
        }
        if (seen[FIELD_PRIMARY_PORT]) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_PRIMARY_PORT],
                "primary_port is only valid for role=replica"
            );
        }
        break;

    case KVS_ROLE_PRIMARY:
        if (!seen[FIELD_REPLICATION_PORT]) {
            return config_error(
                error, error_capacity, path, 0,
                "role=primary requires replication_port"
            );
        }
        if (parsed->service_port == parsed->replication_port) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_REPLICATION_PORT],
                "service_port and replication_port must differ"
            );
        }
        if (seen[FIELD_PRIMARY_HOST]) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_PRIMARY_HOST],
                "primary_host is only valid for role=replica"
            );
        }
        if (seen[FIELD_PRIMARY_PORT]) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_PRIMARY_PORT],
                "primary_port is only valid for role=replica"
            );
        }
        break;

    case KVS_ROLE_REPLICA:
        if (!seen[FIELD_PRIMARY_HOST]) {
            return config_error(
                error, error_capacity, path, 0,
                "role=replica requires primary_host"
            );
        }
        if (!seen[FIELD_PRIMARY_PORT]) {
            return config_error(
                error, error_capacity, path, 0,
                "role=replica requires primary_port"
            );
        }
        if (seen[FIELD_REPLICATION_PORT]) {
            return config_error(
                error, error_capacity, path,
                field_lines[FIELD_REPLICATION_PORT],
                "replication_port is only valid for role=primary; "
                "use primary_port for role=replica"
            );
        }
        break;

    default:
        return config_error(
            error, error_capacity, path, 0,
            "internal error: invalid role"
        );
    }
    bool is_replication_role =
        parsed->role == KVS_ROLE_PRIMARY ||
        parsed->role == KVS_ROLE_REPLICA;

    if (is_replication_role &&
        parsed->persistence_mode != KVS_PERSISTENCE_LEGACY_HYBRID) {
        return config_error(
            error, error_capacity, path,
            field_lines[FIELD_PERSISTENCE_MODE],
            "primary/replica currently support only "
            "persistence_mode=legacy_hybrid"
        );
    }

    return 0;
}

int kvs_config_load(
    const char *path,
    kvs_config_t *config,
    char *error,
    size_t error_capacity
)
{
    FILE *file;
    kvs_config_t parsed = {0};
    bool seen[FIELD_COUNT] = {false};
    size_t field_lines[FIELD_COUNT] = {0};
    char *line = NULL;
    size_t line_capacity = 0;
    size_t line_number = 0;
    int result = -1;

    if (error == NULL || error_capacity == 0) {
        return -1;
    }
    error[0] = '\0';

    if (path == NULL || path[0] == '\0' || config == NULL) {
        return config_error(
            error, error_capacity, "config", 0,
            "path and output config must not be empty"
        );
    }

    /* 默认值只放在临时对象中；解析失败不会改动调用方的 config。 */
    snprintf(parsed.bind_ip, sizeof(parsed.bind_ip), "%s", "0.0.0.0");
    parsed.log_level = KVS_LOG_INFO;
    parsed.persistence_mode = KVS_PERSISTENCE_LEGACY_HYBRID;

    file = fopen(path, "r");
    if (file == NULL) {
        return config_error(
            error, error_capacity, path, 0,
            "cannot open file: %s",
            strerror(errno)
        );
    }

    while (getline(&line, &line_capacity, file) != -1) {
        char *entry;
        char *equal;
        char *key;
        char *value;
        int field;

        ++line_number;
        entry = trim(line);

        /* 只支持整行注释；行尾注释不属于当前配置格式。 */
        if (*entry == '\0' || *entry == '#') {
            continue;
        }

        equal = strchr(entry, '=');
        if (equal == NULL) {
            config_error(
                error, error_capacity, path, line_number,
                "expected 'key = value'"
            );
            goto done;
        }

        *equal = '\0';
        key = trim(entry);
        value = trim(equal + 1);

        if (*key == '\0' || *value == '\0' || strchr(value, '=') != NULL) {
            config_error(
                error, error_capacity, path, line_number,
                "expected one non-empty 'key = value' pair"
            );
            goto done;
        }

        field = find_field(key);
        if (field < 0) {
            config_error(
                error, error_capacity, path, line_number,
                "unknown field '%s'",
                key
            );
            goto done;
        }

        if (seen[field]) {
            config_error(
                error, error_capacity, path, line_number,
                "duplicate field '%s'; first defined on line %zu",
                key,
                field_lines[field]
            );
            goto done;
        }

        if (parse_field(
                field, value, &parsed, path, line_number,
                error, error_capacity
            ) < 0) {
            goto done;
        }

        seen[field] = true;
        field_lines[field] = line_number;
    }

    if (ferror(file)) {
        config_error(
            error, error_capacity, path, line_number,
            "failed while reading file: %s",
            strerror(errno)
        );
        goto done;
    }

    if (validate_config(
            &parsed, seen, field_lines, path, error, error_capacity
        ) < 0) {
        goto done;
    }
    result = 0;

done:
    free(line);

    if (fclose(file) != 0 && result == 0) {
        return config_error(
            error, error_capacity, path, 0,
            "failed to close file: %s",
            strerror(errno)
        );
    }

    if (result == 0) {
        *config = parsed;
    }
    return result;
}