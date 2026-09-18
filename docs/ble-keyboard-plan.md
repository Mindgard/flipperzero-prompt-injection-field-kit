# BLE keyboard delivery — technical implementation plan

Status: research complete, not implemented.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

**Conclusion up front:** the firmware's BLE HID profile is not available
to external apps, so an in-app BLE keyboard means reimplementing
HID-over-GATT (Option B, 9–11 days). The official BadUSB app already has
a working BT keyboard mode, and we can hand it a DuckyScript file the
same way we already hand `.nfc` files to the NFC app (Option A, ~2 days).

Option A cannot be made invisible: the loader runs one app at a time, so
BadUSB can only run if we exit, and it will not auto-start a script — the
operator presses Run. The launch queue does let us return to our own app
afterwards, so the handover is a round trip rather than a dead end. Four
button presses on a re-test, six the first time against a new host.

Option A is the recommendation. Option B is documented because it is the
only route to in-app execution with real completion reporting, not
because it should be built now.

## Why this is worth doing

BadUSB is the highest-fidelity delivery channel in the kit: the payload
arrives as real keystrokes in whatever field has focus, so it reaches an
assistant's chat box exactly as a user's own typing would. Its problem is
the cable. Testing a kiosk, a meeting-room display or a colleague's
laptop means physically plugging into it, which rules out most of the
scenarios where an indirect prompt injection is interesting.

The Flipper's other channels avoid the cable but lose the fidelity:

| Channel | Reaches a text field? | Capacity |
|---------|----------------------|----------|
| BadUSB | Yes, as keystrokes | 512 bytes |
| BLE beacon | No — device name only, needs a scanner app | 2079 bytes |
| QR | No — needs a camera and a human | 134 bytes |
| NFC | No — needs a tap and a reader app | ~245 bytes |

A BLE HID keyboard would be the first channel with BadUSB's fidelity and
no cable: pair once, then type into the target from across the room. For
an assistant embedded in a phone or tablet — where there is no USB port
to use at all — it is the only option of the four that actually reaches
the input field.

## The constraint that shapes everything

The firmware has a complete BLE HID keyboard implementation. **It is not
available to external applications.** Every relevant symbol is marked
disabled in the SDK's API table:

```
$ grep -E "ble_profile_hid|ble_svc_hid" api_symbols.csv
Function,-,ble_profile_hid_kb_press,_Bool,"FuriHalBleProfileBase*, uint16_t"
Function,-,ble_profile_hid_kb_release,_Bool,"FuriHalBleProfileBase*, uint16_t"
Function,-,ble_profile_hid_kb_release_all,_Bool,FuriHalBleProfileBase*
Variable,-,ble_profile_hid,const FuriHalBleProfileTemplate*,
Function,-,ble_svc_hid_start,BleServiceHid*,
Function,-,ble_svc_hid_update_input_report,_Bool,"BleServiceHid*, uint8_t, uint8_t*, uint16_t"
```

The second column is the export flag: `+` is linkable from a FAP, `-` is
firmware-internal. For comparison, the USB equivalent we already use is
`Function,+,furi_hal_hid_kb_press`.

This was verified rather than assumed. A probe that calls
`ble_profile_hid_kb_press` from live code compiles cleanly — the headers
are shipped — and fails at the packaging step:

```
APPCHK  pifk.fap
scons: *** pifk.fap: app may not be runnable. Symbols not
resolved using firmware's API: {'ble_profile_hid_kb_press',
'ble_profile_hid_kb_release', 'ble_profile_hid_kb_release_all',
'ble_profile_hid'} (in API, but disabled: {...same four...})
```

Two details worth knowing, both learned the hard way while probing:

- The failure is a **warning, not an error**, unless the strict check is
  enabled. The `.fap` is still produced and installs fine. It fails when
  the loader tries to resolve the imports — so a naive attempt looks like
  it works right up until launch.
- The symbols only reach the import table if the calling code survives
  linking. A probe whose result is unused, or whose function nothing
  calls, gets stripped by `--gc-sections` and the build passes silently.
  Any future check of this kind has to call the symbols from a path
  reachable from `pifk_app_main`.

## What is available

Enough to build the profile ourselves. These are all `+`:

