#!/usr/bin/env python3
"""Summarize PADTRACE v2 without assuming that reports equal game actions."""

import argparse
import bisect
import collections
import csv
import json
import struct
from pathlib import Path


BUTTONS = {
    0x10: "Up", 0x20: "Right", 0x40: "Down", 0x80: "Left",
    0x100: "L2", 0x200: "R2", 0x400: "L1", 0x800: "R1",
    0x1000: "Triangle", 0x2000: "Circle", 0x4000: "Cross", 0x8000: "Square",
    0x2: "L3", 0x4: "R3", 0x8: "Options",
}


def summarize(path):
    counts = collections.Counter()
    filters = collections.Counter()
    states = {}
    presses = {}
    pulses = []
    delivered = collections.defaultdict(list)
    previous_read = {}
    max_read_gap = {}
    max_age = 0
    intercepted = 0
    changed_output = []
    empty_reads = 0
    overwrites = 0
    markers = []
    metadata = []
    start = end = None
    max_raw_to_push = 0
    raw_times = {}
    dropped = 0
    limit_reached = False
    revision = None
    clocks = {}
    clock_previous = {}
    frame_previous = {}
    frame_max_gap = {}
    waits = {}
    wait_max_ms = 0
    wait_timeouts = 0
    read_callers = collections.Counter()
    dmc_pending = {}
    dmc_assignments = []
    dmc_total = 0
    dmc_branches = []
    dmc_probe_limits = []
    aggregate_pending = {}
    aggregate_calls = []
    aggregate_contexts = []
    handoff_calls = {}
    handoffs = []
    buffer_states = []
    buffer_items = []
    graph_states = []
    objects = {}
    name_chunks = collections.defaultdict(dict)
    handoff_sparse = False
    owner_switches = []
    owner_stacks = {}
    game_clocks = []
    gate_hooks = []
    gate_selections = []
    gate_conditions = []
    gate_matches = []
    gate_metadata = []
    node_exits = []
    fire_hooks = []
    fire_schedules = []
    fire_states = []
    fire_ticks = []
    fire_items = []
    fire_origins = []
    fire_commands = []

    def float_word(value):
        return struct.unpack("<f", (value & 0xffffffff).to_bytes(4, "little"))[0]

    def lines(stream):
        nonlocal dropped, limit_reached, revision
        for line in stream:
            if line.startswith("#"):
                text = line.strip()
                if text.startswith("# dropped="):
                    values = dict(item.split("=", 1) for item in text[2:].split())
                    dropped = max(dropped, int(values["dropped"]))
                    limit_reached |= values["limit_reached"] == "true"
                else:
                    metadata.append(text)
                    if "revision=" in text:
                        revision = text.split("revision=", 1)[1]
                continue
            yield line

    with path.open(encoding="utf-8") as stream:
        for row in csv.DictReader(lines(stream)):
            kind = row["kind"]
            counts[kind] += 1
            timestamp = int(row["monotonic_us"])
            start = timestamp if start is None else min(start, timestamp)
            end = timestamp if end is None else max(end, timestamp)
            p = [int(row[f"p{i}"]) for i in range(24)]
            if kind == "CAPTURE":
                presses.clear()
                states.clear()
                previous_read.clear()
                raw_times.clear()
                clock_previous.clear()
                frame_previous.clear()
                waits.clear()
                dmc_pending.clear()
                aggregate_pending.clear()
                markers.append({"recording": bool(p[0]), "monotonic_us": timestamp})
            elif kind == "CLOCK":
                key = (row["thread"], p[0], p[7], p[8])
                stat = clocks.setdefault(key, {"samples": 0, "backwards": 0,
                                               "frequency": p[2], "min_rate": None,
                                               "max_rate": None})
                stat["samples"] += 1
                stat["backwards"] += bool(p[4])
                previous = clock_previous.get(key)
                if previous and timestamp > previous[0] and p[2] > 0:
                    rate = (p[1] - previous[1]) * 1000000 / (p[2] * (timestamp - previous[0]))
                    stat["min_rate"] = rate if stat["min_rate"] is None else min(stat["min_rate"], rate)
                    stat["max_rate"] = rate if stat["max_rate"] is None else max(stat["max_rate"], rate)
                clock_previous[key] = (timestamp, p[1])
            elif kind in ("FRAME_PRESENT", "VBLANK"):
                key = (row["thread"], kind)
                if key in frame_previous:
                    frame_max_gap[key] = max(frame_max_gap.get(key, 0), timestamp - frame_previous[key])
                frame_previous[key] = timestamp
            elif kind == "EQUEUE_WAIT":
                waits[p[0]] = timestamp
            elif kind == "EQUEUE_RETURN":
                began = waits.pop(p[0], None)
                if began is not None:
                    wait_max_ms = max(wait_max_ms, (timestamp - began) / 1000)
                wait_timeouts += p[2] == 0
            elif kind == "READ_STATE":
                read_callers[hex(p[2])] += 1
            elif kind == "DMC_HANDOFF_SPARSE":
                handoff_sparse = bool(p[0])
            elif kind == "DMC_GATE_HOOK":
                gate_hooks.append({"monotonic_us": timestamp, "installed": p[:4],
                                   "budgets_raw": p[4:8]})
            elif kind == "DMC_FIRE_HOOK":
                fire_hooks.append({"monotonic_us": timestamp, "installed": p[:3],
                                   "event_budget": p[3], "tick_budget": p[4],
                                   "sample_interval_ms": p[5], "topology_enabled": bool(p[6]),
                                   "item_hook": bool(p[7]), "item_budget_per_shot": p[8]})
            elif kind == "DMC_FIRE_SCHEDULE" and len(fire_schedules) < 2048:
                fire_schedules.append({
                    "monotonic_us": timestamp, "request": p[0], "serial": p[1],
                    "action": hex(p[2]), "node": hex(p[3]), "timeline": hex(p[4]),
                    "callback_object": hex(p[5]), "callback_name_raw": p[6],
                    "relative_time_s": float_word(p[7]), "holster": bool(p[8]),
                    "index": p[9], "total": p[10], "params": hex(p[11]), "parent": p[12],
                    "timeline_before_after_s": [float_word(p[13]), float_word(p[14])],
                    "flags_raw": p[15], "clipped": bool(p[16]),
                    "kind_raw": p[17], "command": hex(p[18]), "token_raw": p[19],
                    "condition": hex(p[20]), "repeat_count_raw": p[21],
                    "repeat_interval_s": float_word(p[22]),
                })
            elif kind in ("DMC_FIRE_START", "DMC_FIRE_CALLBACK_BEGIN", "DMC_FIRE_CALLBACK_END",
                          "DMC_FIRE_AWAKE_BEGIN", "DMC_FIRE_AWAKE_END",
                          "DMC_FIRE_ITEM_END") and len(fire_states) < 1024:
                fire_states.append({
                    "monotonic_us": timestamp, "kind": kind, "request": p[0], "serial": p[1],
                    "action": hex(p[2]), "node": hex(p[3]), "timeline": hex(p[4]),
                    "timeline_time_s": float_word(p[5]), "node_state_flags_raw": p[6:8],
                    "buffer_count": p[8], "buffer_owner": hex(p[9]), "script_name_raw": p[10],
                    "action_flags_raw": p[11], "action_run_time_s": float_word(p[12]),
                })
            elif kind == "DMC_FIRE_TICK" and len(fire_ticks) < 512:
                fire_ticks.append({
                    "monotonic_us": timestamp, "serial": p[0], "timeline": hex(p[1]),
                    "delta_s": float_word(p[2]),
                    "timeline_before_after_s": [float_word(p[3]), float_word(p[4])],
                    "pending": bool(p[5]), "remaining_items": p[6],
                })
            elif kind == "DMC_FIRE_ITEM_BEGIN" and len(fire_items) < 512:
                fire_items.append({
                    "monotonic_us": timestamp, "request": p[0], "serial": p[1],
                    "timeline": hex(p[2]), "entry": hex(p[3]),
                    "timeline_time_s": float_word(p[4]), "callback_object": hex(p[5]),
                    "callback_name_raw": p[6], "command": hex(p[7]), "token_raw": p[8],
                    "argument_raw": p[9], "condition": hex(p[10]),
                    "relative_time_s": float_word(p[11]), "repeat_count_raw": p[12],
                    "repeat_interval_s": float_word(p[13]), "kind_raw": p[14],
                    "flags_raw": p[15], "parent": p[16], "tick_delta_s": float_word(p[17]),
                })
            elif kind == "DMC_FIRE_AWAKE_ORIGIN" and len(fire_origins) < 128:
                fire_origins.append({
                    "monotonic_us": timestamp, "request": p[0], "serial": p[1],
                    "function": hex(p[2]), "function_outer": hex(p[3]), "context": hex(p[4]),
                    "code": hex(p[5]), "item_request": p[6], "parent": p[7],
                    "tick_delta_s": float_word(p[8]),
                })
            elif kind == "DMC_FIRE_COMMAND" and len(fire_commands) < 128:
                fire_commands.append({
                    "monotonic_us": timestamp, "request": p[0], "serial": p[1],
                    "command": hex(p[2]), "function": hex(p[3]), "function_outer": hex(p[4]),
                    "caller": hex(p[5]), "item_request": p[6], "parent": p[7],
                    "timeline_time_s": float_word(p[8]), "tick_delta_s": float_word(p[9]),
                })
            elif kind == "DMC_GATE_SELECT" and len(gate_selections) < 256:
                gate_selections.append({
                    "monotonic_us": timestamp, "request": p[0], "set": hex(p[1]),
                    "descriptor": hex(p[2]), "type": p[3], "selected_link": hex(p[4]),
                    "condition_count": p[5], "match_count": p[6], "clipped_raw": p[7],
                    "gun_epoch": p[8], "parent": p[9], "caller": hex(p[10]),
                })
            elif kind == "DMC_GATE_CONDITION" and len(gate_conditions) < 8192:
                gate_conditions.append({
                    "request": p[0], "index": p[1], "condition": hex(p[2]),
                    "function": hex(p[3]), "context": hex(p[4]), "pawn": hex(p[5]),
                    "result": p[6], "caller": hex(p[7]), "function_flags_raw": p[8],
                })
            elif kind == "DMC_GATE_MATCH" and len(gate_matches) < 8192:
                gate_matches.append({"request": p[0], "index": p[1],
                                     "definition": hex(p[2]), "result": p[3]})
            elif kind in ("DMC_GATE_TOPOLOGY", "DMC_GATE_LINK", "DMC_GATE_RELATION_SET",
                          "DMC_GATE_RELATION", "DMC_GATE_TOKEN", "DMC_GATE_DEFINITION",
                          "DMC_GATE_TOPOLOGY_LIMIT") and len(gate_metadata) < 4096:
                gate_metadata.append({"kind": kind, "request": p[0], "payload_raw": p[1:8]})
            elif kind == "DMC_NODE_EXIT" and len(node_exits) < 64:
                node_exits.append({
                    "monotonic_us": timestamp, "request": p[0], "aggregator": hex(p[1]),
                    "graph": hex(p[2]), "node": hex(p[3]), "caller": hex(p[4]),
                    "parent": p[5], "buffer_owner_matched": bool(p[6]),
                    "node_state_flags_raw": p[7:9], "internal_counter": p[9],
                })
            elif kind == "DMC_OWNER_SWITCH" and len(owner_switches) < 64:
                owner_switches.append({
                    "monotonic_us": timestamp, "request": p[0], "path": p[1],
                    "aggregator": hex(p[2]), "incoming_owner": hex(p[3]),
                    "caller": hex(p[4]), "parent": p[5],
                    "active_before_after": [hex(p[6]), hex(p[7])],
                    "internal_counter_before_after": p[8:10], "flags_raw": p[10],
                })
            elif kind == "DMC_OWNER_STACK":
                owner_stacks[p[0]] = [hex(v) for v in p[2:2 + min(max(p[1], 0), 6)]]
            elif kind == "DMC_GAME_CLOCK" and len(game_clocks) < 200:
                def float_bits(value):
                    return struct.unpack("<f", (value & 0xffffffff).to_bytes(4, "little"))[0]
                def double_bits(value):
                    return struct.unpack("<d", (value & ((1 << 64) - 1)).to_bytes(8, "little"))[0]
                world_time = float_bits(p[3]) if p[2] else None
                since = float_bits(p[12]) if p[9] else None
                held = p[9] != 0 and p[10] < 32 and bool(p[11] & (1 << p[10]))
                game_clocks.append({
                    "monotonic_us": timestamp, "request": p[0], "world": hex(p[1]),
                    "readable": bool(p[2]), "world_time_s": world_time,
                    "adjacent_time_words_raw": p[4:7],
                    "seconds_per_cycle": [double_bits(p[7]), double_bits(p[8])],
                    "input": hex(p[9]), "button": p[10], "held_mask": hex(p[11]),
                    "held_since_s": since,
                    "held_elapsed_s": world_time - since
                    if held and world_time is not None and since is not None else None,
                })
            elif kind == "DMC_NAME":
                raw = b"".join((v & ((1 << 64) - 1)).to_bytes(8, "little") for v in p[4:16])
                name_chunks[(p[0], bool(p[1]))][p[2]] = raw[:max(0, min(p[3], 96))]
            elif kind == "DMC_OBJECT":
                packed = p[2] & ((1 << 64) - 1)
                objects[hex(p[1])] = {"name_index": packed & 0xffffffff,
                                      "name_number": packed >> 32,
                                      "class": hex(p[3]), "outer": hex(p[4]),
                                      "vtable": hex(p[5])}
            elif kind in ("DMC_BUFFER_CALL", "DMC_GRAPH_CALL", "DMC_MATCH_CALL", "DMC_SELECT_CALL"):
                handoff_calls[p[0]] = {"request": p[0], "parent": p[1], "kind": kind,
                                       "monotonic_us": timestamp, "arguments_raw": p[2:8]}
            elif kind == "DMC_HANDOFF_END":
                began = handoff_calls.pop(p[0], None)
                if began and len(handoffs) < 2000:
                    began.update(result_raw=p[2], duration_us=timestamp - began["monotonic_us"])
                    handoffs.append(began)
            elif kind == "DMC_BUFFER" and len(buffer_states) < 2000:
                buffer_states.append({"request": p[0], "phase": p[1],
                                      "monotonic_us": timestamp, "buffer": hex(p[2]),
                                      "count": p[3], "policy_raw": p[4], "owner": hex(p[5])})
            elif kind == "DMC_BUFFER_ITEM" and len(buffer_items) < 2000:
                buffer_items.append({"request": p[0], "phase": p[1], "index": p[2],
                                     "event": hex(p[3]), "descriptor": hex(p[4]),
                                     "raw_68_6c": p[5:7], "class": hex(p[7])})
            elif kind == "DMC_GRAPH" and len(graph_states) < 2000:
                graph_states.append({"request": p[0], "phase": p[1], "graph": hex(p[2]),
                                     "root": hex(p[3]), "node": hex(p[4]),
                                     "raw_flags_state_sets": p[5:10]})
            elif kind in ("DMC_AGG_EVENT_BEGIN", "DMC_AGG_CLEAR_BEGIN", "DMC_AGG_FLUSH_BEGIN"):
                aggregate_pending[p[0]] = (timestamp, kind, p)
            elif kind == "DMC_AGG_CONTEXT":
                if len(aggregate_contexts) < 100:
                    aggregate_contexts.append({
                        "request": p[0], "event": hex(p[1]), "class": hex(p[2]),
                        "input_class_matches": bool(p[3]), "type_raw": p[4],
                        "held_mask_raw": hex(p[5]), "event_words_raw": [hex(v) for v in p[6:9]],
                    })
            elif kind in ("DMC_AGG_EVENT_END", "DMC_AGG_CLEAR_END", "DMC_AGG_FLUSH_END"):
                began = aggregate_pending.pop(p[0], None)
                if began and len(aggregate_calls) < 100:
                    before, operation, source = began
                    aggregate_calls.append({
                        "monotonic_us": before, "operation": operation,
                        "request": p[0], "parent_request": source[13],
                        "aggregator": hex(source[1]), "event": hex(source[2]),
                        "mode_raw": source[3], "caller": hex(source[11]),
                        "duration_us": timestamp - before, "result_raw": p[12],
                        "buffer_count_before_after": [source[4], p[4]],
                        "owner_before_after": [hex(source[5]), hex(p[5])],
                        "pending_before_after": [source[8], p[8]],
                        "flush_count_before_after": [source[9], p[9]],
                    })
            elif kind == "DMC_LET_BEGIN":
                dmc_pending[p[0]] = (timestamp, p)
            elif kind == "DMC_BRANCH":
                if len(dmc_branches) < 100:
                    dmc_branches.append({
                        "monotonic_us": timestamp, "object": hex(p[1]), "node": hex(p[3]),
                        "operand_before": hex(p[4]), "operand_after": hex(p[5]),
                        "changed_outcome": bool(p[6]), "first_observation": bool(p[7]),
                    })
            elif kind in ("DMC_PROBE_LIMIT", "DMC_BRANCH_LIMIT", "DMC_WATCH_LIMIT",
                          "DMC_NATIVE_LIMIT"):
                dmc_probe_limits.append({"monotonic_us": timestamp, "kind": kind,
                                         "thread": row["thread"], "p0": p[0], "p1": p[1]})
            elif kind == "DMC_LET_END":
                began = dmc_pending.pop(p[0], None)
                if began:
                    dmc_total += 1
                    if len(dmc_assignments) < 100:
                        before, source = began
                        dmc_assignments.append({
                            "monotonic_us": before, "property": hex(source[4]),
                            "owner_property": hex(source[5]), "destination": hex(p[1]),
                            "literal_bits": hex(source[6]), "readback_bits": hex(p[3]),
                            "readable": bool(p[4]), "property_matches": bool(p[5]),
                            "cleanup_candidate": bool(source[7]),
                            "tracked_candidate": bool(source[8]),
                            "handler_duration_us": timestamp - before,
                        })
            elif kind == "RAW":
                raw_times[p[0]] = timestamp
            elif kind == "FILTER":
                filters[p[1]] += 1
            elif kind == "MARKER":
                markers.append({"event_id": p[0], "monotonic_us": timestamp})
            elif kind == "PUSH":
                user, sample_time, buttons = p[1], p[3], p[4]
                old = states.get(user, buttons)
                for mask, name in BUTTONS.items():
                    key = (user, mask)
                    if buttons & mask and not old & mask:
                        presses[key] = (sample_time, timestamp, p[2])
                    elif old & mask and not buttons & mask:
                        press = presses.pop(key, None)
                        if press:
                            pulses.append({
                                "user": user, "button": name, "mask": mask,
                                "press_sample_us": press[0], "release_sample_us": sample_time,
                                "press_monotonic_us": press[1],
                                "duration_ms": (sample_time - press[0]) / 1000,
                                "press_state_id": press[2], "release_state_id": p[2],
                            })
                states[user] = buttons
                overwrites += p[8]
                if p[0] in raw_times:
                    max_raw_to_push = max(max_raw_to_push, timestamp - raw_times[p[0]])
            elif kind == "READ_BEGIN":
                key = (row["thread"], p[1])
                if key in previous_read:
                    max_read_gap[key] = max(max_read_gap.get(key, 0),
                                            timestamp - previous_read[key])
                previous_read[key] = timestamp
            elif kind == "READ_END":
                empty_reads += p[3] == 0
            elif kind == "READ_SAMPLE":
                max_age = max(max_age, p[9] - p[8])
                source, output = p[10], p[11]
                if output & 0x80000000:
                    intercepted += 1
                elif source != output and len(changed_output) < 30:
                    changed_output.append({"request": p[0], "state_id": p[6],
                                           "source": hex(source), "output": hex(output)})
                for mask in BUTTONS:
                    if output & mask:
                        delivered[(p[2], mask)].append(p[8])

    for values in delivered.values():
        values.sort()
    for pulse in pulses:
        times = delivered[(pulse["user"], pulse["mask"])]
        begin = bisect.bisect_left(times, pulse["press_sample_us"])
        finish = bisect.bisect_left(times, pulse["release_sample_us"])
        pulse["returned_samples_during_hold"] = finish - begin
    missed = [pulse for pulse in pulses if not pulse["returned_samples_during_hold"]]
    names = {index: b"".join(chunks[offset] for offset in sorted(chunks)).decode(
        "utf-16-le" if wide else "latin-1", errors="replace")
        for (index, wide), chunks in name_chunks.items()}
    for obj in objects.values():
        obj["name"] = names.get(obj["name_index"])
    for item in fire_schedules:
        item["callback_name"] = names.get(item["callback_name_raw"] & 0xffffffff)
    for item in fire_items:
        item["callback_name"] = names.get(item["callback_name_raw"] & 0xffffffff)
    for item in fire_states:
        item["script_name"] = names.get(item["script_name_raw"] & 0xffffffff)

    return {
        "revision": revision,
        "duration_seconds": (end - start) / 1000000 if start is not None else 0,
        "events": dict(counts), "dropped_records": dropped, "limit_reached": limit_reached,
        "routing_filters": dict(filters),
        "read_state_callers": dict(read_callers),
        "dmc_assignment_count": dmc_total,
        "dmc_aggregate_calls_first_100": aggregate_calls,
        "dmc_aggregate_contexts_first_100": aggregate_contexts,
        "dmc_objects": objects,
        "dmc_handoff_sparse": handoff_sparse,
        "dmc_owner_switches": owner_switches,
        "dmc_owner_stacks": owner_stacks,
        "dmc_game_clock_samples_first_200": game_clocks,
        "dmc_gate_hooks": gate_hooks,
        "dmc_gate_selections_first_256": gate_selections,
        "dmc_gate_conditions_first_8192": gate_conditions,
        "dmc_gate_matches_first_8192": gate_matches,
        "dmc_gate_metadata_first_4096": gate_metadata,
        "dmc_node_exits_first_64": node_exits,
        "dmc_fire_hooks": fire_hooks,
        "dmc_fire_schedules_first_2048": fire_schedules,
        "dmc_fire_states_first_512": fire_states[:512],
        "dmc_fire_states_first_1024": fire_states,
        "dmc_fire_ticks_first_128": fire_ticks[:128],
        "dmc_fire_ticks_first_512": fire_ticks,
        "dmc_fire_items_first_512": fire_items,
        "dmc_fire_origins_first_128": fire_origins,
        "dmc_fire_commands_first_128": fire_commands,
        "dmc_handoff_duration_note": "Sparse CALL/END timestamps cover emission, not game execution"
        if handoff_sparse else "Detailed CALL/END timestamps include instrumentation overhead",
        "dmc_handoffs_first_2000": handoffs,
        "dmc_buffers_first_2000": buffer_states,
        "dmc_buffer_items_first_2000": buffer_items,
        "dmc_graphs_first_2000": graph_states,
        "dmc_branches_first_100": dmc_branches,
        "dmc_probe_limits": dmc_probe_limits,
        "dmc_assignments_first_100": dmc_assignments,
        "clocks": [{"thread": key[0], "api": key[1], "caller": hex(key[2]),
                    "clock_id": key[3], **value} for key, value in clocks.items()],
        "max_frame_gap_ms": {f"{key[0]}:{key[1]}": value / 1000
                             for key, value in frame_max_gap.items()},
        "max_equeue_wait_ms": wait_max_ms, "equeue_timeouts": wait_timeouts,
        "markers": markers, "max_sample_age_ms": max_age / 1000,
        "max_raw_to_push_ms": max_raw_to_push / 1000,
        "max_read_gap_ms_by_thread_handle": {
            f"{key[0]}:{key[1]}": value / 1000 for key, value in max_read_gap.items()
        },
        "empty_reads": empty_reads, "intercepted_samples": intercepted,
        "unexpected_source_output_differences": changed_output,
        "queue_overwrites": overwrites,
        "completed_button_holds": len(pulses),
        "holds_with_no_returned_pressed_sample": missed[:50],
        "holds_with_no_returned_pressed_sample_total": len(missed),
        "button_holds": pulses[-200:],
        "configuration": metadata,
        "interpretation": [
            "Returned samples do not prove the game consumed them or accepted an action.",
            "Queue overwrite is expected with current-state reads; it is not proof of lost input.",
            "Missing samples can reflect capture boundaries, disconnection or interception.",
            "Drops, limit reached, or a forced process termination make the capture incomplete.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(summarize(args.trace), ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
    else:
        print(report)


if __name__ == "__main__":
    main()
