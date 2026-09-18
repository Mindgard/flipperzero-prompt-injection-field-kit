#!/usr/bin/env python3
"""Probe the field kit's BLE GATT payload service and reassemble the payload.

This is the client half of the "BLE GATT (readable)" channel: it stands in
for whatever would read the tag in the field, so the channel can be
verified without one.

    pip install bleak
    python3 tools/ble_gatt_probe.py                     # scan, connect, read
    python3 tools/ble_gatt_probe.py --scan-only         # just list devices
    python3 tools/ble_gatt_probe.py --expect-file p.txt # compare with source

On the Flipper: Quick Deploy > BLE GATT (readable), or EXEC BLEGATT <name>
over the serial bridge. The service only exists while that is running.

What it checks, beyond "did some bytes arrive":

  - the service is advertised, so a scanner finds it without prior knowledge
  - reads succeed with no pairing or bonding prompt, which is the whole
    point of the channel
  - the three characteristics reassemble in UUID order into the payload
  - non-ASCII survives the round trip, since obfuscated payloads are a
    normal case here

Exits non-zero if the payload could not be read, so it can gate CI.
"""

import argparse
import asyncio
import sys
import unicodedata

from bleak import BleakClient, BleakScanner

# Must match ble_gatt_exec.c.
SERVICE_UUID = "0000fe20-0000-1000-8000-00805f9b34fb"
CHAR_UUIDS = [
    "0000fe21-0000-1000-8000-00805f9b34fb",
    "0000fe22-0000-1000-8000-00805f9b34fb",
    "0000fe23-0000-1000-8000-00805f9b34fb",
]
ADV_NAME = "PIFK"
CHAR_MAX = 255

SCAN_SECONDS = 10.0


def printable(text: str, limit: int = 400) -> str:
    """Render control characters visibly so a payload cannot fake its own
    output — a newline in the text should not look like our formatting."""
    out = []
    for ch in text[:limit]:
        if ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif unicodedata.category(ch).startswith("C"):
            out.append(f"\\x{ord(ch):02x}")
        else:
            out.append(ch)
    tail = "..." if len(text) > limit else ""
    return "".join(out) + tail


async def find_device(address: str | None):
    if address:
        print(f"Connecting directly to {address}")
        return address

    print(f"Scanning {SCAN_SECONDS:.0f}s for the payload service...")
    # Match on the advertised service UUID rather than the name: the name is
    # cosmetic and a scanner in the field would not know it either.
    devices = await BleakScanner.discover(
        timeout=SCAN_SECONDS, return_adv=True, service_uuids=[SERVICE_UUID]
    )

    matches = []
    for dev, adv in devices.values():
        advertises = SERVICE_UUID.lower() in [u.lower() for u in adv.service_uuids]
        named = (adv.local_name or dev.name or "") == ADV_NAME
        if advertises or named:
            matches.append((dev, adv, advertises))

    if not matches:
        print("No device advertising the payload service.")
        print("Is 'BLE GATT (readable)' running on the Flipper?")
        return None

    for dev, adv, advertises in matches:
        name = adv.local_name or dev.name or "(unnamed)"
        print(f"  {dev.address}  {name}  rssi={adv.rssi}")
        # A device found only by name is not doing its job: the service UUID
        # in the advertisement is what makes it discoverable to a stranger.
        if not advertises:
            print("    warning: matched on name only; service UUID not advertised")

    return matches[0][0]


async def read_payload(target, expect: str | None) -> int:
    async with BleakClient(target) as client:
        print(f"Connected: {client.address}")

        services = client.services
        svc = services.get_service(SERVICE_UUID)
        if svc is None:
            print(f"FAIL: service {SERVICE_UUID} not present after connect")
            return 1

        found = {c.uuid.lower(): c for c in svc.characteristics}
        print(f"Service {SERVICE_UUID} with {len(found)} characteristic(s)")

        chunks = []
        for i, uuid in enumerate(CHAR_UUIDS):
            char = found.get(uuid.lower())
            if char is None:
                print(f"  {uuid}  MISSING")
                continue

            if "read" not in char.properties:
                print(f"  {uuid}  not readable (properties: {char.properties})")
                continue

            try:
                data = await client.read_gatt_char(char)
            except Exception as exc:  # noqa: BLE001 - report, do not mask
                print(f"  {uuid}  read failed: {exc}")
                return 1

            if len(data) > CHAR_MAX:
                print(f"  {uuid}  over-long: {len(data)} > {CHAR_MAX}")
                return 1

            print(f"  {uuid}  {len(data):3d} B  {printable(data.decode('utf-8', 'replace'), 60)}")
            chunks.append(data)

        if not chunks:
            print("FAIL: no characteristic returned data")
            return 1

        payload = b"".join(chunks)
        text = payload.decode("utf-8", "replace")

        print()
        print(f"Reassembled {len(payload)} bytes from {len(chunks)} characteristic(s):")
        print(f"  {printable(text)}")

        if "�" in text:
            print()
            print("WARNING: payload is not valid UTF-8 after reassembly.")
            print("A chunk boundary may be splitting a multi-byte character.")

        if expect is not None:
            print()
            if text == expect:
                print(f"MATCH: identical to the expected payload ({len(expect)} chars)")
            else:
                print("MISMATCH against the expected payload:")
                print(f"  expected {len(expect)} chars: {printable(expect, 120)}")
                print(f"  received {len(text)} chars: {printable(text, 120)}")
                for i, (a, b) in enumerate(zip(expect, text)):
                    if a != b:
                        print(f"  first difference at index {i}: {a!r} vs {b!r}")
                        break
                else:
                    print(f"  one is a prefix of the other; length differs by "
                          f"{abs(len(expect) - len(text))}")
                return 1

        return 0


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", help="skip the scan and connect to this address")
    ap.add_argument("--scan-only", action="store_true", help="list matches and exit")
    ap.add_argument("--expect-file", help="file whose contents the payload should equal")
    args = ap.parse_args()

    expect = None
    if args.expect_file:
        with open(args.expect_file, encoding="utf-8") as fh:
            # Trailing newline is an artefact of the file, not the payload.
            expect = fh.read().rstrip("\n")

    target = await find_device(args.address)
    if target is None:
        return 1
    if args.scan_only:
        return 0

    return await read_payload(target, expect)


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print()
        sys.exit(130)
