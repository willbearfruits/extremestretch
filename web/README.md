# Forty-Two Years of Happy Birthday

A birthday song stretched across 42 years, published as a web page.

Nothing is stored and nothing is streamed. The page works out where the piece
has got to from the wall clock, then synthesises paulstretch in the browser from
that position. Arrive in 2047 and you hear the middle of the song, because the
middle of the song is where 2047 is — not because anything has been playing.

```
elapsed        = now − epoch
sourcePosition = elapsed / 36,714,000
```

## Behaviour

Sound starts **on arrival**, without asking. Browsers block that until the
visitor has interacted with the site at least once, so it falls back to starting
on the first gesture — any tap, click or keypress, anywhere. `[ listen ]` /
`[ silence ]` toggles it; silencing stops the buffers already scheduled ahead,
not just the queueing of new ones, and starting again re-derives the position
from the clock so you rejoin where the piece has got to.

The field keeps living whether or not there is sound.

Laid out as a **shrine**: one vertical axis, everything centred on it, the field
mirrored about that axis with the lowest frequencies at the centre so it builds
a symmetrical mass rather than scanning left to right. A darker elliptical
niche is carved out of the density for the inscription to sit in, and the glow
gathers around it. Works down to 360 px wide; the niche widens in portrait so
the text never lands on the light.

## The look

Character-only, per the house rule: **every visible form is a typed glyph.**
The field is `fillText` on a canvas — density ramp `·˙:░▒▓█` with CP437/maths
glyphs substituted through the middle tiers — and the informational layer is
plain monospace text with a character progress bar `▓▓▓░░░`. No images, no SVG,
no `arc`/`rect`-as-art, no CSS shapes carrying the design. Colour, layout and a
legibility text-shadow are the only things CSS does.

What the field shows is **the spectrum of the frame currently sounding**: each
column is a log-spaced frequency band, lit top to bottom in proportion to its
energy, with slow decay so history layers up and a dust floor so no region of
the screen is ever dead.

**Zalgo is mapped to elapsed time.** Combining marks accumulate on hot cells as
the piece ages — one mark in 2026, a thicket of five or six by 2068. The field
literally corrodes across the forty-two years. Check it with `?at=`.

Both the field's shimmer and the audio's phases are seeded from the same
clock-derived index, so the picture flickers for exactly the reason the sound
does, identically for everyone.

## Deploy

Two files: `index.html` and `source.ogg`. Any static host will do.

```sh
# locally
python3 -m http.server 8000
```

On GitHub Pages it publishes itself: `.github/workflows/pages.yml` uploads this
folder whenever anything in `web/` lands on `main`. Pages can only serve `/` or
`/docs` when deploying from a branch, which is why it goes through a workflow
rather than a folder setting.

It must be served over **http/https** — opening `index.html` off disk fails,
because the browser refuses to `fetch` the audio from `file://`.

## Configure

At the top of the `<script>` block in `index.html`:

| constant | meaning |
| --- | --- |
| `EPOCH` | when the piece began. **Set this once and never touch it** — changing it moves the whole piece |
| `TOTAL_YEARS` | 42 |
| `SOURCE_URL` | the audio file |
| `WINDOW_SIZE` | FFT size, power of two. Longer is glassier. 65536 ≈ 1.5 s at 44.1 kHz |
| `WIDTH` | stereo phase decorrelation, 0–1 |

## Previewing the future

You cannot wait 21 years to check it works, so append `?at=`:

```
?at=21y     21 years in — the middle
?at=50%     same thing
?at=6months
?at=90      90 seconds in
```

This moves the audio as well as the readout, so you really are auditioning
2047.

## How it stays in sync

Every listener derives the frame index from the clock, and the random phases are
seeded from that frame index. So two people whose clocks agree to within one
frame (~0.74 s) render bit-identical audio — it is a broadcast, not a copy each
person happens to have. The page also reads the `Date` header off its own
request and trusts that over the visitor's clock.

## Why it sounds like that

At 36.7 million ×, the read head advances **one sample of source roughly every
14 minutes**. A day of listening moves you 2.3 ms into the song; a year moves
you 0.86 seconds. It is not really a time-stretch at that scale, it is a very
slowly rotating chord.

Because the analysis frame is bit-for-bit identical for thousands of
consecutive output frames, the forward FFT only runs when `floor(position)`
actually moves — roughly four times a day. Everything else is one inverse FFT
per channel per frame. That is exact, not an approximation, and it is what makes
this run comfortably on a phone.

The window is Paul's `sqrt(1 − |x|^2.5)`, not a Hann. Random phases make frames
add in *power*, so `sum(w²)` has to stay flat across the 50% overlap; a Hann
there tremolos audibly.

## Before you publish

The source is **"It's Your Birthday" by Monk Turner + Fascinoma**, which is
distributed under Creative Commons — but check the exact licence and its
attribution terms yourself before putting this on a public URL, and keep the
credit that is already in the page footer.

## Prior art

Jem Finer's *Longplayer* has run since 1 Jan 2000 on a 1000-year cycle and
computes its position from the clock in exactly this way. John Cage's *Organ²/
ASLSP* in Halberstadt is 639 years long and treats chord changes as events you
travel to attend.
