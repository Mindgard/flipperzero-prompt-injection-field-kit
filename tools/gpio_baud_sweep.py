#!/usr/bin/env python3
"""Measure every baud rate the kit offers, against the analyser's clock.

`docs/gpio-validation.md` measures one rate: 115200, the compiled
default. `Settings > GPIO Baud` offers eight. The others have never been
checked, and they do not fail uniformly — an STM32WB55 USART divides a
64 MHz clock, so the divisor gets coarser as the rate rises and 921600
needs a divisor near 69. Coarse divisors quantise the achievable rate,
which is where an out-of-spec figure would show up first.

A UART receiver typically tolerates 2-3% total clock error before
framing breaks, and the target's own clock spends some of that budget.
So the useful output is a per-rate error figure and a verdict, not a
pass/fail.

    # One rate, from a capture you already took
    python3 tools/gpio_baud_sweep.py --capture capture.csv --nominal 115200

    # Drive the whole sweep through Logic 2's automation API
    python3 tools/gpio_baud_sweep.py --live --channel 3

    # Re-check the committed captures
    python3 tools/gpio_baud_sweep.py --capture docs/captures/gpio-burst-115200.csv

The live sweep cannot change the Flipper's baud for you — that is a
device-side setting with no remote command. It prompts between rates:
set `Settings > GPIO Baud`, press `Settings > GPIO Loopback` a few times
to generate traffic, then press Enter here.

Sample rate matters more than it looks. At 1 MS/s every edge snaps to a
1 us grid, which is 12% of a 115200 bit cell; measuring across whole
bursts recovers the figure, but the resolution is still reported and a
result inside it means "consistent with nominal", not "exact". Capture at
10 MS/s or better and this stops being a consideration. See
uart_edge_decode.sample_period().

Wiring and the ground-loop trap are in docs/gpio-validation.md. Run the
Flipper on battery.
"""

from __future__ import annotations

import argparse
import sys

from uart_edge_decode import (
    decode_bytes,
    load_edges,
    measure_baud,
    sample_period,
)

# Must match gpio_bauds[] in src/scenes/scene_settings.c. Kept as a
# literal rather than parsed out of the C, because a sweep that silently
# skips a rate the UI offers is worse than one that fails to start.
KIT_BAUDS = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)

# Total clock error a UART receiver tolerates before framing fails, as a
# percentage. Both ends contribute, so treat half of it as our budget.
RECEIVER_TOLERANCE_PCT = 2.0


def verdict(error_pct: float, resolution_pct: float) -> str:
    """Classify a measured error against what a receiver will accept."""
    if abs(error_pct) <= resolution_pct:
        return "ok (within capture resolution)"
    if abs(error_pct) <= RECEIVER_TOLERANCE_PCT / 2:
        return "ok"
    if abs(error_pct) <= RECEIVER_TOLERANCE_PCT:
        return "MARGINAL — leaves the target no error budget"
    return "OUT OF SPEC — a standard receiver will see framing errors"


def analyse(path: str, channel: int | None, nominal: int | None) -> dict | None:
    """Measure one capture. Returns a row dict, or None if unusable."""
    edges, used_channel = load_edges(path, channel)
    if len(edges) < 4:
        print(f"  {path}: only {len(edges)} transitions — nothing to measure.")
        print("   Probe on the wrong pin, or the ground-loop trap in")
        print("   docs/gpio-validation.md (a probed line can read flat).")
        return None

    measured, resolution = measure_baud(edges)
    if measured is None:
        print(f"  {path}: could not establish a bit period.")
        return None

    if nominal is None:
        # Pick the kit rate the measurement is closest to, proportionally.
        nominal = min(KIT_BAUDS, key=lambda b: abs(measured - b) / b)

    error = (measured - nominal) / nominal * 100.0
    data, _, framing_errors = decode_bytes(edges, float(nominal))
    period = sample_period(edges)

    return {
        "path": path,
        "channel": used_channel,
        "nominal": nominal,
        "measured": measured,
        "error": error,
        "resolution": resolution,
        "bytes": len(data),
        "framing_errors": framing_errors,
        "sample_rate": (1.0 / period) if period > 0 else 0.0,
        "verdict": verdict(error, resolution),
    }


def print_table(rows: list[dict]) -> None:
    print()
    print(f"{'nominal':>9}  {'measured':>10}  {'error':>8}  {'res':>7}  "
          f"{'bytes':>6}  {'fr.err':>6}  verdict")
    print("-" * 96)
    for row in sorted(rows, key=lambda r: r["nominal"]):
        print(
            f"{row['nominal']:>9,}  {row['measured']:>10,.0f}  "
            f"{row['error']:>+7.2f}%  {row['resolution']:>6.2f}%  "
            f"{row['bytes']:>6}  {row['framing_errors']:>6}  {row['verdict']}"
        )