```
bt_profile_start(Bt*, const FuriHalBleProfileTemplate*, FuriHalBleProfileParams)
furi_hal_bt_start_app(template, params, root_keys, callback, context)
furi_hal_bt_change_app(template, params, root_keys, callback, context)
ble_gatt_service_add(uuid_type, uuid, type, max_records, &handle)
ble_gatt_characteristic_init(svc_handle, descriptor, instance)
ble_gatt_characteristic_update(svc_handle, instance, source)
ble_gatt_characteristic_delete(svc_handle, instance)
bt_disconnect(Bt*)              bt_forget_bonded_devices(Bt*)
bt_keys_storage_*               furi_hal_bt_is_active / is_alive / get_rssi
```

`bt_profile_start` takes a *caller-supplied* template. A profile is only
three function pointers:

```c
struct FuriHalBleProfileTemplate {
    FuriHalBleProfileBase* (*start)(FuriHalBleProfileParams params);
    void (*stop)(FuriHalBleProfileBase* profile);
    void (*get_gap_config)(GapConfig* target, FuriHalBleProfileParams params);
};
```

Nothing stops a FAP defining its own and handing it to
`bt_profile_start`. The GATT layer needed to populate it — service add,
characteristic init/update — is fully exported. So the work is
reimplementing HID-over-GATT on top of public primitives, not patching
firmware.

Note `gap_init`, `gap_start_advertising` and `gap_get_state` are `-`.
We cannot drive GAP directly; we must go through `bt_profile_start` /
`furi_hal_bt_start_app` and express our GAP needs via `get_gap_config`.

## Option A (recommended): delegate to the official BadUSB app

Before writing any BLE code, note that the problem is already solved on
the device. The official BadUSB app is a FAP at
`/ext/apps/USB/bad_usb.fap` that has had a **BT keyboard mode** for
several releases — it is the same app, with a USB/BT interface toggle,
and the firmware ships the `Bluetooth_Connected_16x8` and
`Bluetooth_Idle_5x8` icons it draws for that mode. It handles the
profile, pairing, bonding and connection state that all of Option B
below is about.

It reads DuckyScript from `/ext/badusb`, and — critically — the loader
accepts a file argument, which is exactly the mechanism we already use
for NFC in `nfc_emulate.c`:

```c
loader_enqueue_launch(loader, "NFC", nfc_path, LoaderDeferredLaunchFlagGui);
```

So the whole feature becomes: write the payload as a DuckyScript file,
then hand it to BadUSB.

```c
/* src/execute/ducky_export.c */
bool pifk_ducky_write(const PifkPayload* p,
                          char* out_path, size_t out_path_len) {
    /* /ext/badusb/pifk_<safe_name>.txt */
    snprintf(out_path, out_path_len, "/ext/badusb/pifk_%s.txt", safe_name);
    /* DEFAULT_DELAY paces keystrokes; STRING types a literal line. */
    /* Body: "DEFAULT_DELAY 20\nSTRING <payload>\nENTER\n"          */
}
```

Payload text needs escaping for DuckyScript, not much: `STRING` takes the
rest of the line literally, so the only transform required is splitting
on newlines into repeated `STRING`/`ENTER` pairs. Conversations map
naturally — one `STRING`/`ENTER` per turn with `DELAY <ms>` between them,
which is precisely what `PifkConversation.delay_ms` already means.

### Why this is the better option

- **~2 days instead of 9–11.** No GATT, no profile template, no HOGP
  debugging, no five-platform pairing matrix.
- **Someone else maintains the hard part.** HID descriptors and
  per-OS pairing quirks are exactly the code you do not want to own in a
  security tool, because a subtle bug produces *wrong keystrokes* rather
  than an error.
- **It already works on stock firmware**, with no API-export gamble.
- **It is the pattern this codebase already uses.** NFC delegates the
  same way for the same reason.
- **Conversations and sequences come free**, since DuckyScript has
  `DELAY` built in.

### Can BadUSB be invoked without handing the user over?

No. Not in-process, and not silently. The loader runs **one application
at a time** — `LoaderStatus` has an explicit `LoaderStatusErrorAppStarted`
for the attempt — and BadUSB's BLE typing lives inside that app's own
process. There is no library entry point, no service, and no IPC to drive
it: the only exported handles are `loader_start*` and
`loader_enqueue_launch`, all of which mean "replace or follow the current
app".

