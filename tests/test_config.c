#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 用独立的 /tmp 文件测试真实的“从路径读取配置”流程。
 * 成功后删除；测试失败时保留文件，方便查看输入。
 */
static void test_valid_standalone(void)
{
    static const char contents[] =
        "# 单机配置；字段顺序不固定\n"
        "\n"
        " log_level = debug \n"
        " service_port = 29000 \n"
        " persistence_mode = legacy_hybrid \n"
        " bind_ip = 127.0.0.1 \n"
        " role = standalone \n";

    char path[] = "/tmp/91kv-config-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);

    kvs_config_t config = {0};
    char error[256] = {0};

    int result = kvs_config_load(path, &config, error, sizeof(error));
    if (result != 0) {
        fprintf(stderr, "config test input kept at %s: %s\n", path, error);
    }
    assert(result == 0);

    assert(config.role == KVS_ROLE_STANDALONE);
    assert(strcmp(config.bind_ip, "127.0.0.1") == 0);
    assert(config.service_port == 29000);
    assert(config.log_level == KVS_LOG_DEBUG);
    assert(config.persistence_mode == KVS_PERSISTENCE_LEGACY_HYBRID);

    /* 单机角色不应凭空产生主从端口或上游地址。 */
    assert(config.replication_port == 0);
    assert(config.primary_host[0] == '\0');
    assert(config.primary_port == 0);

    assert(unlink(path) == 0);
}

/*
* 每个用例都写入自己的临时文件，走真实的文件解析流程。
* 预期失败时检查：返回非零、错误包含文件路径和原因、输出对象没有被改一半。
*/
static void expect_invalid(
    const char *name,
    const char *contents,
    const char *expected_error
)
{
    char path[] = "/tmp/91kv-config-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);

    kvs_config_t config = {0};
    config.service_port = 4242; /* 哨兵值：失败后不应被覆盖。 */
    char error[256] = {0};

    int result = kvs_config_load(path, &config, error, sizeof(error));
    if (result == 0 ||
        strstr(error, path) == NULL ||
        strstr(error, expected_error) == NULL ||
        config.service_port != 4242) {
        fprintf(
            stderr,
            "[FAIL] %s: result=%d, error='%s', service_port=%u\n"
            "       input kept at %s\n",
            name,
            result,
            error,
            config.service_port,
            path
        );
        assert(0);
    }

    assert(unlink(path) == 0);
}

