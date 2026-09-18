# Changelog

## [Unreleased]

### Changed
- **Renamed to Prompt Injection Field Kit and de-branded for open
  source.** The app is `Prompt Injection Field Kit` with `appid` `pifk`;
  the `Mindgard*` / `mindgard_` / `MINDGARD_` identifier prefixes are now
  `Pifk*` / `pifk_` / `PIFK_`. `src/mindgard_app.{c,h}` and
  `src/gui/mindgard_menu.{c,h}` became `pifk_app.*` and `pifk_menu.*`.
  The GitHub URL and the Apache-2.0 copyright notice are unchanged —
  the copyright is an authorship statement, not branding.

  **This moves the SD-card data directory.** `APP_DATA_PATH()` resolves
  against `appid`, so payloads, favourites, settings and captures now
  live in `/ext/apps_data/pifk/` rather than `/ext/apps_data/mindgard/`.
  An existing install will come up with its defaults and no captures
  until the old directory's contents are copied across; nothing is
  deleted, just no longer found.

  BLE now advertises as `PIFK` rather than `Mindgard-PIFK`, so anything
  matching on the old advertised name — including
  `tools/ble_gatt_probe.py` — needs updating.

- **Removed the splash screen.** It existed to show the logo and the
  tagline, and the app now boots straight to the main menu.
  `scene_splash.c`, `PifkSceneSplash` and `PifkViewSplash` are gone; the
  version moved to being available in About only. The branded
  `mindgard_logo.png` and `mg_pifk_flipper_splash.png` are deleted and
  the 10x10 launcher icon is redrawn as an unbranded speech bubble.

  Historical entries below still name a few `mindgard_*` functions,
  which is what the code was called at the time.

### Added
- **Nine payloads covering four mechanisms the library had no
  representation of.** 45 → 54 builtins, 17 → 21 categories. Two of the
  three best-evidenced shapes in the published literature were entirely
  absent, while the one shape that measures 0.00 everywhere shipped as a
  headline category.

  `structural` (`structural-verdict`, `structural-endlog`, plus
  `qr-structural` at QR size) forges the pipeline's own formatting so the
  model reads a boundary and verdict it believes it emitted. Published at
  96% injected-summary rate — the highest of any mechanism measured.

  `human-directed` (`human-oncall`, `human-compliance-omit`,
  `human-change-window`) phrases the payload as a note to the human
  recipient and never names AI, an assistant or a model. This is the only
  mechanism here with a production CVE behind it: EchoLeak
  (CVE-2025-32711, CVSS 9.3) bypassed Microsoft's purpose-built
  cross-prompt-injection classifier this way.

  `authority` (`authority-soc-admin`, `qr-authority`) and
  `authority-shed` (`authority-shed-kv`) are deliberate opposites. Forged
  role markers work on targets that trust in-context authority and fail on
  targets that classify authority claims as social engineering — which is
  exactly where the shed variant, stripped of every role marker and policy
  word, lands. The two are anti-correlated across targets and a blind
  channel gets no feedback to choose between them, so both ship.

  **None of these reproduced here.** All four measured 0/110 against
  `claude-haiku-4-5` in a real asset pipeline, including the 96% shape.
  They are added because they are the best-evidenced shapes to *test*, not
  because they are known to work, and `docs/MANUAL.md` says so at the top
  of the payload-library section. `instruction-override` is retained and
  now documented as the control it functions as: 0.00 at every defence
  mode is the baseline that makes the other numbers mean anything.

  `structural-verdict` is 20 bytes and states no instruction at all, which
  makes it the only 96%-ISR shape that fits a 32-byte SSID. With
  `qr-structural` (15 B) and `qr-authority` (12 B), QR eligibility goes
  from 18 of 45 to 27 of 54; every wider channel carries all 54.

  Slots are now 54 of `PIFK_MAX_PAYLOADS` = 56. The database is a
  fixed array at 224 bytes per entry, so the cap is ~12.5 KB of RAM
  whether or not the slots are used — raising it is not free.

