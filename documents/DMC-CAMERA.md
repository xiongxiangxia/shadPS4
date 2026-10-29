# DmC 决定版：蓄力兼容 + 手动镜头2.5倍候选

基线为用户指定的 `dbb1bba7a1692420e6e7d3bff2a0e9f66937d797`（Preserve armed gun charge across the actual play-mode condition），独立分支 `dmc-camera-2p5x`。没有合入后来的轻量根因诊断改动，原蓄力兼容逻辑保持。

## 效果与范围

仅对CUSA01022、01.03的正常Chase镜头手动转动计算，增加水平yaw和垂直pitch的2.5倍速度系数。修改的是游戏进程内的两段手动转动计算，不是把[-1,1]右摇杆输入乘大后限幅，也不改全局时间、移动速度、扳机或按键。原游戏的镜头边界、碰撞、上下角度限制和固定镜头区域保留，因此这些限制下的实际可见转速不保证恒为2.5倍。特殊锁定、脚本和其他镜头类型可能走不同路径，本候选未扩大覆盖。

新代码启动时核对两个位置的完整指令字节；任一不符则两轴均不安装。先准备两个只读可执行代码块并取得两处写权限，再统一改写；准备失败不只改一半。游戏磁盘文件和存档不修改。

每次经过这两处计算，仅新增一次浮点乘法和两次绝对跳转；没有每帧C++回调、属性反射或额外日志。未做本机游戏性能和旋转角度实测，实际效果需短时验证。

## 使用与回退

确认EDITION.txt为COMPAT-CAMERA-2.5x。与以往一样替换启动器实际使用的shadPS4.exe，先备份dbb1bba7原exe。此专用包默认开启2.5倍，正常游玩无需F9。

- dmc-camera-2p5x.cmd：明确启用2.5倍。
- dmc-camera-default.cmd：仅恢复原镜头速度，仍保留蓄力兼容。环境变量为SHADPS4_DMC_CAMERA_SPEED=0，需重新启动游戏才生效；脚本只作用于它启动的exe，另从启动器启动不会继承该脚本的局部变量。
- 直接换回原dbb1bba7 exe可完整回退此镜头改动。

先在能自由转动镜头的普通场景，保持人物不动、右摇杆推满，对比同方向转过相近角度的耗时，再试垂直、松杆停止、锁定和过场恢复；同时随手检查原蓄力操作。无需F9长录。若镜头没变快、跳变或穿过原限制，保留普通shad_log.txt；启动应有 `[DMC CAMERA] Manual yaw/pitch speed x2.5 installed`，字节不符会明确记录skipped。

## 调查依据

搜索未找到可核实适用于PS4决定版的速度滑块或现成配置法，不能据此断言菜单绝无设置。PC原版社区常见的DefaultCamera.ini / NTCam_DebugCamController / m_RotationSpeed建议指向调试镜头；本地NTGameFramework资源也将该字段归属NTCam_DebugCamera，不能视为正常镜头修复。

本地脚本DVCam_CameraController.DVStaticGetChaseCam返回DVCam_Chase。原生GetAddedTweakYaw(dt, stickinput)在0x9b5eac～0x9b5ec1计算手动水平增量；GetNewCurvePosition(stickinput, OldCurvePosition, dt)在0x9ae354～0x9ae365将垂直手动增量加到原曲线位置。系数分别加在计算增量后、原限制前，垂直不缩放OldCurvePosition。这里是定向镜头调整，不是已经确认的模拟器输入根因修复。

静态调用点0x9b7736从镜头m_StickInput.X（对象+0xec）取值，0x9b774b经虚表+0x358调用PerformStickTweaking，再经+0x360到GetAddedTweakYaw；纵向0x9b791f取m_StickInput.Y（+0xf0），0x9b7930调用GetNewCurvePosition。原生注册及脚本参数顺序已核对。工作区外部验证器对照实际eboot指令，并在宿主CPU上执行隔离的原计算／补丁跳板60组数值用例，验证正负满杆、零输入、30/60Hz步长、垂直旧位置保持。并非已运行游戏验证。

参考检索：

- PC原版玩家讨论（不作为PS4设置已存在或不存在的证明）：https://steamcommunity.com/app/220440/discussions/0/613940477749812247/
- Capcom决定版更新说明：https://news.capcomusa.com/lets/browse/dmc-definitive-edition-patch-now-live
