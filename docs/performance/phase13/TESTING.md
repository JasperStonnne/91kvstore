# 测试方案、可行性与边界

[返回首页](../../../README.md) · [性能总报告](REPORT.md) · [复现说明](REPRODUCING.md)

本页依据基线 `719836e9` 的 Makefile 和现有脚本静态核对。本次文档整理**未编译、运行测试或服务**。历史第十一/十二阶段的通过记录与第十三阶段测量分开；新机器重新执行仍须检查真实退出码及断言，不能把示例步骤当作新实测。

## 环境准备

- Linux、GCC/GNU Make、Python 3，NtyCo 子模块固定 `72ab5fd`；普通程序也链接 librdmacm/libibverbs/liburing。
- io_uring 必须可用；容器/seccomp/内核限制导致不可用时明确失败，不建议通过修改全局安全设置让测试变绿。
- 真实 RDMA 测试需 ACTIVE iWARP、IPv4、`rdma`、`ibv_devinfo`、`ip`、`stdbuf`。设备由管理员预先准备，脚本只查询，不配置网络。普通回归中的真实 RDMA 可在同一 Linux 设备上运行，不要求两台 VM。
- 真实 eBPF 专项额外需已有 `kvstore-ebpf`、BPF 对象及批准的加载权限。本实验用 sudo；不假定非 root capabilities 已验证。
- 本批实测环境为每 VM 4 vCPU/7919 MiB，allocator 子服务约275 MiB装满RSS。它不是通用最低硬件承诺。allocator 原脚本有2GiB AS、1GiB RSS守卫、主机1GiB可用内存下限、启动时磁盘1GiB下限；这些限制不应套给地址空间很大的 ASan 程序。
- 测试目录与端口独立。先检查空闲与磁盘/内存；明确 PID、启动 ticks、exe、cwd、SHA，异常/超时只停止自己创建且身份复核通过的进程，不按名称批量 kill。

## 测试矩阵

