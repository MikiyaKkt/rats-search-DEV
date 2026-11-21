# P3：容量账本、背压与有限重试

日期：2026-11-21。继续使用 `feat/crawler-p0-observability` 分支及已有 P0/P1/P2 修改。

## 容量及接纳规则

统一参数定义在 [crawler_limits.h](../src/common/crawler_limits.h)。

| 范围 | 硬限制及处理 |
| --- | --- |
| 公告 mailbox + 活跃候选（含查重、获取、入库、等待重试） | 合计 5,000 个名额；原子计数转移，不能各占 5,000；满时拒绝新公告并计数 |
| 候选状态缓存 | 最多 5,000 项；只淘汰 Cached 项，等待重试的候选不能被淘汰或由重复公告重置 |
| 元数据等待队列 | 延续 1,024 条 / 估算 1 MiB 限制；等待重试仍占发现名额 |
| 数据库任务 + 获取中结果预留 + 重试 + 待通知 | 合计 1,000 项 / 估算 64 MiB；任一达到上限均限制接纳 |
| 每次网络元数据请求 | 启动前预留 1 项和 4 MiB；收到结果后转为该项实际估算字节，不再二次申请名额 |
| 单个网络结果 | 转换前保守估算、转换后模型估算均不得超过 4 MiB；超限直接结束，不重试 |
| 直接调用流水线入库 | 保留单项估算 16 MiB 限制；仍受合计 64 MiB 约束 |
| 元数据并发 | 保持 10；不在 P3 提高并发 |

[Crawler](../src/net/crawler.cpp) 的公告回调将轻量 hash/peer 写入有界 mailbox，只在需要唤醒时向 Qt 投递事件；每轮最多处理 100 条，再让出事件循环。公告中的 IP 字符串最多 64 字节。结果交付最多对应 10 个实际网络请求，不再为每条公告创建一个携带捕获数据的 Qt 闭包。

[CrawlerPipeline](../src/services/crawler_pipeline.cpp) 的账本包含查询、写入、网络结果预留、SQL 重试以及尚未交付的通知。预留以 hash 对应唯一任务 ID；转换时保留原始接纳时间、过滤配置和版本。已经获取的结果可直接使用预留，避免被后来的查重请求占走容量。

上述字节是应用模型和载荷估算，**不是进程 RSS 硬上限**。librats 自己持有的原始元数据、Qt 容器和事件分配、SQL 驱动、数据库进程和其他业务不在此估算内。

## 高低水位

达到 800 项或估算 51.2 MiB（80%）后，暂停启动新的元数据请求；必须同时降到不超过 500 项、32 MiB（50%）才恢复。一次 4 MiB 预留可以跨过高水位，但不能超过总容量。暂停期间仍处理已有元数据、SQL、重试和通知，DHT 维护保持运行。

队列的 100 毫秒定时器检查恢复条件；容量不足时保留队首，不弹出后丢弃。高水位之后降到 70% 不恢复，避免频繁停启。恢复无需等待新公告。

## 错误及重试

| 结果 | 行为 |
| --- | --- |
| 元数据成功且有效 | 转换预留后入库 |
| 元数据 timeout | 自动从已保存的最多 3 个 peer 地址中轮换；最多重试 3 次，原始候选总时限 120 秒 |
| 元数据无效、非 timeout 失败、超限 | 结束并缓存；不进行网络重试 |
| 网络获取服务暂不可用、请求启动异常、等待队列暂满 | 在原候选名额内有限退避，不创建无限新任务 |
| 数据库查询失败、写入未确认、统计暂不能对账、后台异常 | 在原始任务和元数据上最多重试 3 次；接纳到再次调度的总时限 60 秒 |
| 已明确超出 SQL 行大小限制 | 仓库通过 `retryable=false` 逐项传递；直接结束 |
| 无效数据、过滤拒绝、ID 碰撞、重复、合并成功 | 不重试 |

数据库初始退避为 250 毫秒，指数增长并叠加 0–124 毫秒抖动；网络初始退避为 500 毫秒，叠加 0–249 毫秒抖动。使用单个周期定时器扫描等待队列，没有为每项创建独立计时器。网络候选及数据库任务分别最多 4 次尝试（首次 + 3 次重试）；查重重试耗尽后可能进入候选级重试，因此不能把这个数字描述成一个 hash 全部 SQL 调用次数的总上限。

数据库目前的仓库接口将连接、查询及未确认写入错误归为失败，未暴露每个底层 SQL 错误码；这些失败采取有限重试。超大行已明确标记为不可重试。部分提交继续复用 P2 的确认和幂等重放逻辑，避免重复统计或重复新增通知。重试耗尽后不会重新下载同一份元数据。

