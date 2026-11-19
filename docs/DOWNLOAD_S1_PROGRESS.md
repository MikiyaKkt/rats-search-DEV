# S1 实施进度（尚未达到验收门槛）

> 2026-11-18 更新：本文件保留 11 月 16 日的历史检查点；后续实现及验证见 [S1 交付报告](DOWNLOAD_S1_REPORT.md)，人工操作见 [验证清单](DOWNLOAD_S1_VALIDATION.md)。

日期：2026-11-16。依据：已获批准的[执行计划](DOWNLOAD_EXECUTION_PLAN.md)。

本记录是实施检查点，不是 S1 完成报告。当前应用尚未提供实际限速和队列控制。

## 已实现

- [下载策略模型](../src/services/download_policy.h)：全局/任务策略、B/s 精确整数校验、任务 null 继承、0 不能绕过全局限制、有效连接上限合并。未知字段、错误类型、负值、溢出及非法优先级全部拒绝，失败不修改输出对象。API 字段采用 downloadLimitBps/uploadLimitBps/queuePriority。
- [配置存储](../src/app/config_store.cpp)：默认 downloadPolicy、类型化读写、加载校验、通用写入口校验、批量写入前完整校验、QSaveFile 原子保存。
- [会话存储](../src/services/torrent_session_store.cpp)：schemaVersion=1、文档修订、任务策略、用户意图、队列位置。旧数组显式迁移并默认暂停；区分缺失、迁移、解析/校验失败及打开失败。检查重复 hash、文件索引和数值类型；空列表仍保存版本文档。
- [下载服务](../src/services/download_service.cpp)：使用文档接口保存/读取会话，保留任务策略与顺序、递增会话修订。损坏/不支持的会话阻止随后自动覆盖，并发出可诊断错误。恢复仍以引擎重检结果为准。
- [纯调度器核心](../src/services/download_scheduler.cpp)：根据快照及单调时间计算独立下载/做种/元数据/校验准入集合，默认 3/2/2/1。包含优先级、FIFO、等待老化、120 秒停滞让位及 60 秒冷却。**尚未接入服务执行流程，因此当前并行数不受这些准入集合限制。**
- [测试](../tests/test_download_policy.cpp)：策略拒绝与原子性、继承/0、配置持久化、旧会话迁移、损坏文件保留、未知版本/重复项、槽位分类、暂停、降并行数、老化和停滞冷却。

## 尚待完成

| 执行计划项 | 未完成工作 |
|---|---|
| S1-01 | 与服务策略变更事务统一 revision/expectedRevision；验证真实保存失败及恢复顺序 |
| S1-02 | 令牌桶、全局/任务公平带宽、请求预约/回滚、在途上限、传输背压、真实发送统计、TCP/uTP 连接预算及对应测试 |
| S1-03 | 服务接入调度指令、用户意图与执行暂停分离、队列移动、启动前完整恢复、操作终态及持久化结果 |
| S1-04 | Application 装配/热更新、策略/队列 API、设置页和任务属性窗口、有效限额与队列原因展示 |
| 验收交付 | TCP/uTP/MSE/partial-send 集成测试、60 秒实测、独立 S1 补丁、干净补丁应用验证、正式 S1 ZIP 和报告 |

## 构建与发布状态

本次代码可用现有 Windows MinGW/Qt 工具链编译。完成基础回归不等于通过 S1 带宽验收，不得据此声称限速/并行控制已经可用。

最终验证：Release 全量构建成功；CTest 24/24 通过（42.36 秒），其中新增测试包含 10 个业务用例；脚本 PowerShell 语法解析及 git diff --check 通过。稳定/S0 ZIP 的 SHA-256 与先前交付一致。

为防止开发中代码覆盖既有 S0 包，[构建脚本](../scripts/build-windows.ps1)默认后缀临时改为 s1-dev，build-info 标记 S1-in-progress。现有稳定版与 S0 ZIP 保留；此检查点未生成新 ZIP。

文件选择仍保持 S0 行为；S2 的真实文件选择不属于本检查点。
