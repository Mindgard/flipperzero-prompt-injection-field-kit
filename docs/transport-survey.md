# Which other transports are worth adding?

Status: acted on. NFC listener emulation is **implemented** (8b288cf);
the recommendation against adding I2C/SPI/bit-bang still stands.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

**Short answer: no new GPIO-side protocols beyond UART. But the survey
turned up one thing more valuable than any of them — in-process NFC tag
emulation is fully exported, which upgrades a channel the kit already
has from "writes a file and hands off" to "actually presents a tag".
That is the recommendation.**

## The test being applied

A transport earns a place in this kit only if it can plausibly put
attacker-controlled *text* into an LLM's context. Two questions, both
of which must pass:

1. **Can the Flipper emit enough bytes?** (capacity)
2. **Is there anything downstream that turns those bytes into text an
   assistant reads?** (semantics)

Question 2 is the one that kills most candidates. It is what ruled out
IR — 32 bits per message, and no receiver anywhere treats an IR frame as
a string. "The Flipper has this peripheral" is not an argument for
shipping it.

## Verdict table

Everything exported to FAPs, assessed against both questions:

| Transport | Capacity | Reaches text? | Verdict |
|---|---|---|---|
| **UART** | unlimited stream | yes, if the target has a console/parser | **ship (planned)** |
| **NFC listener** | ~200–800 B NDEF | yes — phones auto-read NDEF | **ship this next** |
| I2C (controller) | unlimited | only to hardware the user wrote | defer |
| SPI (controller) | unlimited | only to hardware the user wrote | defer |
| Bit-bang GPIO | unlimited | only to hardware the user wrote | defer |
| SubGHz (CC1101) | ~64 B/packet | no standard text receiver | no |
| 1-Wire / iButton | 64-bit ID | no | no |
| RFID (125 kHz) | ~40-bit ID | no | no |
| ADC / PWM / vibro / light | n/a | no | no |
| Speaker | single tone | no | no |
| Crypto / RTC / SD | n/a | not a transport | no |

## The one worth building: NFC listener

This is the actual finding. The kit already has an "NFC" channel, but
`nfc_emulate.c` is misnamed — it writes an `.nfc` file and calls
`loader_enqueue_launch()` to hand over to the stock NFC app. We noted
that in the code comments during the review.

It turns out full in-process tag emulation is available:

```
Function,+,nfc_listener_alloc,   NfcListener*
Function,+,nfc_listener_start,   void
Function,+,nfc_listener_stop,    void
Function,+,nfc_listener_tx,      NfcError
Function,+,nfc_listener_get_data,const NfcDeviceData*
Function,+,mf_ultralight_alloc,  ...
Function,+,nfc_device_alloc / _load / _get_data / _copy_data
```

Plus per-protocol listeners for `iso14443_3a`, `iso14443_4a`,
`mf_classic`, `mf_ultralight` and `felica`.

**Why this is the strongest candidate in the kit:**

- **NDEF is the one wireless format phones read without an app.** Tap an
  NDEF tag to an unlocked Android or iPhone and the OS parses it and
  offers the content — a URL, or text. No pairing, no cooperating
  software on the target. Of every channel here, this is the only one
  that reaches a general-purpose device with zero setup.
- **The payload lands where assistants look.** A URL leads to a page; a
  text record can be read out or summarised. Phone assistants with
  screen-reading or "what am I looking at" capabilities are exactly the
  targets this kit exists to test.
- **MF Ultralight carries a useful amount** — an NTAG213-class layout is
  around 130 bytes of NDEF, NTAG216 around 800. That is comfortably more
  than QR's 134 bytes and in the same league as BadUSB's 512.
- **It removes a handover.** Today the operator leaves our app for the
  stock NFC app. In-process emulation keeps them in the kit, and — like
  the GPIO plan — means the serial bridge can report real completion.
- **It fixes a naming lie.** `nfc_emulate.c` would finally emulate.

Effort is higher than a GPIO mode because NDEF has to be built into a
tag's memory layout rather than written to a file, and the listener runs
a worker that needs the same abort/teardown discipline as the timers
fixed in `f5b2644`. Rough estimate 3–4 days, most of it in NDEF record
construction and testing against real phones.

I would sequence it **after** the GPIO UART work, because GPIO is
cheaper and lower risk, but I would put it ahead of any additional
GPIO protocol.

## Why I2C, SPI and bit-bang should wait

All three are exported and would work. They fail a different test: the
user has to write the firmware on the other end either way, and if they
are doing that, **a UART is the easier thing for them to receive.**

