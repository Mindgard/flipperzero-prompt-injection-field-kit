# Prompt Injection Field Kit

Native Flipper Zero FAP (Flipper App Package) that turns the Flipper into a standalone AI prompt injection field kit, with a serial bridge mode for real-time control from a host over USB serial.

**[Operator manual](docs/MANUAL.md)** — getting started, all twelve delivery
channels, engagement scenarios, the remote protocol, and troubleshooting.
Start there if you are using the kit rather than building it.

## Features

### Payload Manager (Standalone On-Device)
- **Payload Browser** — Browse and search 54 built-in prompt injection payloads
- **Multi-turn technique** — Six attack scripts documented in [docs/multi-turn-technique.md](docs/multi-turn-technique.md), run a turn at a time so you can read the target between them
- **Multi-Protocol Execution** — Deploy via BadUSB (HID), NFC NDEF, BLE beacon, QR code, USB descriptor, GPIO serial, or I2C
- **USB Descriptor Injection** — Carry the payload in the USB manufacturer, product and serial strings, so a host logs it during enumeration
- **Transport-first navigation** — Pick the channel the target allows,
  then a payload from the ones that channel can actually carry
- **Favorites** — Long-press OK on any payload to star it; starred
  payloads sort to the top of every list
- **SD Card Sync** — Load custom payloads from `/ext/apps_data/pifk/`

### Protocol size limits

The delivery channels differ enormously in capacity, which decides
which payloads work over which protocol:

| Protocol | Limit | Behaviour past the limit |
|----------|-------|--------------------------|
| BadUSB | 512 bytes (`PIFK_MAX_TEXT_LEN`) | Typed character by character; unsupported characters are skipped |
| QR code | 134 bytes (QR v6, ECC-L) | Refused, with the payload size and the limit shown |
| BLE beacon | 2079 bytes (99 chunks x 21) | Refused rather than broadcast truncated |
| BLE GATT | 732 bytes (3 x 244) | Refused rather than served truncated |
| NFC (file export) | 481 bytes (same builder as emulation) | Refused rather than truncated |
| NFC (emulation) | 481 bytes (NTAG215 NDEF) | Refused rather than truncated |
| USB descriptor | 378 chars (3 x 126, ASCII) | Excess dropped; the count is reported |
| GPIO / UART | 512 bytes (`PIFK_MAX_TEXT_LEN`) | Sent verbatim as bytes; no limit of its own |
| I2C | 512 bytes (`PIFK_MAX_TEXT_LEN`) | Sent in 16-byte transactions; no limit of its own |

The general payloads run 150–376 bytes, so they work over BadUSB but
will not fit in a QR code. The eight `qr-*` payloads are all under 80
bytes for exactly this reason.

### USB descriptor injection

Every other channel needs the target to do something — focus a text
field, point a camera, tap a tag, pair a radio, wire a board. This one
needs the cable pushed in.

When a USB device is attached, the host reads its identifying strings
during enumeration and writes them to logs before any driver loads:

