# Printing to a Brother QL-820NWB — feasibility

Status: research only, nothing implemented.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

**Partly superseded.** See `brother-usb-host-followup.md`: the printer's
type-A USB *Host* port accepts HID devices, which the Flipper can be, and
that opens a viable path via P-touch Template. The verdict below is
correct for the type-B port and for Bluetooth, but "USB is impossible" is
too broad — I had not considered the host port when I wrote it.

**Verdict (type-B port and Bluetooth): not feasible.**

- **Bluetooth:** the QL-820NWB speaks Bluetooth Classic (SPP); the
  Flipper's radio is BLE-only. A radio/baseband incompatibility, not a
  missing driver.
- **USB type-B cable:** that port makes the printer a USB device, and so
  is the Flipper. The STM32WB55 has no OTG/host controller; ST lists the
  family as "Device only". Two devices, no host, no bytes move. The
  type-A host port is a different story — see the follow-up.

A UART path via the GPIO header is the only technically viable route, and
it requires an external bridge module that does the actual talking.
Whether that is worth building is a product question, addressed at the
end.

## Why the use case is interesting

Printing is a genuinely useful injection vector, and unlike the kit's
other channels it produces a *physical artefact*. A label carrying a
prompt injection can be stuck on a meeting-room display, a parcel, an
asset tag or a document, and then read by whatever multimodal assistant
later photographs or scans it. That is a real class of indirect
injection: the payload enters through the target's camera, not its
keyboard or network.

The QL-820NWB is also a sensible printer to target — 62mm continuous
tape, two-colour, and common in offices, so a plausible thing to find
on-site.

## The blockers

Both transports the printer actually offers are closed to the Flipper,
for unrelated reasons. Bluetooth first; USB is covered under Option 2b.

Four facts on the wireless side, each verified independently:

**1. The printer is Bluetooth Classic, not BLE.**

Brother's own QL-800-series datasheet lists:

```
Bluetooth      - Bluetooth 2.1+EDR
Bluetooth profiles - SPP (Serial Port Profile)
                     OPP (Object Push Profile)
                     BIP (Basic Imaging Profile)
                     HCRP (Hard Copy Cable Replacement Profile)
```

All four are Classic (BR/EDR) profiles. There is no GATT service and no
BLE advertisement to connect to.

**2. The Flipper has no Classic radio.**

The STM32WB55 runs ST's `stm32wb5x_BLE_Stack_light_fw.bin`. Flipper's
own maintainers, on the request to expose a serial profile:

> Flipper Zero doesn't support Bluetooth classic, only Bluetooth Low
> Energy. That's why `Bluetooth terminal` is not working. Also there is
> no `serial profile` for BLE, it's more complicated.

Confirmed against the SDK — a word-boundary search of the exported API
for `rfcomm`, `spp`, `bredr`, `br_edr` returns nothing.

**3. Even within BLE, the Flipper is peripheral-only.**

Talking to a printer means being the *initiator*. The API exports no
central role and no GATT client:

```
$ grep -iE "gatt_client|ble_central|gap_scan" api_symbols.csv
(no matches)
```

Upstream confirms this is a flash/RAM constraint, not an oversight —
`BLE: add central role and GATT client support` is an open PR, not a
shipped feature. So even if the printer *did* expose GATT, today's
firmware could not initiate the connection.

**4. There is no networking either.**

The 820NWB accepts the same raster stream over raw TCP on port 9100,
which would be the easy path on any networked device. The FAP API exports
no sockets, no TCP, no Wi-Fi.

### Conflicting source, resolved

One page in the `thermal-label/brother-ql` docs claims the 820NWB "also
offers Bluetooth SPP and Bluetooth GATT transports". Three other pages of
the same project contradict it, and so does a commit in that repository:

> **drop BLE placeholder, wire serial for QL-820NWB**
> The QL-820NWB / 820NWBc expose classic Bluetooth (SPP), not BLE — so
> Web Bluetooth isn't an option. […] `BrotherQLDevice` loses the
> `bluetooth?: BluetoothConfig` field and the `BLE_TBD` placeholder
> constant.

They attempted BLE, found it absent on the hardware, and deleted the
placeholder. I am treating the protocol page as a stale error and the
commit as authoritative. Brother's datasheet agrees.

## The good news: the protocol itself is trivial

If a byte pipe existed, the printing work would be small. The raster
protocol is transport-agnostic — the docs are explicit that "the wire
protocol is identical across transports — no framing or handshake layer
is added." A job is a flat byte stream:

