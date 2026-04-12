# STRE-TR v1.4

STRE-TR is a real-time time-stretch and freeze effect with four engines:
- `STRETCH` (WSOLA)
- `GRAIN`
- `FFT1`
- `FFT2`

It combines stretch intensity, independent pitch-rate control, reverse playback, wet-only filtering, chaos modulation, stereo routing, and a dual-stage limiter inside the same compact UI language as the rest of the series.

## Concept

STRE-TR is designed as a playable stretch instrument rather than a clinical offline stretcher.

- `AMOUNT` controls engine advance or hold intensity. It is not a fixed "1x to 4x" ratio.
- `MOD` controls pitch-rate independently of `AMOUNT`.
- `TRG` arms the engine. With `TRG` off, the wet path is dry passthrough.
- `ALIGN` and `PDC` are only meaningful when an FFT engine is active, because that is where latency exists.

## Engines

### STRETCH

WSOLA-based elastic time stretching with overlap search and crossfade between segments.

- Best for more direct, time-domain stretching.
- Supports reverse.
- `AMOUNT` reduces analysis advance toward freeze.

### GRAIN

Granular stretcher with up to 64 active grains and Hann envelopes.

- Best for textural and broken-up results.
- Supports reverse.
- `GRAIN` controls grain length in milliseconds.
- `AMOUNT` reduces read advance toward freeze.

### FFT1

Phase-vocoder engine with overlap-add reconstruction and pitch-rate control.

- Best for spectral stretching and pitched material.
- Supports reverse.
- `AMOUNT` reduces FFT analysis hop toward freeze.

### FFT2

Spectral-hold engine built on the same FFT framework.

- Best for frozen or semi-frozen spectral textures.
- `RVS` is intentionally disabled here.
- `AMOUNT` increases hold intensity rather than moving an analysis hop toward zero.

## Interface

STRE-TR uses the same text-first horizontal bar language as the rest of the series.

- Left view: `AMOUNT`, `MOD`, `GRAIN`, `ENGINE`, `WINDOW`, `STYLE`
- Expanded IO view: `INPUT`, `OUTPUT`, `TILT`, `PAN`, `MIX`, `LIM`
- Bottom controls: routing, limiter mode, invert modes, mix mode, filter/tilt position
- Toggle rows: `ALIGN`, `PDC`, `RVS`, `TRG`
- Chaos row in the expanded view: `CHSF`, `CHSD`
- Right-click numeric prompt on bars and supported controls
- Gear icon for the info/graphics popup
- Resizable editor with persisted size and graphics state

## Signal Flow

The wet path is processed in this order:

1. Input gain
2. `MODE IN`
3. PRE filter and/or PRE tilt if selected
4. Circular input buffer write
5. Selected engine
6. `STYLE`
7. POST filter and/or POST tilt if selected
8. `CHAOS D`
9. `MODE OUT`
10. Engine crossfade
11. DC blocker
12. Output gain
13. `LIMIT` in `WET` mode if enabled
14. `INV POL` / `INV STR` in `WET` mode if enabled
15. Dry/wet mix plus `SUM BUS`
16. `PAN`
17. `LIMIT` in `GLOBAL` mode if enabled
18. `INV POL` / `INV STR` in `GLOBAL` mode if enabled
19. Safety hard clip

`CHAOS F` does not sit as a separate audio stage; it modulates the wet filter cutoff targets.

## Parameters

### AMOUNT (0-100%)

Controls how far the active engine moves away from normal read/analysis advance and toward freeze or hold.

- `STRETCH`, `GRAIN`, `FFT1`: `0%` = normal advance, `100%` = freeze
- `FFT2`: `0%` = minimal hold, `100%` = strongest hold

### MOD (0.0625x to 16x)

Pitch-rate control centered at `1.0x`.

- `0.5` internal = `1.0x`
- `0.0` internal = `0.0625x`
- `1.0` internal = `16x`

It is smoothed per sample.

### GRAIN (1-500 ms)

Grain length for the `GRAIN` engine.

- Only active for `ENGINE = GRAIN`
- Display is dimmed and disabled in other engines

### ENGINE

Selects the active engine:

- `0` = `STRETCH`
- `1` = `GRAIN`
- `2` = `FFT1`
- `3` = `FFT2`

### WINDOW (16-8192)

Shared window/segment size control.

- `STRETCH` and `GRAIN` use the smoothed value directly
- `FFT1` and `FFT2` snap it to the next power of two, with a minimum effective FFT size of `64`

### STYLE

Stereo behavior of the wet signal:

