# Brother QL-820NWB, revisited: the USB-A host port

Status: **tested against hardware; the P-touch Template path is closed
for a FAP.** Supersedes part of `brother-ql820nwb-feasibility.md`.

The printer enumerates a Flipper on its USB-A port but rejects it as
non-compliant, because the Flipper's only HID interface is a composite
keyboard+consumer+mouse device and the printer's host wants a plain boot
keyboard. A FAP cannot supply a different one: no `usbd_*` endpoint
primitives are exported, so custom USB classes are firmware territory.
See "Hardware test results" and the scanner-emulation section.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

Everything from here to "Hardware test results" is the reasoning as it
stood *before* the hardware test, kept because the mass-storage analysis
and the P-touch command detail are still correct and still useful to
anyone who revisits this. The optimism about HID was wrong, and the test
section says why.

**Short version, as originally written: mass storage emulation is out,
but the USB Host port accepts HID devices, and the Flipper is already a
HID keyboard. That makes printing viable through P-touch Template.**

That held up except in one respect, which turned out to be decisive: the
port accepts *HID devices*, not *this* HID device.

I got the direction wrong twice, in opposite ways. My first note said USB
was impossible because "both sides are USB devices" — true of the
printer's type-B port, wrong about the type-A one, which is the printer
acting as a host. Correcting that, I then assumed a Flipper in HID mode
would satisfy it. The hardware disagreed.

---

## Two different things called mass storage

The search results conflate them and it matters, so:

**1. Mass Storage Mode (the type-B port).** The printer *presents itself*
as a 2.5 MB drive to a computer. You drag a file in and press OK. Brother
documents this as a driver-free print path.

Constraints from Brother's own FAQ for this model:

- Accepts `.bin` and `.blf` only. Not `.prn`, not raster, not images.
- 2.5 MB area, files over 2 MB may misbehave.
- Wi-Fi and Bluetooth are disabled while in this mode.
- Contents are wiped at power-off.
- Entered by holding OK + power until the LCD says `Mass Storage Mode`.

Here the printer is the *device* and the computer is the *host*. Useless
to us: the Flipper cannot be a host, so it cannot write into that area.
Also note `.blf` is a compiled template blob produced by P-touch Transfer
Manager (Windows only), not something we can synthesise on-device.

**2. The USB Host port (type-A).** The printer *is* the host and drives
an attached peripheral. Brother's own marketing for this model:

> Also equipped with a USB Host interface, the Brother QL-820NWB is
> well-suited for use with a peripheral such as a scanner.

And the QL-1100 series datasheet, which shares the platform, states it
plainly:

> USB host capability: **Yes (HID class)**

That is the sentence that changes the answer.

---

## Why mass storage emulation is out

For the Flipper to appear as a flash drive on the printer's host port it
would have to present the USB Mass Storage Class. It cannot:

```
$ grep -iE "msc|mass_storage|scsi" api_symbols.csv
(no matches — absent from the API table entirely)

$ ls lib/libusb_stm32/inc/ | grep -iE "msc|storage"
(nothing — no MSC implementation in the device stack)
```

The five roles a FAP can present are the whole list:

```
usb_cdc_single    usb_cdc_dual    usb_hid    usb_hid_u2f    usb_ccid
```

No MSC, not even disabled. Unlike the BLE HID profile — which exists in
firmware and is merely withheld — mass storage is not implemented at all.
Adding it means writing an MSC class plus a SCSI command set plus a FAT
image, in a FAP, to feed a printer a file format we cannot generate.

Dead end, and not a close one.

## Why HID is the opening

The printer's host port speaks HID. The Flipper's `usb_hid` interface is
exported and the kit already drives it — that is what BadUSB is.

So the Flipper can plug into the printer's type-A port and be
indistinguishable from a USB barcode scanner.

That matters because Brother built an entire feature around exactly that
peripheral. **P-touch Template** mode lets a scanner select a stored
template, push data into its fields, and trigger a print — all as
keystrokes. From the user's guide:

