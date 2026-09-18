#!/usr/bin/env python3
"""Check a logic-analyser UART capture against the payload that was sent.

The GPIO loopback test in Settings proves the transport round-trips, but
it proves it using our own driver on both ends. A logic analyser decodes
the wire independently, so it answers a different question: are the bits
leaving the pin the ones we think we sent, at the baud we claim, in the
framing a third-party receiver will expect.

Usage:

    # In Logic 2: capture the TX pin, add an Async Serial analyzer at the
    # kit's baud (Settings > GPIO Baud, default 115200), 8N1, then
    # File > Export Data > (analyzer table) as CSV.
    python3 tools/gpio_verify_capture.py capture.csv --expect-payload qr-ignore

    # Or compare against arbitrary text:
    python3 tools/gpio_verify_capture.py capture.csv --expect-text "hello"

    # Timing check for the paced mode (Settings > GPIO Pacing > 5ms):
    python3 tools/gpio_verify_capture.py capture.csv --expect-payload qr-ignore \\
        --expect-gap-ms 5

Exports vary between Logic 2 versions and analyzer types. This accepts
any CSV with a column holding the decoded byte and, for timing checks,
one holding a timestamp; the column names are detected rather than
assumed. Values may be decimal, 0x-prefixed hex, bare hex, or a quoted
character.

Exits non-zero on any mismatch so it can gate a release check.
"""

import argparse
import csv
import re
import sys
from collections.abc import Sequence

# Column names seen across Logic 2 exports, lowest-friction first.
DATA_COLUMNS = ("data", "value", "0", "mosi", "miso", "rx", "tx", "byte")
TIME_COLUMNS = ("start_time", "time [s]", "time", "timestamp", "start")
ERROR_COLUMNS = ("framing error", "parity error", "error")


def parse_byte(raw: str) -> int | None:
    """Decode one cell into a byte value, or None if it is not one.

    Whitespace is only stripped once quoting has been handled: a space
    character exports as a bare " " in the character-value format, and
    stripping first silently deleted every space in the payload."""
    text = raw

    # A quoted cell holds the character verbatim, spaces included.
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        inner = text[1:-1]
        if len(inner) == 1:
            return ord(inner)
        text = inner

    # An unquoted single character, including a lone space.
    if len(text) == 1 and not text.isdigit():
        return ord(text)

    text = text.strip()
    if not text:
        return None

    # Logic 2 often exports the character itself for printable ASCII.
    if len(text) == 1 and not text.isdigit():
        return ord(text)

    try:
        if text.lower().startswith("0x"):
            return int(text, 16)
        if text.isdigit():
            return int(text, 10)
        return int(text, 16)
    except ValueError:
        # Escapes like \n, or names like "LF".
        named = {"\\n": 10, "\\r": 13, "\\t": 9, "lf": 10, "cr": 13, "nul": 0}
        return named.get(text.lower())


def pick_column(fieldnames: Sequence[str], candidates: tuple[str, ...]) -> str | None:
    lowered = {name.lower().strip(): name for name in fieldnames}
    for candidate in candidates:
        if candidate in lowered:
            return lowered[candidate]
    # Fall back to a substring match, which covers "Data" vs "data [hex]".
    for candidate in candidates:
        for low, original in lowered.items():
            if candidate in low:
                return original
    return None


