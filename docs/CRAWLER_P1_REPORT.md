# P1：后台数据库任务与写入一致性

日期：2026-11-20  
分支：`feat/crawler-p0-observability`（延续 P0 当前分支）

## 实施范围

爬虫数据库查重和逐条分类、过滤、合并、入库改由单个 `crawler-database` 工作线程执行。网络元数据并发上限仍为 10。P1 交付线程隔离、写入一致性和安全启停；批量查重、爬虫批量写入、重试、公平背压和并发参数调优留在 P2～P4。

## P1-01：写入矩阵

| 入口 | 执行线程 | 一致性边界 | 统计与通知 |
| --- | --- | --- | --- |
| DHT 爬虫 | 专用数据库线程 | [CrawlerPipeline](../src/services/crawler_pipeline.cpp) → [IndexingService](../src/services/indexing_service.cpp)，查重、文件补齐、票数合并与新增受同一仓库写协调器保护 | 新增确认后排队回主线程，再调度 tracker；合并不重复通知新增 |
| P2P、REST 单条收录 | Qt 主线程 | 相同同步 IndexingService 入口 | 沿用新增通知 |
| 本地或远程 dump 导入 | QtConcurrent 工作线程 | insertBatch 的查询、合并、整批写入处于同一边界 | 批次更新统计，不发送海量逐条 tracker 通知 |
| 本地投票 | Qt 主线程 | [incrementVote](../src/data/torrent_repository.cpp) 内读取最新计数并局部更新 | 不再使用旧 Torrent 快照 REPLACE 整行；保留投票通知 |
| 远程票数聚合 | Qt 主线程 | updateVotes 仅更新 good/bad | 不覆盖文件、tracker 或 info |
| tracker 数量、info 补齐 | Qt 主线程 | updateTrackerCounts、mergeInfo 在仓库协调器内检查身份并写入 | 局部更新不改变新增统计 |
| 文件详情补齐 | Qt 主线程，或索引合并所在工作线程 | updateFiles 的身份检查、详情写入、摘要计数、统计受同一边界保护 | 沿用详情通知；索引合并仍拒绝短列表覆盖完整列表 |
| 删除 | Qt 主线程、异步迁移线程 | 身份查询、删除详情和摘要、扣减统计处于同一边界 | 零大小、零文件实体也正确扣减数量；SQL 失败返回失败 |
| ID 迁移、分类迁移 | 启动主线程、QtConcurrent 工作线程 | migrateRowIdPage 或局部分类更新受同一边界保护 | 应用退出先请求停止并等待迁移结束 |
| 整行替换 update | 当前生产入口已改为专用局部更新；保留维护/测试 API | 如需读取后修改整行，调用者必须让 WriteGuard 包围读取与 update | 不允许在锁外读取快照后把它作为并发合并接口 |

协调器是仓库内的非 QObject 递归互斥量。已有同步入口与新异步爬虫共用它；没有移动 Application 共享仓库或共享服务的线程归属。仓库的同步数据方法经审核支持跨线程调用，Database 不保存共享 QSqlQuery，Manticore 按实际调用线程创建连接。QObject 定时器、配置读取、tracker 和界面只在各自所属线程运行。

通知先在最外层写操作内收集，再按提交顺序排队到仓库所属线程；不在写锁内直接调用外部槽函数，也不在锁内等待主线程。IndexingService 的新增信号在释放写锁后发送。

## P1-02：一致性修正

- 单条和批量索引的查重、合并、提交使用同一写协调器；直接 add/addMany 也会在边界内重新核对已有记录，过期 RowSlot 无法绕过检查。
- 新输入的 hash 在索引入口统一为小写，批量查询按请求 hash 返回对应记录，保留完整 hash 的 ID 碰撞校验。
- 投票更新不再把旧文件数量、tracker 状态或 info 覆盖回数据库。
- 查询失败不再被索引入口当作空槽；批量查询失败终止本次写入。
- 统计读取仍由独立统计锁保护，修改仅发生在受串行保护的成功写入后。
- 双表不是事务：详情写入成功而摘要失败仍可能留下孤立详情；本阶段不承诺跨表原子性或进程崩溃下的零丢失。

## P1-03 / P1-04：工作线程与容量

[crawler_pipeline.h](../src/services/crawler_pipeline.h) 定义提交结果 Accepted、Stopped、Full、TooLarge。主线程提交即返回，数据库结果通过排队完成事件交付。工作线程内构造本地 FilterPolicy、IndexingService，使用按线程创建的 SQL 连接；没有从工作线程读取 ConfigStore。

| 边界 | 默认限制 | 拒绝/回收行为 |
| --- | --- | --- |
| 网络公告进入 Qt 的待分发事件 | 512 条 | 在网络回调中先限量，再创建 Qt 排队事件 |
| 数据库任务（查重和入库共享） | 256 条、估算 32 MiB，含执行中与等待主线程处理的结果 | 返回 Full，增加计数；已完成但未消费的结果继续占用名额 |
| 单个入库任务 | 估算 16 MiB | 返回 TooLarge；不进入工作线程 |
| 待启动元数据队列 | 1024 条、估算 1 MiB | 拒绝新候选；释放其近期去重标记，允许以后公告重试 |
| 元数据实际请求 | 10 个，跨开关重启仍计算旧请求 | 旧请求结算后才能重新使用名额 |

容量基于 QString、文件路径和 info 等载荷估算，不等价于进程 RSS 的硬上限。共享队列当前不区分查重与入库优先级，队满的入库结果可能被丢弃并计数；可靠重试和公平调度属于后续阶段。

