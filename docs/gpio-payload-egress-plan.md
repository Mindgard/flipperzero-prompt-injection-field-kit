# Payload egress over GPIO — implementation plan

Status: **implemented** (d2c4519, response capture in 282ec09).
Kept for the design rationale and the verification notes; the
estimates and task list below are historical.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

**Note (superseded):** the sequence engine these notes build on has
since been removed, along with the results viewer. Anything below that
assumes a timer-driven sequencer as a base needs rethinking; the
channel reasoning itself still stands.

**Verdict: viable, and the best-supported new channel researched so far.
Everything needed is exported to FAPs, there is no protocol to reverse
engineer, and the design is small — a byte sink plus a UI. Estimated 3–4
days for the useful version.**

This is a different shape of feature from the others. BadUSB, BLE, NFC
and QR each require the kit to speak a specific protocol to a specific
kind of target. Here the kit's job stops at "emit these bytes on this
pin"; whatever the user builds on the other end owns the protocol. That
inverts the hard part onto the user, which is exactly why it is
tractable.

## Why the framing works

The premise in the request — *the field kit remains the payload source,
GPIO is the pipe* — dodges every blocker that killed the previous three
investigations:

| Investigation | Blocker |
|---|---|
| BLE keyboard | Firmware withholds `ble_profile_hid` from FAPs |
| Brother printer (BT) | Printer is BT Classic; Flipper radio is BLE-only |
| Brother printer (USB) | Both sides are USB devices; WB55 has no host controller |
| IR | ~32 bits/message; no receiver treats IR as text |
| **GPIO** | **none** |

The reason is that we stop trying to *be* a peripheral or *drive* someone
else's protocol. We become a source of bytes, which is the one thing the
hardware does unconditionally.

It also serves a real need. The kit currently supports the delivery
channels we chose. A researcher testing an assistant behind an unusual
interface — a serial console on an industrial HMI, a kiosk's debug
header, a robot's UART, a custom sensor bus feeding an LLM pipeline —
has no way in today. GPIO turns the kit from "five channels we picked"
into "any channel you can wire".

## What is available

All exported (`+`), verified against `api_symbols.csv`:

**UART — two independent channels**
```
furi_hal_serial_control_acquire(FuriHalSerialId)   /* null if in use */
furi_hal_serial_control_release(handle)
furi_hal_serial_init(handle, baud)
furi_hal_serial_set_br(handle, baud)
furi_hal_serial_configure_framing(...)             /* data bits, parity, stop */
furi_hal_serial_tx(handle, buffer, size)
furi_hal_serial_tx_wait_complete(handle)
furi_hal_serial_async_rx_start / _rx / _rx_available / _rx_stop
```
Two channels: `FuriHalSerialIdUsart` and `FuriHalSerialIdLpuart`, with
`gpio_usart_tx` / `gpio_usart_rx` on the external header. Physical pin
numbers are resolved at runtime rather than hardcoded — see §5. Framing
is configurable: 6–9 data bits, none/even/odd parity, 0.5–2 stop bits.
`_control_acquire` returns null rather than clobbering the interface if
the CLI or logging owns it, so contention is detectable instead of
silent.

**Raw GPIO — 8 free pins**
```
furi_hal_gpio_init / init_ex / init_simple
furi_hal_gpio_add_int_callback / enable / disable / remove
gpio_ext_pa4 pa6 pa7 pb2 pb3 pc0 pc1 pc3
```

**I2C and SPI, both with an explicit external bus**
```
furi_hal_i2c_acquire / release / tx / rx / trx / is_device_ready
furi_hal_i2c_bus_external, furi_hal_i2c_handle_external
furi_hal_spi_acquire / release / bus_tx / bus_rx / bus_trx
furi_hal_spi_bus_handle_external
```

**Power for the user's board**
```
furi_hal_power_enable_external_3_3v      /* 3V3 rail */
furi_hal_power_enable_otg                /* 5V boost */
```
So a small adapter can be powered from the Flipper rather than needing
its own supply — which matters for a field kit.

## Design

