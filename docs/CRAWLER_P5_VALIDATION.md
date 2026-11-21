# 爬虫 P5 本地测试包

人工效果确认：**待用户验证**。

## 本次产物

- [Windows x64 测试 ZIP](../dist/RatsSearch-Windows-x64-crawler-p5.zip)，64,208,509 字节；[SHA-256 校验文件](../dist/RatsSearch-Windows-x64-crawler-p5.zip.sha256)。
- [已解压的验证目录](../dist/P5-validation-69dd29f5/RatsSearch-Windows-x64/)，可双击其中的 [Start-P5-validation.cmd](../dist/P5-validation-69dd29f5/RatsSearch-Windows-x64/Start-P5-validation.cmd)。
- [本次构建与包验证记录](../build-windows-x64/p5-package-validation.json)；本次重新构建后 31/31 测试通过，实际 ZIP 的中文路径下载及重启续传冒烟通过。人工界面与公网效果尚未验证。

ZIP SHA-256：`e90d282c534e4553a122f20243d7572da035cbc992832068e174f9ca5bf89068`。

## 启动

本包包含 P0–P5 改动以及文件列表显示修复，使用 Windows x64 Release 构建，自带 Qt、MySQL 驱动和 Manticore。

解压 `RatsSearch-Windows-x64-crawler-p5.zip` 后，进入 `RatsSearch-Windows-x64` 文件夹，在 PowerShell 运行：

```powershell
.\RatsSearch.exe --data-dir .\data-p5 --no-remember-data-dir
```

使用独立的 `data-p5` 目录，不更改已记住的数据目录设置。首次启动完成协议确认，并在设置中启用爬虫/索引功能。默认元数据并发为 10。

本地已解压验证目录还提供 `Start-P5-validation.cmd`，可双击以相同参数启动。直接双击 `RatsSearch.exe` 会沿用应用的常规数据目录选择逻辑。

## 人工确认清单

1. 启用爬虫后预热约 5 分钟，再观察至少 15 分钟，记录新增数量及 CPU、内存变化。公网短时间没有公告或新增，不能单独判断爬虫异常。
2. 爬取时搜索、切换页面、选择种子，确认界面响应正常；选择已知包含多个文件的种子，检查底部文件列表、目录树和文件数量，反复切换种子确认无旧内容残留。
3. 关闭并重新开启爬虫约 5 次，确认能恢复工作，无卡死、崩溃或活动数异常。
4. 在设置中将元数据并发从 10 调为 20，再恢复 10，确认设置保存、重启后保留；降低上限时旧请求会自然结束，实际活动数可能暂时高于新上限。
5. 正常退出后以相同命令重新启动，确认已有记录、搜索和完整文件详情保留。

## 自动验证与反馈

[P5 报告](./CRAWLER_P5_REPORT.md) 记录了 31/31 全量测试、实际数据库断连恢复、两次独立两小时 / 各 100 次启停验证。小幅内存增长和公网样本不足的限制也已记录，人工效果尚未确认。

日志在 `data-p5/rats-search.log`，可能附带轮转文件。出现问题时保留发生时间、操作步骤、相关 hash、预期/实际文件数量及对应日志；同时记录观察时长、并发设置和新增数量。

停止时尚未开始的任务可能取消；内存队列不会跨进程保存。已提交数据库但尚未交付通知的内容会保留。容量、取消和重试语义见 [用户手册](./MANUAL.md)。
