# Menu icons

9x9 one-bit PNGs, one per menu row. Drawn by hand on a pixel grid rather
than exported from vector art — see below for why.

## Why 9x9

Rows are 12 px tall and the screen is 64 px, so four rows are visible
under the header. A 14 px icon, which is what Flipper's own app-category
icons are, forces the row to 14 px and drops the window to three. On the
payload lists that is a quarter of the visible library, which costs more
than the extra legibility is worth.

## Relationship to the official Flipper icons

Four of these are redrawn from the official set (`badusb`, `gpio`, `nfc`,
`settings` in Flipper's icon download):

| ours | official motif |
|---|---|
| `mg_keyboard` | BadUSB — keycaps, without the bezel |
| `mg_wires` | GPIO — pin pairs over a connector block, two pairs not three |
| `mg_reader` | NFC — tag at the left, arcs radiating right |
| `mg_settings` | Settings — the wrench half of its crossed screwdriver and wrench |

`mg_settings` was a gear first, and it read badly. The official glyph is
not a gear: it is a screwdriver and a wrench crossed, and the gear was a
reduction of an assumption rather than of the artwork. Taking half the
motif is deliberate — at 9x9 two crossed shapes compete for the same
pixels, while one silhouette stays readable. The jaw is upright rather
than angled because it needs a 1 px gap between two 2 px teeth to stay
open on an LCD, and the diagonal version closes it up.

They are **redrawn, not resampled**. The official files are pixel maps
expressed as SVG paths on an integer grid; rasterising at 1:1 is exact,
but a 14→9 downscale produced 32–51 grey levels on a display that has
two. Every pixel here is a decision rather than an averaging artefact.

The remaining icons have no official counterpart, because the official
set describes *applications* — Sub-GHz, iButton, Infrared, U2F, Games —
while these describe what an operator physically does to a target. One
NFC icon cannot distinguish "Tap a phone" from "Tap a reader", and that
distinction is what the navigation is built on.

## Design constraints

- **Solid silhouettes.** Single-pixel detail disappears next to 8 px
  text on a 128x64 LCD.
- **Distinguishable by outline**, not by interior texture. Two icons that
  differ only in their middle read as the same mark in the field.
- **Five payload families, not seventeen categories.** At 9x9 the
  difference between "encoding" and "obfuscation" cannot be drawn, and a
  mark you cannot tell from its neighbour is worse than none — it looks
  like information.

## Editing

The glyphs were generated from ASCII art by a throwaway script; there is
no build step and the PNGs are the source of truth. To change one, edit
the PNG as 1-bit black-on-white — black is ink — or regenerate it from a
9x9 grid of `#` and `.`. `fap_icon_assets` in `application.fam` compiles
everything in `images/` into `I_<filename>` symbols automatically.

Because that sweep is by directory rather than by reference, an unused
PNG left in `images/` still ships inside the `.fap`. Delete rather than
orphan.

## The launcher icon

`../icon_10x10.png` is the app-menu icon named by `fap_icon`, not part of
this set, but it follows the same rules: 10x10, mode `L`, pure black on
white. It is a speech bubble — the subject is what gets said to a model,
and the glyph carries no logo so the kit is not branded.
