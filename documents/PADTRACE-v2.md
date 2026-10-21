# PADTRACE v2 input diagnostic build

## DmC script assignment observer (experimental, Windows only)

### Native held-input and buffer-clear probes

The native extension installs version-specific entry detours after loading eboot and before
Linker::Execute starts guest execution. It does not install entry detours while F9 recording is
running. Serial, app version, native registration strings and all displaced entry bytes are
checked; mismatches leave the affected entry unchanged.
Copied instruction sequences have complete instruction boundaries and no relative operands:
IsHeld at eboot+0x91d750 (14 bytes), Node.ClearBufferedInput at eboot+0xa21c90 (17 bytes).
Each delegates to an executable trampoline containing the original instructions, followed by an
absolute jump back. Trampoline memory stays allocated for process lifetime. Game files are unchanged.
This still changes in-process execution and introduces overhead; runtime validation is required.
Set `SHADPS4_DMC_NATIVE=0` before launching to skip all native detours. Baseline builds omit them.

The registered UNTInputexecIsHeld wrapper calls 0x91d750; that function tests object+0x220.
UNTInputexecGetHeldTime calls 0x91d7a0, which uses timestamps at object+0x228+button*4.
UNTActionGraph_NodeexecClearBufferedInput calls 0xa21c90. Registration-string relocations and
wrapper disassembly associate these names with the entrypoints; names alone are not the evidence.
Native IsHeld calls can be inlined or bypassed. An absent event does not mean the game saw release.
Other buffer-clearing paths may still bypass these entrypoints, including inlined implementations.

`DMC_NATIVE_HOOK`: held-entry installed flag, node-clear-entry installed flag, emitted at F9 start.
Startup also logs each installation result to shad_log.txt, even when recording is initially off.
`DMC_NATIVE_HELD`: input object, queried button index, original function result, held bitmask,
held-since float bits (only for button<32), native caller return address, slot call count.
Snapshots are recorded on a changed mask/result/timestamp or at most once per 100 ms for unchanged
object/button/caller slots. There are 128 direct-mapped slots and 4096 events per thread/capture;
slot collisions may cause extra snapshots. The timestamp is not a measured charge duration and
button indices must be associated with input events before calling one the gun button.

`DMC_NATIVE_CLEAR_BEGIN`: request, node, event argument, owner-graph pointer (node+0x70),
node state (node+0xd0), raw flags word (node+0xb4), native caller return address.
`DMC_NATIVE_CLEAR_END`: request, original return value, resulting node state, raw flags word.
These offsets are observed in the native implementation. Raw state/flag meanings and the event
layout remain provisional. Read-back assumes the live input/node object remains valid through
the call; no deep pointer traversal is performed. Events may be nested. A clear call is not by
itself proof of an incorrect cancellation or identification of the charged weapon's request.
Node-clear event pairs are capped at 1024 per thread/capture. `DMC_NATIVE_LIMIT` reports kind
(0 held, 1 clear) and limit. F9 capture starts reset the per-thread budgets.

### Native input aggregator probes

The aggregator class registration references its native constructor at eboot+0x212fb20, which
installs vtable eboot+0x31fca70. Relocated vtable entries identify InputEvent (+0x248, 0xa1f970),
InternalEvent (+0x250, 0xa1fe00), ClearBufferedInput (+0x260, 0xa1f5d0), and Global_BufferFlush
(+0x268, 0xa1f220). InputEvent/InternalEvent tail-jump to 0xa1f980 with third argument 1/0.
That shared entry (17 bytes), ClearBufferedInput (17 bytes), and Global_BufferFlush (20 bytes)
are detoured before guest execution with exact displaced-byte checks. These instruction prefixes
are complete and have no relative operands. `DMC_AGG_HOOK` reports installation flags in that order;
the common log also reports each flag at startup. Original arguments/results are preserved.

`DMC_AGG_EVENT_BEGIN/END`, `DMC_AGG_CLEAR_BEGIN/END`, `DMC_AGG_FLUSH_BEGIN/END` share this layout:
request, aggregator, event pointer (zero for clear/flush), input-mode argument (zero for clear/flush),
buffer count at +0x80, opaque buffer-owner pointer at +0xa0, opaque active pointer at +0xa8,
raw flags word at +0xb0, pending count +0xc0, flush count +0xd0, internal-event counter +0xd8,
native caller return address, original result (END of EVENT only), parent request.
Pointer/count roles are inferred from implementation use; exact ownership and state meanings are
not fully decoded. Parent request correlates nested observed calls on the same thread only.
The original return value alone does NOT prove a charge request was accepted or consumed.