Keep the kit's side deliberately dumb. It emits payload bytes; it does
not know or care what is downstream.

### 1. Transport abstraction

```c
/* src/execute/gpio_exec.h */

typedef enum {
    PifkGpioModeUart = 0,   /* default: byte stream on TX */
    PifkGpioModeBitBang,    /* clock + data on two GPIOs */
    PifkGpioModeI2c,        /* we are controller, target is a device */
    PifkGpioModeCount,
} PifkGpioMode;

typedef struct {
    PifkGpioMode mode;
    uint32_t baud;              /* UART */
    FuriHalSerialId serial_id;  /* Usart | Lpuart */
    uint8_t line_ending;        /* none | LF | CRLF */
    bool append_null;           /* some parsers want a NUL terminator */
    uint32_t inter_byte_delay_ms; /* pacing for slow receivers */
    uint8_t i2c_address;        /* I2C target */
} PifkGpioConfig;

bool pifk_execute_gpio(PifkApp* app, const PifkPayload* p);
bool pifk_execute_gpio_conversation(PifkApp* app,
                                        const PifkConversation* c);
void pifk_gpio_abort(PifkApp* app);
```

UART is the only mode I would ship first. It covers the overwhelming
majority of "I have a device with a serial console" cases, and the other
two can be added later without changing the interface.

### 2. UART path

The whole transmit function is about this size:

```c
static bool gpio_uart_send(PifkApp* app, const char* text) {
    FuriHalSerialHandle* h =
        furi_hal_serial_control_acquire(app->gpio_cfg.serial_id);
    if(!h) return false;   /* CLI or logging owns it — report, don't clobber */

    furi_hal_serial_init(h, app->gpio_cfg.baud);

    size_t len = strlen(text);
    if(app->gpio_cfg.inter_byte_delay_ms == 0) {
        furi_hal_serial_tx(h, (const uint8_t*)text, len);
    } else {
        /* Paced: for receivers with no flow control and small buffers */
        for(size_t i = 0; i < len && app->executing; i++) {
            furi_hal_serial_tx(h, (const uint8_t*)&text[i], 1);
            furi_hal_serial_tx_wait_complete(h);
            furi_delay_ms(app->gpio_cfg.inter_byte_delay_ms);
        }
    }

    if(app->gpio_cfg.line_ending == LineEndingCrLf)
        furi_hal_serial_tx(h, (const uint8_t*)"\r\n", 2);
    else if(app->gpio_cfg.line_ending == LineEndingLf)
        furi_hal_serial_tx(h, (const uint8_t*)"\n", 1);

    furi_hal_serial_tx_wait_complete(h);
    furi_hal_serial_deinit(h);
    furi_hal_serial_control_release(h);
    return true;
}
```

Two details worth getting right:

- **Acquire/release must be balanced on every path**, including abort.
  Leaking the handle leaves the CLI dead until reboot — the same class
  of bug as the timer leak fixed in `f5b2644`.
- **The paced loop checks `app->executing`**, so Stop works mid-payload.
  Note this is *better* than BadUSB, which cannot be interrupted
  mid-type; here each byte is a natural yield point.

### 3. Loopback: RX makes this testable

`furi_hal_serial_async_rx_*` is exported, so the kit can read back what
it sent. That enables a genuine self-test with nothing but a jumper wire
between the USART TX and RX pins:

```
Settings -> GPIO -> Loopback test
  "Connect pin <TX> to pin <RX>, then press Test."
     ^ both resolved via furi_hal_resources_get_ext_pin_number(), so the
       numbers shown are whatever this firmware actually means
  -> sends a known string, reads it back, compares
  -> "PASS 34/34 bytes @ 115200" or "FAIL: got 12 of 34"
```

This is worth building early. Every other channel in this kit is
hard to verify without the target hardware; this one can prove the
transport works before the user's board is even involved, which turns
"it didn't work" into a two-way diagnosis instead of a guess.

### 4. UI

- **Quick Deploy** gains "GPIO / Serial", consistent with the existing
  protocol entries.
