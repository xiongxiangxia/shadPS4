# DmC 游玩版实现 / Gameplay implementation

本分支适用于 CUSA01022 / 01.03，入口字节经过校验，运行时调整游戏状态，不修改磁盘上的游戏文件。蓄枪兼容追踪双枪和霰弹的持续输入意图，处理重新尝试和攻击缓冲交接。爆弹枪不添加蓄力。手动镜头水平与垂直增量使用 3 倍系数。

空闪调整跟踪天使闪避相关的 EvadesBuffer，仅在角色腾空、当前输入为人类或恶魔闪避且满足检查时，临时允许原生动作图处理并跳过该缓冲的附加条件。原生闪避资格和次数条件继续参与判断。代码位于 `src/input/dmc_play.cpp`、`src/input/dmc_evade_trace.inl` 和 `src/input/dmc_camera_speed.inl`。空闪功能已有用户实测，偶发延迟的原因尚未确认。持续诊断记录已删除，功能必需的检查仍有开销。

The branch targets CUSA01022 / 01.03 and validates entry bytes before applying runtime changes. Charge compatibility tracks held-fire intent for pistols and shotgun and handles retries and attack-buffer handoff. The bomb gun gains no charge. Manual horizontal and vertical camera increments are scaled by 3.

For qualifying airborne human or demon evades after angel evasion, the patch tracks EvadesBuffer, temporarily permits the native action graph to process the input, and bypasses the additional buffer predicate. Native eligibility and evade-count checks remain in effect. Implementation lives in `src/input/dmc_play.cpp`, `src/input/dmc_evade_trace.inl`, and `src/input/dmc_camera_speed.inl`. The user tested aerial evades; occasional perceived delay remains unexplained. Continuous diagnostic logging is removed, while functional checks still have a cost.