- **`tools/payload_parity.py`, guarding the two shipped copies of the
  library against drift.** The builtins ship twice — compiled into
  `src/payload/builtin_payloads.c` so the app works with no SD card, and
  as `payloads/pifk_payloads.json` for the host to sync — both edited
  by hand, with nothing comparing them. A JSON-only edit is invisible
  until someone runs the app on a fresh device.

  Checks name-set parity in both directions, then text/category/
  description for every shared name, then that each field fits its fixed C
  buffer with room for the NUL, then the payload cap. C escapes are
  decoded before comparing, so a `"\n"` in a C literal and a real newline
  in the parsed JSON compare equal — comparing source text instead would
  flag every multi-line payload as drift, and a guard that cries wolf gets
  turned off. Verified against seven deliberate mutations, and run against
  the pre-existing tree first: the two copies already agreed at 45
  entries, so this guards the future rather than fixing a shipped bug.

- **I2C payload channel, with a bus scan.** Reaches targets the UART
  channel cannot: an EEPROM holding configuration text, a display
  controller, a sensor's register space — embedded hardware with no serial
  console at all. `Quick Deploy > I2C scan bus` probes 0x08–0x77 and lists
  what answers; `I2C write payload` writes to the address in
  `Settings > I2C Address`. Over the bridge as `SCAN I2C` and
  `EXEC I2C`.

  The scan is not a convenience. Writing to a guessed I2C address is
  destructive in a way a UART write is not — an unexpected device might be
  a PMIC, and payload text in its control registers is a bricked board —
  so the write path probes the address and refuses if nothing ACKs.

  Payloads go out in 16-byte transactions rather than one long write,
  because I2C is transactional and an unknown device's receive buffer is
  by definition unknown; a 512-byte single write would overrun most real
  parts. `app->executing` is checked between chunks so Stop interrupts a
  transfer, and a partial write reports how many bytes landed — an I2C
  write cannot be rolled back.

  **Master only**, and documented as such: `furi_hal_i2c_tx()` addresses a
  slave and the exported HAL has no slave mode, so the kit writes into a
  device that behaves as a slave and cannot impersonate a sensor that a
  target polls. Pin numbers (PC0/PC1) resolve at runtime via
  `furi_hal_resources_get_ext_pin_number()`, as the UART channel's do.

  `tests/test_i2c_addr.c`: 230 checks over the 7-bit-to-wire address
  shift across the whole valid range, the reserved-range exclusions, and
  chunk arithmetic for every payload length from 1 to 512. The shift is
  pinned against the 24Cxx datasheet convention (0x50 → 0xA0), because a
  wrong shift addresses the device one position away from the one the
  operator named.

- **`tools/uart_edge_decode.py` and `tools/gpio_baud_sweep.py`.** The
  first decodes a UART line from a logic analyser's raw digital export:
  edge timestamps, so it can measure the baud rate rather than assume it.
  The analyser's own Async Serial decoder is *told* the baud and can only
  report whether bytes decoded at the rate you claimed — the same
  circularity `gpio-validation.md` identifies in the loopback self-test,
  one level up.

  The second sweeps every rate in the `Settings > GPIO Baud` picker.
  Only 115200, the compiled default, has ever been measured; the STM32WB55
  USART divides a 64 MHz clock, so the divisor coarsens as the rate rises
  and 921600 needs one near 69. It reports a per-rate error and a verdict
  against a receiver's tolerance, and exits non-zero only for rates
  genuinely out of spec.