> P-touch Template mode allows you to insert data into text and barcode
> objects from a downloaded template using other devices connected to the
> printer.
>
> P-touch Template mode is also compatible with other input devices like
> a scale, testing machine, controller, or a programmable logic device.

A "programmable logic device" typing ASCII is precisely what we have.

### What the command set looks like

P-touch Template commands are printable sequences, which is what makes
them typeable:

```
^II            Initialise
^TS<n>         Select template n
^DI            Directly insert object
^FF            Start printing
^QS            Select print options
^QV            Specify QR Code version
^SS            Specify delimiter
```

Plus the ESC/P dynamic mode switch we already know from the raster work:

```
ESC i a <n>    Select command mode (n=3 -> P-touch Template)
```

So a print job becomes a keystroke sequence. The kit's existing
`hid_ascii_to_key()` covers `^`, digits, letters and 35 punctuation
cases, so most of this is already typeable with code we have.

### The catch, and it is a real one

**The template has to be on the printer first.** P-touch Template selects
a *stored* template and fills its fields; it does not accept an arbitrary
label design over HID. Templates are authored in P-touch Editor and
pushed with P-touch Transfer Manager, both Windows-only, over USB or the
network.

So this works in one of two situations:

1. **The printer already has a suitable template**, with a QR or barcode
   object we can populate. Plausible in a shipping or asset-tagging
   environment where the printer is provisioned for exactly that.
2. **We provision it once ourselves** during setup, then the Flipper
   drives it standalone thereafter.

Situation 1 is the interesting one for an engagement. If the target's
printer is already provisioned with a barcode template — and a printer
with a scanner attached almost certainly is — then a Flipper in that port
can print arbitrary QR content into that template.

Whether the QL-820NWB accepts `^DI` to populate a QR object with
arbitrary data, versus only selecting among pre-baked templates, is the
thing I could not settle from the documentation. The P-touch Template
Command Reference is a separate manual and the search results only gave
me its table of contents. That question decides whether this is "print a
QR of my choosing" or merely "trigger a print of a QR someone else
defined".

---

## What this would look like as a feature

Assuming `^DI` behaves, the shape is small because the transport already
exists:

```c
/* src/execute/ptouch_template_exec.c
 *
 * Present as a USB HID keyboard on the printer's host port and drive
 * P-touch Template. The printer thinks it is talking to a barcode
 * scanner.
 */
bool pifk_execute_ptouch(PifkApp* app,
                             const PifkPayload* p,
                             uint8_t template_id);
```

Sequence, as keystrokes:

```
ESC i a 3        switch to P-touch Template mode
^II              initialise
^TS<id>          select the stored template
^DI<payload>     insert the payload into the object
^FF              print
```

Every one of those is ASCII the existing keymap already handles. The work
is the sequence, a template-ID setting, and the physical test. Call it
**1–2 days**, most of it verifying behaviour against a real printer,
compared with the 9–11 days I estimated for the BLE keyboard.

Practical notes:

- The payload has to be ASCII. `hid_ascii_to_key()` drops anything else
  silently, which the kit now warns about — and QR content is usually
  ASCII anyway.
- Timing needs care. The printer is a slow host and P-touch Template has
  no flow control, so the existing `badusb_delay_ms` setting and the
  10ms per-keystroke pacing may both need raising.
- The printer must be in P-touch Template mode, configurable from its
  LCD or via the Printer Setting Tool. `ESC i a 3` should switch it
  dynamically but that is worth confirming.

---

## Why this is worth doing at all

The physical artefact was always the point. A QR code on a label, stuck
to a parcel or an asset or a meeting-room door, is read later by whatever
photographs it — and increasingly that is a multimodal assistant asked
"what is this". The kit can already show a QR on its screen, but a
printed label persists, travels, and looks legitimate in a way a
handheld screen does not.

