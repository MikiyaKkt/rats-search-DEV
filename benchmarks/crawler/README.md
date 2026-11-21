# P0 爬虫观测与基线工具

当前源码也包含 P2 批处理。P2 的接口、计数语义和部分提交限制见 [P2 报告](../../docs/CRAWLER_P2_REPORT.md)；历史 P0 结果保持原样。

## P2 可控测量

```powershell
& ./build-windows-x64/tests/crawler_benchmark.exe --output benchmarks/crawler/results/p2-controlled.json
& ./build-windows-x64/tests/crawler_batch_benchmark.exe --output benchmarks/crawler/results/p2-batch-pipeline.json
```

第一项在无网络的临时数据库中对比逐条和每 100 条一批的索引调用，记录 SQL 次数及调用耗时。第二项运行真实后台管道，每轮提交 300 项、重复 3 轮，验证统计和新增通知均为 300、主线程 Database SQL 计数为零，并记录从接纳到通知消费的 task_latency_ms。它包含批次等待、容量排队及每 50 毫秒最多 5 条的通知限速，不是 DHT 端到端吞吐。

P2 的 known_lookup_calls 计查重批次，known_lookup_hashes 计候选；index_ms 计入库批次。单条调用和批量调用的 p95 单位不同，不能直接比较倍数。任务延迟分位数仍使用最近最多 1024 个样本；每轮 300 项时覆盖完整测量窗口。应在没有其它测试或基准同时运行时执行测量。

本目录的工具最初用于 P0 基线；当前源码已实施 P1 专用数据库线程，见 [P1 报告](../../docs/CRAWLER_P1_REPORT.md)。保存的 p0 结果是当时二进制的历史测量；用当前源码重新运行会测量 P1，不能标记成原 P0 基线。工具本身不修改生产爬虫的并发和线程模型。生产指标从进程启动累计；受控工具仅在没有在途任务的边界 reset。既有文件显示修复是本轮测试前提。

## 构建与测试

在仓库根目录使用本地工具链，保留现有 CMake 配置：

```powershell
$env:PATH = "$PWD/.tools/Qt/Tools/mingw1310_64/bin;$PWD/.tools/Qt/6.10.1/mingw_64/bin;" + $env:PATH
& .tools/Qt/Tools/CMake_64/bin/cmake.exe -S . -B build-windows-x64
& .tools/Qt/Tools/CMake_64/bin/cmake.exe --build build-windows-x64 --target rats-search test_performance_metrics test_crawler_metrics test_manticore_queries test_download_ui crawler_benchmark crawler_live_probe -j 4
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 -R '^(test_performance_metrics|test_crawler_metrics|test_manticore_queries|test_download_ui)$' --output-on-failure
```

工具随测试目标构建，需要 Qt SQL/QMYSQL 及与驱动匹配的 libmysql。与现有数据库集成测试共用测试二进制目录；不得用 Manticore 自带的另一版本 libmysql 替换驱动依赖。资源采样脚本针对 Windows PowerShell 5.1，核心指标不依赖 Windows。

## 生产应用开关

默认启用轻量指标，每 5 秒输出一行 `[CrawlerMetrics]` JSON，正常停止时再输出最终快照。摘要可从应用现有日志中提取。每 200 毫秒探测一次主线程事件漂移，不通过忙等采样。

```powershell
$env:RATS_METRICS = '0'           # 完全禁用指标更新与定时摘要，下次启动生效
$env:RATS_VERBOSE_CRAWLER = '1'   # 开启逐条数据库/爬虫/入库调试日志，下次启动生效
```

不设置这些变量时，指标开启，成功路径逐条日志默认关闭。调试分类名为 `rats.crawler.detail`；标准 Qt 分类规则仍可使用，但正式应用还受现有 librats 日志等级限制，建议通过 RATS_VERBOSE_CRAWLER 一并开启。不要在性能对比过程中改变日志或 tracker 配置。

## JSON 指标口径

schema=1；没有出现的计数键表示此进程尚无相应事件。禁用时不更新历史，不能将禁用期间解释为零流量。值名称以 `_ms`、`_bytes` 等标明单位。

| 分组 | 字段/含义 |
| --- | --- |
| counters | 累计事件数，不随摘要输出清零 |
| gauges | current 当前值、peak 进程内峰值 |
| samples | count/mean/max 为进程累计；retained/p50/p95 为最近最多 1024 个样本的精确排序分位数（nearest rank） |
| elapsed_ms | 指标初始化/受控 reset 后的单调时钟耗时 |
| interval_ms | 距上一次摘要的时间 |
| inserted_per_minute | 本摘要间隔 crawler.index_inserted 增量折算每分钟；不是累计平均 |
| metadata_success_ratio | 成功完成数 /（成功完成数 + 失败完成数）；无完成时 null |
| metadata_timeout_ratio | 超时完成数 / 全部完成数；超时是失败的子集，无完成时 null |