- **Captures: the kit now records what came back, not just what went
  out.** Every GPIO capture is appended to `captures.jsonl` in the app's
  data directory, and a new `Main Menu > Captures` screen reads them back
  newest first, showing the full reply for a selected record.

  This is the piece the removed results viewer was missing. That viewer
  read a `results.json` only the host tooling ever wrote, so the Flipper
  rendered a summary it did not produce; the dependency now runs the
  other way — the device is the producer and the host reads the file.
  Captures driven over the bridge (`EXEC GPIOCAP`) write identical
  records, so scripting a sweep still leaves evidence on the device.

  JSON Lines rather than a JSON array, because appending is then one
  open-write-close with no read of prior content and no in-RAM copy of
  the log: a half-written final line costs one record instead of the
  file. A captured reply is arbitrary bytes off a UART, so bytes ≥ 0x80
  are escaped as well as the usual control characters — emitting a raw
  0x80 would produce a file host JSON parsers reject, which is the one
  moment the evidence has to be readable. `write_json_escaped()` in
  `payload_db.c` is now shared rather than duplicated, and deliberately
  left passing ≥ 0x80 through: its own inputs are valid UTF-8 that must
  round-trip byte-identically for the homoglyph payloads.

  The log is capped at 32 KB and rotates once to `captures.jsonl.1`.
  `ts` is `furi_get_tick()`, reported as seconds since boot: the Flipper
  has no reliably synchronised RTC, and a confidently wrong timestamp in
  report evidence is worse than an obviously relative one.

  Memory was the constraint, since removing the previous attempt at this
  reclaimed ~10 KB of heap on a device reporting ~37 KB free. Nothing is
  cached in `PifkApp`: the viewer holds eight one-line summaries in
  `.bss` and re-reads the selected record from the file for its body, so
  only one reply is ever in memory. `.bss` grew 664 bytes rather than the
  ~4.8 KB holding eight full records would have cost. The FAP is 9,684
  bytes larger, which roughly cancels the 9,492 that removal saved —
  worth stating plainly, since the flash is spent where the heap is not.

  `PifkSceneCaptures` is appended to the scene enum, so the three
  `_Static_assert`s on the handler tables cover it. The main menu reuses
  index 5, which held the results viewer: the index is persisted as scene
  state, so a restored selection lands on the row that means roughly the
  same thing.

- **Seven concealment payloads**, taking the built-in set from 38 to 45
  and filling the families the previous concealment work left open:
  `html-comment`, `css-invisible` and `aria-hidden` (a new `concealment`
  category) hide a directive in markup that renders as nothing;
  `meta-ai-namespace` and `systag-impersonate` (`impersonation`) borrow
  the authority of a metadata standard and a chat template; `url-encode`
  and `nested-encode` (`encoding`) add percent-encoding and base64
  wrapping URL-encoding, so a single-pass decoder still sees ciphertext.

  These target an agent reading a page or document rather than a chat
  box: invisible in the rendered view, plain text in the source the model
  consumes. All seven are ASCII, so unlike `homoglyph-override` and
  `zero-width-split` they type intact over BadUSB — asserted in the tests
  rather than left as a claim.

  `PIFK_MAX_PAYLOADS` rises 48 → 56. At 45 builtins the old cap left
  three slots for the user's own `payloads.json`; `PayloadDb` is a fixed
  inline array, so this is ~1.7 KB of heap per 8 slots whether or not
  they are used.

- **`payloads/pifk_payloads.json` synced to the builtin set.** It
  held only the original 17 payloads and was missing every `qr-*`,
  concealment and encoding entry added since. Regenerated through the
  same escaping the device's own `payload_db_export_all()` uses, so the
  committed file matches what the Flipper writes on first run.

- **`tests/test_capture_log.c`.** 60 checks over JSON escaping of quotes,
  control bytes and bytes ≥ 0x80; replies containing NULs; newest-first
  indexing; a truncated final line and hand-edited junk being skipped
  without consuming an index; and `rx` over-claiming being clamped.
  Passes under ASan and UBSan. Verified by mutation: removing the ≥ 0x80
  escaping, substituting `strlen` for the byte count, and making a
  malformed line abort the walk each fail the suite.

- **NFC emulation as a URI record.** Quick Deploy gains "NFC emulate
  (URL)" and the bridge gains `EXEC NFCEMUURL`. iOS reads a Text record
  and silently discards it — a banner is only raised for a URI record —
  so the text-only channel appeared broken against a stock phone when it
  was in fact being read end to end. Confirmed from the device's own
  trace: a full `CMD_READ` page sweep for both record types. The payload
  is percent-encoded into an `example.com` query string, a domain RFC
  2606 reserves so a stray tap cannot reach a live host.

