# RDMA 全量同步验收

运行位置：Linux 的 `91kvstore` 仓库根目录。

```sh
make test-rdma
# 多个 iWARP IPv4 地址时显式选择：
RDMA_TEST_IP=172.16.145.135 make test-rdma
# 完整回归也包含这组真实 RDMA 验收：
PRIMARY_PORT=29200 REPLICA_PORT=29201 make test
```

`test-rdma` 调用 `tests/test_rdma_replication.py`，完整回归直接依赖
该目标。完整回归中的其他测试组仍各自验证原有行为，不能理解为所有
测试都使用 RDMA。

## 运行条件

需要 ACTIVE 的 iWARP 设备（例如 siw0）、网卡的全局 IPv4 地址，以及
`rdma`、`ibv_devinfo`、`ip`、`stdbuf` 命令。脚本只查询设备，不配置网络。
没有可用设备会报错，不会回退到 TCP 全量，也不会跳过后显示通过。

Primary 和 Replica 在同一台 Linux 上通过实际 iWARP 设备通信。
因此普通 `make test` 不需要 SSH 到另一台虚拟机。这组测试验证真实
RDMA 路径；跨机器网络连通性仍由双机验收验证。

## 代码结构

| 代码 | 职责 |
| --- | --- |
| `discover_rdma_host` | 查询 ACTIVE iWARP 设备和 IPv4，校验可选环境变量 |
| `Client` | 编解码测试使用的 RESP 命令和响应 |
| `Fixture` | 创建临时配置、启动服务、等待状态、清理自己启动的进程 |
| `SnapshotGate` | 代理 TCP，暂停全量启动通知；不代理 RDMA 快照数据 |
| `assert_rdma_snapshot` | 检查分块、累计 ACK、快照字节数及 SHA-256、正常断开 |
| `assert_tcp_catchup_only` | 检查全量通知后 TCP 只有 AOF 增量与 ONLINE 行 |
| 各个 `*_case` | 按业务场景组织操作与断言 |

## 五个场景

1. **空数据集**：小快照正常结束，ONLINE 后插入、修改、删除继续复制，
   Primary 流水线响应顺序正确。
2. **恰好 1 MiB**：最后一个块正好填满缓冲区，不等待不存在的下一块。
3. **32 MiB 数据追赶**：校验多块及不足 1 MiB 的尾块，并逐个读取
   512 个原有键和 17 个增量键；非 ONLINE 时 PING 可用、读取返回 LOADING。
4. **Replica 重启**：停止副本后破坏其测试目录中的旧快照和 AOF，更新
   Primary，再从同一目录启动副本，确认重新全量同步得到最新数据。
5. **Primary 本地恢复**：保存二进制快照后再写入 AOF，重启后同时恢复
   两部分数据，且恢复过程不改写 AOF。

第三个场景的顺序是：

```text
Primary 生成快照、固定 O
    → TCP 代理暂停 FULLRESYNC_RDMA 通知
    → Primary 写入 17 条增量
    → 放行通知，Replica 建立真实 RDMA 连接
    → 分块接收快照、安装、累计 ACK、正常断开
    → TCP 从 O 开始发送 AOF，通知 ONLINE
    → 核对全部数据及传输证据
```

暂停通知可确定性地安排增量发生在 O 之后；不依赖固定休眠猜测时机。
TCP 代理还统计通知之后的字节数，要求恰好等于
`ONLINE offset - O + ONLINE 行长度`，防止快照混入 TCP 路径而测试仍通过。

每个场景使用独立临时目录和端口。成功时删除测试目录；失败时保留
配置、快照、AOF 和日志，并打印路径。退出时只终止本脚本启动的进程。


## 全量路径迁移完成后的约定

Primary 必须显式配置 `rdma_port`，且与 `service_port` 不同。
只生成、接受 `FULLRESYNC_RDMA` 通知；全量期间的 TCP 数据被拒绝。
快照的生成、校验、安装及边界 O 继续复用现有 store/io 接口。
`legacy_hybrid` 仍是当前主从持久化配置名，数据使用二进制快照与 AOF，
不代表恢复旧的文本快照实现。eBPF 增量不属于此次迁移。

`test_replication_boundary.c` 使用仅存在于单测中的传输替身，无需设备，
检查控制行边界、旧通知拒绝、全量期间 TCP 数据拒绝、RECV 早于 SEND
完成、过早断开和超出剩余长度的快照块。它不验证真实 RDMA；真实收发由
`test-rdma` 和 `test_replication.sh` 验收，二者都必须有可用 iWARP 设备。

可选的 `run_two_vm_acceptance.py` 接受 `--rdma-port`（默认 29747）；
`demo_two_vm.py` 的个人 JSON 配置也可增加 `"rdma_port": 29747`。
双机脚本暂停的是 RDMA 启动通知，快照不会经过 TCP 代理。
