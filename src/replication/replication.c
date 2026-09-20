#include "replication.h"
#include "persistence.h"
typedef struct {
    kvs_role_t role;               // 当前进程是单机、Primary 还是 Replica
    long long replication_offset;  // 当前复制进度
    int initialized;               // 是否完成初始化
} kvs_replication_manager_t;

static kvs_replication_manager_t replication_manager = {
    .role = KVS_ROLE_STANDALONE,
    .replication_offset = 0,
    .initialized = 0
};

int kvs_replication_init(kvs_role_t role){
    if(replication_manager.initialized){
        return -1;
    }

    if (role != KVS_ROLE_STANDALONE &&
    role != KVS_ROLE_PRIMARY &&
    role != KVS_ROLE_REPLICA) {
    return -1;
    }

    long long offset =kvs_aof_get_offset();
    if(offset<0){
        return -1;
    }
    replication_manager.role = role;
    replication_manager.replication_offset = offset;
    replication_manager.initialized = 1;

    return 0;
}

void kvs_replication_destroy(void)
{
    if (!replication_manager.initialized) {
        return;
    }

    replication_manager.role = KVS_ROLE_STANDALONE;
    replication_manager.replication_offset = 0;
    replication_manager.initialized = 0;
}