Concretely: the audience for this feature is someone wiring the Flipper
to hardware they control. Give them a byte stream on a TX pin and any
microcontroller, SBC, or USB-serial adapter receives it with three lines
of code. Give them I2C and they must implement a target-mode state
machine and pick an address; give them SPI and they need to be a
controller-driven peripheral with chip-select timing. Both are more work
*for the user* than UART, for no gain in what arrives.

There are narrow cases where they win — a target that genuinely only
exposes an I2C bus, or a sensor emulation scenario where the kit must
impersonate a specific chip. Those are real, but they are specific
enough to build on request rather than speculatively. The transport
abstraction in the GPIO plan already leaves room:
`PifkGpioModeI2c` and `PifkGpioModeBitBang` are in the enum,
unimplemented.

**One caveat if I2C is ever added:** `furi_hal_i2c_*` exposes controller
operations (`tx`, `rx`, `trx`, `read_reg_8`). I did not find exported
*target*-mode support, so the Flipper would be the bus controller. That
means it can push bytes to a device the user built, but cannot pose as a
peripheral that an existing controller reads. Worth confirming before
promising the second shape.

## Why SubGHz is tempting and still wrong

`furi_hal_subghz_start_async_tx` and `write_packet` are exported, the
CC1101 does arbitrary modulation, and the kit already mentions Sub-GHz
in its UI. So capacity is fine — roughly 64 bytes per packet, chainable.

It fails question 2. There is no standard receiver that turns a
Sub-GHz packet into text for an assistant. To use it you would define
your own framing and write your own receiver, at which point it is
Option "user's own hardware" again — and UART over a wire is strictly
easier than a radio link for that. The existing Sub-GHz menu entry
already says the honest thing: deploy `.sub` files via the Sub-GHz app.

Regulatory point too: transmitting arbitrary payloads on ISM bands has
regional restrictions the kit should not encourage casually. The
`furi_hal_region` symbols exist for a reason.

## Why the rest are non-starters

- **1-Wire / iButton** — `furi_hal_ibutton_emulate_set_next(uint32_t)`.
  A 64-bit key. It is an identifier, not a message.
- **RFID 125 kHz** — ~40-bit IDs, same reasoning.
- **Speaker** — `furi_hal_speaker_start(float freq, float volume)` is a
  single-tone piezo, not a DAC. It cannot synthesise speech, so the
  obvious "talk to a voice assistant" idea does not work. (An
  ultrasonic/DolphinAttack-style approach needs a real transducer and
  amplifier, and would be a hardware project, not a FAP.)
- **ADC / PWM / vibro / light** — no text semantics at any layer.
- **Crypto / RTC / SD / power** — not transports.

## Recommendation

**Add nothing to the GPIO plan. Ship UART only, as written.**

Then, if a further channel is wanted, build **NFC listener emulation**
rather than another GPIO protocol. It is the only remaining transport
that reaches an unmodified consumer device with no cooperating software,
it upgrades an existing channel instead of adding a sixth, and it
retires a piece of the codebase we already documented as misleading.

Order of work, highest value first:

1. GPIO UART (~3.85 days, per the GPIO plan) — cheapest, self-testable
2. NFC listener emulation (~3–4 days) — highest reach, replaces a handover
3. Nothing else, unless a specific engagement asks for it

The honest summary of this survey is that the Flipper has many
peripherals and very few of them are *text* channels. UART and NFC are
the two that are, and the kit should have both before it has anything
else.

## Verification notes

- Peripheral family counts and every symbol quoted above are from
  `~/.ufbt/current/sdk_headers/f7_sdk/targets/f7/api_symbols.csv`
  (API 87.1), filtered to `+` (FAP-linkable) entries.
- NFC listener availability: `nfc_listener_alloc/start/stop/tx/get_data`
  and `mf_ultralight_alloc` all confirmed `+`; per-protocol listener
  headers present under `lib/nfc/protocols/*/`.
- `furi_hal_ibutton_emulate_set_next` signature (`uint32_t`) and
  `furi_hal_speaker_start` (`float, float`) read from the same table —
  these are the basis for the capacity claims, not assumptions.
- I2C target-mode absence is a negative result from searching the
  exported symbol list; worth re-checking against the firmware source
  before relying on it.
- NDEF capacity figures (NTAG213 ~130 B, NTAG216 ~800 B) are from the
  NTAG21x product family and should be confirmed against whichever tag
  type the listener is configured to emulate.
- No code was written or built for this assessment.
