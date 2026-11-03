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

## 八个集成场景

1. **空数据集**：小快照正常结束，ONLINE 后插入、修改、删除继续复制，
   Primary 流水线响应顺序正确。
2. **恰好 1 MiB**：最后一个块正好填满缓冲区，不等待不存在的下一块。
3. **32 MiB 数据追赶**：校验多块及不足 1 MiB 的尾块，并逐个读取
   512 个原有键和 17 个增量键；非 ONLINE 时 PING 可用、读取返回 LOADING。
4. **Replica 重启**：停止副本后破坏其测试目录中的旧快照和 AOF，更新
   Primary，再从同一目录启动副本，确认重新全量同步得到最新数据。
5. **Primary 本地恢复**：保存二进制快照后再写入 AOF，重启后同时恢复
   两部分数据，且恢复过程不改写 AOF。
6. **全量中途停止再重启**：首块实际落盘后停止 Replica，确认只有不完整
   临时文件、没有正式快照；同目录重启使用新同步 ID，重新全量得到最新数据。
7. **RDMA 监听端口占用**：第二个 Primary 绑定同一 RDMA 端口，要求
   10 秒内非零退出，日志含失败的 IPv4/端口，不能降级启动 TCP 服务。
8. **通知中的 RDMA 端口错误**：分别改为未监听端口（连接拒绝）和普通 TCP
   监听者（不回应 RDMA 握手）。后者要求 3 秒主动建连截止时间生效；
   两者均记录端点并取消同步、删除临时快照，不进入 ONLINE；PING 仍可用。

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


## RDMA 模块架构：按这个顺序阅读代码

```text
kvstore.c：读配置、启动 Primary 监听、提供快照安装回调
  └─ ntyco.c：TCP 连接调度，调用 message_handler / stream_handler
       └─ replication.c：复制状态、O、同步 ID、READY / 累计 ACK
            ├─ rdma_transport.c：CM 建连/断开，CQ 收发完成，PD/QP/MR
            ├─ snapshot_io.c：分块读取、临时文件写入/提交/中止
            └─ 安装回调 → snapshot_store.c：校验并加载二进制快照
```

| 代码入口 | 作用 |
| --- | --- |
| `kvs_replication_start_rdma_listener` | Primary 创建独立 RDMA 监听器 |
| `kvs_replication_frame_protocol` 的 PSYNC 分支 | 固定 O、生成快照及同步 ID、返回 TCP 通知 |
| `kvs_replication_upstream_frame_protocol` | Replica 解析通知、准备临时文件、发起 RDMA 建连 |
| `kvs_replication_stream` | Primary 推进 RDMA，并在正常结束后复用 AOF 追赶 |
| `kvs_replication_upstream_stream` | Replica 推进 RDMA、发送 READY、接收快照、累计 ACK |
| `kvs_replication_receive_snapshot_data` / `finish_full_sync` | 写入、提交、调用安装回调并进入 CATCH_UP |
| `kvs_replication_on_primary_disconnected` | Replica 取消本轮并删除未完成快照 |
| `kvs_replication_on_replica_disconnected` | Primary 释放旧同步任务，允许下一轮 PSYNC |

RDMA 连接对象拥有 CM ID、PD、CQ、QP、MR 和注册缓冲区；复制层只持有
不透明指针，不操作 verbs 资源。传输层不认识快照、AOF 或 O。
复制同步上下文保存传输进度、累计确认位置、发送占用及正常断开标志。
主动建连截止时间属于传输连接对象，ESTABLISHED 后清零；不向复制管理器新增计时字段。
快照模块不依赖 RDMA，仍可独立用于单机持久化。

| 对象 | 拥有的资源/状态 | 谁关闭 |
| --- | --- | --- |
| `kvs_rdma_listener` | CM 事件通道、监听 ID | 复制模块销毁阶段，晚于其接受的连接 |
| `kvs_rdma_connection` | CM ID、PD/CQ/QP、收发 MR；主动侧还拥有自己的 CM 通道 | `kvs_rdma_connection_close` |
| `kvs_replication_rdma_sync_t` | 连接指针、进度、累计 ACK、待完成发送/断开标志 | 复制关闭回调释放连接并复位进度 |
| snapshot reader/writer | 文件 FD、剩余长度；writer 指向临时文件 | 完成时提交，取消时中止并删临时文件 |

