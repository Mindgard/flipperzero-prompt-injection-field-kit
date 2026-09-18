# Infrared as a prompt injection channel — research

Status: research only, nothing implemented.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

**Note (superseded):** the sequence engine these notes build on has
since been removed, along with the results viewer. Anything below that
assumes a timer-driven sequencer as a base needs rethinking; the
channel reasoning itself still stands.

**Summary: IR is not a text channel, and that is the whole problem. It
cannot deliver a prompt. What it can do is act as a *trigger* — press
buttons on a device that then ingests a payload delivered some other
way. That is a real and under-tested attack pattern, but it is a
different feature from the kit's existing channels, and it is dishonest
to file it under "prompt injection over IR".**

The most defensible version of an IR feature in this kit is a small
one: use IR to put a target device into a state where an assistant will
read something. The payload still arrives via QR, BadUSB, or a screen.

## What the hardware can actually do

The IR API is fully exported to FAPs — no capability gap here, unlike
the BLE HID work:

```
infrared_send(const InfraredMessage*, int times)
infrared_send_raw(const uint32_t timings[], uint32_t cnt, bool start_from_mark)
infrared_send_raw_ext(..., uint32_t frequency, float duty_cycle)
furi_hal_infrared_async_rx_start / _stop      (receive + learn)
infrared_alloc_decoder / infrared_decode      (14 protocols)
```

Encoded protocols available: NEC, NECext, NEC42, NEC42ext, Samsung32,
RC5, RC5X, RC6, SIRC, SIRC15, SIRC20, Kaseikyo, RCA, Pioneer.

And the raw path allows arbitrary carrier frequency and duty cycle,
capped at `MAX_TIMINGS_AMOUNT 1024` timings per signal.

So transmit is flexible. The constraint is not the Flipper.

## Why this cannot carry a prompt

An `InfraredMessage` is:

```c
typedef struct {
    InfraredProtocol protocol;
    uint32_t address;
    uint32_t command;
    bool repeat;
} InfraredMessage;
```

That is the entire payload: an address and a command. For NEC, 8–16
bits of address and 8 bits of command — call it **32 bits per message**,
and most of those bits are a fixed device address, not free data.

Compare the kit's existing channels:

| Channel | Payload capacity | Reaches a text field? |
|---|---|---|
| BadUSB | 512 bytes | Yes, as keystrokes |
| BLE beacon | 2079 bytes | No (device name) |
| QR | 134 bytes | Via camera |
| NFC | ~245 bytes | Via reader app |
| **IR** | **~1 byte of command per message** | **No** |

Worse than the size limit is the semantics. There is no receiver
anywhere in the consumer IR ecosystem that treats an incoming IR frame
as *text*. A TV receives `NEC addr=0x04 cmd=0x08` and looks it up in a
keymap: that is "volume up". There is no code path where an IR command
becomes a string, let alone a string that lands in an LLM's context
window. Nothing to inject into.

You could in principle define your own encoding — 1024 raw timings is
maybe 60 bytes of arbitrary data at a generous bit rate — but that only
works if something on the other end is running software you wrote to
decode it. At that point you control the target, and it is not an
attack.

## The genuinely interesting angle: IR as a trigger, not a carrier

Reframe it. IR is not how the payload arrives; IR is how you *arrange
for the payload to be read*. The Flipper's universal-remote capability
means it can drive almost any TV, projector, set-top box, soundbar or
conference-room display in range, without pairing, authentication, or
being on the network.

That matters because a growing number of those devices have an
assistant attached. Concretely plausible chains:

**1. Turn on the display that shows the payload.**
A meeting-room screen is off. The injection is a QR code or text on a
signage feed. IR powers the display on and selects the right HDMI input;
whatever is looking at that room — a smart-camera assistant, a
note-taking bot with a camera feed, a person's phone assistant asked to
"read the screen" — now ingests it.

**2. Navigate a smart TV to attacker-controlled content.**
Smart TVs run browsers and app stores. IR gives full remote control:
Home, arrows, OK, and on many models digit keys and a text-entry
keyboard. Driving an on-screen keyboard over IR to type a URL is slow
but entirely mechanical. The resulting page carries the injection, and
the TV's own assistant (or a connected agent) reads it.

**3. HDMI-CEC bridging.**
Many TVs relay remote-control keypresses to attached devices over
HDMI-CEC ("Remote Control Passthrough"). So an IR command to the TV can
become a keypress delivered to a Chromecast, Apple TV, or games console
— extending reach to devices with no IR receiver of their own. Note the
Flipper cannot speak CEC itself (no HDMI); it is riding the TV's
existing bridge.

**4. Denial and misdirection during a test.**
Turning a display off, muting audio, or switching inputs mid-assessment
is a legitimate part of testing whether a monitoring assistant notices
or reports state changes it should.

In all four the injection text arrives through a channel the kit already
has. IR contributes reach and timing, not content.

## What the literature supports

Worth being precise about what is and is not demonstrated:

