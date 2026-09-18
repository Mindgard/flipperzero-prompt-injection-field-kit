#!/usr/bin/env python3
"""Decode a UART line from a logic analyser's raw digital export.

Why this exists rather than using the analyser's own Async Serial
decoder: that decoder is *told* the baud rate, so it can only report
whether bytes decoded at the baud you claimed. It cannot tell you the
baud is wrong. Measuring the actual bit period needs the edge
timestamps, which is what a raw digital export gives you.

That distinction is the same one `docs/gpio-validation.md` makes about
the loopback self-test: a measurement that shares an assumption with the
thing being measured cannot find an error in that assumption.

The figures in that document were produced by decoding the committed
edge lists this way. This module is that decoder, factored out so the
numbers can be reproduced and so `gpio_baud_sweep.py` can reuse it.

Input format is a Logic 2 raw digital CSV: a time column followed by one
column per captured channel, one row per sample or per transition.

    Time [s],Channel 0,Channel 3
    0.000000000,0,1
    0.948089000,0,0

Used as a library:

    from uart_edge_decode import load_edges, measure_baud, decode_bytes

Or standalone, to check a capture without knowing its baud:

    python3 tools/uart_edge_decode.py docs/captures/gpio-burst-115200.csv
    python3 tools/uart_edge_decode.py capture.csv --channel 3 --baud 115200
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections.abc import Sequence

# 8N1 framing: one start bit, eight data, one stop. Every timing figure
# here assumes it, because that is what the kit sends.
BITS_PER_FRAME = 10

# Standard rates a capture is likely to be at, for --baud auto.
COMMON_BAUDS = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)


def load_edges(path: str, channel: int | None = None) -> tuple[list[tuple[float, int]], int]:
    """Read a raw digital CSV into [(timestamp, level), ...] transitions.

    Returns the transition list and the channel number used. Rows that
    repeat the previous level are dropped, so a sample-per-row export and
    a transition-per-row export both reduce to the same thing.

    With channel=None the busiest channel is chosen. That is deliberate:
    a capture with the probe on the wrong pin shows up as "0 transitions"
    on the channel you expected, and picking the busiest one instead
    surfaces where the signal actually was.
    """
    with open(path, newline="") as handle:
        reader = csv.reader(handle)
        try:
            header = next(reader)
        except StopIteration:
            raise SystemExit(f"{path}: file is empty")

        if len(header) < 2:
            raise SystemExit(
                f"{path}: expected a time column plus at least one channel, "
                f"got {header!r}. This wants a raw digital export, not an "
                f"analyzer data table."
            )

        # Column order is Time, then channels in capture order. Map the
        # header names so --channel refers to the channel number Logic 2
        # shows rather than a column index.
        channel_cols: dict[int, int] = {}
        for idx, name in enumerate(header[1:], start=1):
            digits = "".join(c for c in name if c.isdigit())
            if digits:
                channel_cols[int(digits)] = idx

        if not channel_cols:
            raise SystemExit(f"{path}: no channel columns found in {header!r}")

        rows = [row for row in reader if len(row) >= 2]

    if not rows:
        raise SystemExit(f"{path}: no data rows")

    def transitions_for(col: int) -> list[tuple[float, int]]:
        out: list[tuple[float, int]] = []
        prev: int | None = None
        for row in rows:
            try:
                stamp = float(row[0])
                level = int(row[col])
            except (ValueError, IndexError):
                continue
            if level != prev:
                out.append((stamp, level))
                prev = level
        return out

    if channel is None:
        counted = {ch: transitions_for(col) for ch, col in channel_cols.items()}
        channel = max(counted, key=lambda ch: len(counted[ch]))
        edges = counted[channel]
    else:
        if channel not in channel_cols:
            raise SystemExit(
                f"{path}: channel {channel} is not in this capture "
                f"(have {sorted(channel_cols)})"
            )
        edges = transitions_for(channel_cols[channel])

    return edges, channel


def sample_period(edges: Sequence[tuple[float, int]]) -> float:
    """Infer the capture's sample period from the timestamp grid.

    Every timestamp in a logic capture is a multiple of the sample
    period, so the GCD of the intervals recovers it. This matters because
    it sets the floor on what any timing measurement here can resolve: a
    1 MS/s capture snaps every edge to a 1 µs grid, which cannot
    distinguish a 115200 bit cell (8.68 µs) from a 125000 one (8.00 µs).

    Getting this wrong is how a capture appears to show a 8.5% baud error
    that is entirely an artifact of the sample rate.
    """
    from math import gcd

    stamps = [round(t * 1e9) for t, _ in edges]
    intervals = [b - a for a, b in zip(stamps, stamps[1:]) if b > a]
    if not intervals:
        return 0.0
    step = 0
    for value in intervals[:5000]:
        step = gcd(step, value)
    return step / 1e9


def measure_baud(
    edges: Sequence[tuple[float, int]], idle_high: bool = True
) -> tuple[float | None, float]:
    """Estimate the baud rate, and the resolution limit of that estimate.

    Returns (baud, resolution_percent). The second figure is the
    quantisation floor imposed by the capture's sample rate — a result is
    only meaningful to within it.

    Method: time whole runs of back-to-back frames rather than individual
    bit cells. A single pulse at 1 MS/s carries ±1 µs of grid error, which
    is 12% of a 115200 bit cell; measured across a 33-byte burst the same
    error is spread over 330 bit times and falls to ±0.07%. That is the
    difference between a useless number and the ±0.23% figure in
    docs/gpio-validation.md.

    Bursts are delimited by idle gaps, so this works for paced sending
    (where each byte is its own burst) and for burst mode alike — though
    with pacing there is only one frame per burst and the resolution is
    correspondingly worse.
    """
    if len(edges) < 3:
        return None, 0.0

    period = sample_period(edges)
    idle = 1 if idle_high else 0

    # A gap longer than ~2 frames means the line went idle between
    # transmissions; anything shorter is within a continuous burst.
    widths = sorted(b[0] - a[0] for a, b in zip(edges, edges[1:]) if b[0] > a[0])
    if not widths:
        return None, 0.0
    narrowest = widths[0]
    idle_threshold = narrowest * BITS_PER_FRAME * 2

    # Split into runs of contiguous activity.
    runs: list[list[tuple[float, int]]] = []
    current: list[tuple[float, int]] = []
    for prev, nxt in zip(edges, edges[1:]):
        current.append(prev)
        if nxt[0] - prev[0] > idle_threshold:
            runs.append(current)
            current = []
    current.append(edges[-1])
    runs.append(current)

    # Measure across whole runs of back-to-back frames, not single bit
    # cells: one grid step of error spread over 300 bit times instead of
    # one. For the committed 1 MS/s burst capture that is the difference
    # between reading 125,000 baud and reading ~115,200.
    #
    # The bit count over a run has to come from frame *structure*, not
    # from rounding the span against the quantised narrowest pulse. Both
    # endpoints of a run are transitions, and the last one is the rise
    # ending the final data bit run — whose position depends on the last
    # byte's top bits. Guessing it costs ±1 bit, which at 330 bits is
    # ±0.3%: the same order as the error being measured.
    #
    # So: count frames per run, then use start-bit to start-bit, which is
    # exactly (frames - 1) * 10 bit times with no dependence on the data.
    #
    # Frames are counted from falling edges that begin a frame rather than
    # by decoding, which would need the answer first. Within a run, a
    # falling edge is a start bit if it is at least one frame-length after
    # the previous start bit — and that test tolerates a badly wrong
    # provisional bit time, because frame spacing is 10x a bit.
    candidates: list[float] = []
    spans: list[float] = []
    for run in runs:
        if len(run) < 4 or run[0][1] == idle:
            continue

        frame_starts: list[float] = []
        for stamp, level in run:
            if level == idle:
                continue
            if not frame_starts:
                frame_starts.append(stamp)
                continue
            # 90% of a nominal frame: comfortably rejects mid-frame
            # falling edges while accepting the next start bit even if
            # `narrowest` is off by several percent.
            if stamp - frame_starts[-1] >= narrowest * BITS_PER_FRAME * 0.9:
                frame_starts.append(stamp)

        if len(frame_starts) < 2:
            continue

        span = frame_starts[-1] - frame_starts[0]
        if span <= 0:
            continue
        bit_count = (len(frame_starts) - 1) * BITS_PER_FRAME
        candidates.append(bit_count / span)
        spans.append(span)

    if not candidates:
        # Single-pulse fallback, flagged by a resolution figure that says
        # not to trust it.
        resolution = (period / narrowest * 100.0) if narrowest > 0 else 0.0
        return 1.0 / narrowest, resolution

    candidates.sort()
    baud = candidates[len(candidates) // 2]  # median across runs

    # One grid step of error, spread over the typical measured span.
    mean_span = sum(spans) / len(spans)
    resolution = (period / mean_span * 100.0) if mean_span > 0 else 0.0
    return baud, resolution


def nearest_standard_baud(measured: float) -> tuple[int, float]:
    """Closest standard rate to a measured figure, and the error percent."""
    best = min(COMMON_BAUDS, key=lambda b: abs(measured - b) / b)
    return best, (measured - best) / best * 100.0


def decode_bytes(
    edges: Sequence[tuple[float, int]], baud: float, idle_high: bool = True
) -> tuple[bytes, list[float], int]:
    """Decode 8N1 frames by sampling the reconstructed waveform.

    Returns (data, frame_start_times, framing_errors).

    Sampling happens at the centre of each bit cell, which is what a real
    UART receiver does, so this tolerates the same clock error a receiver
    would. A frame whose stop bit is not at the idle level is counted as a
    framing error and its byte is still returned — a wrong baud shows up
    as a high error count rather than as silence, which is the
    distinction worth having.
    """
    if not edges or baud <= 0:
        return b"", [], 0

    bit = 1.0 / baud
    idle = 1 if idle_high else 0

    def level_at(t: float) -> int:
        """Level of the reconstructed waveform at time t."""
        # Edges are sorted; walk to the last transition at or before t.
        lo, hi = 0, len(edges) - 1
        if t < edges[0][0]:
            return idle
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if edges[mid][0] <= t:
                lo = mid
            else:
                hi = mid - 1
        return edges[lo][1]

    out = bytearray()
    starts: list[float] = []
    errors = 0
    last_end = -1.0

    for stamp, level in edges:
        # A start bit is a transition to non-idle, and cannot begin
        # inside a frame already being decoded.
        if level == idle or stamp < last_end:
            continue

        # Confirm it is still non-idle at the middle of the start bit;
        # otherwise it was a glitch rather than a frame.
        if level_at(stamp + bit * 0.5) == idle:
            continue

        value = 0
        for n in range(8):
            centre = stamp + bit * (1.5 + n)
            if level_at(centre) != idle:
                # Non-idle is a 0 for idle-high lines: UART sends LSB
                # first with a low start bit, so data bits are inverted
                # relative to the idle level.
                pass
            else:
                value |= 1 << n
        if not idle_high:
            value ^= 0xFF

        stop_centre = stamp + bit * 9.5
        if level_at(stop_centre) != idle:
            errors += 1

        out.append(value)
        starts.append(stamp)
        last_end = stamp + bit * BITS_PER_FRAME * 0.95

    return bytes(out), starts, errors


def printable(data: bytes, limit: int = 200) -> str:
    out = []
    for byte in data[:limit]:
        if byte == 0x0A:
            out.append("\\n")
        elif byte == 0x0D:
            out.append("\\r")
        elif byte == 0x09:
            out.append("\\t")
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append(f"\\x{byte:02x}")
    return "".join(out) + ("..." if len(data) > limit else "")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("capture", help="Logic 2 raw digital CSV export")
    ap.add_argument("--channel", type=int, help="channel number (default: busiest)")
    ap.add_argument(
        "--baud",
        help="decode at this baud; 'auto' (default) measures it from the edges",
        default="auto",
    )
    ap.add_argument("--idle-low", action="store_true", help="line idles low (unusual)")
    args = ap.parse_args()

    edges, channel = load_edges(args.capture, args.channel)
    print(f"{args.capture}: channel {channel}, {len(edges)} transitions")

    if not edges:
        print("no transitions. Probe on the wrong pin, or see the ground-loop")
        print("note in docs/gpio-validation.md — a probed line can read flat.")
        return 1

    span = edges[-1][0] - edges[0][0]
    period = sample_period(edges)
    rate = (1.0 / period) if period > 0 else 0.0
    print(f"span {span:.3f}s, sampled at {rate:,.0f} S/s")

    measured, resolution = measure_baud(edges, idle_high=not args.idle_low)
    if measured is None:
        print("not enough transitions to measure a bit period")
        return 1

    nearest, error = nearest_standard_baud(measured)
    print(
        f"measured baud {measured:,.0f} -> nearest standard {nearest:,} "
        f"({error:+.2f}%, resolution +/-{resolution:.2f}%)"
    )
    if abs(error) < resolution:
        print("  error is within the capture's resolution: consistent with nominal")
    elif resolution > 1.0:
        print("  resolution is poor — capture at a higher sample rate for a real figure")

    baud = float(nearest) if args.baud == "auto" else float(args.baud)
    data, _, errors = decode_bytes(edges, baud, idle_high=not args.idle_low)
    print(f"decoded {len(data)} bytes at {baud:,.0f} baud, {errors} framing error(s)")
    if data:
        print(f"  {printable(data)}")

    # A wrong baud decodes bytes but fails their stop bits, so the error
    # rate is the signal that the rate is wrong rather than the data.
    if data and errors > len(data) * 0.02:
        print(f"\n{errors}/{len(data)} frames have bad stop bits — the baud is wrong,")
        print("or the capture is corrupted. Try --baud auto.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
