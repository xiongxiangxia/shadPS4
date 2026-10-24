# JC jump-cancel melee capture

COMPAT-experimental now also contains an experimental bounded melee retry for Demon Pull Leap. This is a workaround, not a confirmed emulator root-cause fix. Gun compatibility remains active. ROOTCAUSE-diagnostic does not contain either compatibility change. Game files are not modified.

## Bounded melee retry

Start with F9 off. After a Normal-mode Jump selects the specific Dante_DevilPull.Nodes.DemonPullJump link, a new Normal-mode LightMelee/StrongMelee press may be retained only if the player's aggregate rejects it and the Graph_MeleeRoot MeleeDenied condition was observed rejecting that same live event. The pending slot stores the button descriptor, never the pooled event pointer. The synchronous rejection observer is cleared before returning from the aggregate.

On the existing input-enabled PlayerTick path, use the controller's verified native SendActionGraphEvent implementation to construct a fresh Pressed event. All original melee conditions remain in force. Retry at most once per 16 ms, at most 12 attempts and no longer than 200 ms from rejection; the Leap eligibility itself expires after 400 ms. Stop at the first native acceptance (which may mean buffering, not animation execution). No global token override or Leap removal is added.

Require the same player/pawn/input context, the original Leap node still active in the current player's graph list, Normal mode, positive health and m_fcpInAir. A new Pressed command cancels the old pending slot; a newer eligible melee rejection can replace it. An accepted same-button Pressed/Held event also clears it. Releasing a short tap alone does not cancel an unaccepted press. Context changes, leaving Leap, landing, changing mode, death, expiry, or an unknown native sender discard it. A pause can prevent ticking; elapsed wall time then prevents stale replay on resume. No long-lived event object is retained and no input is inserted into unrelated gameplay states.

The possible visible side effect is a single delayed melee, at most 200 ms after the rejected press. This does not guarantee the same timing/route as a real PS4 JC. It deliberately does not carry the attack beyond Leap; if permission stays closed until Leap exits, it cancels instead of forcing an attack. Runtime performance and effectiveness are not yet verified.

Use dmc-jc-off.cmd to disable only this retry while keeping the working gun compatibility. dmc-compat-on.cmd enables both. SHADPS4_DMC_JC_COMPAT=0 is the independent environment switch; restart to apply. SHADPS4_DMC_COMPAT=0 still disables all compatibility. Ordinary shad_log.txt markers [DMC JC COMPAT] report enabled, retained, accepted, or cancelled; F9 is not required for initial play testing.

## Optional observation

F9 starts/stops capture. Arrive in the test area before starting. Keep each capture short (roughly 5–10 seconds): airborne demon pull, then the usual closely spaced jump followed by melee. Aim to capture one missing-melee attempt and one successful attempt; report the actual order. Do not deliberately change timing for every attempt or assume every attempt failed. Stop with F9 and exit normally; provide the newest padtrace CSV and ordinary shad_log.txt. If capture noticeably changes gameplay, stop and report the disturbance; do not treat its timings as undisturbed evidence.

Keyboard users need not install special tools. Existing DISPATCH/FILTER/RAW/INPUT_ACCEPT records cover SDL delivery/filtering/mapping; BUTTON/PUSH/QUEUE_READ/READ_SAMPLE/READ_STATE cover mapped input and the values returned to the game. These are application-observed events, not independent measurements of physical keyboard switches. Compare sequence IDs as well as time, and inspect all samples in a returned batch before deciding the game reversed or lost an input.

The added game records use the existing timestamped asynchronous recorder. Each thread/capture permits at most 4096 JC budget units (including identity/dictionary work), with JC_LIMIT on exhaustion and at most 128 decoded names. F9 resets this budget. Exhaustion or dropped rows invalidates arguments based on absent records. No global field/graph walk, instruction stepping, stack unwinding, or per-instruction logging is enabled. One validated graph-event detour is added to the six existing compatibility detours; when F9 is off it forwards directly to the original graph function.

Format 2 narrows full JC scopes to native input type 0 (Pressed), verified against keyboard/button transitions in the first capture. Held/Released remain available in the existing input-delivery records but no longer activate full JC graph/condition logging. Jump-Held graph processing retains sparse result/active-node changes (up to 64 graph identities), so delayed jump acceptance is not completely hidden. Matching records include only definitions with the current event's descriptor; expected mismatches against other event classes are omitted. Object identity records are deduplicated by pointer/name/class, with at most 512 identities. Raw graph/condition pointers remain recorded if identity/name dictionaries are full. These filters reduce work rather than raising the 4096 budget. They do not cover every Held rejection condition; absence in this scope is not proof it never occurred.

## Record layout

All pointers are observations only; pooled event addresses are not identities across time. Nesting uses request/parent IDs, not event-pointer equality. Game results must be interpreted at their own layer: an aggregate/graph return value or selected link does not prove the attack animation executed.

- JC_CAPTURE: format version=2, budget=4096, name limit=128, compatibility active, identity limit=512. Version 1 did not filter Held/Released or unrelated candidate descriptors and could exhaust its budget rapidly.
- JC_BEGIN p0=request, p1=parent, p2=stage (0 graph, 1 aggregate, 2 selector), p3=owner, p4=event, p5=kind (1 Jump, 2 LightMelee, 3 StrongMelee, 4 RangedAttack), p6=raw event type, p7=combo, p8=flags, p9=event play mode, p10=held input mask, p11=NTInput current mask, p12=mask readable, p13=NTInput pointer, p14=caller RVA. Modes: Normal=0, Demon=1, Angel=2. Do not confuse NTInput bit positions with the scePad mask.
- JC_END: request, stage, result (selector uses the returned link pointer).
- JC_ACTIVE: request, before=0/after=1, graph, active node. Only synchronous graph-call boundaries are observed, not all future node changes.
- JC_HELD_GRAPH: graph, active node before, active node after, result. Jump-Held only; emitted for first accepted result, changes relative to the graph's previous result/after-node pair, or a node change during this call. Repeated unchanged results are omitted. No parent request or condition expansion is implied.
- JC_MATCH: active request, candidate definition, native match result. This is before link conditions, not final acceptance.
- JC_CONDITION: active request, caller RVA, condition object, function, context, pawn, original result. Captures verified graph/selector/link condition sites only. For gun events this precedes any compatibility override; jump and melee conditions are not rewritten.
- JC_TOKEN: active request, condition, raw token FName. Only emitted for token-condition class.
- JC_LIFECYCLE: active request, object, function, caller RVA. Limited to the already-identified immediate action-lifecycle dispatch range. Absence does not prove no later/asynchronous action callback occurred.
- JC_OBJECT: active request, object, full FName, class, full class FName. Low 32 bits of an FName are its name index; high bits are the instance suffix.
- JC_NAME: index, wide flag, sixteen little-endian 64-bit payload words (128 bytes, null-terminated if the name fits). Names longer than this payload are truncated. The data is copied while the game objects are live; no event pointer is retained for later use.

The first capture should locate the first divergence: mapped input to scePad delivery, delivery to game event creation, or event creation to a rejected/accepted transition. If a selected melee later disappears, a further tightly scoped lifecycle/buffer observation may be necessary. These records do not by themselves prove an emulator defect or reproduce PS4 hardware behavior.