每种存储最多接纳 128 个固定名称，样本容量固定；不按 hash、SQL、路径或 peer 生成指标键。snapshot 在锁内复制、锁外排序。分位数是最近样本窗口，不等于“最近 5 秒”或实时探针的整个测量区间；不得对两份分位数相减。区间计数用结束值减起始值。

### 爬虫计数和队列（以下保留 P0 口径）

P1 的生产 known lookup/index 调用已转到工作线程；`metadata_inflight` 现在按任务账本结算，包含等待 Qt 处理的完成事件，避免旧请求在重启后占用未计数的实际名额。新增 `crawler.db_*` 队列、取消、异常、配置版本指标及入口拒绝指标，详见 [P1 报告](../../docs/CRAWLER_P1_REPORT.md)。已确认新增仍转发至共享 IndexingService 通知总线。

- `crawler.announcements`：网络公告回调接收次数；`announce_dispatch_pending`：尚未在 Qt 线程处理的公告数，另有其估算字节数与 `announce_dispatch_ms` 排队耗时。
- `crawler.recent_duplicates`：命中当前内存去重集合；`crawler.candidates`：未命中该集合的候选。当前集合按 hash 排序淘汰一半，**并非按时间保存的历史，也不能解释为全生命周期唯一 hash 数**。本阶段不修改其行为。
- `crawler.known_lookup_calls/known_lookup_ms`：同步已收录检查次数和耗时；`already_indexed`：检查返回已存在。
- `metadata_queued`：入元数据队列次数；`metadata_queue_items/metadata_queue_estimated_bytes/metadata_queue_oldest_ms`：当前队列、峰值、最老任务年龄；`metadata_queue_wait_ms`：开始获取前的排队耗时。
- `metadata_attempts`：实际调用元数据接口次数；`metadata_inflight`：已开始但未返回的请求；`metadata_fetch_ms`：请求完成耗时；`metadata_succeeded/metadata_failed/metadata_timeouts`：互斥成功/失败及超时子集。超时目前按 librats 返回的 timeout 文本识别，后续应改为类型化错误。
- `delivery_pending/delivery_estimated_bytes`：已取得元数据、尚未交给 Qt 入库路径的结果；`delivery_wait_ms`：已交付结果的排队耗时。Qt 结果事件没有可枚举队列，本阶段不声称已测得它的当前最老年龄。
- `delivered`：交付完成；`index_inserted/index_merged/index_rejected/index_failed`：仅爬虫来源、按入库服务返回结果分类；`index_ms` 为完整同步入库调用耗时。
- `running/legacy_active_fetches`：现有爬虫运行标志及其活动计数；`network.dht_nodes`：摘要时的 DHT 节点数。观测不修复既有停止/重启计数问题，后续 P1 单独处理。

estimated_bytes 是对象及字符串载荷估算，排除 Qt 事件、allocator、共享对象和库内部内存，**不是 RSS 或内存硬上限**。P0 没有实现重试、背压丢弃或专用待入库队列，相关能力为“尚未实现”，不是“已实现且计数为零”。

### 数据库与所有来源入库

- `db.query_calls/query_ok/query_failed`、`write_calls/write_ok/write_failed` 在成功和错误分支均统计；`query_ms/write_ms` 包含连接打开和完整同步调用。
- `db.main_query_calls/main_write_calls` 及对应耗时，只计 QCoreApplication 所属线程。它们与后台调用的差值能确认主线程数据库负担；不包含直接绕过 Database 的健康检查 SQL。
- `db.statement_bytes`：实际写 SQL 的 UTF-8 字节数；`rows_per_statement`：INSERT/REPLACE 每个真实拆分语句包含的行数，不代表受影响行数，也不包括 UPDATE/DELETE/DDL。
- `index.single_attempts/inserted/merged/invalid/rejected/collided/failed` 覆盖所有通过入库服务的来源；不能直接当作爬虫产出。`single_ms/dedupe_ms/classify_ms/filter_ms/persist_ms` 区分各环节；持久化耗时包含 SQL 序列化。
- `index.batch_*` 记录批次输入、重复、合并、无效、过滤、碰撞等；`batch_inserted_reported` 是现有接口报告的新增，`batch_unconfirmed` 是新候选数与报告新增的差，**不能据此精确判断部分提交结果**。P2 才升级批量确认模型。