```
NULL × 400                  parser reset (200 works in practice)
ESC i a 01    1B 69 61 01   switch to raster mode
ESC @         1B 40         initialize
ESC i z ...   1B 69 7A ..   print info: media type, width, raster count
ESC i M n     1B 69 4D n    autocut flag
ESC i K n     1B 69 4B n    two-colour / cut-at-end / high-res flags
ESC i d n1 n2 1B 69 64 ..   feed margin, little-endian dots
  per row:
    g 00 n d1..dn  (67)     one raster row, 90 bytes for 62mm media
    Z              (5A)     blank row
Control-Z     1A            print + feed (last page)
```

For 62mm continuous tape the head is 720 dots wide — 90 bytes per row,
uncompressed, one `g` opcode per row. No compression needed (`M 00`),
no status parsing needed for a fire-and-forget print.

We would also need to rasterise text, which is the part the kit does not
currently have: there is no font renderer, because the QR encoder emits a
bitmap and everything else is screen text drawn by the OS canvas. Options
are a small bitmap font compiled in (a 5×7 or 8×8 face is a few KB), or
reusing the existing QR encoder and printing the payload as a large
scannable QR label — which is arguably the more useful artefact anyway,
and needs no font at all.

## Options

### Option 1 — Bluetooth. Not possible.

No path. Not "hard", not "needs a custom profile": the two devices have
no radio in common. Nothing in software fixes this. Dismissed.

### Option 2 — UART over the GPIO header

The only technically sound route. The full serial API is exported:

```
furi_hal_serial_control_acquire / release
furi_hal_serial_init / deinit / set_br
furi_hal_serial_tx / tx_wait_complete
furi_hal_serial_async_rx  (for ESC i S status replies)
gpio_usart_tx / gpio_usart_rx        (pins 13 / 14)
```

So the Flipper can emit arbitrary bytes at a chosen baud rate, which is
all the raster protocol needs.

The problem is the other end. The QL-820NWB has **no TTL serial port**.
Its interfaces are USB device, Ethernet, Wi-Fi and Bluetooth Classic.
Bridging the gap needs hardware between them, and each option undermines
the point:

| Bridge | Notes |
|---|---|
| HC-05 / RN-42 Bluetooth Classic module on the UART | Works electrically. Adds a module that must be pre-paired to the specific printer, plus power. The Flipper becomes a serial cable to a dongle that does the actual Bluetooth. |
| ESP32 (Wi-Fi) forwarding to port 9100 | Also works. Now we need the printer's IP and the ESP32 on its network — at which point a laptop is easier. |
| USB host adapter | Not viable — see the USB section below. |

In every case the Flipper is not printing — it is feeding bytes to
something else that prints. For a field kit whose selling point is "one
device, no accessories", that is a poor trade.

### Option 2b — wired USB to the printer. Also not possible.

The printer's primary interface is USB, and its raster protocol over the
bulk OUT endpoint is the best-documented path of all. So a cable seems
like the obvious answer. It is not, and the reason is at the silicon
level rather than in software.

**USB is asymmetric.** One side is the *host*: it supplies bus power,
enumerates devices, assigns addresses, reads descriptors and initiates
every transfer. The other is the *device*, which only responds. A printer
is a device. To print over a cable, the Flipper would have to be the
host.

**The STM32WB55 cannot be a USB host.** ST's own peripheral matrix
classifies each family as "USB Device only", "USB OTG FS", or "USB OTG
HS". `STM32WB` appears under **Device only** — it has no OTG controller
at all. This is a missing hardware block, not a disabled feature.

The firmware matches. `libusb_stm32` ships `usbd_core.h` and nothing
else — a search for `usbh_`/`USBH_` across the library returns no
results. The exported API offers five `FuriHalUsbInterface` descriptors:

```
usb_cdc_single   usb_cdc_dual   usb_hid   usb_hid_u2f   usb_ccid
```

Every one is a role the Flipper *presents* to a host. There is no API to
enumerate or address a downstream device. The only `*_host` symbols in
the entire SDK are `onewire_host_*`, which is the 1-Wire bus used for
iButton — unrelated to USB.

The OTG symbols that do exist are a red herring worth naming, because
they look promising:

```
furi_hal_power_enable_otg     power_enable_otg
furi_hal_power_check_otg_status / _fault
```

These control the 5V boost regulator on the GPIO header — they let the
Flipper *supply* 5V to external circuitry. That is the power half of OTG
with none of the protocol half. Enabling it does not create a host
controller.

