# 构建与测量复现说明

[首页](../../README.md) · [总报告](REPORT.md) · [测试可行性](TESTING.md)

所有命令仅作说明，本次文档整理没有运行负载、诊断、编译、安装或服务。重新测量需在另行授权的环境安排。以下使用变量与独立临时目录，不依赖个人路径或旧证据目录。

## 构建与运行条件

Makefile静态核对基线 `719836e9`：普通 `make all` 生成 `bin/kvstore`，必要时自动在 NtyCo 子模块调用make；`make ebpf-startup` 另生成 `bin/kvstore-ebpf` 和 `build/ebpf_capture.bpf.o`。主程序 `-std=gnu11 -Wall -Wextra -g`，不含优化选项；BPF对象单独 `clang -target bpf ... -O2 -g`。编译无需root。

```bash
# Ubuntu/Debian依赖示例，需管理员批准；不是本轮执行记录。
sudo apt update
sudo apt install -y build-essential git python3 liburing-dev librdmacm-dev libibverbs-dev \
  rdmacm-utils ibverbs-utils iproute2 netcat-openbsd
# 真实BPF构建的额外依赖：
sudo apt install -y clang libbpf-dev libelf-dev zlib1g-dev
# 可选客户端与三方案内存入口：
sudo apt install -y redis-tools pkg-config libhiredis-dev libjemalloc2
```

```bash
git clone https://github.com/JasperStonnne/91kvstore.git
cd 91kvstore
git submodule update --init --recursive
make all
make ebpf-startup
REPO="$PWD"
```

实际测量使用先前已有二进制，**未重新执行这些构建命令**。全新安装/clone/init流程本阶段没有重测。复测前记录HEAD、工作区、stash、子模块、两版程序/BPF对象/入口SHA、ldd及依赖内容；HEAD相同不证明二进制相同。已有二进制缺失或来源不明应停止，不能补编后冒充原成绩。

普通单机不需root，但内核须允许io_uring。真实RDMA需要管理员已准备好的ACTIVE iWARP/IPv4；本说明不配置设备或改sysctl。BPF加载本环境用sudo；替代capability权限未验证。用户态程序仍负责TCP发送，BPF不会自行跨机。

## 单机：一个128B热点key

服务终端，在仓库根目录先保存REPO，所有目录均新建：

```bash
REPO="$PWD"
RUN_DIR=$(mktemp -d)
cat > "$RUN_DIR/server.conf" <<'CONF'
role=standalone
bind_ip=127.0.0.1
service_port=48651
log_level=error
persistence_mode=snapshot
allocator=pool
CONF
# 必须先确认48651空闲；冲突就停止，不自动重试。
cd "$RUN_DIR"
"$REPO/bin/kvstore" "$RUN_DIR/server.conf"
```

另一个客户端终端，使用同一VM、本机回环；服务身份经PID/ticks/exe/cwd/SHA核对后才发请求。工具版本为redis-benchmark/redis-cli 7.0.15：

```bash
VALUE_A=$(printf '%128s' '' | tr ' ' a)
VALUE_B=$(printf '%128s' '' | tr ' ' b)
# 只创建一次，检查实际回复为OK；重复HSET会覆盖，不能算不同key的新增。
[ "$(redis-cli -h 127.0.0.1 -p 48651 --raw HSET perf:hot "$VALUE_A")" = OK ]
[ "$(redis-cli -h 127.0.0.1 -p 48651 --raw HGET perf:hot)" = "$VALUE_A" ]
redis-benchmark -h 127.0.0.1 -p 48651 -c 1 -P 1  -n 128000 -d 128 HGET perf:hot
redis-benchmark -h 127.0.0.1 -p 48651 -c 1 -P 16 -n 128000 -d 128 HGET perf:hot
redis-benchmark -h 127.0.0.1 -p 48651 -c 1 -P 64 -n 128000 -d 128 HGET perf:hot
redis-benchmark -h 127.0.0.1 -p 48651 -c 8 -P 1  -n 128000 -d 128 HGET perf:hot
# 本更新组固定用HMOD：每轮前恢复a并读回，测后应为b；与SET覆盖负载分开。
[ "$(redis-cli -h 127.0.0.1 -p 48651 --raw HMOD perf:hot "$VALUE_A")" = OK ]
redis-benchmark -h 127.0.0.1 -p 48651 -c 1 -P 1 -n 128000 -d 128 HMOD perf:hot "$VALUE_B"
[ "$(redis-cli -h 127.0.0.1 -p 48651 --raw HGET perf:hot)" = "$VALUE_B" ]
```

每组先独立热身不计成绩；三轮顺序：①g1/g16/g64/g8/mod；②mod/g8/g64/g16/g1；③g16/mod/g1/g8/g64。每组前HMOD恢复a并GET核对，后GET核对预期；stdout/stderr、完成数、退出码、错误/超时全部留存。已有CONFIG warning保留，不算业务负载请求。不得只信显示出的QPS或退出码，不忽略错误文本。