- **Windows** — EID 6416 (Security log) records vendor ID, product ID
  and serial on *every* connection; `C:\Windows\INF\setupapi.dev.log`
  records vendor name, product name and serial on first install;
  `HKLM\SYSTEM\CurrentControlSet\Enum\USB\` persists them
- **Linux** — `udev` / `dmesg` / `journalctl` on enumeration
- **macOS** — unified logging, and `system_profiler SPUSBDataType`

The interesting consumer is not the endpoint user but whatever reads
those logs afterwards. If a SIEM or log-triage step is LLM-assisted,
the payload reaches a model's context with no user interaction at all.

The Flipper enumerates as a well-formed CDC serial device that merely
has an unusual name — the CDC class implementation and configuration
descriptor are reused unchanged, because a malformed descriptor gets the
device rejected before the host reads any strings.

Capacity is 126 characters per string across manufacturer, product and
serial. ASCII only: bytes ≥ 0x80 become `?` and the count is reported,
so an obfuscated payload will not silently arrive mangled.

Leaving the scene restores the previous USB configuration.

### GPIO / serial egress

The other channels each speak a specific protocol to a specific kind of
target. This one does not: it emits payload bytes on the UART TX pin and
leaves the receiving end to whatever you have wired up. That makes it the
way into targets the kit cannot anticipate — a serial console on an
industrial HMI, a kiosk debug header, a robot's UART, a custom pipeline
feeding an LLM.

Wiring, with pin numbers shown in `Settings > GPIO` (they are resolved
from the firmware at runtime, not printed here, so they cannot drift):

```
Flipper UART TX  ->  your RX
Flipper UART RX  <-  your TX      (only needed for the loopback test)
Flipper GND      ->  your GND     (required)
```

**GND is not optional.** A floating ground produces garbage that looks
exactly like a software bug. **The Flipper's GPIO is 3.3V** — a 5V target
needs a level shifter, or you risk damaging the MCU.

Settings covers baud (9600–921600), port, line ending, and pacing:

- **Port** — USART or LPUART. The CLI and logging own the USART by
  default; if it is busy the app says so and suggests LPUART rather than
  failing silently.
- **Pacing** — `off` hands the whole buffer to the driver at once.
  Anything else sends one byte at a time, which both helps receivers with
  small buffers (there is no RTS/CTS flow control) and makes **Stop
  effective mid-payload**, which BadUSB cannot do.
**Loopback** (`Wires > UART loopback test`) — bridge the TX and RX pins
with a jumper and this sends a known string, reads it back and compares.
It is the only channel here that can prove its transport works before
your hardware is involved, which turns "it didn't work" into a two-way
diagnosis. It sits beside the I2C bus scan, under the channels both
qualify.

**Response capture.** Every other channel is write-only: it reports that
a payload was delivered, never whether it had any effect. "GPIO + capture
reply" (`Wires > Send + capture`) sends the payload and then listens on RX
for a configurable window
(`Settings > GPIO Listen`), showing whatever came back. If the target has
a serial console, a leaked system prompt read off the wire is the actual
evidence — worth more than a delivery confirmation.

Up to 512 bytes are retained; a longer reply is marked truncated. A
silent target is reported as a result rather than an error, since "it
said nothing" is a finding. Over the bridge: `EXEC GPIOCAP <name>`.

Every capture is written to `captures.jsonl` and listed under
**Main Menu > Captures** — see [Captures](#captures--did-the-injection-work)
below.

### I2C payload write

The UART channel reaches targets with a serial console. This one reaches
the ones without: an EEPROM holding configuration text, a display
controller's frame buffer, a sensor's register space. Those turn up on
embedded hardware that exposes no console at all, and the consumer is an
LLM-assisted pipeline that later reads device configuration or summarises
what a display showed.

```
Flipper SCL (PC0)  ->  target SCL
Flipper SDA (PC1)  ->  target SDA
Flipper GND        ->  target GND     (required)
```

Pin numbers are shown in `Settings > I2C Pins`, resolved from the
firmware at runtime. **Pull-ups are the target board's job** — the
Flipper's I2C pins float on release and the HAL enables no internal
pull-ups, so a bus without them reads as every address being absent,
which looks identical to nothing being connected. 3.3V; a 5V bus needs a
level shifter.

**Scan before you write.** `Wires > Scan the I2C bus` probes 0x08–0x77 and lists what
responds, flagging the addresses most likely to be an EEPROM or a
display. Writing to a guessed address is destructive in a way the UART
channel is not — an unexpected device might be a PMIC, and payload text
in its control registers is a bricked board. The write path refuses an
address that does not ACK.

Payloads go out in 16-byte transactions, each re-addressing the slave,
because an unknown device's receive buffer is unknown and a single
512-byte write would overrun most real parts. `app->executing` is checked
between chunks, so Stop interrupts a transfer — though the bytes already
sent have arrived, and an I2C write cannot be rolled back. A partial
write reports how many bytes landed.

**Master only.** `furi_hal_i2c_tx()` addresses a slave and the exported
HAL has no slave mode, so the kit writes *into* a target that behaves as
a slave; it cannot impersonate a sensor that a target polls. If the
target is the bus master, this channel has nothing to say to it.

Over the bridge: `SCAN I2C` and `EXEC I2C <name>`.

### BLE GATT readable surface

The beacon channel broadcasts 29 bytes at a time and hopes a scanner is
listening. This one publishes a readable GATT service instead: a client
connects, reads a characteristic, and gets the payload whole — 244 bytes
per characteristic, three of them, no chunking and no loss.

No pairing is required, which is the point: BLE enumeration is something
an assistant with device-discovery capability does routinely, and a
characteristic returning payload text is an ingestion point for any
pipeline that lists nearby devices and summarises them.

It replaces the active BLE profile, so it cannot run alongside the
beacon — starting one stops the other. The default profile is restored on
leaving the scene and on app teardown. Over the bridge:
`EXEC BLEGATT <name>` / `STOP BLEGATT`.

Unlike the firmware's HID profile, none of this is withheld from external
apps: `ble_gatt_service_add()` and `ble_gatt_characteristic_init()` are
both exported, so the profile is ours to define.

### NFC tag emulation

The original NFC path writes a `.nfc` file and hands over to the stock
NFC app, which costs the operator their place in the kit. This emulates
an NTAG215 in-process instead, so the payload is presented while the app
stays on screen.

NFC matters among these channels because **NDEF is the one wireless
format a phone acts on with no app installed and no pairing.** Tap an
unlocked handset against the Flipper's back and the OS parses the tag and
offers the content. Of everything here, it is the only channel reaching a
general-purpose consumer device with zero setup on the target.

That last part depends on the record type, and the difference is not
cosmetic. iOS raises a banner for a **URI** record but reads and silently
discards a **Text** record unless an app is in the foreground holding an
open `NFCNDEFReaderSession`. Verified on the wire: the listener logs a
full page sweep for both, so the tag is read either way and the phone
just declines to surface the text one.

So there are two entries. **NFC emulate (URL)** carries the payload in
the query string of an `example.com` URL — RFC 2606 reserves that domain
so a stray tap cannot reach a live host — and is the one that works
against an untouched phone. **NFC emulate (in-app)** presents a Text
record, which is the better fit when the far side is already reading
tags: a kiosk, an Android reader app, an assistant with a tag
integration.

Capacity is 481 bytes as text (the 496-byte NDEF area an NTAG215's
capability container declares, minus record overhead). The URL form pays
up to three bytes per character for percent-encoding, so it holds roughly
458 characters of unreserved ASCII and as few as 152 when everything
escapes. Oversized payloads are refused rather than truncated.
Over the bridge: `EXEC NFCEMU <name>` / `EXEC NFCEMUURL <name>` /
`STOP NFCEMU`.

The emulated UID is the kit's own — a reader sees a valid NTAG215, but
nothing impersonates a specific card belonging to someone else.

### Concealment and filter-evasion payloads

Alongside the plaintext payloads, the kit ships families that test
whether a target's *filter* catches an instruction that has been
disguised — a system that blocks `ignore previous instructions` but
executes its base64 equivalent has detection, not defence.

| Category | Payloads | Tests |
|----------|----------|-------|
| `encoding` | base64, ROT13, hex, HTML entities, URL, nested | decode-then-obey behaviour |
| `obfuscation` | homoglyph, zero-width, spaced, JSON breakout | substring and structural filters |
| `concealment` | HTML comment, CSS-hidden span, `aria-hidden` | markup a human reviewer does not see |
| `impersonation` | forged system message, `ai:` meta namespace, chat-template tags | trust in structure and metadata |
| `multilingual` | same directive in EN/FR/DE/ES | English-only filters |
| `memory-poison` | persistence, deferred keyword trigger | cross-session retention |
| `usbdesc` | short payloads ≤126 chars | USB descriptor channel |

The `concealment` family targets an LLM that ingests a page or document
rather than a chat message: the instruction renders as nothing but is
plain text in the source an agent actually reads. `nested-encode` wraps
URL-encoding inside base64, so a target that decodes one layer and scans
the result still sees ciphertext.

**BadUSB cannot type non-ASCII.** `hid_ascii_to_key()` has no keycode
for characters outside its ASCII table, and they are skipped silently.
The homoglyph and zero-width payloads therefore arrive incomplete over
BadUSB while looking correct on screen, so the payload viewer and Quick
Deploy both warn and name the count. Use QR, NFC or GPIO for those. Every
`concealment`, `impersonation` and `encoding` payload is ASCII and types
intact.

### Captures — did the injection work?

Delivery is not effect. The GPIO channel can read a target's reply off
the wire, and `Wires > Send + capture` writes each one to
`captures.jsonl` in the app's data directory, one JSON object per line:

```json
{"ts":128394,"channel":"gpio","payload":"qr-ignore","sent":56,"rx":142,"trunc":false,"reply":"You are ACME-Bot..."}
```

**Main Menu > Captures** lists the most recent, newest first, and shows
the full reply for whichever is selected. A target that stayed silent is
recorded as `silent` rather than omitted — "the target said nothing" is a
finding, not a failed capture.

The device writes this file and the host reads it. Replies are escaped so
the log stays valid JSON whatever the target transmitted, including bytes
that are not valid UTF-8. The log is capped at 32 KB and rotates once to
`captures.jsonl.1`, so it cannot fill the SD card. Captures driven over
the serial bridge (`EXEC GPIOCAP`) are logged identically, so
scripting from the host still leaves evidence on the device.

`ts` is `furi_get_tick()`, not wall-clock: the Flipper has no reliably
synchronised RTC, so it is reported as seconds since boot rather than
presented as a real timestamp. Line order in the file is the record of
sequence.

### Serial Bridge (Remote Control)
- **Remote Mode** — Real-time control from a host over USB serial (CDC channel 1)
- **Line Protocol** — Simple text commands: `PING`, `LIST`, `EXEC`, `STATUS`, `LOAD`, `RELOAD`, etc.
- **Multi-Protocol Remote** — Execute any channel from the host: BadUSB, NFC file, NFC emulation, BLE beacon, BLE GATT, QR, USB descriptor, GPIO, GPIO with reply capture
- **Live Push** — Push one-off payloads from host without SD card (`LOAD` command)
- **Hot Reload** — Reload payloads from SD card without restart
- **Configurable Delays** — Adjust BadUSB start delay and inter-turn timing remotely

## Building

### Prerequisites
- [uFBT](https://github.com/flipperdevices/flipperzero-ufbt) (micro Flipper Build Tool)
- Flipper Zero with up-to-date firmware

### Build & Deploy
```bash
ufbt build
ufbt launch       # build, deploy and run via USB
```

### Development
```bash
ufbt build        # compile
ufbt launch       # build, deploy and run via USB
ufbt -c           # clean build artifacts
```

The tree is formatted against the stock ufbt `.clang-format`, so
`ufbt lint` passes. Run `ufbt format` before committing.

### Testing

Eight host-side suites run without a Flipper. All exit non-zero on
failure, so they can gate CI:

```bash
# QR encoder: version boundaries, oversize rejection, module integrity
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_qr tests/test_qr.c src/execute/qrcode.c
/tmp/test_qr