当前用的是 RDMA SEND/RECV。单块路径为：文件读入临时块 → 复制到注册发送
缓冲区 → 提交 SEND → 对端 RECV 后写临时文件 → 提交累计 ACK。
因此当前不是端到端零拷贝；每方向只允许一个未完成请求，最多一块快照待确认。
传输层的 `send_pending` 表示底层 WR 占用，复制上下文的同名字段表示上层
还未处理对应完成通知；两层各自维护自己可见的进度。

每轮复制经过 `HANDSHAKE → FULL_SYNC → CATCH_UP → ONLINE`。
CM 的 `ESTABLISHED/DISCONNECTED` 与 CQ 的 `SEND/RECV` 是不同事件；
连接建立不代表数据已传完，SEND 完成也不代表对端已安装快照。
最终累计 ACK 在安装成功之后提交，正常断开后才切回 TCP 追赶。

`stream_handler` 非阻塞地推进一次；返回 `KVS_STREAM_WAIT` 后 NtyCo
约 50ms 后重试，因此没有在回调里忙等整个传输。当前仍是轮询驱动，
并未把所有 CM/CQ 事件 FD 统一接入 epoll；不把它描述为完全事件驱动。

构建通过 `-Wl,--exclude-libs,libntyco.a` 隐藏 NtyCo 静态库的动态导出。
程序自身保留协程 hook，动态链接的 librdmacm 则使用 libc 设备 I/O，避免
错误设备写入被 hook 后等待 socket 就绪。此选项的 ELF 语义见
[GNU ld 说明](https://sourceware.org/binutils/docs/ld/Options.html)。

边界单测在接收一半数据后注入 CM DISCONNECTED，核对未安装、未 ONLINE，
外层关闭回调删除临时文件。它模拟传输事件，不会关闭系统 siw 设备。
真实集成测试通过终止本测试启动的 Replica 使连接中断，不改全局设备状态。

## 配置启动示例

先在两端构建 `make all`，确认 `rdma link show` 中 siw 链路为 ACTIVE。
下面以 Primary `172.16.145.135`、Replica `172.16.145.136` 为例，替换为实际地址。
在两个独立目录中分别保存以下 `server.conf`，不能共用快照/AOF 文件：

```ini
# Primary 的 server.conf
role=primary
bind_ip=172.16.145.135
service_port=29000
rdma_port=29747
persistence_mode=legacy_hybrid
log_level=info
```

```ini
# Replica 的 server.conf
role=replica
bind_ip=172.16.145.136
service_port=29001
primary_host=172.16.145.135
primary_port=29000
persistence_mode=legacy_hybrid
log_level=info
```

在各自配置目录运行 `/绝对路径/91kvstore/bin/kvstore "$PWD/server.conf"`。
单机 SIW 实验时两个配置均可使用同一 iWARP IPv4，服务端口仍须不同。

## 固定复现命令及退出码

在仓库根目录运行（Bash），先修改网卡 IP：

```bash
make all
set -o pipefail
RDMA_TEST_IP=172.16.145.135 PRIMARY_PORT=29200 REPLICA_PORT=29201 \
    make test 2>&1 | tee /tmp/91kv-rdma-make-test.log
status=${PIPESTATUS[0]}
printf 'make test exit=%s\n' "$status"
```

本机单独验收：`RDMA_TEST_IP=172.16.145.135 make test-rdma`。
失败场景的 10 秒是测试等待上限；生产主动建连有 3 秒整体截止时间，
涵盖地址/路由解析与等待 ESTABLISHED，不限制已建立连接的快照传输时长。
当前并无自动重连/续传，失败后重新启动 Replica 发起新全量。

两端同提交且 VM-A 可 SSH 登录 VM-B 时，可在 VM-A 使用现有双机入口：

```bash
mkdir -p /tmp/91kv-rdma-evidence
# VM-B 也预先创建 /tmp/91kv-rdma-evidence，端口须空闲。
python3 -B tests/run_two_vm_acceptance.py --mode both --repo "$PWD" \
    --primary-host 172.16.145.135 --primary-port 31410 --rdma-port 31747 \
    --replica-ssh jaspersao@172.16.145.136 --replica-repo /home/jaspersao/dev/91kvstore \
    --replica-host 172.16.145.136 --replica-port 31411 \
    --artifacts /tmp/91kv-rdma-evidence --replica-artifacts /tmp/91kv-rdma-evidence \
    --run-id stage9_failure_acceptance
status=$?
printf 'two-vm exit=%s\n' "$status"
```

双机命令需要两端代码及构建版本一致。工作负载为前后各 5 万记录，以及
独立的 32 MiB 受控追赶；不等同于失败事件单测。
