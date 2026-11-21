# P2：批量查重、批量入库与提交确认

日期：2026-11-20

分支：`feat/crawler-p0-observability`，延续 P1 的工作区修改。P1 验证 ZIP 保留，本阶段没有覆盖该包。

## P2-01：候选状态与去重

[Crawler](../src/net/crawler.cpp) 将 hash 统一为小写，用显式 Checking、Queued、Fetching、Indexing、Cached、RetryWaiting 状态替换原来的排序集合。每个 hash 最多保留 3 个 peer 地址（包括首选地址），重复公告只补充有限备用地址，不重复排队。

状态表最多 10,000 项；容量满时只淘汰已完成或等待重试的项，不遗忘仍在处理的 hash。成功或终止处理缓存 300 秒，失败等待 5 秒后允许新公告重新发现；失败不会永久屏蔽。每次查重携带候选 token 和运行 generation，重复回复或过期回复不能重复排队、修改新候选。备用地址的自动切换、定时重试与退避仍属于 P3；P2 的重新发现依赖新公告。

## P2-02：后台批量查重

[CrawlerPipeline](../src/services/crawler_pipeline.cpp) 默认在达到 100 条或最老任务等待 100 毫秒时提交。主线程只处理候选状态，实际 SQL 在 P1 专用线程执行。队列定时器按最老任务计算剩余时间，不因新任务到来不断推迟稀疏任务。

[checkMany](../src/data/torrent_repository.cpp) 用 docid 批量查询并核对完整 hash；结果区分 Known、Missing、Incomplete、Collision、Failed。已存在摘要还会批量检查文件详情，缺失或数量不足的详情返回 Incomplete，进入元数据补齐路径。查询错误不会被当成缺失，也不会触发整批元数据请求。仓库每次查询最多 500 个候选，并显式设置 LIMIT。

`crawler.known_lookup_calls` 现在计批次调用，`crawler.known_lookup_hashes` 计候选条数，不能将前者直接与 P1 的逐条调用计数相比。

## P2-03：后台批量写入

入库同样按 100 条/100 毫秒提交，复用 [IndexingService::insertBatch](../src/services/indexing_service.cpp)。同一个批次使用相同过滤配置版本；版本变化会拆批，已排队任务继续使用接纳时的不可变配置。

[Database](../src/data/database.cpp) 按转义后的实际 UTF-8 SQL 字节数拆分 INSERT/REPLACE，每条最多 1 MiB、500 个不同 ID。单行超过限制会明确拒绝；不会用大文件列表绕过包大小保护。详情先写入，只有详情确认成功的项才能提交摘要。

P1 的容量仍有效：数据库任务最多 256 条、估算 32 MiB，单项估算 16 MiB。排队、执行中、等待完成交付及等待通知的项都占用账本。内存估算不是 RSS 硬上限；高低水位、预留名额和完整背压留在 P3。

## P2-04：逐项提交结果与部分失败

批次结果为每个输入提供 hash、Outcome、阶段和错误信息，区分 Inserted、Merged、Rejected、Invalid、Collision、Failed、Duplicate。阶段包括 validation、lookup、filter、files、summary、votes、worker、reconcile；结果顺序不作为输入顺序保证，消费者按 hash 和状态处理。

- 每个 SQL 子批次分别记录已确认、结果不明、超限拒绝的 ID。
- 写入失败或确认丢失后重查 ID。详情须完整匹配预期 hash、路径和大小；摘要须匹配已在写锁内确认空闲的 hash。
- 只对确认新增项累计增量。摘要已写入但确认查询也失败时，统计标记为不精确，并尝试总量重算；重算恢复前索引入口拒绝继续写入。后续重放会取回待确认的新增凭据，不重复累计统计。
- 待确认凭据最多 10,000 项，容量不足时拒绝新批次写入，避免故障期间无界增长。该凭据表在内存中，不承诺跨进程崩溃的通知恢复。
- 文件表和摘要表仍不是事务。摘要失败可能留下孤立详情，后续重放可修复；本阶段不承诺跨表原子提交。

[DatabaseImporter](../src/services/database_importer.cpp) 遇到部分失败会返回失败并保留该帧之前的 offset；[DatabaseSyncService](../src/services/database_sync_service.cpp) 仅在成功完成后清除断点。重试可以重复已提交项，不能跳过失败项，也不会重复增加统计。

## P2-05：限速完成通知

爬虫批次只有确认 Inserted 的项进入主线程通知队列，每 50 毫秒最多分发 5 条；同一回复内的重复 hash 只通知一次。同时提供 batchIndexed 批量事件，并保留原有逐条通知总线，因此 tracker、活动界面和 REST 事件继续收到更新。合并、过滤、碰撞、失败、重复项不触发新增动作。

