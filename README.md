# 输入与蓄枪诊断 / Input and charge diagnostics

## 中文

**用途**：研究历史和输入诊断。

研究输入采样、持续按枪请求、动作切换和蓄力失败，包含多轮针对游戏内部状态的实验与诊断。该分支适合继续追查根因，日志和探针可能影响帧率。

**适用范围**：DmC Devil May Cry: Definitive Edition，PS4 游戏编号 CUSA01022，版本 01.03。代码通过 shadPS4 运行时兼容逻辑处理，其他游戏版本和编号未验证。

**使用方法**：从对应 GitHub Actions 构建下载 Windows 压缩包，解压并备份现有 shadPS4.exe 后替换。游戏数据、配置和其他运行文件继续使用已有安装。具体效果以游戏内测试为准。

**已知情况**：这是针对 DmC 表现的兼容调整；输入问题的模拟器根因尚未确认。个别输入或动作时序仍可能出现异常。

## English

**Purpose:** Research history and input diagnostics.

Investigates input sampling, held-fire requests, action transitions, and charge failures with experimental compatibility and diagnostics. Probes and logging may affect performance.

**Target:** DmC Devil May Cry: Definitive Edition for PS4, title ID CUSA01022, version 01.03. The changes run through shadPS4 compatibility code. Other title IDs and versions have not been verified.

**Usage:** Download the Windows artifact from the corresponding GitHub Actions run, extract it, back up the existing shadPS4.exe, and replace it. Keep the existing game data, configuration, and other installation files. Verify behavior in game.

**Known limitations:** These are DmC-specific compatibility adjustments. The underlying emulator cause of the input problems is still unconfirmed. Some input and action timing issues may remain.
