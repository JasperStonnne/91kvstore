# Stage 5：证据、复现与离线制图

[正式报告](REPORT.md) · [数据](data/measurements-2026-11-26.json) · [历史复现文档](REPRODUCING.md)

## 离线重生成图表

已有Python3/matplotlib/numpy环境即可；此命令不会安装依赖、启动服务或跑测试：

```bash
python3 -B docs/performance/plot_results.py \
  --final-data docs/performance/data/measurements-2026-11-26.json
```

可用`--preview-dir /tmp/91kv-stage5-preview`生成PNG检查。SVG标题/脚注含版本、轮次和计时边界；支持旧`--data`、`--direct-data`和不带参数的历史绘图入口，不会默认覆盖旧图。单轮轮间波动标N/A；三轮点与min/max范围只是观测范围，不是置信区间。

## 固定负载命令

以下只是复现说明，本次Stage 5未重新执行。使用干净49b8d3d工作副本与独立运行目录，保留版本、构建输出及ELF SHA256。不要在含待提交Stage 5文档改动的工作区绕过基线检查。普通项目默认OPT=-O2，NtyCo参数单列。

```bash
make -B OPT=-O2 all ebpf-capture benchmark-tools
redis-server --version
sha256sum bin/kvstore bin/kvstore-ebpf-capture build/ebpf_capture.bpf.o
```

单机配置：role=standalone、127.0.0.1:29191、allocator=pool；persistence_mode按组选择none或aof，aof_checksum=none。每个实例使用独立临时cwd，不改仓库config文件。

```bash
payload=$(python3 -c 'print("x"*128)')
redis-benchmark -h 127.0.0.1 -p 29191 -c 1 -P 1 -n 100000 --precision 3 ECHO "$payload"
redis-benchmark -h 127.0.0.1 -p 29191 -c 1 -P 1 -n 100000 --precision 3 SET hotkey "$payload"
# 将P依次改成10/20/40/80/160；每P、每服务3轮，逐个启动/停止服务。
```

Redis 7.0.15：none用`--save "" --appendonly no`；aof用`--save "" --appendonly yes --appendfsync always --auto-aof-rewrite-percentage 0`。每轮独立空AOF目录；切换前确认29191释放，不并发运行对照。单请求/AOF本报告保留Stage 1成绩，不能将新复测再并进旧三轮。

SAVE频率使用现有脚本：

```bash
python3 -B -u tests/benchmark_save_frequency.py \
  --entries 1000000 --save-every 1000000 100000 10000 1000 --rounds 1
```

只等SAVE接受回复，最后统一等SAVEINFO收尾；读取write/drain/total、实际completed/triggered、fork/save累计耗时、百万条重启校验及SHA。后台累计耗时与前台重叠，不相加。

内存组一轮按实际运行分开：

```bash
python3 -B -u tests/benchmark_memory.py --entries 1000000 --rounds 1 --reclaim-policy all \
  --backends system system-trim pool jemalloc jemalloc-decay0 --build-note=-O2
python3 -B -u tests/benchmark_memory.py --entries 1000000 --rounds 1 \
  --reclaim-policy comparable --backends system-malloctrim pool --build-note=-O2
```

显式`--backends`保持原先五模式与补充二模式分组；当前`all`默认已扩为六模式，不自行将五模式pool与补充trim-pool配对。RSS每阶段5次取中位；ALLOCTRIM只在显式测试请求时调用，不改变默认free路径。

## 双机c8固定编排源码

当时临时编排的源码归档于[tools/two_vm](tools/two_vm/README.md)。仅测试控制程序和C客户端，没有业务实现变更；客户端总计100000次、8连接、每连接最多1个未完成请求。135内部运行客户端，Mac只负责传包，不经过逐请求数据路径。136原仓库不覆盖，使用代码bundle与相同ELF的独立副本。

准备阶段（135普通用户）：工具源码可留在当前Stage 5文档目录；`--repo`必须指向另一个main/HEAD均为49b8d3d的干净工作副本，而不是当前待提交文档目录。