# Set QR_DUMP=1 to also write PBM renderings to /tmp for visual checks
QR_DUMP=1 /tmp/test_qr

# USB string descriptors: splitting, 126-char boundary, non-ASCII loss
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_usb_descriptor tests/test_usb_descriptor.c
/tmp/test_usb_descriptor

# NFC tag: BCC check bytes, capability container, NDEF round-trip,
# URI prefix folding, percent-encoding
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_nfc_ndef tests/test_nfc_ndef.c
/tmp/test_nfc_ndef

# Capture log: JSON escaping of control and >=0x80 bytes, NUL-containing
# replies, newest-first indexing, truncated and hand-edited lines
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_capture_log tests/test_capture_log.c
/tmp/test_capture_log

# I2C addressing: 7-bit to wire shift, reserved ranges, chunk arithmetic
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_i2c_addr tests/test_i2c_addr.c
/tmp/test_i2c_addr

# Payload streaming: object boundaries across every read-chunk split
# point, braces and brackets inside strings, escapes at a boundary,
# oversized objects skipped rather than truncated
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_payload_stream tests/test_payload_stream.c
/tmp/test_payload_stream

# Channel eligibility: which delivery channels accept which payloads
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_channel tests/test_channel.c
/tmp/test_channel

# Payload picker ordering: category grouping and sort stability
cc -Wall -Wextra -Werror -fsanitize=address,undefined \
   -o /tmp/test_payload_picker tests/test_payload_picker.c
