# USB descriptor injection, BLE audio, and USB DAC — evaluation

Status: idea 1 **implemented** (c00ffcd). Ideas 2 and 3 remain
blocked by absent hardware and are kept here as the record of why.
Target: firmware API 87.1 (`ufbt` SDK, f7 / Flipper Zero).

Three ideas assessed. They land very differently:

| Idea | Verdict |
|---|---|
| 1. USB descriptor strings as an injection carrier | **Viable and novel. Best idea proposed for this kit so far.** |
| 2. BLE microphone emulation, play an audio payload | Not possible — no Bluetooth audio profile at any level |
| 3. USB DAC on the Flipper → 3.5mm audio out | Not possible — needs USB host (absent) and an audio source (absent) |

Idea 1 is worth building. It is the only vector found in any of these
surveys that is **zero-interaction on the target**: no pairing, no
keystrokes, no camera, no user action beyond the physical insert.

---

## Idea 1 — USB descriptor strings as an injection carrier

### The insight

This is a genuinely good idea and it has been missed by every previous
survey, because those surveys were all looking for *data channels*. This
one is different: it uses metadata that the target logs **automatically,
as a side effect of enumeration**, before any driver loads and before the
user does anything.

That is precisely the "implicit prompt injection" class from the recent
literature — adversarial text entering an LLM's context through routine
system behaviour the user neither requested nor observed. Except instead
of a URL preview or Open Graph tag, the carrier is a USB string
descriptor.

### It is fully supported by the API

`FuriHalUsbInterface` exposes the descriptors as writable pointers, and
a FAP can construct its own interface and pass it to
`furi_hal_usb_set_config` (both `+`):

```c
struct FuriHalUsbInterface {
    void (*init)(usbd_device*, FuriHalUsbInterface*, void* ctx);
    void (*deinit)(usbd_device*);
    void (*wakeup)(usbd_device*);
    void (*suspend)(usbd_device*);
    struct usb_device_descriptor* dev_descr;   /* VID, PID, bcdDevice, class */
    void* str_manuf_descr;                     /* iManufacturer  */
    void* str_prod_descr;                      /* iProduct       */
    void* str_serial_descr;                    /* iSerialNumber  */
    void* cfg_descr;
};
```

```
Function,+,furi_hal_usb_set_config, _Bool, "FuriHalUsbInterface*, void*"
Function,+,furi_hal_usb_get_config, FuriHalUsbInterface*,
```

So we control manufacturer, product and serial strings, plus VID/PID and
device class. No firmware modification, no withheld symbols.

### Capacity

`usb_string_descriptor.bLength` is a `uint8_t` and strings are UTF-16:

```
(255 - 2 bytes header) / 2 bytes per char = 126 characters per string
```

Three strings → **~378 characters total**, and that is a hard protocol
ceiling, not an implementation limit. For comparison: QR is 134 bytes,
BadUSB 512. So descriptor injection sits comfortably in the useful range
— enough for a real instruction, not enough for a long one. Payloads
would need authoring specifically for it, which fits the payload-shape
work recommended in `unexplored-vectors.md`.

### Where the text actually lands

This is the part that makes it credible rather than theoretical. On
Windows:

- **EID 6416 (Audit PNP Activity, Win10+)** — logs vendor ID, product
  ID and `iSerialNumber` on **every** connection, not just the first.
  It lives in the **Security log**, which is the channel most likely to
  be shipped to a SIEM.
- **EID 20001 / 20003** — Plug and Play driver install, carries device
  identifying information including serial. First connection only.
- **`C:\Windows\INF\setupapi.dev.log`** — records **vendor name,
  product name and serial number** as text on first install. A plain
  log file, trivially ingested.