Scenario worth testing: an asset-tagging workflow where labels are
printed by a scanner-attached printer and later scanned by an
inventory assistant. Print a label whose QR contains `qr-exfil`. When the
assistant reads that tag during a stock check, the payload is in its
context. Nobody typed anything and the label looks exactly like the
hundreds around it.

That is a supply-chain-shaped injection and it is hard to test any other
way.

---

## Corrections to the earlier assessment

`brother-ql820nwb-feasibility.md` says, under Option 2b:

> **Blocker A: a USB DAC is a USB device, so the Flipper would have to be
> the host.**

That reasoning is sound for the printer's type-B port and for the DAC
question it was answering. It does **not** apply to the type-A host port,
which I did not consider. The blanket "USB is impossible" framing in that
document's verdict is too strong and should be read as "impossible over
the type-B port".

The Bluetooth conclusion is unaffected: the printer is still Classic-only
and the Flipper still has no Classic radio.

## Recommendation

**Worth an hour with the hardware before anything else.** Three questions
in order:

1. Does the printer's type-A port enumerate a Flipper in `usb_hid` mode
   at all? Plug it in and watch the LCD. If it rejects an unknown HID
   device, everything below is moot.
2. Does it have a template stored with a QR or barcode object?
   `^TS1` then `^FF` will tell you.
3. Does `^DI` populate that object with arbitrary data, or only select
   among fixed templates? This is the question that decides whether the
   feature is useful.

If all three land, this is the cheapest new channel available — days, not
weeks, and it reuses the BadUSB keymap wholesale. If question 3 fails, it
degrades to "trigger a print of a label someone else designed", which is
much less interesting but still a physical-artefact channel.

I would not write code before answering question 1. It costs a cable and
five minutes, and it gates everything else.

---

## Implementation notes for a fresh start

Everything above is the research. This section is what someone opening
this file with no other context needs in order to actually build it. I
wrote it after re-reading the doc cold and finding six places where I
would have had to go back to the SDK.

### Blocker: there is no ESC keycode in the keymap

`ESC i a 3` cannot be typed as the sequence above implies.
`hid_ascii_to_key()` in `src/execute/badusb_exec.c` maps `\n` and `\t`
but has no entry for `\x1b`, and its `default:` returns 0, which
`type_char()` skips silently. So the mode-switch command would be sent as
`i a 3` with the ESC missing, which the printer will not recognise.

The keycode exists in the SDK — `HID_KEYBOARD_ESCAPE 0x29` in
`lib/libusb_stm32/inc/hid_usage_keyboard.h` — it is simply not wired up.
Add it:

```c
/* in hid_ascii_to_key(), alongside the existing '\n' and '\t' cases */
case '\x1b': return HID_KEYBOARD_ESCAPE;
```

Check `pifk_badusb_count_unmappable()` after doing this: it counts
anything `hid_ascii_to_key()` rejects, so adding ESC changes what the
payload viewer reports for any payload containing `\x1b`. That is
correct behaviour, but the count moves.

Whether a barcode scanner would ever send ESC is a separate question. If
the printer only accepts the LCD/Printer-Setting-Tool route into P-touch
Template mode, the ESC sequence is unnecessary and this blocker
disappears — the operator sets the mode by hand and the Flipper only
sends `^` commands. Worth establishing early, because it is the
difference between "needs a keymap change" and "does not".

### Second blocker: `ESC i a 3` has an untypeable argument

This one is worse than the ESC gap and I missed it on the first pass.

In `ESC i a <n>`, the `n` is the **byte value** `0x03`, not the character
`'3'` (`0x33`). HID keycodes cover printable characters and a handful of
named keys; control bytes below `0x20` have no keycode at all. So even
with ESC wired up, the mode-switch argument cannot be typed:

```
ESC  0x1b -> HID_KEYBOARD_ESCAPE   ok, once added
i    0x69 -> mapped                ok
a    0x61 -> mapped                ok
0x03      -> no keycode            CANNOT BE TYPED
```

The `^`-prefixed P-touch Template commands are all printable and type
fine. It is only the ESC/P-style mode switch that is out of reach.

