# Transport-first UI

Design record for the navigation rework. Written before the code moved,
so the reasoning survives the diff.

## The problem

The app asks the operator to pick a payload, then pick a channel. That
is the wrong order, and the payload library proves it:

| Channel | Payloads it can carry |
|---------|----------------------|
| QR code | **18 / 45** |
| BadUSB | 43 / 45 (two contain non-ASCII) |
| Every other channel | 45 / 45 |

Payload sizes run 43–373 bytes against a QR limit of 134.

Channel capacity is the scarce axis, and it is fixed before the app is
opened — by what is in front of the operator. A kiosk with an exposed
port is BadUSB. A tablet behind glass is QR. An HMI with a debug header
is UART. A phone in someone's hand is NFC-URL. A SIEM ingesting device
logs is USB descriptor. **The target picks the channel; the operator
picks the payload.**

Selecting the free variable first and then testing it against the fixed
one is what produces the four rejection dialogs in
`scene_quick_deploy.c` — `Too Long for QR`, `Not typable`, `Too Long`,
`Too Long for NFC`. Those dialogs are the menu order apologising for
itself. Choose QR first and 18 payloads remain, none of them a dead
end. Choose a 373-byte payload first and QR is simply not available,
discovered one screen too late.

On a 128x64 monochrome screen there is no room for a validation layer —
no inline error text, no tooltip, no disabled-with-explanation state.
The only affordable validation is not offering the invalid option.
Filtering is cheaper than explaining, in pixels and in attention.

## The structure

Group channels by **what the operator must physically do to the
target**, because that is the question they can answer by looking:

| Group | Channels | Precondition |
|----------|----------|--------------|
| USB | BadUSB, USB descriptor | Cable into a port |
| Wireless | NFC URL, NFC text, BLE GATT, BLE beacon | Proximity |
| Screen | QR code | A camera pointed at the Flipper |
| Wires | UART TX, UART + capture, I2C write | Header access + shared GND |

Named for the operator's action, not the technology. "USB / Wireless /
Screen / Wires" is answerable on sight; "HID / RF / Optical / Serial"
requires already knowing the answer.

Four rows on a screen that shows about six, against today's thirteen-row
Quick Deploy in which `I2C scan bus` — a diagnostic, not a delivery —
sits between two delivery channels.

```
┌──────────────────────────┐   ┌──────────────────────────┐
│ PIFK            │   │ USB — cable into port    │
│ ─────────────────────────│   │ ─────────────────────────│
│ > USB          (2)       │ → │ > Type it       BadUSB   │
│   Wireless     (4)       │   │   Name it       descript.│
│   Screen       (1)       │   └──────────────────────────┘
│   Wires        (3)       │                  ↓
│ ─────────────────────────│   ┌──────────────────────────┐
│   Captures               │   │ Type it · 43 of 45 fit   │
│   Remote Mode            │   │ ─────────────────────────│
│   Settings               │   │ > * qr-ignore            │
│   About                  │   │   system-prompt-leak     │
└──────────────────────────┘   └──────────────────────────┘
```

The payload list header names the eligible count. An operator who is
not told that 27 payloads were filtered out will conclude the library
is small.

## The channel table

One table, replacing three hand-synchronised enumerations of the same
concept: `PifkProtocol` (5 values, 1 retired), the 13
`QuickDeployIndex` rows, and the bridge's `EXEC` verb strings.

```c
typedef struct {
    PifkChannelId id;     /* stable, persisted; never renumber */
    PifkChannelGroup group;
    const char* label;        /* "Type it" */
    const char* sublabel;     /* "BadUSB" */
    const char* verb;         /* "BADUSB" — the bridge's EXEC verb */
    size_t max_bytes;         /* 134 for QR */
    bool ascii_only;          /* BadUSB, USB descriptor */
    bool persists_after_exec; /* stopped on scene exit */
} PifkChannel;

bool pifk_channel_accepts(const PifkChannel* ch, const PifkPayload* p);
```

Capacity limits currently live in five headers
(`PIFK_QR_MAX_BYTES`, `PIFK_NFC_MAX_TEXT`,
`PIFK_GATT_TOTAL_BYTES`, `PIFK_USBDESC_TOTAL_CHARS`,
`PIFK_MAX_TEXT_LEN`). Those stay where they are — each belongs next
to the code enforcing it — and the table references them, so the list
filter and the executor cannot disagree about what fits.

`persists_after_exec` replaces the current arrangement, where knowing
which channels keep running after the callback returns means reading a
comment and cross-checking four `pifk_*_stop()` calls in
`on_exit`.

## Payload selection is one widget, not three

Payload lists exist in three places today, each with its own copy of
the list-and-index logic: `scene_payload_list.c`,
`scene_favorites.c`, and the favorites picker in `scene_settings.c`.
Filtering has to apply to all of them, so the list builder becomes
shared and takes an optional channel to filter against.

## Migration of persisted state

Two on-disk formats encode the old structure:

- `settings.json` stores `default_protocol` as a `PifkProtocol`
  value. New `PifkChannelId` values are assigned so the four live
  `PifkProtocol` values keep their numbers; slot 3 stays retired.
- `scene_manager_set_scene_state` persists menu indices per scene. The
  main menu's `MainMenuIndexCaptures = 5` gap exists for exactly this
  reason. Regrouping changes the main menu's rows, so its saved state
  is reset once rather than silently landing on a different row —
  a one-time cost, taken deliberately.

Channel IDs are decoupled from display order, so the groups can be
reordered later without touching a saved setting.

## What is removed

- **"All protocols"** — fires BadUSB, a BLE beacon and an NFC file
  write together, reporting `BadUSB: Done / NFC: file written / BLE:
  broadcasting`: three different meanings of success in one dialog,
  none of them confirmation the payload landed. The three channels also
  have mutually exclusive physical preconditions, so under the group
  structure the entry cannot be placed anywhere honest.
- **The four rejection dialogs** — superseded by filtering.
- **`Add Favorite` / `Remove Favorite` submenu rows** — replaced by
  long-press OK on any payload list, via `submenu_add_item_ex`.
  Verified against the firmware: extended callbacks receive every
  `InputType` on OK, so the handler must switch on `InputTypeShort` and
  `InputTypeLong` explicitly and ignore the rest, or one press fires
  the action several times.
- **`Favorites` as a top-level row** — starred payloads sort to the top
  of every payload list with a `*` prefix, which is where they are
  useful. The scene's empty state, three dead submenu rows with `NULL`
  callbacks reading as broken menu items, goes with it.

## What is kept

- **The splash screen.** branding on a tool used in front of
  clients. Stock apps do not have one; this app has a reason to.
- **A payload-first path.** There is one genuine payload-first
  workflow: *this payload got a hit, try it everywhere else.* The
  payload viewer keeps a `Deploy via...` action listing the channels
  that accept that payload — the same table queried the other way.
  Transport-first is the default, not the only route.
- **`Payloads (45)`** as a browsable, unfiltered library. Reading the
  set to learn what prompt injection looks like is a legitimate use of
  this kit as a teaching tool, and it is payload-first by nature.
- **Diagnostics, regrouped.** `I2C scan bus` (the only entry that
  ignores the selected payload) and the GPIO loopback test currently
  sit in Quick Deploy and Settings respectively. Both answer *is the
  wire good before I blame the payload?* They belong together, under
  Wires, as pre-flight checks.