static void test_invalid_configs(void)
{
    struct invalid_case {
        const char *name;
        const char *contents;
        const char *expected_error;
    };

    static const struct invalid_case cases[] = {
        {
            "unknown field",
            "role=standalone\nservice_port=29000\nmystery=1\n",
            "unknown field 'mystery'"
        },
        {
            "duplicate field",
            "role=standalone\nservice_port=29000\nrole=primary\n",
            "duplicate field 'role'"
        },
        {
            "missing role",
            "service_port=29000\n",
            "missing required field 'role'"
        },
        {
            "missing service port",
            "role=standalone\n",
            "missing required field 'service_port'"
        },
        {
            "non-numeric port",
            "role=standalone\nservice_port=12abc\n",
            "invalid service_port"
        },
        {
            "zero port",
            "role=standalone\nservice_port=0\n",
            "invalid service_port"
        },
        {
            "port too large",
            "role=standalone\nservice_port=65536\n",
            "invalid service_port"
        },
        {
            "invalid role",
            "role=leader\nservice_port=29000\n",
            "invalid role"
        },
        {
            "invalid log level",
            "role=standalone\nservice_port=29000\nlog_level=trace\n",
            "invalid log_level"
        },
        {
            "unsupported persistence mode",
            "role=standalone\nservice_port=29000\n"
            "persistence_mode=aof_only\n",
            "unsupported persistence_mode"
        },
        {
            "invalid bind IP",
            "role=standalone\nservice_port=29000\nbind_ip=not-an-ip\n",
            "invalid bind_ip"
        },
        {
            "primary rejects old equal replication port",
            "role=primary\nservice_port=29000\n"
            "replication_port=29000\n",
            "replication_port has been removed; use service_port"
        },
        {
            "primary rejects old different replication port",
            "role=primary\nservice_port=29000\n"
            "replication_port=29100\n",
            "replication_port has been removed; use service_port"
        },
        {
            "replica missing primary host",
            "role=replica\nservice_port=29001\nprimary_port=29100\n",
            "requires primary_host"
        },
        {
            "replica missing primary port",
            "role=replica\nservice_port=29001\n"
            "primary_host=127.0.0.1\n",
            "requires primary_port"
        },
        {
            "standalone with replication field",
            "role=standalone\nservice_port=29000\n"
            "replication_port=29100\n",
            "replication_port is only valid"
        },
        {
            "primary rejects snapshot mode",
            "role=primary\nservice_port=29000\n"
            "persistence_mode=snapshot\n",
            "primary/replica currently support only"
        },
        {
            "primary rejects aof mode",
            "role=primary\nservice_port=29000\n"
            "persistence_mode=aof\n",
            "primary/replica currently support only"
        },
        {
            "replica rejects snapshot mode",
            "role=replica\nservice_port=29001\n"
            "primary_host=127.0.0.1\nprimary_port=29100\n"
            "persistence_mode=snapshot\n",
            "primary/replica currently support only"
        },
        {
            "replica rejects aof mode with mode field first",
            "persistence_mode=aof\nrole=replica\n"
            "service_port=29001\nprimary_host=127.0.0.1\n"
            "primary_port=29100\n",
            "primary/replica currently support only"
        },
        {
            "malformed line",
            "role=standalone\nservice_port 29000\n",
            "expected 'key = value'"
        }
    };

    size_t count = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < count; ++i) {
        expect_invalid(
            cases[i].name,
            cases[i].contents,
            cases[i].expected_error
        );
    }

    printf("invalid config cases: %zu passed\n", count);
}
/* 将有效配置写入临时文件，解析成功后删除文件并返回结果。 */
static kvs_config_t load_valid(const char *contents)
{
    char path[] = "/tmp/91kv-config-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);

    kvs_config_t config = {0};
    char error[256] = {0};

    int result = kvs_config_load(path, &config, error, sizeof(error));
    if (result != 0) {
        fprintf(stderr, "[FAIL] %s: %s\n", path, error);
    }
    assert(result == 0);
    assert(unlink(path) == 0);

    return config;
}

static void test_valid_snapshot_mode(void)
{
    kvs_config_t config = load_valid(
        "role=standalone\n"
        "service_port=29000\n"
        "persistence_mode=snapshot\n"
    );

    assert(config.persistence_mode == KVS_PERSISTENCE_SNAPSHOT);
    puts("snapshot mode config test passed");
}

static void test_valid_aof_mode(void)
{
    kvs_config_t config = load_valid(
        "role=standalone\n"
        "service_port=29000\n"
        "persistence_mode=aof\n"
    );

    assert(config.persistence_mode == KVS_PERSISTENCE_AOF);
    puts("aof mode config test passed");
}

static void test_valid_primary_and_replica(void)
{
    kvs_config_t primary = load_valid(
        "role = primary\n"
        "bind_ip = 127.0.0.1\n"
        "service_port = 29000\n"
        "log_level = error\n"
    );

    assert(primary.role == KVS_ROLE_PRIMARY);
    assert(strcmp(primary.bind_ip, "127.0.0.1") == 0);
    assert(primary.service_port == 29000);
    assert(primary.log_level == KVS_LOG_ERROR);
    assert(primary.persistence_mode == KVS_PERSISTENCE_LEGACY_HYBRID);
    kvs_config_t replica = load_valid(
        "role = replica\n"
        "service_port = 29001\n"
        "primary_host = 127.0.0.1\n"
        "primary_port = 29100\n"
    );

    assert(replica.role == KVS_ROLE_REPLICA);
    assert(replica.service_port == 29001);
    assert(strcmp(replica.primary_host, "127.0.0.1") == 0);
    assert(replica.primary_port == 29100);

    /* 省略的可选字段应使用明确的默认值。 */
    assert(strcmp(replica.bind_ip, "0.0.0.0") == 0);
    assert(replica.log_level == KVS_LOG_INFO);
    assert(replica.persistence_mode == KVS_PERSISTENCE_LEGACY_HYBRID);
}