过滤配置在主线程保存不可变值快照及版本，每条已接纳任务携带提交时的配置；更新不改变已排队任务的语义。指标 `crawler.db_filter_version` 表示最近执行的配置版本。

## P1-05：启停策略

控制器状态为 Stopped → Starting → Running → Draining → Stopped。每轮运行有 generation，每项任务有单调 task ID 与在途账本。

关闭爬虫开关时：停止公告接纳和定时器、注销网络公告回调、清空未启动元数据队列并使旧查重回复失效；数据库任务停止接纳、取消尚未执行的任务、等待当前执行结束，在工作线程关闭并移除 SQL 连接后退出并 join。工作线程不等待主线程事件，因此主线程阻塞 join 不会造成循环等待。

元数据请求由 librats 管理，单独关闭爬虫时不会强行终止它们；原名额继续占用。晚到请求先按 task ID 结算一次，旧 generation 的成功结果不再发送 discovered。应用退出时 transport 会唤醒并 join 元数据等待线程，随后显式清理已取消的账本；销毁前的共享回调门阻止网络回调访问已析构的 Crawler。

应用退出顺序见 [Application::stop](../src/app/application.cpp)：关闭 REST → 停止爬虫生产 → 停止 tracker、异步迁移 → 保存会话/Feed、停止复制 → 等待导入/导出生产者 → 清退数据库管道 → 关闭网络并清理元数据账本 → 最终指标 → 关闭 Manticore。异步迁移和导入/导出任务也在所属工作线程释放自己的 SQL 连接。

SQL 连接配置连接超时 3 秒、单次读/写超时 5 秒。清退采用 10 秒软期限：超过期限记录警告，继续安全等待，不强杀线程或提前关闭数据库。它不是整个退出过程 10 秒的硬保证；一个写操作可包含多条 SQL，还可能等待其它写生产者释放协调器。

所有候选、元数据结果、数据库任务队列均未持久化。正常停机也可能取消未执行任务、丢弃未交付结果；重新启动依赖后续公告重新发现。`crawler.db_cancelled` 记录未开始任务，`crawler.db_shutdown_unreported` 记录关闭时仍未由主线程消费的账本项（可能已执行，不能当作丢失条数）。

## 验证

本地 Qt 6.10.1 / MinGW Release 构建通过，完整 CTest **30/30 通过**，耗时 183.87 秒。最终专项回归再次执行 crawler_metrics、crawler_pipeline、manticore_queries，**3/3 通过**，耗时 4.04 秒；包含新增的字节容量、大小写 hash、过期槽碰撞保护和共享新增通知转发断言。真实数据库测试合计 34 项（含初始化与清理）通过。

真实数据库回归确认：三个生产者反复以单条和批次插入同一 hash 只新增一行；投票、tracker、文件列表和 info 并发更新互不覆盖；新增/删除交错不重复累计统计；连续三次启动/停止工作线程后连接数回到初值；爬虫后台查重和写入期间 `db.main_query_calls`、`db.main_write_calls` 没有增加。

使用独立临时数据库执行 `crawler_live_probe --seconds 3 --warmup 0`，返回码 0，记录 crawler_running、isolated_database、peer_ingestion_disabled 均为 true，正常停止网络与应用。该短时检查只用于启动/退出冒烟，不是吞吐基准，不替代 P0 的正式网络采样。

复核命令（仓库根目录，本地 Qt/MinGW 已加入 PATH）：

```powershell
& .tools/Qt/Tools/CMake_64/bin/cmake.exe --build build-windows-x64 -j 4
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 --output-on-failure
& .tools/Qt/Tools/CMake_64/bin/ctest.exe --test-dir build-windows-x64 -R '^(test_crawler_pipeline|test_crawler_metrics|test_manticore_queries)$' --output-on-failure
```

修正了旧批次去重测试的夹具：原测试复用已经存在的 hash，却期望覆盖并增加统计；现在使用独立 hash 验证批内去重，同时验证重放不覆盖既有行、不重复累计统计。没有放宽生产一致性要求。

新增回归见 [test_crawler_pipeline.cpp](../tests/test_crawler_pipeline.cpp)、[test_crawler_metrics.cpp](../tests/test_crawler_metrics.cpp)、[test_manticore_queries.cpp](../tests/test_manticore_queries.cpp)：慢查询期间事件循环响应、容量拒绝、任务取消、重启隔离、异常处理、配置快照、新增通知、旧元数据单次结算、析构安全，以及真实 Manticore 下并发索引/删除/投票/文件补齐/info/tracker 写入与连接清理。

本次不以线程迁移本身声称爬取吞吐提高；真实网络 P0 样本只有少量输入与超时，无法支持可靠的加速倍数判断。

## 本地验证包

已构建 [Windows x64 P1 验证包](../dist/RatsSearch-Windows-x64-crawler-p1.zip)，使用当前分支含未提交修改的 Release 源码。人工效果确认仍为待验证，操作清单见 [P1 验证说明](./CRAWLER_P1_VALIDATION.md)。

本次打包重新执行完整 CTest，30/30 通过，耗时 127.31 秒。最终 ZIP 解压后已通过独立运行库启动、中文路径下载、逐字节校验、策略保存及重启恢复冒烟检查。包内 build-info.json 已标记 crawler-P1-awaiting-human-validation，并记录分支、源码修改状态及可执行文件 SHA256。校验文件与 ZIP 同目录。