P16追加长轮次是独立批次，先热身再三轮，每轮相同命令：

```bash
redis-benchmark -h 127.0.0.1 -p 48651 -c 1 -P 16 -n 12043648 -d 128 HGET perf:hot
```

历史长轮次使用同样配置和新的测试实例，具体空闲端口在原始证据；端口不影响客户端/负载定义。每轮约24.91–24.99秒。不能把短样本与长样本混成六轮，也不能称差异为代码优化。

工具只做完成统计和测前/后值检查，没有逐响应内容验证。7.0.15的工具延迟边界是批次发送到首个read事件，不是完整单请求完成；pipeline延迟不除以深度、不与P1分位数混比。图表仅画吞吐。

## Snapshot/AOF十万条

在仓库根目录使用原入口，每次新TMPDIR；脚本内部新建两个模式的独立目录：

```bash
RUN_ROOT=$(mktemp -d)
mkdir "$RUN_ROOT/tmp"
TMPDIR="$RUN_ROOT/tmp" python3 -B tests/test_persistence_100k.py --stop-method kill
```

原批次先一组热身、三轮；初次pidstat完整路径匹配配置错误，只有表头，计时和数据仍保留。修正采样后预先声明采用resource-complete批次的三轮，非按好成绩挑选；本报告只用后者，旧组不混入。

每模式10万不同key：Array1000，其他三引擎各33000；串行TCP、脚本变长值含空格/CRLF。Snapshot写内存→成功SAVE→SIGKILL；AOF全部写成功→SIGKILL，不发SAVE。二者不是同耐久性吞吐比较。

原脚本重启计时从启动前到端口可连接，约0.1秒轮询；之后全量GET精确比对另计。端口可连接不能冒充已核对key的恢复时刻；小数输出不是毫秒精度证明。未清缓存/重启VM，不称冷盘或断电测试。

## 百万条allocator