**Consequence: the mode switch has to happen some other way.** The
printer must already be in P-touch Template mode — set from its LCD, or
via the Windows Printer Setting Tool, or persisted from a previous
session — before the Flipper is plugged in. That is a real operational
constraint for an engagement, not just an implementation detail: it means
the channel needs either a pre-configured printer or a moment of physical
access to its front panel.

Verified by walking the whole sequence through a copy of the keymap: of
22 characters in `ESC ia\x03 ^II^TS1^DIhello^FF`, exactly one is
undeliverable, and it is the `0x03`.

This does not kill the idea. It means the feature is "drive a printer
that is already in P-touch Template mode", which is still useful — a
printer with a scanner attached is almost certainly already in that mode,
because that is what the mode is for.

### The existing typing helpers are static

`type_string()` and `press_enter()` are both `static` in
`badusb_exec.c`, so a new file cannot call them. Three options, in order
of preference:

1. **Put the P-touch code in `badusb_exec.c`.** It is the same transport
   with a different byte sequence, and the file is under 200 lines. This
   is what I would do.
2. Expose `type_string()` in `badusb_exec.h`.
3. Duplicate the loop. Do not do this.

Note also that `press_enter()` must **not** fire for P-touch: `^FF`
triggers the print, and a trailing Enter would be an extra keystroke the
printer may interpret as data. `pifk_execute_badusb()` sends Enter
unconditionally, so the P-touch path needs its own body rather than a
wrapper around it.

### The USB switch and teardown pattern to copy

`pifk_execute_badusb()` already does this correctly and the P-touch
path should mirror it exactly:

```c
FuriHalUsbInterface* prev_usb = furi_hal_usb_get_config();
furi_hal_usb_set_config(&usb_hid, NULL);
furi_delay_ms(app->badusb_delay_ms);        /* let the host enumerate */

if(!furi_hal_hid_is_connected()) {           /* printer did not accept us */
    furi_hal_usb_set_config(prev_usb, NULL);
    app->executing = false;
    return false;
}

/* ... type the sequence ... */

furi_hal_usb_set_config(prev_usb, NULL);     /* always restore */
furi_delay_ms(200);
```

`furi_hal_hid_is_connected()` is the answer to research question 1. If
the printer's host port refuses a generic HID keyboard, this returns
false and you have your answer without a logic analyser.

### Wiring a new channel into the app: the full checklist

The kit has ten channels and they all follow the same pattern. Miss a
step and it either fails to build or silently does nothing. In order:

1. **`src/execute/*.h` + `.c`** — or extend `badusb_exec.c` per above.
2. **Quick Deploy** (`src/scenes/scene_quick_deploy.c`): add to the
   `QuickDeployIndex*` enum, add a `case` in `quick_deploy_callback()`,
   add the `submenu_add_item()` call, and update the ASCII menu diagram
   in the file header. That diagram has drifted before.
3. **Bridge request kind** (`src/pifk_app.h`): add
   `PifkReqExecPtouch` to the `PifkRequestKind` enum.
4. **Bridge handler** (`src/pifk_app.c`): add the `case` in
   `pifk_service_bridge_request()`. Acquire `app->db_mutex` around
   the `payload_db_find()` and release it on **every** exit path
   including the not-found branch.
5. **Bridge dispatcher** (`src/serial/bridge_protocol.c`): add
   `starts_with(cmd, "EXEC PTOUCH ")` and the reply verb. **Prefix order
   matters** — longer prefixes must be tested before shorter ones that
   share a stem, or the short one shadows the long one.
6. **Protocol docs**: the comment block at the top of
   `bridge_protocol.c`, and the two tables in `README.md`.
7. **Settings**, if you add a template-ID option: the item, the
   persistence in `settings_save()`/`settings_load()` in
   `src/payload/payload_db.c`, and a range check on load because
   `settings.json` is user-editable.
8. **`docs/MANUAL.md`**: the channel section, the capacity table, the
   bridge command reference and the response table.