- **`tools/ble_gatt_probe.py`.** The client half of the BLE GATT
  channel, so it can be verified without a target. Discovers by
  advertised service UUID rather than by name — matching on the name
  would pass even if the advertisement omitted the service, which is
  exactly what a real client cannot do — reads the characteristics,
  reassembles them, and compares byte for byte against `--expect-file`.

- **`tests/test_nfc_ndef.c`.** 78 checks over BCC derivation, the
  capability container, NDEF round-trips across the short/long TLV and
  record boundaries, URI prefix folding and percent-encoding.

- **GPIO / UART payload egress.** Emits payload bytes on the UART TX pin
  and leaves the receiving end to whatever the user has wired up, which
  is what makes it reach targets the kit cannot anticipate. Configurable
  baud (9600–921600), port (USART or LPUART), line ending and per-byte
  pacing; available in Quick Deploy and over the bridge as `EXEC GPIO`.

  Two things it does better than the existing channels: pacing checks
  `app->executing` between bytes so **Stop works mid-payload**, which
  BadUSB cannot do; and the loopback self-test sends a known string over
  a TX-RX jumper and compares it back, so the transport can be proven
  before the user's hardware is involved. Header pin numbers are resolved
  at runtime via `furi_hal_resources_get_ext_pin_number()` rather than
  hardcoded, so the on-device wiring hint cannot drift from the firmware.

  `furi_hal_serial_control_acquire()` returns NULL when the CLI or
  logging holds the port rather than clobbering it, so contention is
  reported with a suggestion to switch to LPUART instead of a bare
  failure. Every exit path releases the handle — leaking it would leave
  the CLI dead until reboot.
- **GPIO response capture.** "GPIO + capture reply" sends a payload then
  listens on RX for a configurable window, retaining up to 512 bytes of
  whatever the target says back. This makes the kit evidence-producing
  rather than write-only: for a serial-console target, a leaked system
  prompt read off the wire is the finding, where every other channel can
  only confirm delivery. A silent target is reported as a result, not an
  error, and the message names the likely causes (target said nothing vs
  RX not wired) rather than leaving the user guessing. Over the bridge as
  `EXEC GPIOCAP`. The RX callback runs in interrupt context so it does
  nothing but stash the byte in a fixed buffer.
- **BLE GATT readable payload surface.** Publishes the payload from
  readable GATT characteristics (255 bytes each, three of them, 765
  total) instead of broadcasting 29 bytes at a time through an
  advertisement name. A client connects and reads it whole, with no
  chunking, no reassembly and no pairing. Quick Deploy entry plus
  `EXEC BLEGATT` / `STOP BLEGATT` on the bridge.

  This is a custom `FuriHalBleProfileTemplate` — the firmware withholds
  its HID profile from external apps but accepts a caller-supplied
  template, and `ble_gatt_service_add()` /
  `ble_gatt_characteristic_init()` are exported. Confirmed the nine
  required symbols appear in the FAP import table with no "disabled"
  warning from APPCHK, which is where an attempt to use
  `ble_profile_hid` fails.

  Characteristics are deleted before the service that holds them, or
  their handles leak in the BLE stack's attribute table until reboot.
  Starting GATT stops the Extra Beacon, since `bt_profile_start()`
  replaces whatever profile is active; the default profile is restored on
  scene exit and in `mindgard_app_free()`.