## 可控数据库基线

其中 index.batch_attempts 计输入条目数，不是批次数；index.batch_size 的样本数是批量接口调用次数。

```powershell
New-Item -ItemType Directory -Path benchmarks/crawler/results -Force | Out-Null
& ./build-windows-x64/tests/crawler_benchmark.exe --output benchmarks/crawler/results/p0-controlled.json
& ./benchmarks/crawler/summarize.ps1 -InputJson benchmarks/crawler/results/p0-controlled.json
```

运行 3 轮，每轮轮换 metrics_off、metrics_on、verbose 顺序。每种模式建立独立临时 Manticore，预热 12 条；依次执行新种子逐条入库、全部重复、256 文件/种子的大列表、每批 100 条的新种子批量入库。前三个常规/批量场景各 300 个输入，大列表场景 60 个。重复场景的预置数据和所有 fixture 构造都排除在计时外。

每个场景验证新增/合并数、仓储统计与真实数据库 COUNT 一致，并抽查首尾文件列表。文件写日志的 flush 包含在计时内。verbose 使用本地 QFile 日志接收器，测量的是这套接收器的日志成本，不等同于正式应用日志的全部成本。

基线从 IndexingService 输入，不含 DHT、元数据网络耗时、主线程事件积压和 tracker。fixture 已有内容类型，分类器未纳入此基线。写入时间为 Manticore RT 接收时间，未强制逐条磁盘 flush；数据库启动、fixture 生成和停止不计时。新批量场景测量既有接口能力，并不表示爬虫已改为批量入库。

metrics_off 是同一二进制关闭指标的参照，仍有插桩分支及既有计时开销，不是改造前的源码二进制。短用例对机器噪声敏感，必须报告三次范围和中位数，不据此宣称稳定提速倍数。

## 真实网络探针与外部资源采样

```powershell
# 短冒烟测试
& ./benchmarks/crawler/run-live.ps1 -Seconds 20 -WarmupSeconds 0 -Runs 1
# 计划中的正式采样：3 ×（预热 5 分钟 + 测量 15 分钟）
& ./benchmarks/crawler/run-live.ps1 -Seconds 900 -WarmupSeconds 300 -Runs 3
# 三轮完成后汇总；InputDirectory 必须指向上述命令实际输出目录
& ./benchmarks/crawler/summarize-live.ps1 -InputDirectory benchmarks/crawler/results/live -OutputJson benchmarks/crawler/results/p0-formal-summary.json
```

探针使用独立临时数据库和临时网络端口，关闭 tracker、复制、共享服务、UPnP、打洞和中继，并在探针内屏蔽 peer feed/公告等入库来源，防止污染 DHT 基线；正式应用不受此隔离改动影响。开始/结束快照及测量 UTC 时间写到 JSON；不在活动请求中 reset。预热阶段收录的数据保留用于测量期已知 hash 查重。

每秒资源 JSONL 记录探针进程的 CPU 秒、单核百分比、RSS、峰值 RSS、private bytes 和线程数，**不含 searchd 子进程的资源**，不作为整机资源报告。用每行 UTC 对齐结果 JSON 的测量 UTC；phase_estimate 仅按进程启动时间粗分，含启动误差。20 秒无公告仅说明短窗口没观察到，不能断言网络整体不可用或吞吐量为零。

探针只保存指标结果，临时数据库退出后删除；stderr/stdout 是诊断日志，可能包含 peer 来源或种子名称，默认被仓库忽略，不应作为公开报告附件。长测原始资源 JSONL 较大，默认不提交；正式报告保存汇总和必要的无内容快照。

正式汇总器要求指定数量的完成轮次，验证隔离标志、有效测量时长、计数单调性、周期快照数量；资源样本首尾覆盖须达到 95%，采样密度至少为目标 1 Hz 的 90%，最大间隔不超过 10 秒。按 UTC 排除启动、预热和退出样本，CPU 平均值使用实际覆盖区间。每轮同时导出测量期 `.metrics.jsonl`，只包含无内容指标；最近样本分位数仍不得解释为整个 15 分钟的分位数。

## 回归检查清单

- [ ] 成功/失败 SQL 与主线程计数可对账，查询错误不误计为成功。
- [ ] 单条入库新增、已有、非法、过滤等结果计数可对账；批量结果保留现有语义限制。
- [ ] 多线程并发更新指标无丢计数，样本和键数量达到上限后不增长。
- [ ] 指标关闭后不再采样，定时摘要不开启。
- [ ] 公告去重、已知 hash 跳过、队列长度/字节/年龄观测正确。
- [ ] 文件片段不冒充完整列表、缺失详情可补齐、部分详情不覆盖完整列表。
- [ ] 既有 ID 碰撞、导入导出和统计测试通过。
- [ ] 实时日志每 5 秒出现快照，资源采样输出可解析；没有完成的网络长测明确标记未测。