Three consequences worth being precise about, because they bound the UX:

1. **We must exit for BadUSB to run.** Any design that keeps our UI on
   screen while BadUSB types is impossible.
2. **BadUSB does not auto-run a script.** Passing a path opens it with
   the run screen focused; the firmware's `DolphinDeedBadUsbPlayScript`
   deed is recorded when the *user* presses Run. So even after handover
   the operator presses one button. There is no exported way to
   auto-start it.
3. **`loader_signal` cannot help.** It sends a signal to the *currently
   running* app, and its meaning is app-defined. BadUSB does not document
   or export a "run now" signal, so this is not a lever.

### What we can do: a round trip

The launch queue makes the handover recoverable rather than terminal.
`loader_enqueue_launch` schedules an app to start *after the current one
exits*, so BadUSB can be told to come back to us when the operator
leaves it:

```c
/* Before we exit, queue ourselves to run after BadUSB finishes. */
FuriString* self = furi_string_alloc();
loader_get_application_launch_path(loader, self);   /* our own .fap path */
loader_enqueue_launch(loader, furi_string_get_cstr(self), NULL,
                      LoaderDeferredLaunchFlagNone);
furi_string_free(self);

/* Then hand over to BadUSB with the script preloaded. */
loader_enqueue_launch(loader, "/ext/apps/USB/bad_usb.fap", ducky_path,
                      LoaderDeferredLaunchFlagGui);
view_dispatcher_stop(app->view_dispatcher);
```

`loader_get_application_launch_path` returns the path the loader used to
start us, so the return trip needs no hardcoded install location — which
matters, since `fap_category="Tools"` puts us at
`/ext/apps/Tools/pifk.fap` today but that is not guaranteed.

Whether the queue preserves ordering for two chained entries needs
testing on hardware; `loader_clear_launch_queue` exists to reset it if a
sequence goes wrong.

We can also observe the transition. `loader_get_pubsub` publishes
`LoaderEventTypeApplicationStopped` and
`LoaderEventTypeNoMoreAppsInQueue`, so a *resident* consumer can tell
when BadUSB exited. That does not help us while we are unloaded, but it
does mean a future in-app design has a completion hook available.

### Resulting user flow

Option A, from the operator's point of view:

```
         1. Payloads -> pick payload -> Execute
                  2. Choose "BT Keyboard (via BadUSB)"
                  3. Dialog: "Script written. BadUSB will open —
                     set Interface to BT, pair, then press Run.
                     Back returns here."            [Cancel] [Open]
                       |
                       v  (we exit; queue = [BadUSB, ])
Bad USB           4. Opens with pifk_<name>.txt loaded
                  5. Interface -> BT   (once per host; bond persists)
                  6. Pair from the target's Bluetooth settings
                  7. Press Run -> payload types into the focused field
                  8. Back
                       |
                       v  (queue resumes)
         9. Returns to the payload list
```

Steps 5 and 6 are one-time per target. On a re-test against the same
host the flow is: pick payload, Open, Run, Back — four actions.

Compare the two BadUSB paths we would then have:

| | USB (in-app) | BT (via BadUSB) |
|---|---|---|
| Actions to fire | 2 | 4 (first time: 6) |
| Stays in our UI | yes | no |
| Completion reported | yes | no |
| Bridge automatable | yes | no |
| Needs a cable | yes | no |

### What it costs

- **The operator leaves our app.** Mitigated by the round trip above, but
  a context switch either way, with two extra button presses.
- **No programmatic result.** We are unloaded while typing happens, so
  the serial bridge cannot report `OK DONE <name>`. An `EXEC DUCKY`
  command can only honestly mean "script written, launch queued". This
  is the one real functional loss, and it makes Option A unsuitable if
  remote automation of BLE typing is a requirement.
- **It depends on the app being installed.** BadUSB ships in the official
  catalog, not the firmware image, so it may be absent. Check for
  `/ext/apps/USB/bad_usb.fap` and say so plainly rather than failing
  silently.
- **The payload lands on disk** in `/ext/badusb`, outside our app data
  directory, and persists after the run. It belongs in the cleanup
  checklist — a plaintext injection payload left on the SD card is a
  disclosure risk if the device is shared or lost.