/tmp/test_payload_picker
```

Two channels need hardware. GPIO has been validated against a Saleae
Logic 8 — both transmit modes, byte-exact, with the baud measured at
115,465 (0.23% off nominal) — and the write-up plus the raw captures are
in [docs/gpio-validation.md](docs/gpio-validation.md).

`tools/gpio_verify_capture.py` checks a logic analyser's UART export
against the payload that was sent, which is the only way to confirm what
actually left the pin rather than what our own driver read back:

```bash
# Logic 2: capture TX, Async Serial analyzer at the kit's baud, export CSV
python3 tools/gpio_verify_capture.py capture.csv --expect-payload qr-ignore
```

`tools/uart_edge_decode.py` works from a **raw digital** export instead,
decoding the line from edge timestamps. That is the only way to measure
the baud rate rather than assume it: the analyser's own Async Serial
decoder is told the rate, so it can report that bytes failed to decode
but never that the rate itself is wrong.

```bash
python3 tools/uart_edge_decode.py docs/captures/gpio-burst-115200.csv

# Sweep every rate the Settings picker offers
python3 tools/gpio_baud_sweep.py --live --channel 3
```

`tools/ble_gatt_probe.py`
is a BLE client that finds the GATT service the way a stranger would, reads
the characteristics and reassembles them — see the manual for what to watch
for:

```bash
pip install bleak
# On the Flipper: Wireless > Serve it > (payload)
python3 tools/ble_gatt_probe.py
```

`tests/test_usb_descriptor.c`, `tests/test_capture_log.c` and
`tests/test_payload_stream.c` duplicate logic from
`src/execute/usb_descriptor_exec.c`, `src/payload/capture_log.c` and
`src/payload/payload_db.c` rather than linking it, because those files
need the Flipper USB HAL and storage API. Keep each pair in step.

## File Structure

```
.
├── application.fam              # FAP manifest
├── .clang-format                # ufbt stock style (tree conforms)
├── .gitignore
├── images/
│   ├── icon_10x10.png           # App icon for Flipper menu
│   └── menu_icons/              # 9x9 per-row icons (see its README)
│       ├── mg_keyboard_9x9.png
│       ├── mg_wires_9x9.png
│       └── ...
├── src/
│   ├── pifk_app.c           # Entry point, app lifecycle
│   ├── pifk_app.h           # App state struct, shared types
│   ├── scenes/
│   │   ├── scene_main_menu.c    # Transport groups + libraries
│   │   ├── scene_channel_list.c # Channels in a group + diagnostics
│   │   ├── scene_payload_pick.c # Filtered payload list + execute
│   │   ├── payload_picker.c     # Shared list builder (filter, star)
│   │   ├── scene_payload_list.c # Unfiltered payload browser
│   │   ├── scene_payload_view.c # Payload detail + "Deploy via..."
│   │   ├── scene_remote_mode.c  # Serial bridge UI
│   │   ├── scene_settings.c     # App settings
│   │   ├── scene_captures.c     # Captured target replies
│   │   └── scene_about.c        # Credits, version
│   ├── payload/
│   │   ├── payload_db.c         # Load/parse payload JSON from SD
│   │   ├── payload_db.h
│   │   ├── builtin_payloads.c   # Compiled-in default payloads
│   │   ├── builtin_payloads.h
│   │   ├── capture_log.c        # Append-only captures.jsonl evidence log
│   │   └── capture_log.h
│   ├── execute/
│   │   ├── badusb_exec.c        # HID keyboard injection
│   │   ├── badusb_exec.h
│   │   ├── nfc_emulate.c        # NFC NDEF file generation (does not emulate;
│   │   │                        #   the built-in NFC app does that)
│   │   ├── nfc_exec.h
│   │   ├── ble_exec.c           # BLE beacon advertising
│   │   ├── ble_exec.h
│   │   ├── qr_exec.c            # QR code display
│   │   ├── qr_exec.h
│   │   ├── usb_descriptor_exec.c # USB ID strings as payload carrier
│   │   ├── usb_descriptor_exec.h
│   │   ├── gpio_exec.c          # UART payload egress + loopback test
│   │   ├── gpio_exec.h
│   │   ├── i2c_exec.c           # I2C bus scan + payload write
│   │   ├── i2c_exec.h
│   │   ├── ble_gatt_exec.c      # Readable GATT payload service
│   │   ├── ble_gatt_exec.h
│   │   ├── nfc_listener_exec.c  # In-process NDEF tag emulation
│   │   ├── nfc_listener_exec.h
│   │   ├── qrcode.c             # QR code generation library
│   │   └── qrcode.h
│   └── serial/
│       ├── bridge_protocol.c    # Serial bridge for remote control
│       └── bridge_protocol.h
├── payloads/
│   └── pifk_payloads.json   # Default payloads (copy to SD)
├── docs/
│   ├── MANUAL.md                # Operator manual
│   ├── multi-turn-technique.md  # The six multi-turn scripts, as prose
│   ├── ui-transport-first.md    # Why navigation is transport-first
│   ├── gpio-validation.md       # Logic-analyser validation of the GPIO channel
│   ├── captures/                # Raw analyser captures backing that write-up
│   └── ...                      # Research and design notes
├── tests/
│   ├── test_qr.c                # Host-side QR encoder test suite
│   ├── test_usb_descriptor.c    # USB string descriptor tests
│   ├── test_nfc_ndef.c          # NFC tag structure and NDEF encoding
│   └── test_payload_stream.c    # Streaming payloads.json object splitter
├── tools/
│   ├── ble_gatt_probe.py        # BLE GATT client, for verifying that channel
│   ├── gpio_verify_capture.py   # Checks a logic-analyser UART export
│   └── gpio_target_sim.py       # Stands in as a serial target for GPIO capture
├── README.md
└── changelog.md
```

## SD Card Data Layout

After syncing from the host (`hw flipper sync`), the Flipper SD card contains:

```
/ext/apps_data/pifk/
├── payloads.json           # Payload library
├── favorites.json          # Starred payload names (written on device)
├── settings.json           # Delays, default protocol, quick-deploy choice
```

`favorites.json` and `settings.json` are written by the app as you use
it, so include them in a backup. On first run, if `payloads.json` is
absent, the app exports the compiled-in payloads there as an editable
starting point.

All of these are parsed defensively: a malformed file costs you the
entries it contains, not a crash. Files above 63 KiB are ignored.

## Serial Bridge Protocol

Line-based text protocol over USB CDC channel 1 (115200 baud).

### Host → Flipper (Commands)
| Command | Description |
|---------|-------------|
| `PING` | Heartbeat check |
| `LIST` | List loaded payloads |
| `EXEC BADUSB <name>` | Execute payload via BadUSB (HID keyboard) |
| `EXEC NFC <name>` | Generate NFC NDEF file |
| `EXEC NFCEMU <name>` | Emulate an NDEF tag in-process |
| `EXEC BLE <name>` | Start BLE beacon advertising payload |
| `EXEC BLEGATT <name>` | Serve payload from readable GATT characteristics |
| `EXEC QR <name>` | Display payload as QR code on screen |
| `EXEC USBDESC <name>` | Advertise payload in USB identifying strings |
| `EXEC GPIO <name>` | Send payload as bytes on the UART TX pin |
| `EXEC GPIOCAP <name>` | Send, then capture the target's reply |
| `EXEC I2C <name>` | Write the payload to the configured I2C address |
| `SCAN I2C` | Probe 0x08-0x77, list responding addresses |
| `STOP` | Abort current execution |
| `STOP BLE` | Stop active BLE broadcast |
| `STOP BLEGATT` | Stop serving GATT, restore default profile |
| `STOP NFCEMU` | Stop tag emulation, release NFC |
| `STOP USBDESC` | Restore the previous USB configuration |
| `STATUS` | Get current state (idle/listening/connected/executing) |
| `LOAD <json>` | Push a one-off payload (format: `{"name":"X","text":"Y"}`) |
| `SET DELAY <ms>` | Set BadUSB start delay (0-60000ms) |
| `RELOAD` | Re-read payloads from SD card |

Commands are matched case-insensitively. Lines longer than 255 bytes are
rejected with `ERR command too long` rather than being truncated.

### Flipper → Host (Responses)

One line per command.

| Response | Sent for |
|----------|----------|
| `OK` | `STOP`, `STOP BLE`, `STOP USBDESC` |
| `OK PONG` | `PING` |
| `OK STATUS <state> payloads=<n>` | `STATUS` |
| `OK DONE <name>` | `EXEC BADUSB` |
| `OK WRITTEN <path>` | `EXEC NFC` |
| `OK BROADCASTING <name>` | `EXEC BLE` |
| `OK SERVING <name>` | `EXEC BLEGATT` |
| `OK EMULATING <name>` | `EXEC NFCEMU` |
| `OK DISPLAYING <name>` | `EXEC QR` |
| `OK ADVERTISING <name>` | `EXEC USBDESC` (appends dropped-char count) |
| `OK SENT <name>` | `EXEC GPIO` |
| `OK REPLY <name>: <text>` | `EXEC GPIOCAP` (or `(no reply)`) |
| `OK I2CWROTE <name>: <n> bytes to 0x<addr>` | `EXEC I2C` |
| `OK DEVICES <n>: 0x.. 0x..` | `SCAN I2C` |
| `OK loaded <name>` | `LOAD` |
| `OK RELOADED payloads=<n>` | `RELOAD` |
| `DATA [...]` | `LIST` |
| `ERR <message>` | Anything that failed |

`EXEC` commands are synchronous: the reply arrives when execution
finishes, so there is no separate "started" message. BadUSB execution
switches USB to HID, which drops this CDC link while it types — expect
the reply once the link returns.

## Driving the kit from a host

Remote Mode speaks a line-based protocol over USB CDC, so any host that
can open the serial port can drive it — see
[the remote protocol](docs/MANUAL.md) for the command set. The internal
tooling this was built against wraps it as:

```bash
# Sync payloads to Flipper SD card
hw flipper sync