def load_capture(path: str) -> tuple[bytes, list[float], int]:
    """Return (payload bytes, timestamps, count of frames flagged as errors)."""
    with open(path, newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        if not reader.fieldnames:
            raise SystemExit(f"{path}: no header row; is this an analyzer export?")

        data_col = pick_column(reader.fieldnames, DATA_COLUMNS)
        if data_col is None:
            raise SystemExit(
                f"{path}: no data column found. Columns are: {reader.fieldnames}"
            )
        time_col = pick_column(reader.fieldnames, TIME_COLUMNS)
        error_cols = [c for c in reader.fieldnames
                      if any(e in c.lower() for e in ERROR_COLUMNS)]

        data = bytearray()
        times: list[float] = []
        errors = 0

        for row in reader:
            value = parse_byte(row.get(data_col, ""))
            if value is None:
                continue
            data.append(value & 0xFF)

            if time_col:
                try:
                    times.append(float(row[time_col]))
                except (TypeError, ValueError):
                    times.append(float("nan"))

            # A framing error means the analyser's baud does not match the
            # line, which invalidates the decode rather than the payload.
            for col in error_cols:
                cell = (row.get(col) or "").strip().lower()
                if cell and cell not in ("0", "false", "no", ""):
                    errors += 1
                    break

    return bytes(data), times, errors


def printable(data: bytes, limit: int = 300) -> str:
    out = []
    for byte in data[:limit]:
        char = chr(byte)
        if char == "\n":
            out.append("\\n")
        elif char == "\r":
            out.append("\\r")
        elif byte < 0x20 or byte == 0x7F:
            out.append(f"\\x{byte:02x}")
        elif byte > 0x7F:
            out.append(f"\\x{byte:02x}")
        else:
            out.append(char)
    return "".join(out) + ("..." if len(data) > limit else "")


def payload_text(name: str) -> str:
    """Pull a built-in payload's text straight from the C source, so the
    expected value cannot drift from what the kit actually ships."""
    source = "src/payload/builtin_payloads.c"
    try:
        with open(source, encoding="utf-8") as fh:
            src = fh.read()
    except FileNotFoundError:
        raise SystemExit(f"{source} not found; run from the repository root")

    match = re.search(
        r'\.name\s*=\s*"' + re.escape(name) + r'"(.*?)\.text\s*=\s*'
        r'((?:\s*"(?:[^"\\]|\\.)*"\s*)+)',
        src,
        re.S,
    )
    if not match:
        raise SystemExit(f"payload {name!r} not found in {source}")

    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(2))
    return "".join(parts).encode().decode("unicode_escape")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("capture", help="CSV exported from the Async Serial analyzer")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--expect-payload", help="built-in payload name")
    group.add_argument("--expect-text", help="literal text")
    ap.add_argument("--expect-gap-ms", type=float,
                    help="assert the median inter-byte gap matches this pacing")
    ap.add_argument("--line-ending", default="lf", choices=("none", "lf", "crlf", "cr"),
                    help="line ending the kit was configured to append (default lf)")
    args = ap.parse_args()

    expected = args.expect_text
    if expected is None:
        expected = payload_text(args.expect_payload)

    endings = {"none": "", "lf": "\n", "crlf": "\r\n", "cr": "\r"}
    expected_bytes = expected.encode("utf-8") + endings[args.line_ending].encode()

    captured, times, errors = load_capture(args.capture)

    print(f"captured {len(captured)} bytes, expected {len(expected_bytes)}")
    if errors:
        print(f"  {errors} frame(s) flagged as errors by the analyzer")
        print("  A framing error usually means the analyzer's baud does not")
        print("  match Settings > GPIO Baud. Fix that before trusting a diff.")

    failures = 0

    if captured == expected_bytes:
        print("MATCH: the wire carried exactly the expected bytes")
    else:
        failures += 1
        print("MISMATCH:")
        print(f"  expected: {printable(expected_bytes)}")
        print(f"  captured: {printable(captured)}")
        for i, (a, b) in enumerate(zip(expected_bytes, captured)):
            if a != b:
                print(f"  first difference at byte {i}: "
                      f"expected 0x{a:02X} ({chr(a)!r}), got 0x{b:02X} ({chr(b)!r})")
                break
        else:
            shorter = "captured" if len(captured) < len(expected_bytes) else "expected"
            print(f"  one is a prefix of the other; {shorter} is shorter by "
                  f"{abs(len(captured) - len(expected_bytes))} bytes")

    if errors:
        failures += 1

    if args.expect_gap_ms is not None:
        if len(times) < 3:
            print("timing: not enough timestamps in the export to check pacing")
            failures += 1
        else:
            gaps = sorted((times[i + 1] - times[i]) * 1000.0
                          for i in range(len(times) - 1))
            median = gaps[len(gaps) // 2]
            # A byte at 115200 8N1 is ~87us on the wire, so the gap between
            # byte starts is the pacing delay plus that. Allow a wide band:
            # the point is to tell paced from burst, not to measure a clock.
            low, high = args.expect_gap_ms * 0.5, args.expect_gap_ms * 2.0 + 1.0
            print(f"timing: median inter-byte gap {median:.2f} ms "
                  f"(expected ~{args.expect_gap_ms} ms)")
            if low <= median <= high:
                print("  pacing looks right")
            else:
                failures += 1
                print(f"  OUT OF RANGE: expected {low:.2f}-{high:.2f} ms")
                if median < low:
                    print("  Bytes are back to back — pacing is probably off.")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
