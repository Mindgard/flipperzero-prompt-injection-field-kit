#!/usr/bin/env python3
"""Check the kit's UART output against a real receiver, at every rate.

`gpio_baud_sweep.py` answers a different question. It drives a logic
analyser and *measures* the line, which is the only way to catch a clock
that is wrong but close — 115,465 against a nominal 115,200. Use that
when you want to know what the hardware is actually doing.

This answers the operational question instead: at rate X, does a real
receiver get the bytes? A USB-TTL adapter is told the rate and can only
report whether framing succeeded, but that is exactly what decides
whether the Settings picker should keep offering 921600 over unshielded
jumper wire.

    # Listen at one rate while you send a payload from the kit
    python3 tools/gpio_rate_check.py --port /dev/cu.usbserial-XXXX --baud 115200

    # Walk every rate the picker offers, prompting between each
    python3 tools/gpio_rate_check.py --port /dev/cu.usbserial-XXXX --sweep

    # Non-interactive: expect a specific payload's text at each rate
    python3 tools/gpio_rate_check.py --port /dev/cu.usbserial-XXXX --sweep \\
        --expect "Ignore all previous instructions"

Wiring, and the ground-loop trap, are in docs/gpio-validation.md. Run
the Flipper on battery: with both devices on one laptop the grounds are
already common through USB, and the second path corrupts the line.

    adapter RX  -> Flipper pin 13 (TX)
    adapter TX  -> Flipper pin 14 (RX)
    adapter GND -> Flipper pin 18 (GND)

Prove the adapter receives before trusting a FAIL. Short its own RX and
TX together and run --selftest: one of ours transmitted fine and never
received a byte, which looks identical to bad wiring at the Flipper end.
"""

import argparse
import sys
import time

import serial

# The eight rates Settings offers.  Keep in step with gpio_bauds[] in
# src/scenes/scene_settings.c — a rate that ships in the picker and
# cannot carry bytes is a bug in the picker.
KIT_RATES = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)

SELFTEST_PROBE = b"PIFK-SELFLOOP-0123456789\n"


def render(data: bytes, limit: int = 200) -> str:
    """Show control bytes rather than letting them move the cursor."""
    out = []
    for byte in data[:limit]:
        if byte == 0x0A:
            out.append("\\n")
        elif byte == 0x0D:
            out.append("\\r")
        elif byte == 0x09:
            out.append("\\t")
        elif byte < 0x20 or byte >= 0x7F:
            out.append(f"\\x{byte:02x}")
        else:
            out.append(chr(byte))
    return "".join(out) + ("..." if len(data) > limit else "")


def classify(data: bytes) -> str:
    """Name the failure mode rather than just reporting bytes.

    A wrong baud rate does not produce silence — it produces bytes, and
    they are usually high-bit or NUL because the receiver samples a bit
    cell at the wrong offset. Distinguishing that from "nothing arrived"
    is the difference between a rate problem and a wiring problem.
    """
    if not data:
        return "silent"
    non_ascii = sum(1 for b in data if b >= 0x80 or (b < 0x20 and b not in (9, 10, 13)))
    if non_ascii > len(data) // 2:
        return "garbage"
    if non_ascii:
        return "partial"
    return "clean"


def listen(port: str, baud: int, seconds: float, idle_ms: int) -> bytes:
    """Collect bytes until the line goes quiet for idle_ms, or time runs out."""
    with serial.Serial(port, baud, timeout=0.05) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        buf = bytearray()
        deadline = time.time() + seconds
        last_rx = None

        while time.time() < deadline:
            chunk = ser.read(512)
            if chunk:
                buf += chunk
                last_rx = time.time()
            elif last_rx and (time.time() - last_rx) * 1000 >= idle_ms:
                break
        return bytes(buf)


def selftest(port: str) -> int:
    """Prove the adapter receives, with its own RX and TX shorted.

    Worth its own mode because an adapter that transmits but cannot
    receive passes every other check — it enumerates, binds a driver,
    accepts writes — and presents exactly like miswiring at the far end.
    """
    print(f"Self-loop on {port}. Short the adapter's own RX and TX together.\n")
    failures = 0
    for baud in (9600, 115200, 921600):
        try:
            with serial.Serial(port, baud, timeout=0.4) as ser:
                time.sleep(0.15)
                ser.reset_input_buffer()
                ser.write(SELFTEST_PROBE)
                ser.flush()
                time.sleep(0.25)
                got = ser.read(len(SELFTEST_PROBE) * 2)
        except serial.SerialException as exc:
            print(f"  {baud:>7} : ERROR {exc}")
            failures += 1
            continue

        ok = SELFTEST_PROBE in got
        failures += not ok
        print(f"  {baud:>7} : {'PASS' if ok else 'FAIL'}  {len(got)} bytes")

    print()
    if failures:
        print("Adapter RX is not working. Do not trust a FAIL from --sweep")
        print("until this passes: the symptom is identical to bad wiring.")
    else:
        print("Adapter receives. A FAIL from --sweep is the kit or the wiring.")
    return 1 if failures else 0


def sweep(args: argparse.Namespace) -> int:
    rates = KIT_RATES if args.sweep else (args.baud,)
    results = []

    for baud in rates:
        print(f"\n--- {baud} baud ---")
        if not args.expect:
            print(f"  On the Flipper: set Settings > GPIO Baud to {baud},")
            print("  then Wires > Send it > (any payload). Enter when sent.")
            try:
                input("  [Enter to listen, s to skip] ").strip().lower()
            except EOFError:
                pass
        else:
            print(f"  Listening {args.seconds}s...")

        data = listen(args.port, baud, args.seconds, args.idle_ms)
        state = classify(data)

        if args.expect:
            ok = args.expect.encode() in data
            verdict = "PASS" if ok else "FAIL"
        else:
            ok = state == "clean" and bool(data)
            verdict = {"clean": "PASS", "silent": "NO DATA", "partial": "PARTIAL", "garbage": "GARBAGE"}[state]

        results.append((baud, verdict, len(data)))
        print(f"  {verdict}: {len(data)} bytes, {state}")
        if data:
            print(f"  {render(data)}")

    print("\n=== summary ===")
    for baud, verdict, count in results:
        print(f"  {baud:>7} : {verdict:<8} {count} bytes")

    bad = [b for b, v, _ in results if v not in ("PASS",)]
    if bad:
        print(f"\n{len(bad)} rate(s) did not deliver clean bytes: {bad}")
        print("A rate the picker offers but the wire cannot carry is a bug")
        print("in the picker, not in the test.")
        return 1
    print("\nEvery rate delivered clean bytes.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--port", required=True, help="USB-TTL device node")
    ap.add_argument("--baud", type=int, default=115200, help="single rate to check")
    ap.add_argument("--sweep", action="store_true", help="walk all eight kit rates")
    ap.add_argument("--selftest", action="store_true", help="prove the adapter receives")
    ap.add_argument("--expect", help="text that must appear; makes the sweep non-interactive")
    ap.add_argument("--seconds", type=float, default=8.0, help="listen window per rate")
    ap.add_argument("--idle-ms", type=int, default=400, help="silence that ends a capture")
    args = ap.parse_args()

    if args.selftest:
        return selftest(args.port)
    return sweep(args)


if __name__ == "__main__":
    sys.exit(main())
