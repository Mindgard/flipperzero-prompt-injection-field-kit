# Contributing

## Pull requests are not being accepted yet

This is an initial public release and Mindgard is not taking outside code
contributions at this stage. Pull requests will be closed unmerged — not
because they are unwelcome in spirit, but because accepting them without a
contributor licence agreement in place would leave the copyright position
unclear.

**Bug reports and feature requests are very welcome** — please open an issue.
Reports that include the Flipper firmware version, the channel in use, and
what the target did are the most actionable.

If a CLA is put in place later, this section will say so.

## Building

```bash
git clone https://github.com/Mindgard/flipperzero-prompt-injection-field-kit
cd flipperzero-prompt-injection-field-kit
ufbt          # build the FAP
ufbt launch   # build, install to the SD card, and start it
```

`ufbt` fetches the Flipper SDK on first run. See
[docs/MANUAL.md](docs/MANUAL.md) for prerequisites.

## Tests

Eight host-side suites run without a Flipper, under ASan and UBSan. The
commands are listed in the [README](README.md#testing); all of them must exit
zero and report no sanitiser findings.

Two channels — GPIO and BLE GATT — can only be verified against real
hardware. `docs/gpio-validation.md` records the logic-analyser procedure and
keeps its raw captures in `docs/captures/`, so the timing claims can be
re-checked rather than taken on trust.

## Style

`ufbt format` before committing; the tree follows the Flipper firmware's
`.clang-format`. Warnings are errors in the FAP build (`-Wall -Wextra
-Werror`), so a build that emits one will not link.