9. **Teardown**: this channel needs none, since it restores USB
   synchronously. Channels that keep running (USB descriptor, BLE GATT,
   NFC emulation) must also be stopped in `pifk_app_free()`, because
   scene `on_exit` handlers do not run during app teardown.

### Verify the symbols actually link

This is the trap that cost me time on the BLE work. A FAP can `#include`
a header and compile cleanly against symbols the firmware **does not
export**, and `--gc-sections` will strip the calls if nothing reachable
uses them — so the build passes and the app dies at launch.

Everything P-touch needs is already imported by the existing BadUSB
channel (`usb_hid`, `furi_hal_hid_kb_press`, `furi_hal_hid_kb_release`,
`furi_hal_hid_is_connected`, `furi_hal_usb_set_config`,
`furi_hal_usb_get_config`), so there is no new linkage risk here. But if
you reach for anything else, check it:

```bash
ufbt -c && ufbt
grep -iE "<your_symbol>" ~/.ufbt/build/pifk.impsyms
```

If the symbol is missing from `.impsyms` while your code calls it, it was
stripped. If `APPCHK` prints "in API, but disabled", the firmware is
withholding it and there is no way through from a FAP.

### Timing is the likely failure mode

The printer is a slow, embedded USB host with no flow control on the
P-touch command stream. `type_char()` uses a fixed 10ms press/release,
which is tuned for desktop operating systems.

Expect to need slower pacing. If characters go missing, that is the first
thing to change — and note the fix belongs in a P-touch-specific delay
rather than in `type_char()`, since raising it globally would slow every
BadUSB payload.

`app->badusb_delay_ms` (Settings > BadUSB Delay, default 1000ms) is the
post-enumeration wait and is already configurable. The printer may need
more than a desktop does.

### Suggested first commit

Do not build the whole channel first. Build the smallest thing that
answers research question 1:

1. Add the ESC keycode.
2. Add a Quick Deploy entry that switches to `usb_hid`, waits, and
   reports whether `furi_hal_hid_is_connected()` came back true, then
   restores USB. No typing at all.
3. Plug into the printer's type-A port and read the result.

That is an hour of work and it tells you whether the remaining days are
worth spending. If the printer rejects a generic HID keyboard, stop
there and the answer is documented.

### Test payloads that already exist

`qr-ignore` (56 bytes) and `qr-exfil` (62 bytes) are short, ASCII-only
and already in the payload database. Use those for the first print rather
than authoring anything new — if the label comes out and the QR scans,
the channel works.

---

## Hardware test results

Tested against a real QL-820NWB. Both research questions 1 and 2 now
have answers, and they are more useful than a plain yes/no.

### Question 1: does the type-A port accept a Flipper as HID?

**Connection yes, acceptance no.** The two observations disagree in a way
that is itself the finding:

- The Flipper's probe reported `OK 150ms`. `furi_hal_hid_is_connected()`
  went true, so the printer powered the port, enumerated the device and
  completed enough of the HID handshake for our side to consider itself
  connected.
- The printer's LCD then showed **"USB device non-compliant"** (earlier
  wording observed: "USB device not compatible. Remove the USB device").

So the port is live and does enumerate HID peripherals. It rejects *this*
HID device after inspecting it. That rules out the boring explanations —
the port is not disabled, not power-only, and not refusing unknown
vendors outright.

### Why it is almost certainly the composite descriptor

The Flipper's `usb_hid` interface is not a plain keyboard. It bundles
keyboard, consumer-control and mouse into one composite HID device with
multiple report IDs — `furi_hal_usb_hid.h` includes both
`hid_usage_keyboard.h` and `hid_usage_consumer.h`, and the API exposes
`furi_hal_hid_kb_press`, `furi_hal_hid_consumer_key_press` and
`furi_hal_hid_mouse_move` against the same interface.