### Work breakdown (Option A)

| # | Task | Est. |
|---|------|------|
| 1 | `ducky_export.c`: payload/conversation → DuckyScript, with name sanitising reused from `pifk_nfc_write_file` | 0.5d |
| 2 | Quick Deploy entry: "BT Keyboard (via BadUSB)", plus a dialog explaining the handover | 0.25d |
| 3 | Presence check for `bad_usb.fap` with a clear message when absent | 0.1d |
| 4 | Round trip: queue ourselves after BadUSB via `loader_get_application_launch_path`; verify queue ordering on hardware | 0.25d |
| 5 | Bridge `EXEC DUCKY <name>` returning the written path, documented as launch-queued not executed | 0.25d |
| 6 | Cleanup: delete generated scripts from `/ext/badusb` on request | 0.15d |
| 7 | Test on macOS + iOS + Android via BadUSB's BT mode | 0.5d |

About 2 days. Tasks 1 and 4 are the only ones with any real substance;
task 4 is the one that could surprise us, since chained queue entries are
unverified.

## Option B: write our own BLE HID stack

Worth documenting because it is the only route to an *in-app* BLE
keyboard with real completion reporting — but it is a large amount of
work for that increment, and it should not be started before Option A
has shipped and proven insufficient.

Reimplement the HID service in the app, reusing the existing keycode
table and execution structure. Four pieces:

1. **`src/execute/ble_hid_service.c`** — HID-over-GATT service built from
   `ble_gatt_service_add` / `ble_gatt_characteristic_init`.
2. **`src/execute/ble_hid_profile.c`** — a `FuriHalBleProfileTemplate`
   wrapping that service, plus `get_gap_config` setting the keyboard
   appearance and pairing mode.
3. **`src/execute/ble_kb_exec.c`** — the typing engine, mirroring
   `badusb_exec.c` but emitting HID reports over GATT notifications.
4. **UI + bridge wiring** — a Quick Deploy entry, a pairing screen, and
   `EXEC BLEKB` on the serial bridge.

### 1. HID service (GATT layer)

HID over GATT (HOGP) needs service `0x1812` with, at minimum:

| Characteristic | UUID | Properties | Notes |
|---------------|------|-----------|-------|
| Protocol Mode | `0x2A4E` | read, write-no-resp | `0x01` = report mode |
| Report Map | `0x2A4B` | read | the descriptor blob below |
| HID Information | `0x2A4A` | read | `bcdHID 0x0111`, country 0, flags |
| HID Control Point | `0x2A4C` | write-no-resp | suspend/exit-suspend |
| Input Report | `0x2A4D` | read, notify | 8-byte keyboard report |
| Output Report | `0x2A4D` | read, write | LED state; optional but expected |

The Input Report characteristic also needs a Report Reference
descriptor (`0x2908`) carrying `{report_id, type}` — `{0, 0x01}` for
input. `BleGattCharacteristicDescriptorParams` in `furi_ble/gatt.h`
covers this.

Alongside `0x1812`, hosts expect Device Information (`0x180A`) and
Battery (`0x180F`). Both are started by the firmware for any profile, so
we should not add our own.

The report map is the standard 8-byte boot keyboard descriptor —
modifier byte, one reserved byte, six keycode slots:

```c
static const uint8_t hid_report_map[] = {
    0x05, 0x01,  /* Usage Page (Generic Desktop)   */
    0x09, 0x06,  /* Usage (Keyboard)               */
    0xA1, 0x01,  /* Collection (Application)       */
    0x05, 0x07,  /*   Usage Page (Keyboard)        */
    0x19, 0xE0,  /*   Usage Min (LeftControl)      */
    0x29, 0xE7,  /*   Usage Max (Right GUI)        */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,  /*   Input (Data,Var,Abs) = mods  */
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x01,  /*   Input (Const) = reserved     */
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,  /*   Input (Data,Ary) = 6 keys    */
    0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02,  /*   Output = LEDs                */
    0x95, 0x01, 0x75, 0x03,
    0x91, 0x01,  /*   Output padding               */
    0xC0         /* End Collection                 */
};
```

Sending a keystroke is then: fill an 8-byte report, notify, fill zeros,
notify. The all-zero report is the key release — omitting it makes the
host see a held key and auto-repeat.

