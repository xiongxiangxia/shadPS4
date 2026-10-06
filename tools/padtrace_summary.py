#!/usr/bin/env python3
"""Summarize PADTRACE v2 without assuming that reports equal game actions."""

import argparse
import bisect
import collections
import csv
import json
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
    return {
        "revision": revision,
        "duration_seconds": (end - start) / 1000000 if start is not None else 0,
        "events": dict(counts), "dropped_records": dropped, "limit_reached": limit_reached,
        "routing_filters": dict(filters),
        "read_state_callers": dict(read_callers),
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
