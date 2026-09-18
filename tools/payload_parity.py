#!/usr/bin/env python3
"""Check the two shipped copies of the payload library have not drifted.

The kit ships its builtins twice, on purpose:

  - ``src/payload/builtin_payloads.c``   compiled in, works with no SD card
  - ``payloads/pifk_payloads.json``  the SD-card copy the host syncs

Both are edited by hand, and nothing else in the tree compares them. Adding
a payload means transcribing it into two files in two languages, which is
exactly the shape of change that drifts silently: the C copy is what a
Flipper with no SD card runs, so a JSON-only edit is invisible until someone
tests on a fresh device.

    python3 tools/payload_parity.py           # check, exit non-zero on drift
    python3 tools/payload_parity.py --verbose # also list every payload

Checks, in the order a failure is most likely:

  - the two copies hold the same payload names
  - text, category and description agree for every shared name
  - name/category/description fit the fixed C buffers, with room for the NUL
  - the payload count is within PIFK_MAX_PAYLOADS

Escapes are decoded before comparing, so the C literal ``"a\\nb"`` and the
JSON string ``"a\\nb"`` (a real newline once parsed) compare equal. Comparing
the source text instead would flag every multi-line payload as drift.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
C_SOURCE = ROOT / "src/payload/builtin_payloads.c"
JSON_SOURCE = ROOT / "payloads/pifk_payloads.json"
APP_HEADER = ROOT / "src/pifk_app.h"

# Fixed-size char buffers in PifkPayload (src/pifk_app.h). The value
# is the buffer size, so a field needs len(utf-8 bytes) + 1 <= cap.
FIELD_CAPS = {"name": 48, "category": 32, "description": 128}

COMPARED_FIELDS = ("text", "category", "description")

# One brace-delimited initialiser. Non-greedy up to the closing "}," so
# adjacent entries do not run together.
_ENTRY_RE = re.compile(r"\{\s*\.name\s*=.*?\n\s*\},", re.S)

# A run of adjacent string literals, as the C compiler concatenates them.
_LITERAL_RUN_RE = re.compile(r'((?:"(?:[^"\\]|\\.)*"\s*)+)')

_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# The subset of C escapes that can appear in a payload literal. \x and octal
# are deliberately absent: none are used today, and silently mis-decoding
# them would be worse than refusing to guess.
_C_ESCAPES = {
    "n": "\n",
    "t": "\t",
    "r": "\r",
    "\\": "\\",
    '"': '"',
    "'": "'",
    "0": "\0",
}


def decode_c_string(raw: str) -> str:
    """Decode the escapes in a C string literal's contents.

    Args:
        raw: Literal contents, without the surrounding quotes.

    Returns:
        The string value the C compiler would produce.

    Raises:
        ValueError: If an escape sequence is not one of the supported set,
            rather than passing it through and comparing the wrong thing.
    """
    out: list[str] = []
    i = 0
    while i < len(raw):
        ch = raw[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        if i + 1 >= len(raw):
            raise ValueError(f"trailing backslash in C literal: {raw!r}")
        esc = raw[i + 1]
        if esc not in _C_ESCAPES:
            raise ValueError(
                f"unsupported C escape '\\{esc}' in {raw!r}; add it to "
                "_C_ESCAPES in tools/payload_parity.py if it is intended"
            )
        out.append(_C_ESCAPES[esc])
        i += 2
    return "".join(out)


def _field(block: str, field: str) -> str:
    """Return the decoded value of one ``.field = "..."`` designator."""
    match = re.search(rf"\.{field}\s*=\s*", block)
    if not match:
        return ""
    run = _LITERAL_RUN_RE.match(block, match.end())
    if not run:
        return ""
    return "".join(decode_c_string(s) for s in _STRING_RE.findall(run.group(1)))


def parse_c_builtins() -> list[dict[str, str]]:
    """Parse the compiled-in payload table into dicts.

    Returns:
        One dict per initialiser, with decoded name/text/category/description.
    """
    src = C_SOURCE.read_text(encoding="utf-8")
    entries = [
        {
            "name": _field(block, "name"),
            "text": _field(block, "text"),
            "category": _field(block, "category"),
            "description": _field(block, "description"),
        }
        for block in _ENTRY_RE.findall(src)
    ]
    if not entries:
        raise SystemExit(
            f"parsed 0 payloads from {C_SOURCE.relative_to(ROOT)} — the file "
            "format changed and this parser needs updating"
        )
    return entries


def load_json_payloads() -> list[dict[str, str]]:
    """Load the SD-card payload copy."""
    return json.loads(JSON_SOURCE.read_text(encoding="utf-8"))


def read_payload_cap() -> int:
    """Return PIFK_MAX_PAYLOADS as compiled."""
    hdr = APP_HEADER.read_text(encoding="utf-8")
    match = re.search(r"#define\s+PIFK_MAX_PAYLOADS\s+(\d+)", hdr)
    if not match:
        raise SystemExit(
            f"PIFK_MAX_PAYLOADS not found in {APP_HEADER.relative_to(ROOT)}"
        )
    return int(match.group(1))


def check_parity(js: list[dict], c: list[dict]) -> list[str]:
    """Compare the two copies name-by-name.

    Reports missing names in both directions before comparing fields, so a
    forgotten transcription reads as one clear error rather than a count
    mismatch plus a field diff for every subsequent entry.
    """
    errors: list[str] = []
    by_name_c = {e["name"]: e for e in c}
    by_name_js = {e["name"]: e for e in js}

    for name in by_name_js.keys() - by_name_c.keys():
        errors.append(f"{name!r} is in the JSON but missing from builtin_payloads.c")
    for name in by_name_c.keys() - by_name_js.keys():
        errors.append(f"{name!r} is in builtin_payloads.c but missing from the JSON")

    if len(by_name_js) != len(js):
        errors.append(f"duplicate names in the JSON ({len(js)} entries, {len(by_name_js)} unique)")
    if len(by_name_c) != len(c):
        errors.append(f"duplicate names in the C table ({len(c)} entries, {len(by_name_c)} unique)")

    for name in sorted(by_name_js.keys() & by_name_c.keys()):
        for field in COMPARED_FIELDS:
            want = by_name_js[name].get(field, "")
            got = by_name_c[name][field]
            if want != got:
                errors.append(
                    f"{name}.{field} differs:\n"
                    f"    json: {want!r}\n"
                    f"    c   : {got!r}"
                )
    return errors


def check_field_limits(js: list[dict]) -> list[str]:
    """Verify every string field fits its fixed C buffer, NUL included."""
    errors: list[str] = []
    for entry in js:
        for field, cap in FIELD_CAPS.items():
            need = len(entry.get(field, "").encode("utf-8")) + 1
            if need > cap:
                errors.append(
                    f"{entry.get('name', '<unnamed>')}.{field} needs {need}B "
                    f"(with NUL), buffer is {cap}B"
                )
    return errors


def check_cap(js: list[dict], c: list[dict]) -> list[str]:
    """Verify both copies fit the fixed-size PayloadDb array."""
    cap = read_payload_cap()
    errors = [
        f"{label} has {n} payloads, exceeding PIFK_MAX_PAYLOADS={cap}"
        for label, n in (("the JSON", len(js)), ("builtin_payloads.c", len(c)))
        if n > cap
    ]
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check the two shipped copies of the payload library agree."
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="list every payload checked"
    )
    args = parser.parse_args()

    js = load_json_payloads()
    c = parse_c_builtins()

    errors = check_parity(js, c) + check_field_limits(js) + check_cap(js, c)

    if args.verbose:
        for entry in js:
            print(
                f"  {entry['name']:<28} {entry['category']:<20} "
                f"{len(entry['text'].encode()):>4}B"
            )

    if errors:
        print(f"FAIL: {len(errors)} problem(s) in the payload library\n", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(
        f"OK: {len(js)} payloads, both copies agree, "
        f"all fields fit, cap is {read_payload_cap()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