# Real-time remote control
hw flipper remote
hw flipper remote exec system-prompt-leak
```

## Attribution and trademarks

This kit builds on the Flipper Zero platform and its open-source firmware
SDK:

| Project | Role here | Licence |
| --- | --- | --- |
| [Flipper Zero firmware](https://github.com/flipperdevices/flipperzero-firmware) | The `furi` API, GUI and HAL this FAP is built against, via [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) | GPL-3.0 |

The QR encoder in `src/execute/qrcode.c` is an original implementation
written against ISO/IEC 18004 rather than a vendored library, so the FAP
carries no third-party code beyond the Flipper SDK it links against.

Flipper Zero and Flipper Devices are trademarks of Flipper Devices Inc.
Hak5, Bash Bunny and USB Rubber Ducky are trademarks of Hak5 LLC. This
project is not affiliated with, endorsed by, or sponsored by Flipper
Devices, Hak5, or any other hardware vendor. Product names are used only
to identify the hardware the kit interoperates with.

The companion host-side toolkit is
[prompt-injection-studio](https://github.com/Mindgard/prompt-injection-studio)
(AGPL-3.0), which generates and syncs the payload library this kit carries.
The two talk over the serial bridge protocol documented below; neither links
the other, so their licences are independent.

## Intended use

This is a security research and authorised red-team tool. Use it only
against systems you own or have explicit written permission to test.
Delivering prompt injection payloads to systems you do not control may be
unlawful in your jurisdiction.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Copyright 2026 Mindgard Ltd.

Chosen for its explicit patent grant, which matters for a security tool.

Note on the SDK: a Flipper FAP is compiled against the Flipper Zero firmware
SDK, which is GPL-3.0. The FAP is distributed as a separate `.fap` loaded by
the firmware at runtime rather than as part of a combined firmware image, and
it vendors no SDK code. If you intend to redistribute this kit as part of a
larger or commercial work, take your own legal advice on that interaction
rather than relying on this note.