### 2. Profile template

```c
static FuriHalBleProfileBase* ble_kb_profile_start(FuriHalBleProfileParams p);
static void ble_kb_profile_stop(FuriHalBleProfileBase* profile);
static void ble_kb_profile_get_gap_config(GapConfig* cfg,
                                          FuriHalBleProfileParams p);

static const FuriHalBleProfileTemplate ble_kb_profile_template = {
    .start = ble_kb_profile_start,
    .stop = ble_kb_profile_stop,
    .get_gap_config = ble_kb_profile_get_gap_config,
};
```

`get_gap_config` must set:

- `adv_service.Service_UUID_16 = 0x1812` so hosts see a HID device
- `appearance_char = 0x03C1` (keyboard) — this drives the OS pairing icon
- `pairing_method = GapPairingNone`, `bonding_mode = true`
- `adv_name` — see the naming note under Risks
- `conn_param` — request a short connection interval; the default is
  tuned for low power, and typing latency depends on it

### 3. Typing engine

`hid_ascii_to_key()` in `badusb_exec.c` already maps ASCII to HID
keycodes with `KEY_MOD_LEFT_SHIFT` in the high byte. That is exactly the
encoding a BLE report needs, so it can be reused unchanged — this is the
main reason the work is tractable. Factor it into a shared
`src/execute/hid_keymap.{c,h}` used by both backends rather than
duplicating the table.

The report differs from USB in one respect: USB HID calls take the
combined keycode, whereas a BLE report needs the modifier and keycode in
separate bytes:

```c
static bool ble_kb_type_char(BleKbCtx* ctx, char c) {
    uint16_t key = hid_ascii_to_key(c);
    if(key == 0) return true;                 /* unsupported, skip */

    uint8_t report[8] = {0};
    report[0] = (key & KEY_MOD_LEFT_SHIFT) ? 0x02 : 0x00;  /* modifier */
    report[2] = (uint8_t)(key & 0xFF);                     /* keycode  */

    if(!ble_kb_notify_report(ctx, report, sizeof(report))) return false;
    furi_delay_ms(BLE_KB_KEY_DELAY_MS);

    memset(report, 0, sizeof(report));                     /* release  */
    if(!ble_kb_notify_report(ctx, report, sizeof(report))) return false;
    furi_delay_ms(BLE_KB_KEY_DELAY_MS);
    return true;
}
```

Public surface mirroring the USB backend, so callers stay uniform:

```c
bool pifk_execute_ble_kb(PifkApp* app, const PifkPayload* p);
bool pifk_execute_ble_kb_conversation(PifkApp* app,
                                          const PifkConversation* c);
void pifk_ble_kb_abort(PifkApp* app);
bool pifk_ble_kb_is_connected(PifkApp* app);
```

### 4. Timing

USB HID uses a fixed 10ms per press and release. BLE cannot: reports are
notifications, delivered once per connection interval. Sending faster
than that either queues or drops depending on stack buffering.

The connection interval is negotiated, not chosen, and the host has final
say. iOS in particular tends to settle near 15ms and ignores aggressive
requests. So the delay must be derived from the actual interval rather
than hardcoded, with a floor:

```c
#define BLE_KB_KEY_DELAY_MIN_MS 15   /* one interval at typical 15ms */
```

Budget: a 300-byte payload at 2 notifications per character and 15ms each
is roughly 9 seconds. Slower than USB's ~6s, and worth surfacing in the
UI as a progress indicator rather than a frozen screen — the sequence
scene's timer-driven pattern applies here too.

### 5. Pairing and connection state

This is the part with no USB analogue, and the main source of UX risk.
USB is plug-and-it-works; BLE requires a bonded, connected host before a
single keystroke can land. The state machine:

```
Idle ──start──> Advertising ──host connects──> Connected
                     │                             │
                     │                             ├──type──> Typing
                     │                             │            │
                     └──timeout/cancel──> Idle <───┴────────────┘
                                            ▲
                       host disconnects ─────┘
```

Requirements this imposes:

- **Never type while disconnected.** Check connection state before every
  payload and abort mid-payload if the link drops, otherwise keystrokes
  vanish silently and the operator believes the test ran.