| 功能/目标 | 现有入口及执行步骤 | 正确性判据 | 环境、规模与记录状态 |
| --- | --- | --- | --- |
| 配置、协议/半包粘包、引擎、文件恢复、复制边界 | `make test-unit`；可单独选 Makefile 中的 `test-config`、`test-protocol`、`test-aof-replay`、`test-replication-boundary` 等 | 断言、错误返回及退出码；不以“能启动”代替正确性 | 单机Linux；无真实BPF，边界替身不是实际RDMA；含ASan/UBSan编译目标。既有第十二阶段回归通过，本次未运行 |
| 万条完整功能链 | `make test-functional-bulk` | 每阶段10000：SET、GET、MOD、修改后GET、EXIST、DEL、删除后GET、删除后EXIST；精确回复/值/未命中；ERROR级日志为空 | 单机；Array1000，其他三引擎各3000。第十二阶段80000次操作通过，不机械扩大每个单测 |
| 独立Snapshot/AOF恢复 | `make test-persistence-100k`；或 `python3 -B tests/test_persistence_100k.py --stop-method kill` | Snapshot成功SAVE后、AOF所有写成功后SIGKILL；退出-9；各10万逐条匹配；模式文件独立、恢复前后文件SHA不变 | 单机；Array1000，其他各33000。第十三阶段两模式各3轮通过；进程崩溃，不是断电 |
| 普通完整回归 | 配置 `RDMA_TEST_IP` 后 `make test` | 单元和集成均通过、退出0；观察失败/残留，不只看汇总行 | 单机有真实iWARP即可。含真实RDMA与故障/恢复组，不含所有大数据、客户端或真实BPF专项。第十二阶段既有通过记录，本次未重跑 |
| 真实RDMA | `RDMA_TEST_IP=实际IPv4 make test-rdma` | 完整快照SHA/大小、累计ACK、安装→追赶→ONLINE、失败不安装半份快照；非空追赶及断线限制 | 同机ACTIVE iWARP；既有场景含32MiB快照。没有设备明确失败，不降级TCP全量、不记SKIP为PASS |
| 混合恢复组合 | `make test-hybrid-recovery` | 缺AOF、检查点超界/已知错位拒启；保留现场，不自动修剪 | 单机。不是检测所有不配套但语法合法的文件组合 |
| 持久化失败/崩溃窗口 | `make test-aof-write-failure test-aof-sync-failure test-snapshot-save-failure test-snapshot-crash` | 部分写/同步失败不回复成功；合法恢复或明确拒启；rename后目录同步失败单独验证 | 单机；部分目标编译独立包装程序，故障开关不加入正式服务。历史阶段通过；真实磁盘故障/VM断电未测 |
| 复制故障 | `make test-replication-faults`；真实eBPF版本见下节 | 不应用半命令、不假ONLINE、断线后拒过时读、重启重新全量、AOF字节与状态边界核对 | 普通组需iWARP；真实BPF组另需sudo。历史第十一阶段两VM各9例通过；本批未追加故障负载 |
| 客户端支持子集 | `make test-clients` | redis-cli支持场景、hiredis八个检查；显式RESP PING/HGET完成数及错误检查，前后值核对 | 可选 redis-tools、pkg-config、cc、hiredis。历史4组PASS、0 SKIP；默认PING为UNSUPPORTED。不是全面Redis兼容或性能验收 |
| 严格客户端对照 | `make test-clients-strict` | 任意FAIL、SKIP或默认PING未支持均非零 | 当前默认 `redis-benchmark -t ping` 未支持，不能期待全绿；缺依赖也不能返回通过 |
| 模拟eBPF发送逻辑 | `make test-ebpf-online` | 队列替身检查offset栅栏、分段、去重、缺口/尾部补偿及拒收 | 单机无BPF权限；不冒充真实挂载证据；普通 `test-unit` 包含该目标 |
| 真实eBPF专项 | 先 `make ebpf-startup`，再按下节直接运行脚本 | 必须source=eBPF；超限、真实队列满、consumer -EIO、旧事件、版本拒收、AOF补偿及逐条数据 | sudo + ACTIVE iWARP；历史专项有原始/终端交付证据，不能把纯模拟单测代替它 |
| 双VM 5万+5万 | `tests/test_two_vm_100k.py`：write1→启动Replica→ONLINE→verify1→write2→两端verify all | accepted=50000；各端matched=100000，错误0；两端快照SHA；保留offset及日志 | 同一程序/依赖指纹，独立数据目录，A需BPF权限。第十三阶段热身+3轮数据通过，末轮编排非零详见总报告 |
| allocator三方案 | `tests/benchmark_memory.py --backend all --entries 1000000 --rounds 3` | 配置与库绑定、四阶段各百万操作成功、三状态内存与守卫；分配范围见[源码核对](REPORT.md#分配覆盖范围只读源码核对) | 单机；需已有jemalloc库；9次测量共3600万次检查通过。不是allocator API微基准；system=libc，pool含key/value等统一入口分配，jemalloc=system加预加载 |
| 可选诊断入口 | `make test-sanitizers`、`make fuzz-protocol` | 保留最小复现/退出码、非零失败；SKIP与未跑不作PASS | 会在隔离目录重编译，需相应编译器/runtime；本批文档任务不执行。历史ASan/UBSan 15单测及有界协议诊断有记录，不覆盖真实NtyCo/CM/CQ/BPF运行；文件/控制行未做独立覆盖引导验证 |

## 明确步骤与权限分界

在仓库根目录，先按[构建说明](REPRODUCING.md#构建与运行条件)构建普通程序。根据现场选择测试：

```bash
# 无真实BPF、不需sudo；可能编译测试程序。
make test-unit
make test-functional-bulk
make test-persistence-100k
make test-clients
# 完整普通回归还需要已准备好的真实iWARP：
: "${RDMA_TEST_IP:?先设置本机ACTIVE iWARP IPv4}"
make test
```

真实 BPF **先普通用户构建**，不要用 sudo make 来混合安装、编译和运行：

```bash
make ebpf-startup
REPO="$PWD"
: "${RDMA_TEST_IP:?先设置本机ACTIVE iWARP IPv4}"
sudo env KVS_EBPF_OBJECT="$REPO/build/ebpf_capture.bpf.o" \
  RDMA_TEST_IP="$RDMA_TEST_IP" \
  python3 -B "$REPO/tests/test_ebpf_replication.py" "$REPO"
# 真实中断专项（额外场景，另行安排，不是本阶段性能负载）：
sudo env KVS_EBPF_OBJECT="$REPO/build/ebpf_capture.bpf.o" \
  RDMA_TEST_IP="$RDMA_TEST_IP" \
  python3 -B "$REPO/tests/test_replication_faults.py" "$REPO" \
  --binary "$REPO/bin/kvstore-ebpf" --require-ebpf
```

这些是可复现说明，不是文档整理阶段已执行的新步骤。依赖缺失、权限不够、设备不可用，应记录脚本原始行为（SKIP或FAIL），不能替换成成功。客户端普通入口允许可选SKIP不等于覆盖全部；严格入口SKIP也失败。普通集成无设备会失败，不能拿单元通过替代。

## 持久化失败契约

Snapshot普通写入仅在内存；SIGTERM也不自动等于SAVE。SAVE先临时文件、文件同步、rename，再父目录同步：替换前失败保留旧正式文件；rename后目录同步失败可能返回错误却已有完整新文件，不承诺回滚。

AOF在成功回复前追加和同步。内存可能先修改；AOF失败时不回复成功并退出，失败回复不保证命令绝不会在重启后出现。损坏/截断日志拒绝启动，不自动跳过或截尾。hybrid检查只覆盖已知边界，不保证识别任意语法合法但来自不同历史的文件组合。

真实磁盘错误、断电没有实测。Replica临时快照使用write/fsync，单机AOF/SAVE数据写入使用io_uring，恢复用mmap；不能把“所有落盘都经io_uring”标完整通过。

## 复制与已知限制

只有单Primary/单Replica范围；无自动重连、断点续传、逐条增量ACK、选主、认证/TLS。失败后人工重启Replica重新全量。Primary提交给网络的offset不是Replica应用确认，最终读回一致不等于exactly-once。软件SIW同宿主机不代表硬件RDMA。

第十一阶段曾出现SIW偶发 `Address already in use`，当时现场不足，根因未确认；后续成功不能写为修复。本批第三轮收尾编排错误也保留，数据通过不能抹掉流程非零状态。

新环境完整clone/init/安装流程、其他内核/容器、非root BPF权限、硬件RDMA、真实磁盘/断电均未在本阶段重新验证。详细的通过来源与未测项见[总报告](REPORT.md)和[证据索引](data/evidence-index.json)。
