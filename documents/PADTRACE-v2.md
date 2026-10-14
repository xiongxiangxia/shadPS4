# PADTRACE v2 input diagnostic build

## DmC script assignment observer (experimental, Windows only)

This extension replaces selected interpreter dispatch pointers in process memory, leaving
the original game files intact. It delegates to the original interpreter handlers and does
not change field values or action decisions. It is installed when F9 starts recording, only
for CUSA01022 01.03 with matching interpreter bytes, global-reference bytes, and assignment
dispatch pointer. A validation failure leaves the hook uninstalled and emits `DMC_HOOK` with 0.
The build has not been validated in a running game by its author. Hooks and probes add timing
overhead; compare with the baseline if the reproduction changes.

`DMC_LET_BEGIN`: request, object, FFrame, operand code pointer, property metadata pointer,
owner-property metadata pointer, literal float bits, cleanup-pattern candidate flag,
previously identified candidate-property flag. Only contextual float constants 0/1 are captured.
`DMC_LET_END`: request, interpreter destination address, interpreter property pointer,
read-back float bits, readable flag, property-match flag, final operand pointer.
Read-back is evidence only when the readable/property-match flags are both 1. Earlier writes
can be associated retroactively using the property metadata address from a cleanup candidate.

`DMC_FRAME`: request, depth, frame address, node, object, code, previous-frame candidate,
node-name bits, outer-name bits. The FFrame/UObject offsets other than the confirmed operand
pointer are provisional. These are raw diagnostic snapshots, not resolved function names or
proof of a fully validated stack. Only the current frame is recorded at bounded discovery
sites; previous-frame pointers are not traversed.

The cleanup-pattern candidate matches a superclass call followed by three contextual 0.0
assignments to distinct properties of the same owner, as found in RangedCharge.CommonCleanup.
It is not a unique function fingerprint; other functions may match. Names and identity must
be confirmed from runtime data before treating the first property as m_fcpGunCharge.

The low-overhead revision removes virtual/final call hooks. Assignment decoding, including
negative results, is cached per thread and operand address, reset on each F9 capture start.
This assumes script bytecode stays immutable during a capture. All matching contextual float
0/1 assignments are recorded, including repeated writes, with a hard limit of 4096 assignment
pairs per thread per capture (`DMC_PROBE_LIMIT`). Current-frame snapshots are limited to 128
discovery sites. Operand/frame reads use live interpreter objects; read-back is directly copied
only when the interpreter's destination is nonzero and property metadata matches. Cold decoding
and provisional node metadata still use checked reads. These probes assume the native handler's
live objects remain valid until it returns. They do not observe every possible field-write opcode.
There are no DMC_CALL_BEGIN/END or DMC_HOOK_CALL events in this revision. Consequently these
logs cannot independently identify every action entry, interruption, or caller. Candidate
identity remains provisional, and missing events do not prove absence of an action or write.
No gameplay repair is included.

`DMC_HOOK_BRANCH`: installed flag, original JumpIfNot handler. This experimental probe handles
token 0x07 only when its dispatch pointer is within the matching game's code image and its
member-function adjustment is zero. It delegates to the original handler without changing its
arguments or branch decisions. The initial contextual-condition prefix is read from live code.
Per-thread decoding is capped at 1024 cache misses; branch events are capped at 512 per capture,
with `DMC_BRANCH_LIMIT` emitted at either limit. The broad contextual-prefix filter is discovery,
not a confirmed charge-condition filter. Native action-graph decisions can bypass this opcode.

`DMC_BRANCH`: request, object, frame, node, operand before evaluation, operand after evaluation,
changed-from-previous-outcome flag, first-observation flag, cache-miss count, event count.
Only first observations and changed post-evaluation pointers are recorded. The pointer change
is raw control-flow evidence, NOT an already decoded true/false condition or cancellation reason.
Compare against the function's bytecode before interpreting its direction. Repeated identical
outcomes and different objects at the same site may be omitted. For the first 64 recorded events,
new sites include a current-frame snapshot and `DMC_BRANCH_CODE` (request, four raw u64 words
starting at the pre-evaluation operand) to help associate the condition with static scripts.

Recommended test order: one successful delayed-attack charge first, then an immediate-attack
failure, then another success, without changing weapon or scene. Keep the capture short and
report the outcome/order and any recording-induced stalls. Timing conclusions require smooth
reproduction and must not treat hook-induced stalls as the original defect.

## Frame and clock extension