## P3 验证记录

P3 的新参数和背压/重试语义见 [P3 实施报告](../../docs/CRAWLER_P3_REPORT.md)。[验证摘要](./results/p3-validation.json) 保存本地构建、测试及源码/二进制 SHA-256；[隔离生命周期冒烟结果](./results/p3-lifecycle-smoke.json) 仅验证启动与停止。未测量 P3 的真实网络吞吐或 RSS，不将 P0/P2 历史测量解释成 P3 的性能结果。

## P4 并发对照

[P4 报告](../../docs/CRAWLER_P4_REPORT.md) 说明新配置、热更新和观测口径。构建 `crawler_concurrency_probe`、`crawler_live_probe` 后，运行 `./benchmarks/crawler/run-concurrency.ps1 -Runs 3`，再执行 `node benchmarks/crawler/summarize-concurrency.cjs`。完整脚本需要事先保存的 P3 固定 10 并发探针；仅复现可控档位可添加 `-ControlledOnly`。

[原始记录与汇总](./results/p4/summary.json) 分别保存 10/20/40 的三轮可控入库、12 个短网络窗口及底层等待线程采样。可控输入固定 50 毫秒元数据延迟和 300 个种子，不代表真实网络；各轮使用独立临时数据库。网络窗口没有公告，不能计算真实吞吐、成功率或 p95，带宽未测量，标为 null。默认值维持 10，未测试 64，没有改写底层等待线程。[验证摘要](./results/p4-validation.json) 保存源码/二进制摘要及全量测试结果。以后测量可通过 `-OutputDirectory` 选择新目录，保留本次证据。

## P5 故障与两小时稳定性验证

[P5 报告](../../docs/CRAWLER_P5_REPORT.md) 记录确定性调度、回调异常、实际数据库断连恢复及长时运行。构建 `crawler_soak_probe`、`crawler_live_probe` 后运行：

```powershell
& ./benchmarks/crawler/run-soak.ps1 -Seconds 7200 -Cycles 100 -OutputDirectory build-windows-x64/p5-new-controlled
# 如果当前沙箱不能访问公网，在获准的网络环境执行同一份 live 二进制快照：
& ./benchmarks/crawler/run-network-soak.ps1 -Seconds 7200 -Cycles 100 -OutputDirectory build-windows-x64/p5-new-network
node benchmarks/crawler/summarize-soak.cjs build-windows-x64/p5-new-controlled build-windows-x64/p5-new-summary.json build-windows-x64/p5-new-network
node benchmarks/crawler/archive-soak.cjs build-windows-x64/p5-new-controlled build-windows-x64/p5-new-network build-windows-x64/p5-new-archive
```

`run-soak.ps1` 保存探针快照、SHA-256 和 PID；不要在长测期间重新运行该脚本覆盖快照。`run-network-soak.ps1` 使用已保存的 live 快照，汇总核对两次运行的版本及参数一致。网络可用时也可以只运行双探针脚本，并省略汇总器的最后一个网络目录参数。

受控输入为每秒 8 个候选，真实 SQL 前注入短暂故障，100 次启停分散到两小时；旧代次元数据回调在重启后重复返回。网络探针独立运行真实 DHT，关闭其他入库来源。两者都使用临时数据库；采样覆盖探针进程，不包含 searchd 或整机带宽。受控吞吐与公网吞吐必须分别解释。

每次选择新输出目录。创建输出目录内的 `STOP` 文件可正常取消；结果会标记取消，不能拼接成合格时长。汇总要求真实墙钟不少于两小时、至少 100 次启停、停止后连接和容量归零、统计精确；采样覆盖不少于 95%、密度不少于 0.9 Hz、最大间隔不超过 10 秒。

归档只保存无种子内容的结果 JSON 与资源 JSONL，gzip 后逐个解压核对 SHA-256。原始文件和可能包含 peer/种子信息的 stdout/stderr 留在本地构建目录，排除在归档之外。

本次结果：[两小时汇总](./results/p5-soak-summary.json)、[原始结果与压缩资源索引](./results/p5/archive.json)、[源码/构建/测试摘要](./results/p5-validation.json)。两次独立运行各完成 100 次启停，连接与容量停止后归零；资源仍有小幅增长，限制和完整数值见报告。公网仅 4 次尝试、1 条新增，默认并发继续保留 10。