- **A pairing screen** showing advertising status, the device name, and
  connection state. Reuse the Remote Mode polling pattern (250ms timer,
  redraw only on change) rather than inventing another.
- **Bond management.** Expose `bt_forget_bonded_devices()` in settings.
  Stale bonds are the most likely support question.
- **Restore the previous profile on exit.** `bt_profile_restore_default()`
  puts the Flipper back to its normal BLE identity; skipping it leaves
  the device advertising as a keyboard after the app closes.

## Interaction with existing features

`bt_profile_start` replaces the active BLE profile wholesale, which
collides with two things already in the app:

1. **The BLE beacon** (`ble_exec.c`) uses the Extra Beacon API. Extra
   Beacon is designed to coexist with the active profile, so they may
   work together — but this is unverified and the combination is not
   worth shipping untested. Refuse to start the keyboard while a beacon
   is running, and vice versa, until someone measures it.
2. **BadUSB** reconfigures USB, not BLE, so the two are independent. But
   "All protocols" in Quick Deploy currently runs BadUSB then starts a
   beacon; adding a third channel that seizes the BLE profile needs that
   sequence rethought.

Also worth noting: `bt_profile_start` needs the `Bt*` record
(`furi_record_open(RECORD_BT)`), which the app does not currently open.
That has to be added to `pifk_app_alloc`/`free`, and the record must
be closed on teardown or the app leaks a service reference.

## Work breakdown (Option B)

| # | Task | Est. | Risk |
|---|------|------|------|
| 1 | Extract `hid_keymap.{c,h}` from `badusb_exec.c`, no behaviour change | 0.5d | low |
| 2 | HID GATT service: report map, input/output report, HID info, control point, report-reference descriptor | 2–3d | **high** |
| 3 | Profile template + `get_gap_config` | 1d | medium |
| 4 | Typing engine with interval-derived pacing | 1d | low |
| 5 | Pairing/status scene | 1d | low |
| 6 | Quick Deploy entry + `EXEC BLEKB` bridge command | 0.5d | low |
| 7 | Cross-host testing: macOS, iOS, Windows, Android, Linux | 2d | **high** |
| 8 | Bond management in settings, profile restore on exit | 0.5d | medium |

Roughly 9–11 days. Tasks 2 and 7 carry nearly all the uncertainty.

Task 2 is the real work: HOGP is fussy, and the failure mode is a device
that pairs but never types, with no error to inspect. Budget time for
sniffing (`nRF Connect`, or `bluetoothd` logging on macOS) rather than
guessing.

Task 7 is where platform reality intrudes. Each OS has its own view of
what a HID keyboard must present before it will accept input.

## Risks

**Host-specific pairing behaviour.** The main unknown. iOS is strictest
about HOGP conformance; Windows caches a device's services on first pair,
so a malformed early version can poison the bond and need manual removal
to retest. Test on a spare account before a primary machine.

**Report map correctness.** A descriptor the host parses but interprets
differently produces wrong characters rather than no characters — worse
than an outright failure, because it looks like the payload was delivered
wrong rather than the transport being broken. Verify against a text
editor before trusting any assistant-facing result.

**Device name and consent.** A BLE keyboard advertises a name that
appears in the pairing list of every nearby device. `hid_profile.h`
supports a `device_name_prefix`, and the default firmware name is
recognisably a Flipper. Keep it identifiable — do not add impersonation
of specific vendors' peripherals. The kit is for testing systems you are
authorised to test, and a keyboard that lies about what it is undermines
the operator's ability to demonstrate that.

**Pairing is a durable change to the target.** Unlike BadUSB, which
leaves nothing behind, a bond persists on the host until removed. Any
engagement using this needs it in scope and in the cleanup checklist. The
UI should make the bond visible and removable rather than silent.

**Firmware API drift.** This plan is against API 87.1. The GATT
primitives we would depend on are `+` today; they could change. Pin the
API version in `application.fam` and re-check the export flags on SDK
bumps. If the firmware ever exports `ble_profile_hid`, most of tasks 2
and 3 should be deleted in favour of it.

## Alternatives considered

**Wait for the firmware to export the HID profile.** Zero work, and the
result is better than anything we would write. Not in our control and no
indication it is planned, but worth opening an upstream request: if it
lands, Option B collapses to a few hundred lines.