- **Indirect prompt injection via ingested content is well established.**
  The Gemini "Invitation Is All You Need" work (14 scenarios across five
  threat classes) includes control of home-automation devices as a
  *consequence* of injection, and demonstrates on-device lateral
  movement. That is the shape of attack IR could set up — but the
  injection vector in that work is calendar invites, emails and shared
  documents, not IR.
- **Optical/physical injection into assistants is established** —
  Light Commands (Michigan / Zhejiang) injected commands into voice
  assistants using modulated *laser* light aimed at the MEMS
  microphone, exploiting a photoacoustic effect. This is the closest
  published analogue to "injection over a light channel", and it is
  worth being clear that it is **not** what the Flipper's IR LED does:
  Light Commands needed a laser with precise aim and enough power to
  induce a mechanical response in the mic diaphragm. A remote-control
  LED at 38kHz is a different physical mechanism, and I found no
  research showing an IR blaster can drive a microphone this way. I
  would not claim it without testing.
- **HDMI-CEC remote passthrough is documented behaviour**, not a
  vulnerability — Android's `HdmiControlService` implements it by
  design. Using it as reach is abuse of a feature, which is exactly the
  kind of thing worth testing.
- **I found no published work on IR as a prompt injection carrier.**
  Absence of research here is not an opportunity; it reflects that IR
  carries no text.

## Options

### Option 1 — IR as a payload carrier. Not viable.

~32 bits per message, and no receiver treats IR frames as text. Nothing
to inject into. Dismissed on protocol semantics, not effort.

### Option 2 — IR "stager": trigger a device, deliver by other means

A small feature: alongside a payload, store an optional IR action
("power on + HDMI 2") that fires before the payload is presented. The
operator's actual injection still goes out over QR/BadUSB/screen.

Implementation is modest because the heavy lifting is in the firmware:

- `infrared_send()` for encoded protocols; the kit already knows how to
  parse JSON, so an IR step in a sequence is a natural fit
- The sequence engine we just rebuilt (timer-driven, one step per tick)
  is exactly the right structure for "IR power-on, wait 4s for the panel
  to wake, then show the QR"
- Universal remote code databases are large; shipping our own is
  ill-advised. Point at the existing Flipper IR app's `.ir` files on
  the SD card instead of duplicating them

Rough effort: 1–2 days for an `PifkProtocolIr` sequence step type
that plays a named `.ir` file, given the sequence engine already exists.

### Option 3 — IR reconnaissance

`furi_hal_infrared_async_rx_*` and the decoder are exported, so the kit
could learn and identify the remote protocol in use in a room. Useful
for building a target profile during an engagement. Genuinely easy, but
the Flipper's stock IR app already does this better, so the only reason
to build it here is workflow integration. Low value.

### Option 4 — don't build it

The stock Flipper IR app can already power on a display and switch
inputs. An operator can do that, then open our app and show the QR.
Two apps instead of one, and no new code.

## Recommendation

**Don't build a dedicated IR feature now, and don't describe IR as a
prompt injection channel — it isn't one.**

If the sequence engine grows a protocol-step type later, adding
`PifkProtocolIr` as a *stager* step (Option 2) is a reasonable
increment: it composes with the timer-driven sequencer we already have,
and "wake the display, then present the payload" is a genuinely useful
chain in a room-based assessment. That is the only version I would
build, and it should be documented as staging, not injection.

The thing I would actually want before writing any of it is evidence
that the chain works end to end on real hardware: pick one meeting-room
display and one assistant with a camera or screen-reading capability,
drive the display on over IR with the stock app, present a QR payload,
and see whether the assistant ingests it. If that fails, Option 2 has no
customer. If it works, the demo is the feature and the code is small.

**One thing worth flagging as out of scope.** The most eye-catching idea
here — IR-to-microphone injection à la Light Commands — I am explicitly
*not* recommending, because I could not substantiate that a 38kHz remote
LED can do what a focused laser does. Claiming it without a bench test
would be exactly the kind of unverified capability claim I have been
removing from this repo's docs.

## Verification notes

- Exported IR symbols, the 14-protocol enum, `InfraredMessage` layout
  and `MAX_TIMINGS_AMOUNT 1024` read from
  `~/.ufbt/current/sdk_headers/f7_sdk/` (API 87.1):
  `lib/infrared/encoder_decoder/infrared.h`,
  `lib/infrared/worker/infrared_transmit.h`,
  `lib/infrared/worker/infrared_worker.h`, and `api_symbols.csv`.
- Indirect injection threat classes and consequences: "Invitation Is All
  You Need" / Targeted Promptware Attacks against Gemini-powered
  assistants, via Schneier's summary of the paper.
- Light Commands (laser injection into MEMS microphones): University of
  Michigan / University of Zhejiang, 2019. Cited as a *contrast*, not
  as support for an IR-LED equivalent.
- HDMI-CEC remote passthrough as designed behaviour: Android
  `HdmiControlService` documentation (source.android.com).
- No code was written or built for this assessment.