`DMC_AGG_CONTEXT`: request, event, event class, matched-input-class flag, button-event-type byte,
held-mask word, raw event words at +0x60/+0x68/+0x70, cached expected-input-class pointer.
The input class check follows UObject class (+0x50) / superclass (+0x68), bounded to 16 links,
against the class cached at eboot+0x3b193f8 by ADVPlayerController.CreateInputEvent (0xb0c170).
That function stores its Type argument at event+0x78 and the return of DVInput.GetDVButtonsHeld
(0xb011e0) at event+0xa0. Those two fields are read only after the class check succeeds. Raw
event words are not yet resolved into input names. Type enum values and DV held-mask bit indices
must be correlated with known button events; they are not necessarily the NTInput or scePad indices.
No event is dereferenced after the original handler returns, because pooling may reuse it.
Live object/class reads assume the validated runtime layouts and object lifetime hold.

Input events, internal events, clears, and flushes have separate per-thread/capture budgets of
512, 1024, 256, 256 pairs. DMC_NATIVE_LIMIT kind 2/3/4/5 identifies the exhausted budget. All probes
remain diagnostic and timing-sensitive. No buffering, animation, or emulation repair is included.

### Correlated action/buffer handoff probes

### Pending-owner transition and game-clock audit

Two additional narrow detours observe native functions at eboot+0xa1e2c0 and +0xa1e510.
Their displaced 14/17-byte prologues are complete, PC-independent instructions. The former
switches aggregator+0x70 and can clear pending input when internal counter +0xd8 is zero and
incoming owner differs from buffer owner +0xa0. The latter clears on an owner mismatch without
that counter test. Both implement the clear inline rather than calling ClearBufferedInput.
The labels are inferred from disassembly, not recovered debug symbols. No condition is changed.

Only calls with a nonempty pending buffer AND a changed before/after snapshot emit records.
At most 64 changed calls are recorded per thread/capture; DMC_NATIVE_LIMIT kind 10 reports
exhaustion. Empty-buffer/unchanged calls do not perform name lookup, stack traversal, clock
traversal or CSV writes. These probes also observe calls outside an aggregator-event scope,
where parent request is zero. DMC_OWNER_HOOK reports the two installed flags.

DMC_OWNER_SWITCH: request, path (0 a1e2c0, 1 a1e510), aggregator, incoming owner, direct caller,
parent request, active pointer (+0x70) before/after, internal counter (+0xd8) before/after,
raw flags (+0xb0) after. DMC_BUFFER/ITEM snapshots with the same request show what disappeared.
DMC_OWNER_STACK: request, count, up to six frame-chain return addresses within the eboot code
range. The direct caller is separate. Unwind is best effort; missing/optimized frames do not
mean missing execution. Guarded reads run only for emitted owner changes.

DMC_GAME_CLOCK: request (zero for a sampled held query), world pointer, readable flag,
WorldInfo+0x690 float bits, raw words +0x694/+0x698/+0x69c, double bits at eboot+0x3940be0 and
+0x3f58870, input object, button index, held mask, held-since float bits. The world pointer is
resolved by reproducing the non-interacting esi=0 path of 0x521ea0, starting with the global at
+0x3a548e8 and reading +0x80, +0x60, then the first pointer. No game function is invoked.
0x91d7a0 uses WorldInfo+0x690 minus NTInput+0x228+button*4 to compute held time. The adjacent
world fields remain uninterpreted; they are NOT claimed to be separate verified clocks.
The two seconds-per-cycle globals are populated from sceKernelGetTscFrequency at 0xa5be and
0x1bcfc04. This supplies game-side timing evidence rather than assuming that correct exported
kernel clocks imply correct gameplay timing. Samples occur only alongside existing sampled
NTInput button-6 held records and emitted owner changes. Each pointer hop is guarded.

Static audit so far has not identified an emulation fault: the game queries the TSC frequency
when constructing its time conversion factors, and NTJobCode.prx is loaded later despite early
unresolved-import warnings. Neither observation proves all timing or job synchronization is
correct. This build does not change kernel timing, scheduling, input consumption, weapon state,
or game files, and does not implement an input retry workaround.

