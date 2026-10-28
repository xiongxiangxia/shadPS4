# DmC 蓄力帧内顺序：轻量根因诊断

此包是 ROOTCAUSE-light-timing，兼容补丁关闭，原始掉蓄力现象预期仍可出现。当前认可的可玩版本仍为 dbb1bba7 COMPAT，正常游玩不用更换。游戏文件不修改，JC/Leap不做补救。

## 本轮回答什么

追同一轮射击中：持续按枪的游戏计时、缓冲接收、动作时间线回调、节点唤醒与重放、近战引发的切换／清理，发生在哪个世界时间锚点和调用嵌套位置。区分请求在唤醒前已取消，与唤醒实际发生但未完成蓄力的情况。

没有实机参照时，记录只能验证模拟器下的执行顺序，不能独立证明PS4应该如何执行。没有增加Charge动作启动/伤害探针，不把事件返回1或特效当成蓄力成功。

## 怎么测

确认包内EDITION.txt写的是ROOTCAUSE-light-timing。用包内exe启动；如启动器必须使用特定路径，先保留已确认可玩的exe，再把此诊断exe放到启动器实际使用的位置。默认就是轻量模式，也可用dmc-root-light.cmd明确设置。

1. 进同一测试场景，先不按扳机；用霰弹枪即可。
2. F9开始。先按住枪、晚点接剑，松枪确认成功；再按住枪、较早接剑，松枪确认失败。记录实际顺序，结果不符时按实际结果说明。
3. 松开所有键，F9停止，正常退出。尽量控制在10～15秒；不再要求第三轮成功样本。
4. 保存新CSV和普通shad_log.txt到原日志目录。若明显卡顿，停止录制并保留已有文件，不强行打完。

无需现在用旧ROOTCAUSE包重测，需等这一提交的云端构建成功。结束后可恢复dbb1bba7 COMPAT日常游玩。

## 与旧重诊断的区别

仅在CUSA01022、01.03及对应原生入口字节验证通过后装10个只读观察入口；F9关闭时直接转发。新路径不安装旧全局赋值／分支探针，不做名称解析、反射字段遍历、全局对象扫描或额外游戏函数调用。只读复制固定大小数据；原函数参数、调用次数及返回值不变。

旧重路径仅在启动前显式设置SHADPS4_DMC_TRACE=full时启用；本轮不要用full。SHADPS4_DMC_NATIVE=0仍可关闭原生探针。

固定读取通过ReadProcessMemory检查失败，不把失效指针直接当可读内存。对象名仅记录原始FName数值，不在热路径展开。FName值不是可跨版本通用的资源编号。

每次F9开启时重置共享12000条预算；耗尽明确DMC_LIGHT_LIMIT。最多跟踪最近16个玩家动作时间线，START的计数超过16时存在覆盖，要据此判断记录范围。时间线共享登记供跨线程观察；parent只表达同线程嵌套，不代表跨线程因果顺序。

DMC_LIGHT_COST每128个完成的测量阶段输出累计纳秒和最慢阶段。计入固定快照及普通记录调用，排除原游戏函数执行；部分资格检查、未满128阶段尾部、全局Pad输入/时钟记录和报告本身不包含在内。因此是局部测量，不能承诺总开销或FPS。实际时间线delta若出现大步进，必须结合这些测量及帧记录判断，不能直接认定模拟器计时错误。

## CSV字段（p0起，未列出的列为0）

所有浮点数以原始32位bits记录。世界时间是World+0x690；不是宿主时间，也不是经验证的真实帧号。TICK附带World+0x690的连续4个原始float，其中其余字段暂不据猜测命名。

- DMC_LIGHT_CAPTURE：格式1、hook位图、预算、eboot基址、时间线槽数、cost报告周期。hook全装为0x3ff。
- DMC_LIGHT_HELD_BEGIN：request、parent、input、脚本函数、实参前字节码位置、按住mask、枪按下时间bits、World、世界时间bits。只在NTInput枪位0x40按住时观察GetHeldTime；同一脚本也可能查询其他按钮，不能把每条都当枪的查询。
- DMC_LIGHT_HELD_END：request、原函数返回float bits、世界时间bits。BEGIN的脚本/字节码位置用于区分按钮查询；枪起点字段固定为input+0x240。
- DMC_LIGHT_EVENT：request、parent、aggregator、event、input参数、快照有效、descriptor、type、held mask、mode、descriptor原始FName、caller、世界时间bits。玩家聚合器通过输入事件held mask8和type0～2识别；这是沿用已有记录的筛选依据，不是任意事件类别的类型认证。快照在原函数前完成，返回后不解引用池化event。
- DMC_LIGHT_EVENT_END：request、原返回值、世界时间bits。
- DMC_LIGHT_QUEUE：request、phase0前/1后、aggregator、有效、buffer数组、count、buffer owner、processing graph、flags、主操作队列count、次操作队列count、世界时间bits。
- DMC_LIGHT_BUFFER：request、parent、原五个参数、世界时间bits；END为request、原返回值。
- DMC_LIGHT_GRAPH：parent、graph、event地址、前节点、后节点、原返回值、caller、已知type6/7重放调用点标记、世界时间bits。仅保留接受、节点变化或已知重放，拒绝且无变化的普通遍历不输出；不把省略当作未尝试。不会在返回后读取event内容。
- DMC_LIGHT_CLEAR：request、parent、node、event地址、世界时间bits；END为request、原返回值、世界时间bits。前后QUEUE描述缓冲变化。
- DMC_LIGHT_START：request、parent、action、node、graph、timeline、action原始FName、node原始FName、世界时间bits、已登记时间线次数。是玩家图中通用动作时间线启动，不保证每条都是射击。END为request、时间线时间bits、世界时间bits。
- DMC_LIGHT_TICK：request、parent、timeline、delta bits、前时间bits、世界快照有效、4个世界时间字段bits、input、按住mask、枪按下时间bits。END为request、后时间bits、世界时间bits。
- DMC_LIGHT_ITEM：request、parent、timeline、item、有效、item连续9个u64原始字、世界时间bits。END为request、世界时间bits。原始字对应旧探针的回调、名称、负载、时间/控制字段，记录后离线解释，不额外执行回调。
- DMC_LIGHT_AWAKE：request、parent、node、graph、前state、世界时间bits。END为request、后state、世界时间bits；伴随前后QUEUE。
- DMC_LIGHT_SWITCH：request、parent、aggregator、incoming、世界时间bits；END为request、世界时间bits，伴随前后QUEUE。
- DMC_LIGHT_COST：完成阶段数、累计纳秒、最慢阶段纳秒。按CSV thread分组，累计值不能逐行相加。

包内dmc_light_summary.py用Python标准库只读概览：`python dmc_light_summary.py 日志.csv`。无本地Python时无需运行，交回CSV分析即可。旧padtrace_summary.py不认识这些新记录，但原SDL/按钮/READ_SAMPLE/FRAME_PRESENT仍可分析。
