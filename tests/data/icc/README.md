# The ICC corpus

Six real ICC profiles and the fixtures that carry them.

## Why it exists

Before this directory, **no fixture in this repository carried a profile a
CMM could parse.** Three claimed to:

| fixture | "profile" |
|---|---|
| `tests/data/png/png_iccp.png` | the eleven bytes `b"minimal_icc"` |
| `tests/data/jpeg/jpeg_with_icc.jpg` | a 128-byte header with no tag table |
| `tests/data/tiff/tiff_4x4_metadata.tif` | 296 bytes of plausible-looking filler |

Those are correct fixtures for the question they were written to ask - *does
an opaque blob survive a round trip* - and they stay, at their awkward sizes,
because a real profile is never 11 bytes and would not test that path.

What they cannot do is tell a colour transform from one that does nothing.
A scan of every image under `tests/data` and `third_party` - 551 files, so
counting the gitignored oracle caches as well as the committed fixtures -
found **zero** parseable non-sRGB profiles. A colour engine built against
that corpus would have been unfalsifiable: apply a perfect transform or the
identity and all 551 agree.

## The profiles

Written by `generate.py`, which computes its colorants from CIE
chromaticities with a Bradford adaptation to D50.

| profile | space | primaries | curve |
|---|---|---|---|
| `srgb_g22.icc` | RGB | sRGB / BT.709 | gamma 2.2 |
| `adobergb_g22.icc` | RGB | Adobe RGB (1998) | gamma 2.2 |
| `displayp3_g22.icc` | RGB | Display P3 | gamma 2.2 |
| `srgb_sampled_trc.icc` | RGB | sRGB / BT.709 | 1024-point sampled sRGB curve |
| `gray_g22.icc` | GRAY | — | gamma 2.2 (`kTRC`) |
| **`swap_rg.icc`** | RGB | **sRGB's green as red and red as green** | gamma 1.0 |

### The discriminating one

`swap_rg.icc` is the reason the others are not enough. Every profile above
is close enough to sRGB that a *missing* transform looks like a rounding
difference - which is exactly the failure a corpus is supposed to catch.
`swap_rg.icc` cannot be ignored quietly: convert a red pixel through it into
sRGB and littleCMS returns `(0, 255, 0)`.

## How it is checked, and by what

`verify_icc.py` runs in `make test` (or on its own with
`make test-verify-icc`) and re-execs into the pinned Pillow image, so the
answers come from **littleCMS 2.16**, not from whatever the host has:

- every profile parses, and its description reads back;
- the colorants sum to the media white point, the property that makes a set
  of matrix colorants well formed;
- the computed colorants agree with the **published** figures transcribed by
  hand in `src/color/icc_synth.c`, which is a genuine cross-check: one side
  is computed here, the other transcribed there, and a disagreement
  implicates whichever is wrong;
- `swap_rg.icc` actually exchanges red and green;
- `adobergb_g22.icc` actually moves a colour.

That last one took three attempts and the reason is worth keeping. Saturated
green `(0, 255, 0)` comes back unchanged - not because nothing happened, but
because it is out of gamut in sRGB, so relative colorimetric clips it to
sRGB's own maximum green, which has the same coordinates. `(0, 128, 0)` moves
by one, because a colour on a primary axis stays on that axis. Only a colour
mixed from all three channels separates the two readings: `(160, 96, 64)`
moves by twenty. **A probe has to be in the set where the two answers
differ, and a primary is not.**

## The fixtures

| fixture | carries | why |
|---|---|---|
| `png/png_icc_swap_rg.png` | `swap_rg.icc` | iCCP, the discriminating profile |
| `png/png_icc_adobergb.png` | `adobergb_g22.icc` | iCCP, an ordinary wide gamut |
| `png/png_icc_sampled_trc.png` | `srgb_sampled_trc.icc` | iCCP, and long enough to split across JPEG's APP2 segments on a re-save |
| `jpeg/jpeg_icc_swap_rg.jpg` | `swap_rg.icc` | APP2 |
| `tiff/tiff_icc_swap_rg.tif` | `swap_rg.icc` | tag 34675 |
| `bmp/bmp_4x4_v5_icc_swap_rg.bmp` | `swap_rg.icc` | V5 `PROFILE_EMBEDDED` |

`tests/unit/test_icc_corpus.cpp` asserts what this library can actually be
held to, which is carriage and not correction: each fixture decodes to the
profile on disk **byte for byte**, and a save into a different container
preserves it - eight crossings, including the 6668-byte one through JPEG's
segmentation.

## Regenerating

```
python3 tests/data/icc/generate.py     # then each format's generate.py
```

The profiles are byte-stable: the date field is zeroed and no profile ID is
written, so a regeneration that changes nothing produces no diff.