### Sparse buffer and action handoff records

Current sparse mode removes the matcher and selector detours entirely. DMC_HANDOFF_HOOK's last
two flags are intentionally zero; DMC_HANDOFF_SPARSE reports version 1, buffer-record limit 256,
graph-record limit 512, and identity-attempt limit 96. The detailed matcher/selector schemas
below describe older captures only. No candidate-condition traversal runs in current builds.

Buffer helpers copy a fixed-size snapshot (at most eight items) before and after the original
call, then emit records only if it changed. Aggregator entry/exit buffer detail is likewise
change-only. Graph hooks retain first/new observations, successful handling and state changes;
unchanged repeated rejections are suppressed through a 128-entry append-only cache. Once full,
new unchanged rejections are omitted rather than repeatedly replacing existing entries.
The hard emitted-record limits still apply. Unchanged repeated visits do not
spend these limits or perform identity/name lookup, guarded-memory queries or CSV writes.

Handoff CALL/END and buffer/graph snapshots are now emitted AFTER the original call, using saved
scalar snapshots for the before state. Their timestamps do not measure game-function duration
or exact branch execution time. Parent IDs still link each record to its enclosing aggregator
request, but buffer/graph helpers no longer establish additional nested parent scopes. Original
event metadata is captured by DMC_AGG_CONTEXT before dispatch. No pooled event is dereferenced
after it has left a live pending buffer solely to reconstruct a before snapshot.

Static inspection of DevilGame.xxx identifies DVPlayerController_Player.PlayerTick assigning
m_ConsumedHeldInput_RangedAttack from SendActionGraphEvent(RangedAttack, Held), gated by
m_OnlyConsumeHeldInputsOnce. An acknowledged buffered event can therefore stop further Held
submissions without proving that a charge action has started. This is game logic, not evidence
that the emulator's handling differs from hardware. Do not change the consumption flag blindly.

The original detailed implementation used entry detours at 0xa1c3c0 (buffer event), 0xa1fe10 (graph event), 0xa26420 (event
descriptor/conditions match), and 0xa20850 (link selection) preserve all arguments and results.
They record only while nested under an observed aggregator call, clear, or flush. All displaced
17-byte prefixes are complete instructions without relative operands. The buffer owner argument
may be a node OR a graph, depending on the call site; +0xa0 is not an event-context pointer.
The shared event path and buffer helper can replace or clear entries inline without entering
the previously monitored ClearBufferedInput functions.

`DMC_HANDOFF_HOOK`: four installed flags in the order above. Limits per thread/capture are
2048 buffer calls and 4096 calls each for graph, match, and selection; DMC_NATIVE_LIMIT kinds
6/7/8/9 report exhaustion. Parent request IDs form a same-thread call tree. Work outside that
tree is not observed; absence of a record is not proof of absence of execution.

`DMC_BUFFER_CALL`: request, parent request, buffer structure, owner argument, event, first
candidate set, second candidate set, native caller. `DMC_GRAPH_CALL`: request, parent, graph,
event, caller. `DMC_MATCH_CALL`: request, parent, rule, event, expected event descriptor,
condition count, caller. `DMC_SELECT_CALL`: request, parent, set, event, link count, condition
count, caller. `DMC_HANDOFF_END`: request, operation kind (0/1/2/3), original result; selection
returns a link pointer rather than a boolean.

`DMC_BUFFER`: request, phase (0 before, 1 after), buffer structure, count, raw policy word,
opaque owner, array pointer. `DMC_BUFFER_ITEM`: request, phase, item index, event pointer,
event descriptor (+0x60), raw +0x68/+0x6c words, event class. At most eight live pending entries
are read per snapshot; count remains uncapped so truncation is visible. Aggregator event
entry/exit also takes these snapshots, covering inline replacement outside the buffer helper.

`DMC_GRAPH`: request, phase, graph, root set (+0x60), active node (+0x88), raw graph flags
(+0x90), node state (+0xd0), node flags (+0xb4), node sets (+0x80/+0x88). A zero active node
does not by itself imply failure. `DMC_CONDITION_FIELDS`: request, condition, index, native
predicate address, copied byte count, four raw words starting at +0x60. Only predicates whose
native targets match 0xb73310 (DV common, 32 bytes) or 0xa232a0 (NT common, 8 bytes) are copied;
other conditions are identified but not evaluated or interpreted by the logger.