- `MONO`: collapses wet output to mono
- `STEREO`: normal stereo behavior
- `WIDE`: per-engine decorrelation plus extra side boost
- `DUAL`: independent left/right trajectories where supported

### INPUT (-100 to 0 dB)

Pre-engine gain.

### OUTPUT (-100 to +24 dB)

Wet output gain before mix routing.

### MIX (0-100%)

Insert dry/wet crossfade.

### MIX MODE

- `INSERT`: uses the main `MIX` slider
- `SEND`: uses independent `DRY LEVEL` and `WET LEVEL`

`SEND` dry/wet levels are now smoothed like the rest of the series.

### FILTER POS

Independent PRE/POST placement of the wet filter and wet tilt stages:

- `F post / T post`
- `F pre / T pre`
- `F pre / T post`
- `F post / T pre`

### HP / LP FILTER

Wet-only filter block.

- Frequency range: `20 Hz - 20000 Hz`
- Slope options: `6`, `12`, `24 dB/oct`
- Independent enable for HP and LP

### TILT (-6 to +6 dB)

Wet-only first-order tilt around `1 kHz`.

### PAN

Stereo pan applied after dry/wet summing.

- Center remains unity
- Pan motion is smoothed sample by sample

### TRG

Engine arm/trigger behavior.

- `OFF`: wet path is passthrough
- `ON`: selected engine is active
- Rising edge resets the engine read state

### RVS

Reverse playback/read direction.

- Active in `STRETCH`, `GRAIN`, and `FFT1`
- Disabled in `FFT2`

### ALIGN

Delays the dry path by the active FFT latency so dry and wet remain time-aligned when an FFT engine is running.

### PDC

Reports FFT latency to the host.

- `ON`: host latency compensation follows the active FFT size
- `OFF`: no latency is reported

### MODE IN / MODE OUT

Wet-path encoding before and after the engine:

- `L+R`
- `MID`
- `SIDE`

### SUM BUS

How the wet contribution is injected after dry/wet mix:

- `ST`
- `->M`
- `->S`

### CHAOS D

Hermite-interpolated random micro-delay plus gain wobble on the wet path.

- `Amount`: up to about `50 ms` delay span and moderate gain drift
- `Speed`: `0.01-100 Hz`

### CHAOS F

Hermite-interpolated random modulation of HP/LP filter cutoffs.

- `Amount`: up to about `+/-2 octaves`
- `Speed`: `0.01-100 Hz`

### LIM THRESHOLD (-36 to 0 dB)

Threshold for the transparent peak limiter.

### LIM MODE

- `NONE`
- `WET`
- `GLOBAL`

Limiter threshold is now smoothed in both insertion modes.

### INV POL / INV STR

Independent inversion modes for polarity and stereo:

- `NONE`
- `WET`
- `GLOBAL`

## Technical Notes

- Input buffer: up to `262144` samples
- `STRETCH`: WSOLA with overlap search and crossfade
- `GRAIN`: up to `64` grains with Hann envelopes
- `FFT1`: phase vocoder
- `FFT2`: spectral hold built on the FFT engine
- Wet filter: HP/LP biquads with periodic coefficient updates
- Tilt: first-order wet tilt
- Chaos: Hermite-interpolated random targets with drift
- Limiter: dual-stage, stereo-linked
  - Stage 1: `2 ms` attack / `10 ms` release
  - Stage 2: instant attack / `100 ms` release
- Safety stage: hard clip at about `+48 dBFS`

## Smoothing

STRE-TR currently smooths the user-facing continuous controls that matter for fast GUI movement:

- `INPUT`
- `OUTPUT`
- `MIX`
- `SEND DRY LEVEL`
- `SEND WET LEVEL`
- `LIM THRESHOLD`
- `WINDOW`
- `AMOUNT` -> engine speed/hold behavior
- `MOD` -> pitch rate
- `PAN`

Filter, tilt, and chaos subsystems also have their own internal smoothing/update logic.

## Notes

- `ALIGN` and `PDC` matter only for the FFT engines
- `TRG` is central to the creative workflow of this plugin; with `TRG` off the wet path stays clean
- `FFT2` is the only engine that disables `RVS`

## Changelog

### v1.4

- Added the current limiter, routing, chaos, and UI workflow
- Kept `STRETCH`, `GRAIN`, `FFT1`, and `FFT2` under the same editor
- Smoothed `SEND DRY/WET` and `LIM THRESHOLD` to match the rest of the series
- Cleaned documentation and removed stale or incorrect legacy descriptions
