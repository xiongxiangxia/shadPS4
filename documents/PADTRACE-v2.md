# PADTRACE v2 input diagnostic build

This build records input delivery, not DmC internal charge or animation state. It does not change
button mappings, queue consumption, sampling, or emulated pad return values. The baseline uses the
same source with `ENABLE_PADTRACE=OFF`. The diagnostic build uses `ENABLE_PADTRACE=ON` and records
by default. Set `SHADPS4_PADTRACE=0` before starting it to disable recording at runtime.

## Installation and capture (中文)

1. 复制现有模拟器目录作测试副本，备份原 `shadPS4.exe`，替换为 diagnostic 包中的同名 exe。
   这是模拟器核心，不是启动器。确认启动器所选核心路径，测试时关闭自动更新。
2. 启动 DmC 决定版，进入可以稳定测试蓄力的场景。按 F9 为每组实验打标记。
   F9 只增加记录，不拦截原有绑定；如已自行绑定 F9，请暂时使用其它键或不要打标记。
3. 先测试霰弹枪：只蓄力松枪；立即接平 A 后蓄力松枪；延迟接平 A 后蓄力松枪。
   每组按 F9 后重复 3 次，组间留 2 秒。保持枪键按住的总时间尽量相同。
4. 双枪按相同顺序测试，再单独测试狂接平 A。最后另开一次测试 Jump Cancel 吞攻击。
5. 键盘和手柄分别录制。注明枪/近战/跳跃对应的 PS 按钮、游戏版本、输入设备以及各组成功次数。
6. 正常退出游戏和模拟器。将最新 `padtrace-v2-*.csv` 和 `shad_log.txt` 一起发送。
   CSV 位于模拟器实际 user/log 目录：便携模式为 `user/log`，否则通常为
   `%APPDATA%/shadPS4/log`。启动日志中的 `[PADTRACE] Recording to ...` 会显示确切路径。
7. 换用 baseline 包中的 exe，以相同条件确认 bug 仍然存在；该包不会生成 CSV。

CSV 不受普通日志过滤器影响。记录后台每 250 ms 批量写入；输入路径仅复制数字到有界内存队列。
仍有少量锁和时间戳开销，不能保证完全不影响时序。强制结束进程可能丢失最后一批数据。
每次进程运行生成独立文件，最多记录 1,000,000 条；到达上限停止采集，不停止游戏。
`# dropped=N limit_reached=true/false` 会明确报告采集丢失或上限。请单次测试控制在几分钟内。
仅记录模拟器窗口收到的输入；不要在采集期间输入密码或其它与测试无关的文字。

## CSV schema

All numeric payload fields are decimal. The header is `seq,monotonic_us,thread,kind,p0,...,p23`.
Unused payload columns are zero. `seq` is recorder insertion order; concurrent event timestamps can
arrive out of order. `monotonic_us` uses host steady clock and cannot be subtracted from kernel
process time or SDL nanoseconds. SDL event timestamps and kernel process microseconds have their
own clock domains. Only compare times within the same domain.

Each SDL handler has an event ID. Each queued state has a state ID and originating event ID
(zero for timer snapshots). Every pad read has a request ID. IDs share a generator but are not row
sequence numbers. `QUEUE_READ.p0` correlates with `READ_BEGIN.p0`.

| kind | p0 onwards, in order |
| --- | --- |
| RAW | event_id, SDL event type, SDL timestamp_ns, InputType, input_id, gamepad_id (1-based), active, mapped_axis_value, keyboard_repeat, raw_sdl_axis_value |
| INPUT_ACCEPT | event_id, pressed-key-list changed |
| MAP_BEGIN | event_id, pressed-key-list size |
| BINDING | event_id, output_slot (0-based), output_button, output_axis, positive_axis, active, axis_value, key0_type, key0_id, key0_pad, key1_type, key1_id, key1_pad, key2_type, key2_id, key2_pad |
| MAP_OUTPUT | event_id, output_slot, output_button, output_axis, positive_axis, old_active, new_active, old_axis_param, new_axis_param, changed |
| MAP_END | event_id |
| BUTTON | event_id, user_id, state_id, Orbis button mask, down, before_mask, after_mask, kernel_sample_us |
| AXIS | event_id, user_id, state_id, axis, target_value, smooth, before_mask, after_mask, resulting_axis_value, kernel_sample_us |
| PUSH | event_id, user_id, state_id, kernel_sample_us, buttons, connected, connected_count, queue_size, overwrote_oldest, left_x, left_y, right_x, right_y, l2, r2 |
| READ_STATE | wrapper_id, handle; READ_BEGIN.p5 links to this wrapper |
| READ_BEGIN | request_id, handle, requested_count, valid_data_pointer, kernel_begin_us, wrapper_id |
| QUEUE_READ | request_id, user_id, requested_count, returned_count, queue_before, queue_after, mode (0 disconnected, 1 current-state, 2 FIFO) |
| READ_SAMPLE | request_id, handle, user_id, requested_count, returned_count, sample_index, state_id, origin_event_id, source_sample_us, kernel_read_us, source_buttons, output_buttons, connected, l2, r2, left_x, left_y, right_x, right_y, output_timestamp_us, connected_count |
| READ_END | request_id, handle, requested_count, result, kernel_end_us (zero for validation errors) |
| MARKER | F9 event_id |

`InputType`: 0 axis, 1 keyboard/mouse, 2 controller, 3 invalid/unmapped.
`Axis`: 0 left X, 1 left Y, 2 right X, 3 right Y, 4 L2, 5 R2.
Output button/axis IDs are SDL identifiers, not Orbis bit masks. Configuration comments contain
the effective sorted binding list and configuration paths, including reloads.
Inactive, unchanged outputs and inactive bindings with neutral previous outputs are omitted.

Orbis masks: Triangle 0x1000, Circle 0x2000, Cross 0x4000, Square 0x8000;
L2 0x100, R2 0x200, L1 0x400, R1 0x800; intercepted 0x80000000.

## Analysis

`python padtrace_summary.py padtrace-v2-SESSION.csv --output summary.json`

The standard-library-only helper reports sample age, API read gaps per thread/handle, queue
overwrites, interception, source/output differences, and button holds with no returned pressed
sample. Check marker groups and compare failed versus successful attempts. Queue overwrites can
be normal with current-state reads. A returned sample is not proof of game-side consumption.

Most likely investigation order:

1. Binding arbitration: melee activation causes an unexpected gun release in MAP_OUTPUT/BUTTON.
2. Timing and delivery: RAW/PUSH are correct but READ_SAMPLE misses a transition or reports stale
   states, including mixed current-state/FIFO reads and intermediate snapshots.
3. Pad interception/disconnection: source is correct but output is neutral or intercepted.
4. If all delivered reports are correct, investigate guest input buffering, SDK expectations,
   scheduling and timing. Do not conclude that the game is at fault solely from this trace.