`DMC_OBJECT`: request, object, packed FName, class, outer, vtable. Current identity caching uses
96 append-only slots per thread/capture, including failed reads; duplicate attempts do not
repeat guarded reads. This intentionally limits dictionary coverage. Name lookup failures are
also cached to avoid repeatedly querying an unavailable table. `DMC_NAME`: FName index, wide flag, byte offset, valid byte
count, twelve little-endian 64-bit chunks. Decode as UTF-16LE if wide, otherwise the game's
8-bit name encoding; at most 192 bytes are copied per name. FName number is retained in the
upper 32 bits of DMC_OBJECT's packed name, not appended to dictionary strings. The name table
at eboot+0x3984330, entry flag bit zero, and string offset +0x10 come from FName::ToString at
0xb4bb0; reads are bounded and checked without calling game allocation or string functions.

Identity/condition traversal adds overhead and must still be validated in game. This change
does not add globally dispatched VM call tracing, held-event replay, or an unproven repair.

### Script probes

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
This assumes script bytecode stays immutable during a capture. Matching contextual float 0/1
assignments in watched objects, and identified candidate-property writes, include repeated
writes. Other objects contribute only discovery sites, capped at 128 per thread. There is a
hard limit of 4096 assignment pairs per thread per capture (`DMC_PROBE_LIMIT`). Snapshots use 128
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
Only watched objects reach branch decoding; unrelated script objects do not consume its budget.
Each watched object has its own 256-cache-miss/512-event limits per thread and capture, reported
by `DMC_BRANCH_LIMIT` (misses, events, object). Cache keys include both operand and object.
The contextual-prefix filter remains discovery, not a confirmed charge-condition filter.
Native action-graph decisions and decisions made by other objects can bypass these probes.

`DMC_ACTION_WATCH`: object, seed node, owner-property metadata, property metadata, cleanup-seed
flag, seed-node FName bits (zero for cleanup seeds). An object is watched after a cleanup-pattern
match or two adjacent contextual assignments of 1.0 to different properties of the same owner.
This matches observed RangedAttack/RangedCharge setup sequences but is NOT a unique identity
test. Up to 16 objects are watched per thread/capture (`DMC_WATCH_LIMIT` on overflow). Discovery
starts during assignment execution, so earlier branches of the first action invocation are
absent. Start with one successful charge to prime these filters. Later invocations that reuse
the same objects can be compared. New/replaced objects may require rediscovery; pointer reuse
and the provisional object layouts limit the interpretation.

`DMC_BRANCH`: request, object, frame, node, operand before evaluation, operand after evaluation,
changed-from-previous-outcome flag, first-observation flag, object cache-miss count, object event
count, seed-name-match flag. First observations and changed post-evaluation pointers are recorded.
Branches in functions sharing the setup seed's FName are recorded on every execution to retain
repeated starts, including inherited functions. This is a candidate lifecycle marker, not a
validated function-name resolution. The pointer change
is raw control-flow evidence, NOT an already decoded true/false condition or cancellation reason.
Compare against the function's bytecode before interpreting its direction. Other repeated
identical outcomes may be omitted. For the first 64 recorded events of each watched object,
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

### Current DmC condition-gate capture (2026-10-20)

The follow-up build also traces the shot's existing scheduled callbacks when its action
starts, including callbacks configured before F9. HolsterWeapon is identified by its runtime
FName and restricted to the player's input aggregator. It captures the callback's configured
relative time, the player's action-timeline delta/time, the actual HolsterWeapon/lifecycle
dispatch when available, and SetState_Awake's calling script and buffer state. These are
observations only. A late/missing callback is not established until discovery and hook status
are checked. Only the latest discovered firing action is tracked, so a new shot replaces the
previous shot's timer watch. Additional schedules created after action start are not inventoried.

Broad topology discovery is now OFF by default. Previously collected class/graph metadata
is sufficient for the current timer investigation. Actual gate results and condition identities
remain enabled. Set SHADPS4_DMC_TOPOLOGY=1 only when a fresh full relation inventory is needed.
The DMC_FIRE_HOOK record includes this setting. No additional user test sequence is required.

