# eBPF ONLINE 实验与验收

当前在 stage10/ebpf-online 分支验证单 Primary、单 Replica 的 ONLINE 数据源。
实验构建为 bin/kvstore-ebpf，普通 bin/kvstore 继续使用 AOF ONLINE。

| 路径 | 职责 |
| --- | --- |
| RDMA / iWARP | 全量快照 |
| TCP + AOF | 快照边界后的追赶，及 ONLINE 缺口补偿 |
| eBPF + ring buffer + TCP | 正常 ONLINE 的完整命令采集和发送 |

BPF 本身不跨机。Primary 的用户态复制模块把事件中的命令交给现有 TCP。

## 文件与数据流

- src/persistence/aof.c 在写入和同步成功、推进 offset 后调用成功点。
- src/kvstore.c 仅对 Primary 的客户端成功写入设置采集标记。
- src/replication/ebpf_capture.bpf.c 的 on_committed() 读取完整编码命令，
  通过 ring buffer 提交命令和 [start,end)，读取失败、队列满、超限分别计数。
- include/ebpf_event.h 是 BPF 和用户态共同使用的事件结构。
- src/replication/ebpf_capture.c 管理加载、当前进程自附着、单条读取和清理。
- src/replication/replication.c 去重、检查 offset、分段填 output、补 AOF 缺口。
  网络层按原来方式发送 output。

成功探针在接受客户端前加载。进入 ONLINE 时的 AOF offset 是切换栅栏。
追赶已经覆盖的旧事件按 end <= 当前发送位置跳过。
遇到后续事件之前的缺口，保留该事件并先补缺口。
队列读空后也检查已提交尾部，因此最后一条丢失不需要后续事件才能发现。

目前服务写入和队列消费由同一 NtyCo 线程执行，成功探针在 AOF 追加返回前同步执行。
增加并行写线程时必须重新审查提交与队列消费之间的栅栏。

## 容量与失败语义

事件最多携带 4096 字节的完整编码命令，整个 ring buffer 为 1 MiB。
超过 4096 字节的命令保留在 AOF，并以完整字节区间补发，不能截断。
网络输出容量可以小于命令，待发事件及进度保留到全部字节交出。

加载或附着失败会记录原因和 ONLINE source=AOF。
运行时消费者读取错误会明确切回 AOF。队列满及超限写入继续成功，
日志记录 BPF 累计失败数，以及每个 AOF 补偿区间和原因。

事件格式当前为 v1，BPF 和用户态共用 version、length、start、end、payload 布局。
用户态先检查记录大小和版本，未知版本或旧布局明确拒收，记录原因并切回 AOF。
不会把拒收的数据转发给 Replica；专项覆盖大小相同的旧布局和未知版本 99。

发送位置表示 Primary 已交给网络层的命令边界，不是 Replica 已应用的 ACK。
增量没有逐条 ACK，没有自动重连和断点续传。断线后清空待发状态，
新的复制连接仍重新请求全量，不宣称端到端 exactly-once。

## 构建与启动

在 Linux 仓库根目录运行：

~~~bash
make ebpf-startup
~~~

生成 bin/kvstore-ebpf 和 build/ebpf_capture.bpf.o。
构建需要 clang 的 BPF 后端和 libbpf 开发包。

按 README_RDMA.md 准备独立数据目录和 Primary 配置，使用真实 iWARP 地址。
启动时通过 KVS_EBPF_OBJECT 传入上述 BPF 对象的绝对路径。
加载 BPF 需要相应权限，实验可用 sudo 启动 Primary。
启动日志应包含 ONLINE source=eBPF。仅出现 AOF 降级不能算作完成 eBPF 验收。

## 测试

~~~bash
make test-ebpf-online
make test
~~~

单元回归通过事件队列和内存 AOF 测试真实发送逻辑，覆盖分段、切换去重、
中间缺口、末尾丢失、超限、过期事件清理预算和错误行为。
普通测试不加载 BPF，不要求 BPF 权限。
已有 RDMA 测试仍要求 ACTIVE iWARP，端口使用规则沿用原来的测试说明。

真实 BPF 专项先以普通用户编译，再提权运行测试：

~~~bash
make ebpf-startup
sudo python3 -B tests/test_ebpf_replication.py .
~~~

专项复用现有 RDMA 验收工具，检查非空 AOF 追赶、ONLINE 后四引擎
SET/MOD/DEL、失败写入排除、完整 offset 和字节数、4096 字节边界、
1024 字节输出分段、超限补偿后恢复 eBPF，以及缩小测试队列制造的真实溢出。
每次使用独立目录及端口，只停止本脚本启动的进程，保留日志和统计摘要。
权限不足时专项明确失败，普通回归仍可独立运行。

专项会连续运行三次主动队列溢出，逐条校验数据及尾部丢失的补偿。
tests/ebpf_consumer_fault.c 通过测试专用 LD_PRELOAD 在队列确有待处理事件时，
令一次消费返回 -EIO，验证已成功写入且已排队的命令能够从 AOF 完整重放。
故障开关只存在于该测试共享库中。

专项还会在 Replica 启动前写入 600 条，核对快照覆盖后跳过的旧事件数量、
首条新事件恰好接上栅栏，以及不会重复补发已覆盖的数据。此阶段保留提前采集，
采集时机优化属于后续单独决策。

同机专项覆盖受控非空追赶和四引擎 ONLINE。跨 VM 可使用
tests/ebpf_snapshot_gate.py，在 Primary VM 启动 TCP 闸门，固定快照边界后
暂停全量通知，写入期间命令，再放行真实 RDMA。该辅助器的 release 和 stop
文件控制放行与结束，listening.json 和 snapshot.json 记录端口及边界。
后续批量数据用 test_two_vm_100k.py 在明确 ONLINE 后写入并逐条比对。

所有场景均需核对源码快照及程序指纹。SIW 只证明软件 iWARP 功能，
不提供硬件 RDMA 性能结论。没有逐条增量 ACK，不宣称端到端 exactly-once。
