# JC jump-cancel melee capture

This is observation, not a fix for missing melee. Use COMPAT-experimental so the verified gun compatibility changes remain active. ROOTCAUSE-diagnostic retains the older gun-focused probes and does not include this capture. Game files and jump/melee input consumption are not modified.

F9 starts/stops capture. Arrive in the test area before starting. Keep each capture short (roughly 5–10 seconds): airborne demon pull, then the usual closely spaced jump followed by melee. Aim to capture one missing-melee attempt and one successful attempt; report the actual order. Do not deliberately change timing for every attempt or assume every attempt failed. Stop with F9 and exit normally; provide the newest padtrace CSV and ordinary shad_log.txt. If capture noticeably changes gameplay, stop and report the disturbance; do not treat its timings as undisturbed evidence.

Keyboard users need not install special tools. Existing DISPATCH/FILTER/RAW/INPUT_ACCEPT records cover SDL delivery/filtering/mapping; BUTTON/PUSH/QUEUE_READ/READ_SAMPLE/READ_STATE cover mapped input and the values returned to the game. These are application-observed events, not independent measurements of physical keyboard switches. Compare sequence IDs as well as time, and inspect all samples in a returned batch before deciding the game reversed or lost an input.

The added game records use the existing timestamped asynchronous recorder. Each thread/capture permits at most 4096 JC budget units (including identity/dictionary work), with JC_LIMIT on exhaustion and at most 128 decoded names. F9 resets this budget. Exhaustion or dropped rows invalidates arguments based on absent records. No global field/graph walk, instruction stepping, stack unwinding, or per-instruction logging is enabled. One validated graph-event detour is added to the six existing compatibility detours; when F9 is off it forwards directly to the original graph function.

## Record layout

All pointers are observations only; pooled event addresses are not identities across time. Nesting uses request/parent IDs, not event-pointer equality. Game results must be interpreted at their own layer: an aggregate/graph return value or selected link does not prove the attack animation executed.

- JC_CAPTURE: format version=1, budget=4096, name limit=128, compatibility active.
- JC_BEGIN p0=request, p1=parent, p2=stage (0 graph, 1 aggregate, 2 selector), p3=owner, p4=event, p5=kind (1 Jump, 2 LightMelee, 3 StrongMelee, 4 RangedAttack), p6=raw event type, p7=combo, p8=flags, p9=event play mode, p10=held input mask, p11=NTInput current mask, p12=mask readable, p13=NTInput pointer, p14=caller RVA. Modes: Normal=0, Demon=1, Angel=2. Do not confuse NTInput bit positions with the scePad mask.
- JC_END: request, stage, result (selector uses the returned link pointer).
- JC_ACTIVE: request, before=0/after=1, graph, active node. Only synchronous graph-call boundaries are observed, not all future node changes.
- JC_MATCH: active request, candidate definition, native match result. This is before link conditions, not final acceptance.
- JC_CONDITION: active request, caller RVA, condition object, function, context, pawn, original result. Captures verified graph/selector/link condition sites only. For gun events this precedes any compatibility override; jump and melee conditions are not rewritten.
- JC_TOKEN: active request, condition, raw token FName. Only emitted for token-condition class.
- JC_LIFECYCLE: active request, object, function, caller RVA. Limited to the already-identified immediate action-lifecycle dispatch range. Absence does not prove no later/asynchronous action callback occurred.
- JC_OBJECT: active request, object, full FName, class, full class FName. Low 32 bits of an FName are its name index; high bits are the instance suffix.
- JC_NAME: index, wide flag, sixteen little-endian 64-bit payload words (128 bytes, null-terminated if the name fits). Names longer than this payload are truncated. The data is copied while the game objects are live; no event pointer is retained for later use.

The first capture should locate the first divergence: mapped input to scePad delivery, delivery to game event creation, or event creation to a rejected/accepted transition. If a selected melee later disappears, a further tightly scoped lifecycle/buffer observation may be necessary. These records do not by themselves prove an emulator defect or reproduce PS4 hardware behavior.