| kind | p0 onwards, in order |
| --- | --- |
| DMC_FIRE_HOOK | action-start, timeline-tick, awake installed; event budget, tick budget per shot, sample interval ms, topology enabled, item-dispatch installed, item budget per shot |
| DMC_FIRE_SCHEDULE | request, shot serial, action, node, timeline, callback object, packed callback FName, relative seconds bits, is HolsterWeapon, index, total, params, aggregate parent, timeline time before/after start bits, scheduled flags byte, inventory clipped, item kind byte, command pointer, token raw, condition pointer, repeat count raw, repeat interval bits |
| DMC_FIRE_START / DMC_FIRE_CALLBACK_BEGIN / DMC_FIRE_CALLBACK_END / DMC_FIRE_AWAKE_BEGIN / DMC_FIRE_AWAKE_END | request, shot serial, action, node, timeline, timeline time bits, node state, node flags, buffer count, buffer owner, script FName, action flags raw, action run-time bits |
| DMC_FIRE_TICK | shot serial, timeline, delta bits, timeline time before/after tick bits, callback still pending, remaining scheduled items |
| DMC_FIRE_ITEM_BEGIN | request, shot serial, timeline, item address, timeline time bits, callback object, callback FName, command pointer, token raw, argument raw, condition pointer, relative time bits, repeat count raw, repeat interval bits, item kind byte, flags byte, aggregate parent, active tick delta bits |
| DMC_FIRE_ITEM_END | same fields as DMC_FIRE_START, sharing the item-dispatch request |
| DMC_FIRE_AWAKE_ORIGIN | awake request, shot serial, executing function, function outer, FFrame context object, bytecode position, active item request (0 outside tracked dispatch), aggregate parent, active tick delta bits |
| DMC_FIRE_COMMAND | request, shot serial, command object, OnExecute function, function outer, native caller, active item request, aggregate parent, timeline time bits, active tick delta bits |

The command records cover the validated native command-to-OnExecute call site for the
tracked firing pawn, while the shot is pending. Awake origin also identifies the actual
script and context object when there is no tracked timeline dispatch. Scheduled payloads
are tagged unions: interpret command/token fields only for their corresponding item kind.
Inventory now includes up to 64 items rather than 16. Dispatch records mean the item
reached the dispatcher, not that its optional condition passed or its effect completed.

Tick and item budgets restart per shot. Sampling stops when the tracked node is awakened
or the firing action is cleaned up/interrupted; later reuse of its timeline is not treated
as the original pending shot. Native limit 15 includes the shot serial; limit 16 reports
the 64-item dispatch cap. The analyzer retains up to 512 ticks and 1024 fire states, with
the earlier first-128/first-512 keys kept as bounded compatibility views.

Timeline time is advanced after callbacks. Compare a callback against the current tick's
time plus delta, not just the pre-advance clock. Item relative time is not necessarily the
absolute deadline when an item was registered on an already-running timeline. No input,
charge threshold, command scheduling or cleanup behavior is changed by these probes.

All float words are IEEE-754 single-precision bits. FireState names marked CALLBACK include
the known native lifecycle call range, not only HolsterWeapon; inspect the script FName.
Timeline ticks are sampled every 50 ms and at the observed end of the callback watch, not
every frame. Native limit kinds 14/15 indicate shot-event/timeline-sample budget exhaustion.
The regular frame and pad probes remain available to assess timing interference.

This remains diagnosis, not a charge fix. Game files and input semantics are unchanged.
Use the diagnostic executable, not baseline. Capture one F9 segment containing
**success -> failure -> success**, keeping the gun held long enough in every attempt.
For failure, melee near the end of the initial shot; for success, delay melee. Release
all controls between attempts. Stop F9 and exit normally. No extra weapon/controller
matrix is required unless requested. Use shotgun or dual pistols, not the explosive gun.

The new probes filter transition searches to the tracked held-gun descriptor and pressed
inputs delivered while the gun is held. They collect event-match outcomes and actual
IsSatisfied returns into fixed local arrays, then emit changed results or a new observed
gun hold. No global VM instruction stream is recorded. The shared ProcessEvent wrapper
delegates immediately outside a traced search. Call overhead remains and must be checked
on the game machine. Records are emitted after calls, not at condition-call entry times.

The gun descriptor is learned from a held event with DV mask 8. Verify its DMC_OBJECT name
is DVButton_RangedAttack. DMC_GATE_HOOK reports all four installed probes. Missing hooks,
clipped arrays and exhausted budgets limit conclusions. Condition returns cover the two
validated native evaluation sites, not every nested expression/native event-condition leaf.

