# P4：抓取并发配置、热更新与对照测量

日期：2026-11-21。分支：`feat/crawler-p0-observability`，延续 P0–P3 的工作区修改。

## P4-01：配置、保存和界面

[ConfigStore](../src/app/config_store.cpp) 新增三个字段，沿用统一的校验、保存和 `configChanged` 通知路径：

| JSON 字段 | 默认值 | 范围 | 含义 |
| --- | --- | --- | --- |
| `spider.metadataConcurrency` | 10 | 1–64 | 元数据获取的配置上限 |
| `spider.pendingLimit` | 1,000 | 1–1,000 | 查重、结果预留、写入、重试及通知共用的任务上限 |
| `spider.batchSize` | 100 | 1–100 | 数据库单批最大任务数 |

旧配置缺少字段时补默认值。整数超范围时钳制；不可转换、布尔值、非有限或非整数输入回落至字段默认值。加载、类型化 setter、通用 `setValue` 和 `config.set` 的嵌套/点分键使用同一规则。批次最老等待仍为 100 毫秒；字节硬限制仍为估算 64 MiB，单个元数据结果预留仍为 4 MiB，避免同时开放过多调优选项。

[设置界面](../src/ui/settingsdialog.cpp) 的 Indexer → Spider Performance 提供新控件，并单独显示实际活动数与已保存上限。未保存的输入不会冒充运行中的配置。补充了中文翻译；主窗口 Spider 指示器的提示也显示实际数、配置上限及等待容量状态。

[stats.get](../src/rest/api_router.cpp) 保留既有字段，并新增 `crawler` 对象：`running`、`metadataConcurrency`、`activeMetadata`、`waitingForCapacity`。无界面客户端可用它检查运行状态。设置用法见 [API 文档](./API.md) 和 [用户手册](./MANUAL.md)。

## P4-02：安全热更新

[Application](../src/app/application.cpp) 将配置推送到 [Crawler](../src/net/crawler.cpp) 和 [CrawlerPipeline](../src/services/crawler_pipeline.cpp)。

- 提高并发后投递队列调度，在发现队列和结果容量允许时启动更多请求。
- 降低并发不撤销在途请求；即使当前活动数大于新值，也等待它自然下降再启动新请求。
- 降低待处理任务上限不驱逐已接纳任务，包括旧代次仍持有的网络结果预留。仅限制后续接纳。
- 修改批次大小作用于后续分批；已经发给工作线程的批次继续执行。
- 高水位为任务配置上限的 80%（向上取整）或字节预算的 80%；低水位须同时满足两种预算的 50%。上限为 1 时高水位为 1、低水位为 0，避免整数取整造成空账本也永久暂停。

新增 `metadata_configured_limit`、`metadata_budget_blocked`、`db_pending_limit`、`db_batch_limit` 指标；实际在途数继续用 `metadata_inflight`。新配置不增加数据库工作线程，也不绕过 P3 的预留与背压。

## P4-03：10、20、40 档对照

测量工具和原始数据：

- [crawler_concurrency_probe.cpp](../benchmarks/crawler/crawler_concurrency_probe.cpp)：使用真实候选状态、异步查重、结果预留、批量入库和节流通知；只将网络元数据获取替换为固定约 50 毫秒后成功的可控输入。
- 每次独立临时 Manticore，300 个不同 hash，每个 4 个文件；队列检查仍为生产默认 100 毫秒，其他容量、批次和通知参数固定。各档独立运行 3 次。
- [run-concurrency.ps1](../benchmarks/crawler/run-concurrency.ps1) 同时采样进程线程数、工作集、私有字节和 CPU；[汇总脚本](../benchmarks/crawler/summarize-concurrency.cjs) 检查成功数、统计精确性、容量上限与结束清空后生成 [summary.json](../benchmarks/crawler/results/p4/summary.json)。
- 端到端 p95 从每个 hash 进入候选路径计时，到该 torrent 的新增通知交付；包含查重、元数据队列、模拟获取、SQL 和通知等待。它与只统计 SQL 工作或任务预留后的延迟不同。

三次运行范围：

| 配置并发 | 实际并发峰值 | 成功入库且通知/分钟 | 端到端 p95 | 进程线程峰值 | 采样工作集峰值 | 平均 CPU（一个逻辑核） |
| --- | --- | --- | --- | --- | --- | --- |
| 10 | 10 | 4,461–4,521 | 3,790–3,841 ms | 8 | 21.29–21.32 MiB | 70.6%–72.0% |
| 20 | 13 | 4,234–4,534 | 3,767–4,048 ms | 8–9 | 21.48–21.49 MiB | 72.4%–77.9% |
| 40 | 13 | 3,700–4,529 | 3,786–4,606 ms | 8 | 21.40–21.43 MiB | 68.0%–77.4% |