```bash
python3 -B -u docs/performance/tools/two_vm/prepare.py primary \
  --repo /absolute/path/to/clean-49b8d3d
```

它先-O2构建、检查警告、自检临时驱动，生成`/tmp/91kv-stage44b-manual/to136.tar.gz`。允许NtyCo构建产物libntyco.a，其余源码变化仍拒绝。手动将这个包经可信渠道送到136，不打印包内随机令牌。136普通用户：

```bash
umask 077
mkdir /tmp/91kv-stage44b-manual
tar -xzf ~/stage44b-to136.tar.gz -C /tmp/91kv-stage44b-manual
python3 -B -u /tmp/91kv-stage44b-manual/prepare.py replica
sudo python3 -B -u /tmp/91kv-stage44b-manual/agent.py
```

看到READY后，在135：

```bash
sudo python3 -B -u /tmp/91kv-stage44b-manual/controller.py \
  --plan /tmp/91kv-stage44b-manual/plan.json \
  --modes none ebpf-capture-only socket ebpf \
  --rounds 3 --label formal --finish-agent
```

固定地址135/136、端口29191/29192、RDMA29747、临时控制29444；不修改网络或sudo设置。代理仅接受固定操作、限定源IP和随机令牌，无任意shell执行接口。sudo仅服务于BPF/perf和编排；Primary/Replica/客户端降为普通用户。每轮成功后清理自建进程与快照，结果保留；Ctrl-C也走自己的清理，不用pkill全局杀服务。源码只实现本次c8入口；历史c1完整轮次与原始命令另存在JSON，不混入c8。

观察阶段在正式12轮之后：idle和额外持续c8负载各采perf/pidstat8秒；主表不含诊断。完整命令自动记录，核心为：

```text
pidstat -u -w -p PID 1 8
perf record -e cpu-clock -F 199 -g --call-graph dwarf,4096 -p PID -o perf.data -- sleep 8
perf stat -x, -e task-clock,context-switches,cpu-migrations -p PID -- sleep 8
perf report --stdio -i perf.data --no-children --percent-limit 0.5 --sort comm,dso,symbol
```

CPU 100%=一个核心。idle前后核对sent/applied不变；保留Children栈和Self热点，不能叠加两者百分比。perf失败如实记录，不能将缺失数据当成“低开销”。

## 双向传输与缓冲诊断

代码与Stage 3.2相关3份源码哈希一致，快照先用现有SAVE生成，等待SAVEINFO后核对168000052B与SHA。下面135→136，换方向重复；原生两模式各3轮，iperf默认3轮，调参诊断1轮。独立端口且串行，不重用仓库数据文件。

```bash
# 136先启动一个接收端；MODE分别sendfile、rdma，RDMA还使用端口+1。
bin/benchmark_transfer receive MODE 172.16.145.136 42002
# 135
bin/benchmark_transfer send MODE 172.16.145.136 42002 /absolute/path/snapshot.bin

# 136的独立iperf3服务，每次新启动一个
iperf3 -s -1 -B 172.16.145.136 -p 42018 -J
# 135：默认对照
iperf3 -c 172.16.145.136 -B 172.16.145.135 -p 42018 -t 30 -P 1 -J
# 另新启服务，诊断组仅一轮
iperf3 -c 172.16.145.136 -B 172.16.145.135 -p 42018 -t 30 -P 1 -w 131072 -J
```

原生1MiB/window1，R交换后至最后A；setup/CRC单列，落盘/加载未执行。iperf保留sender与receiver，MiB/s与Gb/s精确换算；SIW不代表硬件RNIC。完整原始CLI（包括-P4与其他诊断）在data的transfer.rounds.commands中。

## 文档输出校验

```bash
git diff --check
python3 -B docs/performance/plot_results.py --final-data docs/performance/data/measurements-2026-11-26.json
```

检查所有新增SVG可解析、README/报告相对图片和文档链接可达、JSON中各组轮次和关键数值一致。Stage 5不为文档改动重复运行完整业务回归，不将历史PASS宣称为本阶段新执行。