| kind | p0 onwards, in order |
| --- | --- |
| DMC_GATE_HOOK | select, match, condition dispatch, node exit installed; selection budget, condition capacity, match capacity, topology capacity |
| DMC_GATE_SELECT | request, set, descriptor, input type, selected link (0 rejected), condition count, match count, clipping bits (1 conditions, 2 matches), gun-hold epoch, aggregate parent, caller |
| DMC_GATE_CONDITION | request, index, condition object, function, context, pawn, returned bool word, caller, function flags raw |
| DMC_GATE_MATCH | request, index, event definition, returned bool word |
| DMC_BUFFER_ADMIT | buffer-call request, incoming event, buffer return value, first successfully matched event definition (0 if none observed), matcher caller, matcher call count, combo window, cached play mode, cached focus-attack type, held-button mask |
| DMC_GATE_INPUT | selection request, validated input-context class, input combo-window index, event flags raw, input event type, motion float bits, cached play mode byte, cached focus-attack byte, held-button mask, upgrades pointer |
| DMC_GATE_EVENT_CONFIG | selection request, event definition, declared condition index, condition object, native virtual slot 0x248, known kind (0 unknown, 1 NT common, 2 DV common, 3 combo window), packed configuration at +0x60, DV held-button mask at +0x68, DV button-list array/count, total declared conditions, metadata clipped at 8 |
| DMC_GATE_TOPOLOGY | request, set, descriptor, total links, links clipped |
| DMC_GATE_LINK | request, set, link, event definition, target node, graph, script-condition container |
| DMC_GATE_RELATION_SET | request, set, link (0 common), kind (0 common script, 1 event match, 2 link script), container, total conditions, conditions clipped |
| DMC_GATE_RELATION | request, set, link, kind, index, condition object, container |
| DMC_GATE_DEFINITION | request, condition object, virtual slot 0x248 raw, virtual slot 0x250 raw |
| DMC_GATE_TOKEN | request, condition object, index (0 primary), packed expected token FName, expected presence, total additional checks (at most 8 emitted) |
| DMC_NODE_EXIT | request, aggregator, outgoing graph, outgoing node, direct caller, aggregate parent, buffer owner matched node, node state raw, node flags raw, internal counter |

DMC_NODE_EXIT shares before/after DMC_BUFFER and DMC_BUFFER_ITEM records. Token records
describe configuration, not separately measured presence. Topology is discovered once per
set/descriptor pair per capture. Native limit kinds 11/12/13 indicate gate emissions,
node exits, or gate-key cache exhaustion. DMC_GATE_TOPOLOGY_LIMIT reports topology capacity.

The input-context snapshot is taken before the original selector. Its fields also enter
the existing change-detection cache, so a changed combo window or cached combat context
is not suppressed merely because the aggregate match result remains false. Snapshots
remain bounded by the existing 256-selection budget. No original predicate is re-evaluated.
Motion retains its raw float bits in emitted snapshots, but change detection compares
only `motion > 0`, matching the known native common predicate and avoiding analog noise.

Event-configuration records describe up to eight declared native conditions per observed
event match, emitted after the selector. They do NOT establish that every listed condition
executed: the game short-circuits on the first rejection. Only DMC_GATE_MATCH is the actual
aggregate return. Common/ComboWindow configuration can help interpret that return, but
unknown handlers and upgrade conditions require their own evidence. These fields address
the authored difference between the local shotgun-charge entry (Held plus combo window)
and the root charge entry (Held plus cached combat context and upgrade checks).

Buffer admission is a separate path from actual transition selection. It can accept an
event without running every candidate link's action conditions. DMC_BUFFER_ADMIT observes
the existing match hook only inside a held-gun buffer call and records the first actual
successful event-definition match, not a selected link or a started action. The validated
callers at relative 0xa1c5d8/0xa1c678 identify the first/second supplied transition set.
Admission records share the existing changed-buffer emission rule and 256-call budget;
input fields are snapshotted before the buffer call, and match results are observed inside it.
unchanged buffer calls are not individually reported. A nonzero buffer return with no
observed match must not be assigned a guessed accepting definition. Configuration for
the successful definition uses DMC_GATE_EVENT_CONFIG keyed by the buffer-call request.
No predicate is re-evaluated, and no additional native detour is installed.

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
