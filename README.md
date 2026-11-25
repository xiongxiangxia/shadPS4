# 纯游玩版 / Gameplay edition

## 中文

**用途**：当前游玩候选。

包含蓄枪兼容、3 倍手动镜头，以及天使闪避后空中人类／恶魔闪避调整。角色在空中时，符合条件的输入可绕过天使闪避遗留的缓冲检查，继续由游戏选择动作；地面闪避与天使闪避沿用游戏处理。已移除持续诊断日志，保留启动状态与异常日志。

**适用范围**：DmC Devil May Cry: Definitive Edition，PS4 游戏编号 CUSA01022，版本 01.03。代码通过 shadPS4 运行时兼容逻辑处理，其他游戏版本和编号未验证。

**使用方法**：从对应 GitHub Actions 构建下载 Windows 压缩包，解压并备份现有 shadPS4.exe 后替换。游戏数据、配置和其他运行文件继续使用已有安装。具体效果以游戏内测试为准。

**已知情况**：这是针对 DmC 表现的兼容调整；输入问题的模拟器根因尚未确认。个别输入或动作时序仍可能出现异常。

## English

**Purpose:** Current gameplay candidate.

Includes charge compatibility, 3x manual camera speed, and an aerial human/demon evade adjustment after angel evades. For qualifying airborne inputs, it bypasses the remaining angel-evade buffer checks and lets the game select the action. Ground and angel evades use the game path. Continuous diagnostics are removed; startup and error messages remain.

**Target:** DmC Devil May Cry: Definitive Edition for PS4, title ID CUSA01022, version 01.03. The changes run through shadPS4 compatibility code. Other title IDs and versions have not been verified.

**Usage:** Download the Windows artifact from the corresponding GitHub Actions run, extract it, back up the existing shadPS4.exe, and replace it. Keep the existing game data, configuration, and other installation files. Verify behavior in game.

**Known limitations:** These are DmC-specific compatibility adjustments. The underlying emulator cause of the input problems is still unconfirmed. Some input and action timing issues may remain.

[Download the 3x Windows play build / 下载 3 倍 Windows 游玩版](https://github.com/xiongxiangxia/shadPS4/actions/runs/44106541152)