static void test_missing_file(void)
{
    char path[] = "/tmp/91kv-config-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);

    kvs_config_t config = {0};
    char error[256] = {0};

    assert(kvs_config_load(path, &config, error, sizeof(error)) < 0);
    assert(strstr(error, path) != NULL);
    assert(strstr(error, "cannot open file") != NULL);
}
/* make test-config 从项目根目录运行，因此这里读取仓库中的示例文件。 */
static kvs_config_t load_example(const char *path)
{
    kvs_config_t config = {0};
    char error[256] = {0};
    int result = kvs_config_load(path, &config, error, sizeof(error));

    if (result < 0) {
        fprintf(stderr, "[FAIL] example %s: %s\n", path, error);
    }
    assert(result == 0);
    return config;
}

static void test_example_config_files(void)
{
    kvs_config_t standalone = load_example("config/standalone.conf");
    assert(standalone.role == KVS_ROLE_STANDALONE);
    assert(strcmp(standalone.bind_ip, "127.0.0.1") == 0);
    assert(standalone.service_port == 29002);
    assert(standalone.log_level == KVS_LOG_INFO);
    assert(standalone.persistence_mode == KVS_PERSISTENCE_LEGACY_HYBRID);

    kvs_config_t primary = load_example("config/primary.conf");
    assert(primary.role == KVS_ROLE_PRIMARY);
    assert(strcmp(primary.bind_ip, "127.0.0.1") == 0);
    assert(primary.service_port == 29000);
    assert(primary.log_level == KVS_LOG_INFO);
    assert(primary.persistence_mode == KVS_PERSISTENCE_LEGACY_HYBRID);

    kvs_config_t replica = load_example("config/replica.conf");
    assert(replica.role == KVS_ROLE_REPLICA);
    assert(strcmp(replica.bind_ip, "127.0.0.1") == 0);
    assert(replica.service_port == 29001);
    assert(strcmp(replica.primary_host, "127.0.0.1") == 0);
    assert(replica.primary_port == 29000);
    assert(replica.log_level == KVS_LOG_INFO);
    assert(replica.persistence_mode == KVS_PERSISTENCE_LEGACY_HYBRID);

    puts("example config files: 3 passed");
}
static void test_allocator_config(void)
{
    /* 不写 allocator 时，保持当前默认的 pool。 */
    kvs_config_t config = load_valid(
        "role=standalone\n"
        "service_port=29000\n"
    );
    assert(config.allocator_backend == KVS_ALLOCATOR_POOL);

    /* 三个合法名称分别转换为对应枚举。 */
    config = load_valid(
        "role=standalone\n"
        "service_port=29000\n"
        "allocator=system\n"
    );
    assert(config.allocator_backend == KVS_ALLOCATOR_SYSTEM);

    config = load_valid(
        "role=standalone\n"
        "service_port=29000\n"
        "allocator=pool\n"
    );
    assert(config.allocator_backend == KVS_ALLOCATOR_POOL);

    /* jemalloc 仅用于 benchmark 预加载，不是正式配置后端。 */
    expect_invalid(
        "jemalloc is not a service backend",
        "role=standalone\n"
        "service_port=29000\n"
        "allocator=jemalloc\n",
        "invalid allocator"
    );

    /* 未知后端必须拒绝，不能回退到默认值。 */
    expect_invalid(
        "unknown allocator",
        "role=standalone\n"
        "service_port=29000\n"
        "allocator=invalid\n",
        "invalid allocator"
    );

    /* 重复字段必须拒绝，即使两次值相同。 */
    expect_invalid(
        "duplicate allocator",
        "role=standalone\n"
        "service_port=29000\n"
        "allocator=pool\n"
        "allocator=pool\n",
        "duplicate"
    );

    puts("[PASS] allocator config defaults, names and rejection");
}
int main(void)
{
    test_valid_standalone();
    test_valid_primary_and_replica();
    test_invalid_configs();
    test_missing_file();
    test_example_config_files();
    test_valid_snapshot_mode();
    test_valid_aof_mode();
    test_allocator_config();
    puts("config parser tests passed");
    return 0;
}