使用已有普通版与已安装jemalloc，不编译另一种allocator版。这里的三种benchmark模式不是三个配置枚举：system为`allocator=system`且清除预加载，pool为`allocator=pool`；jemalloc为`allocator=system`加`LD_PRELOAD`。四引擎key/value也通过统一入口，pool按大小使用规格池或独立mmap，不是只覆盖节点；第三方直接分配不由pool接管。jemalloc只替换可动态截获的普通堆调用，不接管直接mmap。详见[分配范围](REPORT.md#分配覆盖范围只读源码核对)。

```bash
REPO="$PWD"
RUN_ROOT=$(mktemp -d)
mkdir "$RUN_ROOT/tmp"
TMPDIR="$RUN_ROOT/tmp" python3 -B tests/benchmark_memory.py \
  --backend all --entries 1000000 --rounds 3 \
  --server "$REPO/bin/kvstore" --output "$RUN_ROOT/measurement" \
  --timeout 3600 --socket-timeout 15 --settle 0.2 \
  --as-limit-mib 2048 --rss-limit-mib 1024 --min-available-mib 1024 \
  --build-note 'Existing default unoptimized userspace binary; verify its SHA first'
```

三轮原顺序system/pool/jemalloc → pool/jemalloc/system → jemalloc/system/pool。每次四阶段各100万操作，key32B/value128B；Array1024、Hash/RBTree/Skiplist各332992。先核对可用内存、cgroup限制、磁盘及继承AS上限；不足停止。AS是2GiB硬上限，RSS1GiB是软件守卫。后端诊断进程不计测量；配置、绑定和映射检查不等于逐个观测全部分配点，不能用malloc符号提供者判断pool的mmap路径。

原脚本每状态5次/proc样本取中位数，随后三轮再取中位数；脚本字段peak实为装满并读回后的采样，不是高水位。外部只读VmHWM/VmPeak证据另存，截止最后有效样本，不保证退出前最后瞬间。没有重插复用阶段、没有从RSS推断泄漏。阶段耗时含数据生成与串行TCP，不是allocator API时间。

## 双VM：既有手动客户端

本批A/B正式仓库HEAD不同，但B独立副本的既有二进制、入口和11项ldd依赖/loader与A逐项SHA相同。通用自动runner `run_two_vm_acceptance.py` 要求两端Git/源码一致并固定普通 `bin/kvstore`，且直接模式第二批时机不等于已确认ONLINE；不能把它冒充本批eBPF测量入口。因此使用下列既有手动客户端，不修改或新增负载生成器。

先在两端选择新工作目录；设置 `PRIMARY_IP`、`REPLICA_IP` 为各自ACTIVE iWARP IPv4。A配置如下，B使用其独立目录、相同程序SHA与入口版本：

```ini
# A: primary.conf
role=primary
bind_ip=<PRIMARY_IP>
service_port=29480
rdma_port=29780
log_level=info
persistence_mode=legacy_hybrid
allocator=pool
```

```ini
# B: replica.conf
role=replica
bind_ip=<REPLICA_IP>
service_port=29481
primary_host=<PRIMARY_IP>
primary_port=29480
log_level=info
persistence_mode=legacy_hybrid
allocator=pool
```

上述尖括号是须替换的配置占位符，不是直接可启动配置。确认端口空闲、程序依赖匹配、目录独立后，前台启动（A/B的REPO分别指各自程序所在目录）：

```bash
# A服务终端，需批准的sudo；必须确认ONLINE source=eBPF，不能把AOF降级当通过。
cd "$A_DIR"
sudo env KVS_EBPF_OBJECT="$REPO/build/ebpf_capture.bpf.o" \
  "$REPO/bin/kvstore-ebpf" "$A_DIR/primary.conf"
# B服务终端，只在第一批全部成功后执行，普通权限即可：
cd "$B_DIR"
"$REPO/bin/kvstore-ebpf" "$B_DIR/replica.conf"
```

所有客户端在A，RUN_ID每轮新建且同轮所有调用一致；正式三轮前缀长度19字节可用 `p13_repeat_round_01`/02/03，新目录确保不与旧数据混用。热身前缀 `p13_repeat_warmup` 为17字节，与本批原热身长度相同。只允许ASCII字母/数字/下划线。

```bash
# 1. A启动并核对身份后先发首批；此时B还未启动。
RUN_ID=p13_repeat_round_01
python3 -B tests/test_two_vm_100k.py write --host "$PRIMARY_IP" --port 29480 \
  --batch 1 --run-id "$RUN_ID" --report "$A_DIR/write-batch1.jsonl"
# 2. accepted=50000、success=true、退出0，才启动B。
# 3. 两端日志确认追赶完成/ONLINE后，先校验B首批：
python3 -B tests/test_two_vm_100k.py verify --host "$REPLICA_IP" --port 29481 \
  --batch 1 --run-id "$RUN_ID" --report "$A_DIR/verify-before-online-write.jsonl"
# 保存退出码；另一次只读调用展示最后complete事件、matched=50000、错误0及退出0。
# 此检查完全结束后，再申请/执行第二批；失败就停止，不连续盲发。
python3 -B tests/test_two_vm_100k.py write --host "$PRIMARY_IP" --port 29480 \
  --batch 2 --run-id "$RUN_ID" --report "$A_DIR/write-batch2.jsonl"
python3 -B tests/test_two_vm_100k.py verify --host "$PRIMARY_IP" --port 29480 \
  --batch all --run-id "$RUN_ID" --report "$A_DIR/verify-primary-all.jsonl"
python3 -B tests/test_two_vm_100k.py verify --host "$REPLICA_IP" --port 29481 \
  --batch all --run-id "$RUN_ID" --report "$A_DIR/verify-replica-all.jsonl"
```

上述命令按编号逐步执行，不是忽略门槛的整块脚本。客户端每轮各批Array500、其他三引擎各16500，正式key31B、value52–148B。Python原生seconds含生成/编解码/连接/网络/回复检查，不提供逐请求复制延迟。

事先落实300秒上限与异常退出清理；实际PID/ticks/exe/cwd/SHA启动时记录，停止前再核对。结束先停B保留A汇总，再停A；只停止本轮已验证PID。A快照 `replication.snapshot` 是root文件时，仅批准的绝对路径 `sudo sha256sum` 读取，不chmod；B文件是 `snapshot.db`。比较快照SHA与日志边界，不能要求可能有稀疏前缀的AOF整文件SHA相同。本批没有AOF字节区间逐字节比较。

实际本批有一组热身和三组正式，失败不静默补跑。末轮外层收尾检查把正常B退出0误当失败，触发A清理；保留非零状态，只补读原快照SHA和独立现场复核，不重新发负载。完整命令和异常由[证据索引](data/evidence-index.json)对应的私有原始包留存。

## 只读资源记录与离线图表

历史成绩仅用pidstat/mpstat/proc读取，没有perf/bpftrace/sanitizer同时运行。示例资源命令仅限已核对PID：

```bash
pidstat -H -h -u -r -d -p "$TEST_PID" 1
mpstat -P ALL 1
```

A root进程I/O字段为-1记未测，不提权补值；B跨VM时钟未核实，不做逐请求延迟。perf/bpftrace诊断是独立历史数据，不与成绩混算；P64诊断到此结束，不再追加探针或调优。

仓库已附5张静态SVG，GitHub显示无需Python。离线重画仅使用已提交数据：

```bash
python3 docs/performance/plot_results.py
```

需要**已存在**的matplotlib（本次使用3.9.4），缺失不自动安装；脚本不执行测试、不访问VM、不调用sudo、不启动服务，只读 `data/measurements.json` 后生成SVG。`--preview-dir` 可将本地PNG校对副本写到指定目录，PNG不必入库。原始大归档/运行数据/个人路径不入Git，只保留轻量数值和哈希索引。