- **In-process NFC tag emulation.** Emulates an NTAG215 carrying the
  payload as an NDEF Text record, so the tag is presented while the kit
  stays on screen. The existing NFC path writes a `.nfc` file and hands
  over to the stock NFC app via `loader_enqueue_launch()`, which costs
  the operator their place and gives no completion signal. Quick Deploy
  entry plus `EXEC NFCEMU` / `STOP NFCEMU`.

  NDEF is the one wireless format a phone reads with no app and no
  pairing, which makes this the only channel here that reaches a
  general-purpose consumer device with zero setup on the target. Capacity
  is 493 bytes (NTAG215's 504-byte user memory less record overhead);
  oversized payloads are refused rather than truncated.

  The GET_VERSION response and the page-3 capability container are filled
  in with real NTAG215 values — a reader that asks and gets nonsense may
  refuse to read user memory at all. Teardown stops the listener before
  freeing anything it holds a pointer to, since it runs on the NFC worker
  thread. The emulated UID is the kit's own rather than a clone of an
  existing card.
- **USB descriptor injection channel.** The payload is carried in the USB
  manufacturer, product and serial strings, so a host records it during
  enumeration — before any driver loads and without the target focusing a
  field, scanning a code, tapping a tag or pairing anything. Windows logs
  it in EID 6416 and `setupapi.dev.log`, Linux via udev/dmesg, macOS in
  unified logging; the interesting consumer is whatever reads those logs
  afterwards. Available in Quick Deploy and over the bridge as
  `EXEC USBDESC` / `STOP USBDESC`.

  The Flipper enumerates as a well-formed CDC device that merely has an
  unusual name: the CDC class implementation and configuration descriptor
  are reused unchanged, since a malformed descriptor gets the device
  rejected before the host reads any strings. Capacity is 126 characters
  per string (`bLength` is a `uint8_t` covering a 2-byte header plus 2
  bytes per UTF-16 character, so 127 would overflow), giving 378 across
  the three. ASCII only — bytes >= 0x80 become `?` and the count is
  reported rather than the payload silently arriving mangled. The
  previous USB configuration is restored on scene exit and in
  `mindgard_app_free()`.
- 13 concealment and filter-evasion payloads, taking the built-in set from
  25 to 38. Four `encoding` (base64, ROT13, hex, HTML entities), four
  `obfuscation` (Cyrillic homoglyphs, zero-width spaces, character
  spacing, JSON structural breakout), one `multilingual` (EN/FR/DE/ES),
  two `memory-poison` (session persistence, deferred keyword trigger),
  and two `usbdesc` sized for the 126-character USB string descriptor
  limit. These test whether a target's filter catches a disguised
  instruction, rather than whether the target follows a plain one.
- `tests/test_usb_descriptor.c`: 16 assertions over descriptor splitting,
  the 126-character boundary, three-string overflow and non-ASCII
  substitution. Passes under ASan and UBSan.

### Fixed
- **`docs/MANUAL.md` claimed a 48-payload cap and 10 free slots.** The cap
  rose to 56 some time ago and the text was never updated; at 54 builtins
  there are 2. The same passage implied deleting entries from
  `payloads.json` frees slots. It does not — `payload_db_load_builtins()`
  runs first and unconditionally, and the file loader skips any name that
  already exists, so the JSON can add payloads but never remove or replace
  a builtin. Corrected, with the actual route (raise the cap and rebuild)
  and its RAM cost named.

- **`docs/gpio-validation.md`: two numbers that the committed captures do
  not support.** Re-deriving them with the newly committed decoder found
  both.

  The burst capture was taken at **1 MS/s**, not the 10 MS/s the document
  recommends: the GCD of its timestamps is 1 µs and every narrow pulse
  measures exactly 8.000 µs, 400 times, with no spread. Real 115200 bit
  cells are 8.68 µs and jitter. So a single-bit measurement of that file
  reads 125,000 baud — an 8.5% "error" that is entirely sample-grid
  artifact. Measured across whole 33-byte bursts the figure is
  **115,108 (−0.08%)**, against the document's original 115,465 (+0.23%).
  The two differ by one bit in the assumed count over the span; the tool
  now measures start-bit to start-bit across detected frames, which is an
  exact multiple of the frame length and independent of the bytes sent.
  The original figure was not wrong so much as more precise than a
  1 MS/s capture can support.

  The paced capture is **not a paced payload send**. It decodes to Flipper
  CLI log output — `[W][ViewPort] ViewPort lockup: ...` — at 230400 baud,
  not `encoding-bypass` at 115200. It caught the firmware's own debug
  channel on the USART. Test 2's pacing conclusions may well be correct,
  since they were read off a live capture at the time, but the evidence is
  not in the repository and the claim that these files let the numbers be
  "checked without hardware" holds only for the burst test.

  The status header, the "what is now proven" table and the reproduction
  steps are corrected accordingly. Ready-to-run procedures are added for
  the three outstanding tests: the RX overflow branch (which sets the
  `trunc` flag the capture log reports, and has never executed on
  hardware), RX baud accuracy against an independent clock, and the baud
  sweep.

### Removed
- **Sequences and the results viewer.** Neither earned its place.
  Sequences chained steps on a timer, but the one interesting case — NFC
  — fell back to BadUSB because it hands off to another app, and Stop
  could not interrupt a BadUSB step mid-type anyway. The results viewer
  read a `results.json` only the host tooling wrote, capped at 8
  vulnerabilities, and showed a summary the host presents better.

  905 net lines, 9.4 KB off the FAP, and ~10 KB of heap no longer
  allocated at startup — `SequenceDb` alone was ~8.2 KB, on a device
  reporting ~37 KB free. Two enums keep gaps rather than renumbering,
  since both are persisted: `PifkProtocol` in `settings.json` and the
  main menu index in scene state.

- **Sub-GHz.** It was never an execution channel: the Quick Deploy entry
  printed a note about where to put `.sub` files, and `EXEC SUBGHZ` over
  the bridge existed only to return an error. Gone from the menu, the
  bridge, the sequence parser, the settings picker, the manual and the
  unused `subghz.png` asset.

  `PifkProtocol` slot 3 is kept as `PifkProtocolReserved3` rather
  than reclaimed. The enum's numeric values are persisted in
  `settings.json` and in sequence files, so renumbering would make every
  existing file that says `gpio` mean something else. An old sequence
  step naming `"subghz"` no longer matches any protocol string and falls
  back to BadUSB, which is effectively what it did before; a stale
  `default_protocol=3` resolves to BadUSB in the picker rather than
  showing an empty value. Both verified with a host-side model.

  The Default Protocol picker now maps its own contiguous indices onto
  real enum values instead of iterating the enum, so scrolling cannot
  land on the retired slot.
- Eight functions with no call sites. Four were introduced by the new
  channels (`mindgard_gpio_abort`, `mindgard_usb_descriptor_was_read`,
  `mindgard_ble_gatt_active`, `mindgard_nfc_listener_reads` plus
  `mindgard_nfc_listener_active`) and four predate them
  (`payload_db_save_to_file`, `convo_db_save_to_file`,
  `sequence_db_find`). Speculative API is API that has never been
  exercised; the two "has it been read yet" helpers would need a polling
  timer in Quick Deploy to be useful and can come back with it.
  Removing the save-to-file pair also collapsed the `user_only` branch
  out of the JSON writers, since every remaining caller exports
  everything.

### Fixed
- **BLE GATT never advertised, and then served the wrong bytes.** Three
  separate faults, all invisible from the device side, which is why they
  survived until the channel was tested against a real client.

  `GapConfig.mac_address` was left zeroed, and an all-zero BLE address is
  invalid, so the controller accepted the profile and never advertised
  while `bt_profile_start()` reported success. `adv_name` was written
  from byte 0, but it is an AD structure whose first byte is the type
  (`gap.c` reads the name from `adv_name + 1`), so the kit advertised as
  "indgard-PIFK". And a 255-byte characteristic returned the correct
  length with stale contents, because the read spills past a single ATT
  exchange; values are capped at 244, which is ATT_MTU minus the 3-byte
  read header for the MTU macOS negotiates. Capacity 765 -> 732 bytes.

  Verified end to end: 373 bytes reassembled byte-identical across two
  characteristics, no pairing prompt.

- **`.nfc` files were written in the wrong format.** The writer emitted
  raw NDEF bytes where a `.nfc` file is a Flipper Format text document,
  so the stock NFC app reported "cannot load key file" and this path had
  never worked. Built via `nfc_device_save()` now, so the format is
  correct by construction.

- **Emulated tags were rejected by every reader.** The UID was written
  only to the ISO14443-3A layer, leaving pages 0-2 zeroed; a reader
  recomputes the BCC check bytes from page 0 and drops the tag on a
  mismatch, so a phone ignored it with no error and no partial read.
  Uses `mf_ultralight_set_uid()`, which writes those pages and both BCC
  bytes. Also zeroes the struct — `mf_ultralight_alloc()` is a bare
  `malloc()` — and corrects the capability container from `0x3F` to the
  `0x3E` a real NTAG215 reports, which drops the advertised capacity
  from an unachievable 493 bytes to 481.

- **NDEF text over 248 characters was silently truncated.** The record
  and TLV length fields were cast into single bytes with no range check,
  so 256 wrapped to 0 and produced a tag that read as an empty record.
  Both long forms are now emitted.

- **BLE reported success with the radio switched off.** Neither the
  beacon nor GATT checked `furi_hal_bt_is_active()`, and
  `bt_profile_start()` succeeds regardless, so the kit reported
  `OK SERVING` while nothing reached the air — the worst possible
  outcome, since an operator concludes the target ignored the payload.
  Both channels now refuse, and Quick Deploy names Bluetooth being off
  as the cause.

- Quick Deploy's header comment listed 8 of its 11 menu entries and
  described only two of the four channels that keep running after the
  callback returns.
- Sequence steps now execute using the protocol they declare. Every step
  ran over BadUSB regardless of the `"protocol"` field in the sequence
  file, so a step asking for BLE or GPIO silently typed over USB HID
  instead. GPIO and BLE now dispatch correctly; NFC and Sub-GHz still
  fall back to BadUSB because neither has a non-interactive executor
  (NFC hands over to another app, Sub-GHz is guidance only).
- Added a `_Static_assert` tying `protocol_labels[]` in the settings
  scene to `PifkProtocolCount`. Adding a protocol without extending
  that array would have indexed out of bounds.
- `mindgard_badusb_count_unmappable()` and warnings in the payload viewer
  and Quick Deploy. `hid_ascii_to_key()` has no keycode for non-ASCII
  and `type_char()` skips those silently, so the homoglyph and
  zero-width payloads would have arrived incomplete over BadUSB while
  looking correct on screen. Both surfaces now name the dropped
  character count and point at QR, NFC or GPIO instead.

## [1.1.0] — 2026-08-25

### Fixed
- Serial bridge no longer mutates the payload database or the GUI from its
  own thread. Commands that execute, load or reload are handed to the main
  thread and the databases are guarded by a mutex. This removes a
  use-after-free reachable by sending `RELOAD` while a payload was on screen.
- Out-of-bounds heap read in the JSON key scanner when parsing a string with
  no closing quote (malformed `payloads.json` / `conversations.json` /
  `sequences/*.json`).
- JSON key scanner now honours `\"` escapes; a payload whose text contained an
  escaped quote made every later key in that object unfindable.
- Files of exactly 65536 bytes read as empty, and larger reads could truncate:
  the read length was cast to `uint16_t`. Reads now loop and the size cap sits
  below the `uint16_t` limit.
- `RELOAD` left stale ownership flags and dangling pointers in database slots
  above the new count, risking a double free.
- Integer overflow (undefined behaviour) when parsing long digit runs in JSON;
  values now saturate.
- Delays loaded from `settings.json`, conversations and sequences are clamped.
  An unbounded value produced an uninterruptible multi-day `furi_delay_ms()`.
- Allocation failures are checked in the BLE, bridge and database paths.
- BLE refuses payloads that exceed the chunked-advertisement capacity instead
  of silently broadcasting a truncated prompt.
- `LIST` / `LIST CONVOS` now report truncation instead of emitting a malformed
  JSON array. The write cursor used `snprintf`'s return value without clamping,
  so on overflow it advanced past the end of the buffer.
- `LOAD` rejects duplicate payload names, which previously consumed slots with
  unreachable entries.
- NFC filenames are built from an allowlist, so payload names can no longer
  inject path separators or `..`.
- NDEF builders no longer underflow their truncation arithmetic, and the output
  file is only reported as written when the write succeeded.
- Results Viewer now loads `results.json` and its NFC export writes a real
  NDEF URI record rather than reporting success unconditionally.
- Quick Deploy explains the QR size limit up front instead of failing silently;
  the bridge reports the limit the encoder actually enforces (134 bytes).
- QR view Back now unwinds through the scene manager, keeping the scene stack
  in step with the visible view.
- Version string is defined once and shared by the splash and About screens.
- `STOP` and `STOP BLE` replied with a trailing space (`"OK "`) because the
  reply builder always appended a verb.
- Corrected comments and docs that described behaviour the code does not have:
  the serial protocol's response list, a Quick Deploy menu missing its QR
  entry, sequence-view pause/skip controls and a progress bar that were never
  implemented, a `[Details]` button on the results screen, a "dolphin" on the
  splash screen, and `badusb_delay_ms` labelled as an inter-keystroke delay
  when it is a one-shot delay before typing starts.
- Dropped two unused NFC includes.

### Changed
- Sequences advance one step per timer tick instead of looping inline in the
  dialog callback. The event loop stays free between steps, so Stop works and
  the screen shows which step is running, which ones are done, and where a run
  stopped. Inter-step delays are timer periods rather than blocking
  `furi_delay_ms()` calls, and Run is hidden mid-run so a sequence cannot be
  started twice.
- Remote Mode polls the bridge state every 250ms, so the status line tracks
  the host connecting instead of showing whatever was true on entry. The text
  is only rebuilt when the state or record counts actually change.
- Both new timers live on the app struct and are disarmed in
  `mindgard_app_free()`. Scene `on_exit` handlers do not run during teardown,
  so a timer left armed there would fire into freed memory.
- Payload and conversation writers share one implementation each, and every
  string field written to disk is JSON-escaped.
- `tests/test_qr.c` is a real assertion-based suite that exits non-zero on
  failure; it is no longer compiled into the FAP.
- Added `.gitignore`.

### Removed
- `src/ui/` (`menu_helpers.c`, `status_bar.c`). Neither file had any caller or
  declaring header, but both were still compiled into the FAP.

### Known limitations
- Stop cannot interrupt a payload mid-type. A single BadUSB step blocks while
  it types, so Stop takes effect at the next step boundary, or between turns
  of a conversation. Interrupting mid-payload needs an abort check threaded
  through type_string().
- Results are capped at 8 vulnerabilities; larger reports display a subset.

## [1.0.0] — 2026-02-27

### Added
- **Payload Manager**: Browse, search, and execute 25 built-in prompt injection payloads
- **Conversation Engine**: 6 multi-turn conversation scripts with configurable delays
- **BadUSB Execution**: Type payloads via USB HID keyboard injection with Enter submit
- **Quick Deploy**: One-tap protocol selection (BadUSB, NFC, BLE, Sub-GHz)
- **Favorites**: Star frequently-used payloads for fast access
- **SD Card Loading**: Load custom payloads and conversations from JSON files
- **Serial Bridge**: Remote control from a host shell over USB CDC serial
  - Line-based text protocol (PING, LIST, EXEC, STATUS, LOAD, SET)
  - Live payload push without SD card access
  - Configurable keystroke and conversation delays
- **Attack Sequencer**: Multi-step, multi-protocol attack sequence execution
- **Results Viewer**: Display security scan results on-device
- **NFC Export**: Generate NDEF URL records for sharing scan report links
- **Settings**: Configurable BadUSB delay, conversation delay, default protocol
- **About Screen**: Version info, payload/conversation counts