A barcode scanner is a single-report boot-protocol keyboard. An embedded
printer host has no reason to implement more than that, and "non-compliant"
is exactly what a minimal HID parser says when it meets a multi-report
descriptor it cannot walk.

This is consistent with everything else observed and it is the only
explanation I can construct that fits both the successful connection and
the subsequent rejection.

### What would be needed to get past it

A **boot-protocol-only HID interface**: `bInterfaceSubClass = 1` (Boot
Interface), `bInterfaceProtocol = 1` (Keyboard), one 8-byte input report,
no report IDs, no consumer page, no mouse.

That is not something a FAP can do. `FuriHalUsbInterface` lets us
override the device descriptor and the three identifying strings — that
is what the USB descriptor injection channel exploits — but the
configuration descriptor and the HID report descriptor come from the
firmware's `usb_hid` implementation, and there is no exported hook to
replace them. Substituting our own `cfg_descr` would mean supplying a
matching `init`/`deinit` pair that drives the endpoints, which is a USB
HID class implementation rather than a descriptor tweak.

So: **closed for a FAP.** Reachable three ways, none of them this app:

1. A firmware fork adding a boot-keyboard-only interface alongside
   `usb_hid`.
2. An upstream request for a `usb_hid_boot` variant. Cheap for the
   firmware to add, since the plumbing exists.
3. An external microcontroller presenting a clean boot keyboard, driven
   from the Flipper over GPIO UART. Works today, but it is the
   "bring your own hardware" story again, and at that point the
   microcontroller could talk to the printer directly.

### Question 2 is now moot, and question 3 with it

Whether a template is stored and whether `^DI` takes arbitrary data no
longer matter for this app, because we cannot get keystrokes into the
printer at all. Both remain open if anyone pursues route 1 or 2 above.

Note also the independent blocker recorded earlier: the `0x03` argument
in `ESC i a 3` has no HID keycode, so even a working boot keyboard could
not send the mode switch. That one is survivable — the printer can be put
into P-touch Template mode by hand — but it means two separate problems
would need solving, not one.

### What the laptop-side test added

Sending `ESC i a 3` + `^SR` through the CUPS `ippusb` queue was
inconclusive: the job was accepted, the printer connected, and the job
then hung at "waiting for job to complete" until cancelled. No label was
consumed. The `ippusb` layer expects well-formed jobs with completion
semantics and cannot surface the printer's 32-byte status reply, so it is
the wrong tool for probing a command set. Answering `^DI` properly needs
raw bulk access:

```bash
brew install libusb && pip3 install pyusb
```

Then write to the bulk OUT endpoint on VID `0x04F9` PID `0x209D` and read
the status reply from bulk IN. Worth doing if route 1 or 2 ever makes the
HID side viable; pointless before then.

---

## "Can the Flipper emulate a software-definable barcode scanner?"

Worth answering directly, because it is the obvious next thought and the
answer splits in two.

### Into a computer: yes, and it already does

A USB barcode scanner is a HID keyboard. It types the decoded barcode
contents into whatever field has focus and usually presses Enter. That is
exactly what `pifk_execute_badusb()` does, keystroke for keystroke.

There is no new feature to build here. If the target is a warehouse
terminal, a POS, a stock-take app or an inventory system expecting
scanner input, the kit's BadUSB channel already **is** a software-defined
scanner. The only difference from a real scanner is that the "barcode"
never existed.

That framing is actually useful for an engagement, because it changes who
the target is. A system that trusts scanner input is often less careful
than one expecting a human at a keyboard — the data is assumed to have
come from a printed label, which is assumed to have come from the
business. Pointing the BadUSB channel at a scanner-fed field tests that
assumption directly.

Two practical notes if you use it this way:

- Most scanners append Enter. `pifk_execute_badusb()` does too, so
  the behaviour matches.
- Real scanners type fast, in a burst, with no human timing. The kit's
  fixed 10ms per keystroke is a reasonable imitation. Some systems
  fingerprint input speed to distinguish scanners from humans; this looks
  like a scanner, not a person.

### Into the Brother printer: no