- **Settings** gains a GPIO submenu: mode, channel (USART/LPUART), baud
  (9600 … 921600), line ending, inter-byte delay, and the loopback test.
  Persist via the existing `settings.json` path — note the delay needs
  the same clamp treatment as `PIFK_MAX_DELAY_MS`.
- **Sequences** gain `PifkProtocolGpio` as a step type, so GPIO
  composes with the timer-driven sequencer.
- **Bridge** gains `EXEC GPIO <name>`. Unlike the BadUSB-over-BLE
  handover, this one *can* report real completion — we never leave the
  app, so `OK DONE <name>` is honest.

### 5. Pin numbers: ask the firmware, don't hardcode them

Do not bake header pin numbers into the source or the docs. The SDK
exports the mapping and the lookups to resolve it at runtime, all `+`:

```
Variable,+,gpio_pins,const GpioPinRecord[]
Variable,+,gpio_pins_count,const size_t
Function,+,furi_hal_resources_get_ext_pin_number, int32_t, const GpioPin*
Function,+,furi_hal_resources_pin_by_name,   const GpioPinRecord*, const char*
Function,+,furi_hal_resources_pin_by_number, const GpioPinRecord*, uint8_t
```

`GpioPinRecord` carries `{ pin, name, channel, pwm_output, number, debug }`,
so the header number for a signal is a lookup, not a constant:

```c
/* Show the operator which physical pins to wire, per this firmware. */
int32_t tx = furi_hal_resources_get_ext_pin_number(&gpio_usart_tx);
int32_t rx = furi_hal_resources_get_ext_pin_number(&gpio_usart_rx);
snprintf(buf, sizeof(buf), "TX: pin %ld  RX: pin %ld", (long)tx, (long)rx);
```

This is better than a static wiring table for three reasons: it cannot
drift from the firmware, it is correct if the numbering ever differs
across hardware revisions, and the wiring help can be rendered *on the
device* where the operator actually needs it — no printed diagram, no
switching to the README mid-engagement.

The loopback screen should print the resolved numbers rather than
hardcoded ones, so the instruction the operator reads is always the
instruction this firmware means.

### 6. Documentation is the deliverable

For a "bring your own hardware" feature, the docs matter more than the
code. The README needs a wiring section — generated from, or at least
checked against, the runtime lookup above rather than transcribed from a
diagram:

```
Flipper GPIO header (USART):
   TX   ->  your RX          (number shown in Settings -> GPIO)
   RX   ->  your TX          (optional; needed for loopback)
   GND  ->  your GND         (required — floating grounds cause garbage
                              that looks like a code bug)
   3V3  ->  your VCC         (optional, modest current budget)
```

Plus the level warning: **the Flipper's GPIO is 3.3V. Feeding 5V logic
into it can damage the MCU.** Anyone wiring to a 5V board needs a level
shifter. This belongs in the UI too, not just the README — a one-time
confirmation dialog before the first GPIO transmit.

The 3V3 rail's current budget should be taken from Flipper's official
hardware documentation before publishing a number; I have deliberately
not guessed one here.

## Work breakdown

| # | Task | Est. | Risk |
|---|------|------|------|
| 1 | `gpio_exec.c`: UART transmit, acquire/release discipline, paced mode, abort support | 1d | low |
| 2 | Loopback self-test + result screen, with pin numbers resolved via `furi_hal_resources_get_ext_pin_number()` | 0.6d | low |
| 3 | Settings submenu (mode, channel, baud, framing, delay) with persistence and clamps | 0.75d | low |
| 4 | Quick Deploy entry + 3.3V warning dialog | 0.25d | low |
| 5 | Conversations: per-turn send with inter-turn delay | 0.25d | low |
| 6 | Sequence step type `PifkProtocolGpio` | 0.25d | low |
| 7 | Bridge `EXEC GPIO <name>` with real completion reporting | 0.25d | low |
| 8 | README wiring + safety docs, one worked example | 0.5d | low |

**~3.85 days.** Every task is low risk, which is unusual and is the
point: there is no unknown protocol, no pairing state machine, no
per-OS behaviour matrix. The riskiest thing here is forgetting to
release the serial handle.