- **Registry** — `HKLM\SYSTEM\CurrentControlSet\Enum\USB\` holds `Desc`,
  `HardwareID`, `Mfg` and friends, persistently.

On Linux: `udev`/`dmesg` log manufacturer and product strings on
enumeration; `journalctl` picks them up. On macOS: unified logging plus
`system_profiler SPUSBDataType`.

The attack chain the kit would be testing:

```
Flipper presents as USB device with iProduct = <injection text>
    -> host enumerates, logs vendor/product/serial automatically
    -> log shipped to SIEM / log-analysis pipeline
    -> LLM-backed triage summarises "recent USB device activity"
    -> injection text enters the model's context
```

That last hop is the assumption to test, and it is increasingly
plausible: LLM-assisted SOC triage and log summarisation are now common,
and "summarise unusual device activity on this host" is exactly the sort
of query these systems serve.

### Why it is the strongest vector surveyed

- **Zero interaction.** Every other channel needs the target to do
  something: focus a text field (BadUSB), scan a code (QR), tap a tag
  (NFC), pair (BLE), wire a board (GPIO). This needs an insert.
- **Persistent.** The payload lands in logs and the registry and stays
  there. Most injections are transient; this one is retained and may be
  re-read on every later query.
- **Pre-driver.** Enumeration metadata is logged before any driver
  loads, so it works even if the device is subsequently rejected by
  policy — arguably *better* if rejected, since a blocked-device alert
  is more likely to reach a human or an LLM triage step.
- **Reaches defenders, not users.** Novel targeting: the consumer is
  the SOC's tooling, not the endpoint user. That is a different threat
  model from anything else in the kit and worth being able to test.
- **Composes with BadUSB.** The kit already switches to `usb_hid` to
  type. The same insert could carry descriptor-based injection *and*
  keystroke injection — two channels, one action.

### Implementation sketch

```c
/* src/execute/usb_descriptor_exec.c */

/* UTF-16 string descriptor built at runtime from payload text. */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;   /* USB_DTYPE_STRING */
    uint16_t wString[126];
} PifkUsbString;

static struct usb_device_descriptor pifk_dev_descr;
static PifkUsbString mfg_str, prod_str, serial_str;
static FuriHalUsbInterface pifk_usb_iface;