This is what the probe tested and it failed at the descriptor, not the
concept. Detail in the section above: the printer enumerated us
(`OK 150ms`) then rejected us as non-compliant, almost certainly because
the Flipper's `usb_hid` is a composite keyboard + consumer + mouse device
with multiple report IDs, where the printer's minimal host expects a
single-report boot keyboard.

### Why we cannot just present a cleaner descriptor

`FuriHalUsbInterface` has `cfg_descr` as a `void*`, so it looks
assignable — and the USB descriptor injection channel already proves a
FAP can override the *device* descriptor and the three identifying
strings.

The configuration descriptor is different, because it does not travel
alone. Replacing it means also supplying the `init`/`deinit` pair that
configures and drives the endpoints it declares, and that needs the
`usbd_*` primitives:

```
$ awk -F, '$2=="+"' api_symbols.csv | grep "usbd_"
usbd_devfs          <- the driver struct, nothing else

$ grep -E "^usbd_(reg|ep|connect|enable)" api_symbols.csv
(no matches — not in the API at all, not even disabled)
```

So a FAP cannot implement a USB class. It picks from the five prebuilt
interfaces (`usb_cdc_single`, `usb_cdc_dual`, `usb_hid`, `usb_hid_u2f`,
`usb_ccid`) and may relabel their strings. `usb_hid_u2f` is FIDO, not a
boot keyboard, so there is no cleaner HID variant to reach for.

A boot-keyboard-only interface is a small addition *for the firmware* —
the plumbing all exists — and it would unlock this and any other picky
embedded USB host. That makes it a good upstream feature request and a
poor FAP project.

### If you want this to work anyway

In rough order of effort:

1. **Upstream request** for a `usb_hid_boot` interface: single 8-byte
   report, `bInterfaceSubClass = 1`, `bInterfaceProtocol = 1`, no report
   IDs. Cheap for the firmware, useful beyond this printer.
2. **Firmware fork** adding the same thing, if waiting is not an option.
   Costs the stock-firmware audience.
3. **External microcontroller** presenting a clean boot keyboard, driven
   over GPIO UART from the kit. Works today with no firmware change, but
   once that microcontroller exists it could equally talk to the printer
   over its own USB or serial, so the Flipper adds little.

Option 1 is the one I would take, and it is worth filing regardless of
whether anyone pursues the printer, because "embedded host rejects the
composite HID descriptor" will keep recurring.

## Verification notes

- Mass Storage Mode constraints (`.bin`/`.blf` only, 2.5 MB, contents
  wiped at power-off, Wi-Fi and Bluetooth disabled, OK+power entry) from
  Brother's support FAQ `faqp00001538_005` for `lpql820nwbeus`.
- "Also equipped with a USB Host interface … well-suited for use with a
  peripheral such as a scanner" from Brother USA's QL-820NWB product
  page.
- "USB host capability: Yes (HID class)" from the Brother QL-1100 /
  QL-1110NWB brochure, which shares the platform. **I did not find that
  line in QL-820NWB-specific documentation**, so treat the HID-class
  claim as strongly implied rather than confirmed for this exact model.
  Question 1 above settles it.
- P-touch Template being driven by scanners and by "a programmable logic
  device", and the `^II` / `^TS` / `^DI` / `^FF` / `^QS` / `^QV` command
  names, from the P-touch Template Command Reference table of contents
  (QL-720NW manual, same command set) and a Brother user's guide.
- Absence of USB MSC: no `msc`/`mass_storage`/`scsi` entries in
  `api_symbols.csv`, and no `usb_msc.h` in
  `lib/libusb_stm32/inc/`. The five `FuriHalUsbInterface` exports are
  the complete list.
- **Not verified:** whether `^DI` accepts arbitrary QR content on this
  model, whether the printer accepts a generic HID device on its host
  port, and whether `ESC i a 3` switches mode dynamically from the host
  port rather than only over type-B. All three need hardware.
- No code was written or built for this assessment.