def live_sweep(channel: int, rates: tuple[int, ...], seconds: float) -> list[dict]:
    """Drive Logic 2 through each rate, prompting for the device-side change."""
    try:
        from saleae import automation
    except ImportError:
        print("The live sweep needs the Saleae automation package:")
        print("    pip install logic2-automation")
        print("Or capture manually and pass --capture.")
        return []

    import os
    import tempfile

    rows: list[dict] = []
    outdir = tempfile.mkdtemp(prefix="baud-sweep-")
    print(f"exports going to {outdir}")

    with automation.Manager.connect() as manager:
        for rate in rates:
            print()
            print(f"--- {rate:,} baud ---")
            print(f"1. On the Flipper: Settings > GPIO Baud > {rate}")
            print("2. Then press Settings > GPIO Loopback several times")
            print("3. Press Enter here to capture (or 's' to skip this rate)")
            if input("> ").strip().lower().startswith("s"):
                continue

            # 10 MS/s or better keeps the resolution out of the way. The
            # Logic 8 has a fixed threshold, so digitalThresholdVolts is
            # deliberately not passed — it is rejected outright.
            device_cfg = automation.LogicDeviceConfiguration(
                enabled_digital_channels=[channel],
                digital_sample_rate=10_000_000,
            )
            # Timed rather than manual: a manual capture blocks
            # add_analyzer and the exports until it is stopped.
            capture_cfg = automation.CaptureConfiguration(
                capture_mode=automation.TimedCaptureMode(duration_seconds=seconds)
            )

            with manager.start_capture(
                device_configuration=device_cfg, capture_configuration=capture_cfg
            ) as capture:
                print(f"   capturing for {seconds:.0f}s — keep pressing Loopback...")
                # A timed capture must be awaited before export, or the
                # capture is reaped and the data is gone.
                capture.wait()
                sub = os.path.join(outdir, str(rate))
                capture.export_raw_data_csv(directory=sub, digital_channels=[channel])

            # Logic 2 names the file by channel inside the directory.
            found = None
            for name in os.listdir(sub):
                if name.endswith(".csv"):
                    found = os.path.join(sub, name)
                    break
            if not found:
                print("   no CSV exported, skipping")
                continue

            row = analyse(found, channel, rate)
            if row:
                rows.append(row)
                print(
                    f"   measured {row['measured']:,.0f} "
                    f"({row['error']:+.2f}%) — {row['verdict']}"
                )

    return rows


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--capture",
        action="append",
        default=[],
        help="a raw digital CSV to analyse; repeatable",
    )
    ap.add_argument(
        "--nominal",
        type=int,
        help="expected baud for --capture (default: nearest kit rate)",
    )
    ap.add_argument("--channel", type=int, help="channel number (default: busiest)")
    ap.add_argument(
        "--live", action="store_true", help="drive Logic 2 through the whole sweep"
    )
    ap.add_argument(
        "--rates",
        help="comma-separated subset of rates for --live (default: all eight)",
    )
    ap.add_argument(
        "--seconds", type=float, default=10.0, help="capture window per rate"
    )
    args = ap.parse_args()

    if not args.capture and not args.live:
        ap.error("pass --capture FILE or --live")

    rows: list[dict] = []

    for path in args.capture:
        print(f"analysing {path}")
        row = analyse(path, args.channel, args.nominal)
        if row:
            rows.append(row)
            if row["sample_rate"] and row["sample_rate"] < 5_000_000:
                print(
                    f"  note: captured at {row['sample_rate']:,.0f} S/s. "
                    f"10 MS/s or better gives a tighter figure."
                )

    if args.live:
        rates = KIT_BAUDS
        if args.rates:
            rates = tuple(int(r) for r in args.rates.split(","))
        rows.extend(live_sweep(args.channel if args.channel is not None else 3,
                               rates, args.seconds))

    if not rows:
        print("nothing measured.")
        return 1

    print_table(rows)

    # Exit non-zero only for genuinely out-of-spec rates, so this can gate
    # a release check without failing on a low-resolution capture.
    bad = [r for r in rows if r["verdict"].startswith("OUT OF SPEC")]
    marginal = [r for r in rows if r["verdict"].startswith("MARGINAL")]
    print()
    if bad:
        names = ", ".join(format(r["nominal"], ",") for r in bad)
        print(f"{len(bad)} rate(s) out of spec: {names}")
        print("Those should come out of the Settings picker, or be documented")
        print("as unusable — offering a rate that cannot work is a trap.")
        return 1
    if marginal:
        names = ", ".join(format(r["nominal"], ",") for r in marginal)
        print(f"{len(marginal)} rate(s) marginal: {names}")
        print("Usable against a target with a good clock; worth a doc note.")
    else:
        print(f"all {len(rows)} measured rate(s) within tolerance.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