The extended build also records `FRAME_QUEUE` (buffer index, flip argument, EOP flag,
pending count, submit TSC), `FRAME_PRESENT` (present count, buffer index, flip argument,
process microseconds, TSC), `VBLANK` (count, process microseconds, TSC),
`FLIP_SUBMIT` (handle, buffer index, mode, argument), and `VBLANK_WAIT`/`VBLANK_WAKE`
(handle, vblank count). Presentation is not a measurement of guest animation progress.

`CLOCK` fields are API (0 process microseconds, 1 process counter, 2 raw TSC,
3 POSIX clock_gettime, 4 POSIX gettimeofday, 5 sceKernelClockGettime, 6 sceKernelGettimeofday),
value, frequency, cumulative call count for that slot, backwards flag, previous call value,
current trace event ID, return address, clock ID, and last recorded value.
Each thread/API/caller/clock ID is sampled at most once per 100 ms except backwards values,
which are always recorded. There are 128 slots per thread; `CLOCK_SLOTS_FULL` reports
when additional call sites cannot be tracked. Emulator internal calls and wrapper calls
are included; use module address ranges in shad_log.txt to identify guest callers.
Direct guest CPU timestamp instructions are not captured by these hooks.
Additional timing probes add overhead and do not expose the game's internal charge state.

`READ_STATE` adds the return address in p2 and `READ_BEGIN` adds it in p6.
`EQUEUE_WAIT` fields: request ID, queue, requested event count, timeout microseconds
(-1 infinite), return address. `EQUEUE_RETURN`: request ID, queue, returned count,
result. `EQUEUE_EVENT`: request ID, queue, sample index, ident, filter, flags, fflags,
data. These expose delivered frame/GPU/timer events and wait durations, including timeouts.
`FLIP_STATUS`: handle, count, process microseconds, TSC, submit TSC, flip argument,
GPU queue count, pending count, current buffer, return address.
`VBLANK_STATUS`: handle, count, process microseconds, TSC, return address.
Return addresses can identify guest call sites, but are not charge variable addresses.
Clock rates in the analyzer compare returned clock progress to host steady time;
sampling overhead, preemption, and CPU-time clock IDs can legitimately produce rates other than 1.

This build records input delivery, not DmC internal charge or animation state. It does not change
button mappings, queue consumption, sampling, or emulated pad return values. The baseline uses the
same source with `ENABLE_PADTRACE=OFF`. The diagnostic build uses `ENABLE_PADTRACE=ON` and records
only after pressing F9. Press F9 again to stop; repeat to capture another segment in the same CSV.
Set `SHADPS4_PADTRACE=1` to start recording immediately, or `SHADPS4_PADTRACE=0` to disable
the recorder entirely. The switch controls PADTRACE CSV only, not ordinary emulator logs.

## Installation and capture (中文)

1. 复制现有模拟器目录作测试副本，备份原 `shadPS4.exe`，替换为 diagnostic 包中的同名 exe。
   这是模拟器核心，不是启动器。确认启动器所选核心路径，测试时关闭自动更新。
2. 启动时默认不采集输入。进入测试场景后按 F9 开始记录，再按 F9 停止记录。
   普通日志会显示 Recording ON/OFF。诊断版的 F9 按下事件用于开关，请不要绑定游戏动作。
3. 先测试霰弹枪：只蓄力松枪；立即接平 A 后蓄力松枪；延迟接平 A 后蓄力松枪。
   每组开始前按 F9 开启，重复 3 次并松开所有键后再按 F9 关闭。保持枪键按住的总时间尽量相同。
4. 双枪按相同顺序测试，再单独测试狂接平 A。最后另开一次测试 Jump Cancel 吞攻击。
5. 键盘和手柄分别录制。注明枪/近战/跳跃对应的 PS 按钮、游戏版本、输入设备以及各组成功次数。
6. 正常退出游戏和模拟器。将最新 `padtrace-v2-*.csv` 和 `shad_log.txt` 一起发送。
   CSV 位于模拟器实际 user/log 目录：便携模式为 `user/log`，否则通常为
   `%APPDATA%/shadPS4/log`。启动日志中的 `[PADTRACE] Trace file ...` 会显示确切路径。
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
| CAPTURE | recording (1 started, 0 stopped); incomplete holds at segment boundaries are excluded by the analyzer |
| DISPATCH | event_id, SDL event type, SDL timestamp_ns, device_id, key/button/axis_id, down, raw_axis_value, keyboard_repeat; captured before routing filters |
| FILTER | event_id, consumed_by (0 forwarded, 1 guest mouse/keyboard, 2 ImGui) |
| WINDOW_DEVICE | event_id, SDL event type, SDL timestamp_ns; window focus and controller add/remove |
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