So a Flipper-to-printer cable has two devices and no host: nothing
enumerates, and not a single byte moves. Plugging them together does
nothing at all — it is not a driver problem that could be solved with
more work.

For completeness, the theoretical escapes and why they are worse than
the problem: a USB host shield on the GPIO header (e.g. MAX3421E over
SPI) would work electrically, but that means writing a USB host stack
plus a printer-class driver on a device with ~860KB of flash, to drive a
printer that a laptop already drives. And the Flipper cannot pose as a
printer to intercept a real host — that inverts the roles but still
leaves it unable to *reach* the printer.

### Option 3 — write the job to the SD card, print it elsewhere

Generate a `.prn` (the raw raster byte stream) or a `.png` on the SD
card, and let the operator print it from a laptop or phone. This mirrors
what the kit already does for NFC: `pifk_nfc_write_file()` writes an
`.nfc` file and hands off.

Honest, cheap, and it works today with no new transport. It also does not
really deliver "printing from the Flipper" — the operator still needs
another machine, so the feature is a file exporter.

### Option 4 — don't do it; print the QR label instead

The kit already renders payloads as QR codes on screen. A phone
photographing the Flipper's display, or a QR printed by any normal
printer from an exported PNG, produces the same physical artefact for
camera-based injection testing. The printer adds convenience, not
capability.

## Recommendation

**Don't build this.** The Bluetooth route — the one actually asked about —
is closed by hardware, and every workaround either needs a second device
(Options 2) or reduces to a file exporter (Option 3), which is not what
"printing from the Flipper" implies.

If label output turns out to matter for real engagements, the cheapest
useful increment is **Option 3 scoped honestly**: an "Export label" action
that writes a 62mm-wide 1-bit PNG of the payload — or of its QR code — to
the SD card, documented as "print this from your laptop". Perhaps a day's
work, reuses the existing QR encoder, and adds no transport code or
hardware dependency. Call it what it is in the UI; do not call it
printing.

Two things that would change this answer:

- **A different printer.** Several vendors' portable label printers
  (some Phomemo, Niimbot and Brother PT models) are genuinely BLE/GATT.
  If the goal is "a Flipper that prints labels" rather than "a Flipper
  that drives *this* printer", a BLE-native model is worth a look —
  though note the Flipper still lacks the central role needed to
  initiate, so this is blocked on that upstream PR too.
- **BLE central landing upstream.** `BLE: add central role and GATT
  client support` would unblock talking to BLE peripherals generally.
  Worth watching, and it does not help with the 820NWB specifically,
  which has no GATT at all.

## Verification notes

- Printer transport and profiles: Brother QL-800-series datasheet
  (`Bluetooth 2.1+EDR`, `SPP/OPP/BIP/HCRP`).
- Raster opcodes and job structure: Brother *Software Developer's
  Manual — Raster Command Reference, QL-800/810W/820NWB* v1.00/1.01,
  cross-checked against the `thermal-label/brother-ql` protocol notes
  and `pklaus/brother_ql`.
- The "GATT transport" claim was traced to a single docs page and
  contradicted by the same project's implementation pages and by commit
  `2b3ee22` ("drop BLE placeholder, wire serial for QL-820NWB").
- Flipper Classic support: maintainer response on
  flipperzero-firmware#3227; radio stack described in Flipper's
  "New Firmware Update System" post
  (`stm32wb5x_BLE_Stack_light_fw.bin`).
- BLE central absence: no `gatt_client` / `ble_central` / `gap_scan`
  symbols in `api_symbols.csv`; upstream PR #4436 still open per
  flipperzero-firmware#2906.
- Exported serial/GPIO/OTG symbols read from
  `~/.ufbt/current/sdk_headers/f7_sdk/targets/f7/api_symbols.csv`
  (API 87.1). No code was written or built for this assessment.
- USB device-only status: ST's "Introduction to USB with STM32" wiki
  peripheral matrix lists `STM32WB` under *USB Device only*, with no
  entry under *USB OTG FS* or *USB OTG HS*. Corroborated in-tree —
  `lib/libusb_stm32/inc/` contains `usbd_core.h` and no `usbh_`/`USBH_`
  symbols anywhere in the library; the only `*_host` exports in the SDK
  are `onewire_host_*` (iButton, not USB).
- The `furi_hal_power_*_otg` symbols were checked and are 5V boost
  control for the GPIO header, not a host controller.