等待通知的完整结果仍占用容量，消费后才释放。停止时清空本地批次和通知，取消未开始批次，等待当前批次结束，然后在所属线程关闭 SQL。旧 generation 的完成事件不能回收新任务的名额。正常停机可能丢弃已提交但尚未通知的结果；队列和通知没有持久化。

导入继续直接调用批量索引服务，不进入爬虫通知管道，不启动海量 tracker 查询。爬虫/导入的通知行为由各自明确的调用入口区分。

## 自动验证与测量

Qt 6.10.1 / MinGW Release 构建成功。完整 CTest **31/31 通过**，耗时 127.17 秒；补充统计不精确/恢复用例后，最终爬虫与数据库专项 **4/4 通过**，耗时 8.10 秒。真实 SQL 故障测试使用独立临时 Manticore，不操作用户数据库。

3 秒应用启动/退出冒烟检查返回码 0，确认 crawler_running、isolated_database、peer_ingestion_disabled 为 true；它只验证生命周期，不作为网络吞吐证据。测量环境、基础版本和可执行文件 SHA256 记录于 [p2-environment.json](../benchmarks/crawler/results/p2-environment.json)，明确标记当前源码包含未提交修改。

使用 [受控数据](../benchmarks/crawler/results/p2-controlled.json)，选取默认 metrics_on、关闭逐条调试日志的三轮结果；每轮 300 个新种子，每个 4 个文件：

| 路径 | SELECT 调用/轮 | 写 SQL 调用/轮 | 入库服务吞吐（条/秒） | 调用 p95（毫秒） |
| --- | --- | --- | --- | --- |
| 逐条参考入口 | 600 | 600 | 939.6～960.4 | 1.150～1.185，单条调用 |
| 每 100 条一批 | 6 | 6 | 14,627.2～15,043.7 | 6.794～7.472，每次 100 条 |

固定输入下，SQL 调用总数从 1,200 降到 12。这里的吞吐是入库服务层测量，不包括网络、排队和新增通知；p95 的调用单位不同，不能据此声称逐条端到端延迟降低。数据来自同一程序的不同场景，场景执行时数据库规模有所增长，存在正常机器负载波动；未据此声称真实 DHT 爬取提升相同倍数。

[真实后台管道三轮测量](../benchmarks/crawler/results/p2-batch-pipeline.json) 每轮实际接纳、提交、通知 300 条，统计增加 300：

| 轮次 | 通知完成吞吐（条/秒） | 接纳到通知消费 p95（毫秒） | SELECT / 写 SQL | 账本峰值 |
| --- | --- | --- | --- | --- |
| 1 | 79.85 | 3,189.73 | 14 / 14 | 256 |
| 2 | 79.73 | 3,191.16 | 14 / 14 | 256 |
| 3 | 79.70 | 3,189.60 | 14 / 14 | 256 |

主线程 Database 查询/写入计数三轮均为零（未出现的指标键按零读取），估算字节峰值均为 195,984 字节。此处是一次突发 300 条的小文件负载，消费速度受到通知定时器和限速限制；Windows 实测约 80 条/秒，不能将配置的 5 条/50 毫秒当作保证达到 100 条/秒。后续接纳随通知释放名额，产生更多小批次，所以这里是 14+14 次 SQL，而不是服务层固定三批的 6+6。未测量 RSS 硬上限或真实网络吞吐。

复核命令：

```powershell
& .tools/Qt/Tools/CMake_64/bin/cmake.exe --build build-windows-x64 -j 4
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 --output-on-failure -j 4
& ./build-windows-x64/tests/crawler_benchmark.exe --output benchmarks/crawler/results/p2-controlled.json
& ./build-windows-x64/tests/crawler_batch_benchmark.exe --output benchmarks/crawler/results/p2-batch-pipeline.json
```

基准应在其它测试结束后顺序运行。两项会创建临时数据库；结果只包含固定名称指标与合成数据统计。

新增 [批量数据库故障测试](../tests/test_crawler_batch_indexing.cpp)，覆盖 SQL 拆批中途失败、摘要/文件写入确认丢失、确认查询不可用后恢复、重复提交、UTF-8 超限、详情修复、大小写和碰撞、导入断点。扩展 [管道测试](../tests/test_crawler_pipeline.cpp) 与 [候选测试](../tests/test_crawler_metrics.cpp)，覆盖批次门槛、稀疏提交、通知限速与容量、失败状态、有限备用 peer、晚到回复和重新发现。

可控吞吐与 SQL 次数使用 [crawler_benchmark](../benchmarks/crawler/crawler_benchmark.cpp)；端到端数据库队列/通知延迟使用 [crawler_batch_benchmark](../benchmarks/crawler/crawler_batch_benchmark.cpp)。后者包含 100 毫秒等待、容量排队及每秒约 100 条的通知限速，`crawler.task_latency_ms` 不包含 DHT/BEP 9 网络时间。`crawler.index_ms` 现在是批量调用耗时，不能与 P1 的单条调用耗时直接作倍数比较。