60/120 秒限制阻止超过期限的**新重试**，不能中断已经执行的阻塞 SQL 或 librats 请求；其实际结束时间仍受底层超时约束。

## 停止、重启与对账

停止爬虫后，不再启动重试；运行代次和查重 token 排除迟到回复。旧的 librats 请求没有单项取消接口，因此它们的 4 MiB 预留在停止、重新启动之间继续计入账本，直到结果回调处理结束。流水线停止时取消自己的排队/重试/通知任务，但保留这些外部预留。

应用退出先等待网络层停止，再通过 `releaseStoppedFetches()` 移除仍排队的 Qt 交付事件，释放最后的预留。这样不能在旧模型仍占用内存时，将同一份容量提前借给新一轮任务。

新增观测项：

- `crawler.discovery_slots`、`crawler.discovery_active`、`crawler.announce_dispatch_pending`：发现总名额、活跃候选、待处理公告。
- `crawler.announce_accepted/processed/cancelled`：公告接纳与收尾；拒绝单计 `announce_rejected_full`。
- `crawler.candidates/candidate_completed/candidate_cancelled`：候选接纳与收尾。
- `crawler.capacity_accepted/completed/cancelled`：流水线容量任务对账。`completed` 包含元数据失败后正常释放的预留，并不等于成功入库数。
- `crawler.result_reservations`、`crawler.db_retry_pending`、`crawler.metadata_paused`：预留、重试与暂停状态。
- `crawler.backpressure_pauses/resumes`、`crawler.db_retries/db_retry_exhausted`、`crawler.network_retries/network_retry_exhausted`、`crawler.metadata_rejected_large`。

无在途操作时，对应接纳量 = 完成量 + 取消量 + 剩余量；拒绝量不计入接纳量。保留的旧网络预留意味着流水线刚停止时 pending 可以暂不为零，网络收尾后必须归零。

## 验证

新增及扩展的 [流水线测试](../tests/test_crawler_pipeline.cpp)、[爬虫测试](../tests/test_crawler_metrics.cpp) 和 [真实数据库故障测试](../tests/test_crawler_batch_indexing.cpp) 覆盖：

- 6,000 条公告突发：仅接纳 5,000；处理完成后填补释放的名额，无丢失唤醒。
- 结果预留与现有任务共享账本，达到高水位暂停、只有低水位恢复，转换不重复记账。
- 单项及合计字节限制、1,000 项限制、超大结果释放预留。
- 10 个估算约 3 MiB 的结果连续到达，沿用此前 10 个预留，峰值估算保持在 40 MiB，没有二次申请名额，最终通知 10 次并清空账本。
- 数据库先失败后恢复，沿用同一任务、原过滤快照和已获取元数据，通知及结算各一次。
- 数据库持续失败最多 4 次尝试，已明确超大行只尝试一次。
- 自动轮换有限 peer，持续失败耗尽，无效数据及总期限不再重试。
- 等待重试期间停止、旧代次回复、跨重启保留网络预留、重复完成只释放一次。
- P2 的真实 Manticore 部分提交、确认丢失、统计对账、导入续传回归。

验证结果：

- Windows x64 Qt 6.10.1 / MinGW 13.1 增量构建通过，生成 [RatsSearch.exe](../build-windows-x64/bin/RatsSearch.exe)。
- 全量 CTest：31/31 通过，约 127 秒。日志：[p3-full-tests.log](../build-windows-x64/p3-full-tests.log)。
- 最终重点回归：爬虫、流水线、真实数据库故障、Manticore 查询共 4/4 通过；QtTest 分别为 12、15、10、34 个通过项（包含初始化/清理）。最后对拒绝与耗尽计数的补充修改也重新执行了这组回归。日志：[p3-focused-tests.log](../build-windows-x64/p3-focused-tests.log)。
- 3 秒临时数据库启动/停止冒烟通过，爬虫处于 Running，随后网络及数据库正常退出。原始结果：[p3-lifecycle-smoke.json](../benchmarks/crawler/results/p3-lifecycle-smoke.json)。它没有足够公告，不作为爬取吞吐证据。
- 源码及二进制摘要、测试记录：[p3-validation.json](../benchmarks/crawler/results/p3-validation.json)。

复现命令（先将本地 Qt/MinGW 工具目录加入 PATH）：

```powershell
& .tools/Qt/Tools/CMake_64/bin/cmake.exe --build build-windows-x64 -j 4
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 --output-on-failure -j 4
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 --output-on-failure -R 'test_crawler|test_manticore_queries' -j 3
```

本阶段不宣称真实 DHT 吞吐提升；长期压力运行和真实网络对照继续按 P4/P5 执行。P1 验证 ZIP 没有覆盖，本阶段没有额外生成发布压缩包。