**Ship as a firmware fork / custom build.** Momentum and Xtreme may
already export these symbols. Cheap, but it fragments the audience — the
kit runs on stock firmware today, and requiring a custom build is a real
ask for a testing tool. Still worth checking what they expose, since a
working reference is worth days of GATT debugging.

**Use the exported BLE serial profile** (`ble_profile_serial_*`, all
`+`). No GATT work, works today, but it is not a keyboard: the target
needs an app that reads the serial characteristic. Fails the one
requirement that matters — reaching a text field with no cooperating
software on the host.

## Recommendation

**Do Option A.** The device already has a maintained BLE keyboard in the
official BadUSB app, and this codebase already has the delegation pattern
for exactly this situation. Roughly 2 days versus 9–11, and the part
that would consume most of that time — HID descriptors and per-OS pairing
behaviour — is the part we least want to own. A subtle bug there types the
*wrong characters*, which in a prompt-injection tool means a test result
that looks valid and is not.

Three caveats to go in with open eyes:

- **It cannot be seamless.** The loader runs one app at a time and BadUSB
  will not auto-run a script, so the operator leaves our UI and presses
  Run. The launch queue brings them back afterwards, which makes it a
  round trip rather than a dead end, but two extra presses are inherent
  to the approach. If a one-tap BLE deploy is the requirement, only
  Option B delivers it.
- **No completion signal.** We are unloaded while typing happens, so the
  serial bridge cannot report `OK DONE`. `EXEC DUCKY` can only honestly
  mean "written and queued". If remote automation of BLE typing is a
  requirement, Option A does not deliver it either.
- **It depends on a catalog app being installed.** Detect
  `/ext/apps/USB/bad_usb.fap` and say so, rather than failing quietly.

Before committing, spend an hour confirming the two hardware assumptions
in the verification notes: that a script passed to BadUSB opens rather
than auto-runs, and that two chained queue entries preserve order. Both
are load-bearing for the flow above and neither could be checked from the
SDK alone.

**Do task B1 anyway.** Extracting `hid_keymap.{c,h}` from
`badusb_exec.c` is a pure refactor that removes a duplicated keycode
table, is useful on its own, and leaves the door open.

**Revisit Option B only if** Option A ships and the lost completion
reporting proves to be a genuine blocker in practice — or if the firmware
starts exporting `ble_profile_hid`, at which point most of it is
unnecessary anyway. Treat the 9–11 day figure as optimistic until a
single character has appeared in a text editor on a real host.

## Verification notes

Everything above about API availability was checked against the installed
SDK (API 87.1) rather than inferred:

- Export flags read from
  `~/.ufbt/current/sdk_headers/f7_sdk/targets/f7/api_symbols.csv`.
- The disabled-symbol failure was reproduced by calling
  `ble_profile_hid_kb_press` from code reachable from
  `pifk_app_main`, then reading `APPCHK` output and
  `~/.ufbt/build/pifk.impsyms`.
- The `--gc-sections` stripping caveat was found the same way: two
  earlier probe attempts passed the build because the calls were
  eliminated before linking. Object-level `nm -u` on
  `_probe_ble.o` showed the symbols as `U` while the linked FAP's
  import table did not contain them.
- BadUSB's identity (`/ext/apps/USB/bad_usb.fap`, script dir
  `/ext/badusb`) and its BT-mode icons
  (`Bluetooth_Connected_16x8`, `Bluetooth_Idle_5x8`) were read from
  `~/.ufbt/current/firmware.elf`.
- The one-app-at-a-time limit is from `LoaderStatusErrorAppStarted` in
  `applications/services/loader/loader.h`; the same header documents
  `loader_enqueue_launch` as running an app *after the current one
  exits*, and `loader_signal` as app-defined.
- That BadUSB does not auto-run a passed script is inferred from the
  `DolphinDeedBadUsbPlayScript` deed, which is recorded on a user Run
  action. **Confirm on hardware before relying on the flow above** — it
  is the assumption the whole UX rests on, and it is the one thing here
  I could not verify directly, since `bad_usb.fap` ships in the app
  catalog rather than the SDK.
- Chained queue entries (BadUSB, then ourselves) are likewise
  untested — `loader_clear_launch_queue` exists as the escape hatch.

The probe was removed afterwards; the working tree is unchanged by this
research.