bool pifk_execute_usb_descriptor(PifkApp* app,
                                     const PifkPayload* p) {
    /* Split payload across the three strings, 126 chars each,
     * ASCII -> UTF-16. Reuse usb_cdc_single's init/deinit and cfg_descr
     * so we present as a benign CDC device that merely has an
     * unusual name — the class must still be valid or the host
     * rejects enumeration before logging anything useful. */
    ...
    FuriHalUsbInterface* prev = furi_hal_usb_get_config();
    furi_hal_usb_set_config(&pifk_usb_iface, NULL);
    /* Host enumerates and logs here. Hold, then restore. */
    ...
}
```

Design notes that matter:

- **Borrow a working class implementation.** Reuse the `init`/`deinit`
  and `cfg_descr` from `usb_cdc_single` (or `usb_hid`) rather than
  writing a class from scratch. We are changing the *labels*, not the
  function. A malformed configuration descriptor gets the device
  rejected before the strings are read.
- **Re-enumeration is the trigger.** The payload only lands when the
  host enumerates, so the flow is: set config → wait for enumeration →
  optionally toggle to force re-enumeration → restore. Needs
  `furi_hal_usb_set_state_callback` (exported) to know when it
  happened, rather than a blind delay.
- **Restore the previous config on exit**, same discipline as
  `bt_profile_restore_default` in the BLE plan and the timer teardown in
  `f5b2644`. Leaving the Flipper presenting a hostile product string
  after the app closes would be a genuine bug.
- **Character set.** Descriptor strings are UTF-16 and not passed
  through `hid_ascii_to_key()`, so unlike BadUSB they can carry
  non-ASCII — including the homoglyph and zero-width payloads noted as
  a BadUSB trap in `unexplored-vectors.md`. That makes this the *better*
  channel for testing filter evasion.

### Effort and risk

| # | Task | Est. |
|---|------|------|
| 1 | Runtime UTF-16 string descriptor construction from payload text | 0.5d |
| 2 | Custom `FuriHalUsbInterface` borrowing CDC class impl; set/restore | 1d |
| 3 | Enumeration detection via `furi_hal_usb_set_state_callback` | 0.5d |
| 4 | Quick Deploy entry + Settings (which strings, VID/PID) | 0.5d |
| 5 | Bridge `EXEC USBDESC <name>` | 0.25d |
| 6 | Verify on Windows (EID 6416, setupapi.dev.log), Linux (dmesg/udev), macOS | 1d |

**~3.75 days.** Main risk is task 2: a wrong descriptor means the host
rejects the device and nothing is logged. Task 6 is the one that proves
the premise, and should arguably come first as a manual experiment —
hand-craft one descriptor, plug into a Windows box, and confirm the
string appears in EID 6416 and `setupapi.dev.log` before writing the
feature.

### Ethical / scope note

This vector targets **defensive tooling** — the SOC's log pipeline — not
the endpoint user. That is legitimate to test with authorisation (it is
exactly the "can our SIEM's AI triage be manipulated" question), but it
deserves explicit framing in the docs, because a payload that reaches
incident-response tooling has a different blast radius than one that
reaches a chat box. It also argues for keeping VID/PID configurable but
*not* shipping presets impersonating specific vendors, consistent with
the position already taken on BLE device naming.

---

## Idea 2 — BLE microphone emulation

**Not possible.** No Bluetooth audio support exists at any level.

```
$ grep -iE "a2dp|audio|hfp|pcm|codec|sbc|lc3" api_symbols.csv
LL_RCC_GetUSBClockFreq
sequence_audiovisual_alert
```

Both matches are false positives (a clock function and a notification
sequence name). There is no A2DP, no HFP/HSP, no LE Audio, no LC3 or SBC
codec.

Three independent blockers, any one fatal:

1. **No audio profile.** A2DP and HFP are Bluetooth *Classic* profiles,
   and the Flipper has no Classic radio at all — established in the
   Brother printer research (ST's BLE-light stack, maintainers confirm
   Classic is unsupported). LE Audio would be the BLE route, and there
   is no LC3 codec or Isochronous Channel support in the exported API.
2. **Direction is backwards.** A microphone is a *source*. Emulating one
   means the Flipper produces an audio stream the host consumes — so we
   would need an audio source, and there is none (see idea 3).
3. **Even with 1 and 2 solved, the target would need to be recording.**
   Injecting into an assistant via a fake mic requires the assistant to
   be listening on that input, which is a narrower precondition than it
   first appears.

The underlying goal — get a spoken prompt into a voice assistant — is
sound and is documented in the literature, but the demonstrated
technique is *acoustic or optical* (Light Commands used a modulated
laser against the MEMS microphone). That needs a transducer and
amplifier, i.e. a hardware project, not a FAP. Noted and dismissed in
`ir-prompt-injection-research.md` for the same reason.

---

## Idea 3 — USB DAC → 3.5mm audio out

**Not possible.** Two blockers, and the first is physical.

**Blocker A: a USB DAC is a USB device, so the Flipper would have to be
the host.** The STM32WB55 has no OTG/host controller — ST's peripheral
matrix lists the WB family under "USB Device only". Confirmed in-tree:
`libusb_stm32` ships `usbd_core.h` with no `usbh_` code, and the only
`*_host` exports in the SDK are `onewire_host_*` (iButton). This is the
same wall the Brother printer hit over USB. The `furi_hal_power_*_otg`
symbols are 5V boost control for the GPIO header, not a host stack.

**Blocker B: there is nothing to play.** Even given a host controller
and a UAC driver, an audio stream needs a source:

```
furi_hal_speaker_start(float frequency, float volume)   /* single tone */
furi_hal_pwm_start / set_params                          /* square wave */
(no furi_hal_dac — ADC exists, but that is input)
```

The speaker is a piezo driven by a frequency, not a DAC. There is no
audio DAC API, no PCM playback path, and no codec. The Flipper can beep;
it cannot render speech.

**Blocker C: `usb_audio.h` does not exist.** The available USB class
headers are `usb_ccid.h`, `usb_cdc*.h`, `usb_dfu.h`, `usb_hid.h`,
`usb_tmc.h`, `usb_std.h`. USB Audio Class is not implemented, so the
Flipper cannot even present *as* a USB audio device to a host — which
would have been the inverse and more plausible framing of this idea.

**The inverse idea is worth noting as also blocked.** Rather than
Flipper→DAC→speaker, one might ask: can the Flipper present as a USB
*headset* to a laptop, so the laptop's assistant hears a payload? That
fails on Blocker C (no UAC) and Blocker B (nothing to send). If UAC were
ever added to the firmware, this becomes interesting, because the host
would treat the Flipper as a legitimate audio input device — but that is
a firmware feature request, not a FAP.

**If audio delivery is genuinely wanted**, the honest path is external:
a small board with a real DAC or an audio-playback module, driven over
GPIO UART/I2C from the kit. That is the "bring your own hardware" story
in `gpio-payload-egress-plan.md`, and it makes the Flipper the payload
source with the audio hardware as the transducer — which is exactly the
framing that made the GPIO plan viable.

---

## Recommendation

**Research and build idea 1.** It is the strongest vector proposed for
this kit: zero-interaction, persistent, pre-driver, fully supported by
the exported API, and it targets a consumer (log-analysis tooling) that
nothing else in the kit reaches. Slot it after the payload-shape work
and alongside GPIO UART in priority.

Before writing code, do the one-hour experiment that de-risks the whole
thing: hand-craft a descriptor with a recognisable string, plug into a
Windows machine, and confirm it appears in EID 6416 and
`setupapi.dev.log`. If the string is truncated or sanitised in practice,
the capacity numbers above change and the feature design changes with
them.

**Drop ideas 2 and 3.** Both are blocked by absent hardware (no Classic
radio, no USB host, no DAC) rather than by missing software, so no amount
of implementation effort reaches them. The underlying goal behind both —
audio-channel injection — is best served by external hardware over the
GPIO channel, if it is wanted at all.

## Verification notes

- `FuriHalUsbInterface` layout (`dev_descr`, `str_manuf_descr`,
  `str_prod_descr`, `str_serial_descr`, `cfg_descr`) from
  `targets/furi_hal_include/furi_hal_usb.h`.
- `usb_string_descriptor` (`uint8_t bLength`, `uint16_t wString[]`) and
  `usb_device_descriptor` (`idVendor`, `idProduct`, `iManufacturer`,
  `iProduct`, `iSerialNumber`) from
  `lib/libusb_stm32/inc/usb_std.h`. The 126-character figure is derived
  from `bLength` being 8-bit with 2 bytes of header and UTF-16 encoding.
- `furi_hal_usb_set_config`, `furi_hal_usb_get_config` and
  `furi_hal_usb_set_state_callback` confirmed `+` in
  `targets/f7/api_symbols.csv`.
- Absence of Bluetooth audio, USB Audio Class, USB host and any DAC API:
  negative results against the same API table and
  `lib/libusb_stm32/inc/` directory listing.
- Windows USB logging behaviour (EID 6416 in the Security log firing on
  every connection with VID/PID/serial; EID 20001/20003 on first
  install; `setupapi.dev.log` recording vendor name, product name and
  serial; `HKLM\...\Enum\USB` registry persistence) from Microsoft
  Learn "USB Device Descriptors" and "USB device-specific registry
  settings", NXLog's Windows USB auditing guide, and two USB-forensics
  write-ups (cyberengage.org; MDPI Electronics 8(11):1322).
- **The final hop — that such logs reach an LLM-backed triage pipeline
  which then ingests the string — is an assumption, not a verified
  fact.** It is the premise the feature rests on and the thing the
  one-hour experiment plus a follow-up conversation with a SOC team
  should establish.
- No code was written or built for this assessment.