9 次均成功写入并通知 300 条，数据库统计精确，模拟元数据成功率为 100%。任务峰值为 300，元数据等待队列峰值为 300；估算待处理字节峰值分别约 40.04、52.10、52.11 MiB，均不超过 64 MiB，结束时在途请求和账本归零。

配置 20/40 时并未同时启动这么多网络请求：每次未完成获取先预留 4 MiB，达到 80% 高水位后暂停，此场景实际峰值为 13。较高配置缩短了元数据等待的一部分，但最终交付仍受现有通知速率等环节约束；较慢的一次运行也保留在范围内，没有用最快单次代表结果。可控输入不创建 librats 等待线程，所以这一表不能用于推断真实网络线程开销。

**默认值继续保留 10；未试验 64。** 20/40 没有显示稳定的最终吞吐收益，尚不满足继续升档的条件。

真实网络对照分别执行 P3 固定 10 与 P4 的 10、20、40，各 3 次、每次测量 10 秒，串行运行并关闭其他入库来源。P3 对照使用修改前保留的探针二进制，摘要记录在 [measurement-binaries.json](../benchmarks/crawler/results/p4/measurement-binaries.json)。12 个窗口均为 0 公告、0 元数据请求、0 新增，DHT 节点结束数为 0。

因此这些短窗口只提供生命周期和环境记录，**不能计算真实网络成功率、p95 或吞吐提升**，也没有资格支持提高默认值。它们不替代 P0 的正式长窗口测量或 P5 的真实网络长稳验证。

全部资源采样只覆盖探针进程，不包含 searchd、操作系统和其他业务；可控场景和底层等待线程场景的采样开销也是测量的一部分。P3 只有外部采样，P4 网络探针另有内部采样，不能把两者 CPU/RSS 差异全部归因于并发实现。**未测量线上的字节带宽，JSON 明确保留 null**；模拟元数据载荷不是网络带宽。估算账本和实际工作集分别报告。

## P4-04：底层等待线程评估

源码 [Bittorrent::fetch_metadata_impl](../src/librats/src/librats/subsystems/bittorrent.cpp) 当前每次调用创建一个等待 `condition_variable` 的线程；超时/元数据完成后回调并退出。单独的隔离探针直接向本机不可用端口发起 1.5 秒超时请求，观察 3 秒，不经过爬虫预留账本；它用于确认线程形态，不是生产爬虫的容量测试。

| 底层请求数 | 启动前线程 | 峰值线程 | 最后线程 | 完成回调数 |
| --- | --- | --- | --- | --- |
| 10 | 30 | 40 | 30 | 10 |
| 20 | 30 | 50 | 30 | 20 |
| 40 | 30 | 70 | 30 | 40 |

进程采样工作集峰值约 24.73、25.24、25.98 MiB，平均 CPU 约 41.8%、44.7%、43.3%（单逻辑核百分比）；这些是整个探针和网络运行时的值，没有证明等待线程是主要 CPU 开销。三组均结束回落，回调数与请求数一致。

本次完成底层开销评估，**保留现有等待线程实现**。生产爬虫仍受结果预留约束；没有稳定吞吐收益和明确的等待线程主导证据，不在 P4 改写 librats reactor 或用户下载所有权。后续若长时间真实流量 profile 证明需要，再单独实施有界状态表/统一超时重构。

## 验证与复现

新增回归覆盖旧配置、异常输入、点分/嵌套 API 更新、保存重载、配置通知到服务、stats 状态；并发升降、乱序/重复完成、背压恢复、停止后旧回调；降低任务容量不驱逐已接纳项、批次热更新及容量为 1 的水位边界。

Windows x64 / Qt 6.10.1 / MinGW 13.1 全量增量构建通过；最终 CTest **31/31 通过**，约 127.19 秒，日志见 [p4-full-tests.log](../build-windows-x64/p4-full-tests.log)。配置、爬虫状态及流水线 QtTest 分别为 15、14、16 个通过项（包含初始化/清理）。源码、工具、二进制 SHA-256 与本次验证摘要见 [p4-validation.json](../benchmarks/crawler/results/p4-validation.json)。没有覆盖既有 P1 发布验证 ZIP，也没有额外生成 P4 ZIP。

```powershell
# 先将本地 Qt/MinGW bin 加入 PATH
& .tools/Qt/Tools/CMake_64/bin/cmake.exe --build build-windows-x64 -j 4
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 --output-on-failure -j 4
& ./benchmarks/crawler/run-concurrency.ps1 -LiveSeconds 10 -Runs 3
node benchmarks/crawler/summarize-concurrency.cjs
```

完整对照需要已保存的 P3 探针 `build-windows-x64/tests/crawler_live_probe_p3_baseline.exe`；没有该文件时可使用 `-ControlledOnly` 复现可控档位测试。将来延长测量应使用独立输出目录，避免覆盖本次原始记录。生产默认值的决定仍以可用证据为界，真实网络吞吐和长时间稳定性由 P5 继续验证。