Optional follow-ups, only if asked for: I2C controller mode (+1d),
bit-bang clock/data for exotic receivers (+1d), RX capture so the kit
can log the target's *response* to an injection (+1d — arguably the most
interesting of the three, since it closes the loop on whether the
payload had an effect).

On whether to add I2C/SPI/bit-bang at all, and what else the Flipper's
peripherals could carry, see `transport-survey.md`. Its conclusion is
that UART is the only GPIO mode worth shipping — anyone wiring their own
receiver finds a UART easier to implement than an I2C target or an SPI
peripheral — and that the next channel worth building is not on the GPIO
header at all, but in-process NFC tag emulation.

## Risks and honest limitations

**It needs hardware the user builds.** The kit ships a pipe, not a
solution. That is fine as long as the docs say so plainly — the failure
mode to avoid is a menu entry that appears to be a delivery channel and
is really a serial port.

**Wiring errors will look like software bugs.** Floating ground and
swapped TX/RX are the two most common, and both produce "it sent
nothing" or "it sent garbage". The loopback test exists to separate
these; it should be prominent, not buried.

**3.3V logic levels.** Real risk of damaging the MCU with 5V hardware.
Needs the warning in both docs and UI.

**USART contention with the CLI.** The Flipper's own console and logging
use the USART by default. `_control_acquire` returning null handles this
correctly, but the UI must explain *why* it failed and suggest LPUART
instead of just saying "failed".

**No flow control.** No RTS/CTS in the exported API, so a receiver with
a small buffer can drop bytes on a fast burst. The inter-byte delay is
the mitigation, and the loopback test at the chosen baud will surface it.

**Scope discipline.** The temptation will be to grow this into a
general-purpose serial terminal. That is a different app, and the
Flipper already has several. This should stay "send *payloads* over
serial" — the payload database is what makes it part of this kit.

## Recommendation

**Build it, UART-only, in the order above.** It is the highest
value-per-day of everything researched in this session:

- No capability gap — unlike the BLE keyboard, which needs a
  reimplemented GATT service, or the printer, which is impossible.
- Genuinely extends the kit's reach to targets we cannot anticipate,
  which is worth more than another channel we chose.
- Real completion reporting over the serial bridge, which the
  BadUSB-over-BLE handover cannot give.
- Interruptible mid-payload, which BadUSB cannot be.
- Self-testable with a jumper wire, which none of the other channels are.

Start with tasks 1 and 2 together. A working loopback test is the
proof the transport is sound, and it is what makes the rest safe to
build on.

## Verification notes

- All symbols above confirmed `+` (FAP-linkable) in
  `~/.ufbt/current/sdk_headers/f7_sdk/targets/f7/api_symbols.csv`
  (API 87.1).
- UART channel IDs, data bits, parity and stop bit enums from
  `targets/f7/furi_hal/furi_hal_serial_types.h`.
- `furi_hal_serial_control_acquire` returning null on contention, and
  the `tx` / `tx_wait_complete` split, from
  `targets/f7/furi_hal/furi_hal_serial.h` and
  `furi_hal_serial_control.h`.
- Free external pins (`gpio_ext_pa4/pa6/pa7/pb2/pb3/pc0/pc1/pc3`) and
  power rails (`furi_hal_power_enable_external_3_3v`,
  `furi_hal_power_enable_otg`) from the same API table.
- Pin numbering is deliberately **not** asserted in this document. An
  earlier draft quoted pins 13/14/11/9 from the standard pinout from
  memory; those were unverified, so they were removed in favour of the
  runtime lookup. `gpio_pins`, `gpio_pins_count`,
  `furi_hal_resources_get_ext_pin_number`,
  `furi_hal_resources_pin_by_name` and `_pin_by_number` are all
  confirmed `+` in the API table, and `GpioPinRecord` (with its
  `number` field) is defined in
  `targets/f7/furi_hal/furi_hal_resources.h`.
- The 3V3 rail current budget is likewise not asserted — take it from
  Flipper's official hardware docs before publishing.
- No code was written or built for this assessment.
