# STRE-TR v1.4

STRE-TR is a real-time time-stretch and freeze effect with four engines:
- `STRETCH` (WSOLA)
- `GRAIN`
- `FFT1`
- `FFT2`

It combines stretch intensity, independent semitone pitch control, reverse playback, wet-only filtering, chaos modulation, stereo routing, and a dual-stage limiter inside the same compact UI language as the rest of the series.

## Concept

STRE-TR is designed as a playable stretch instrument rather than a clinical offline stretcher.

- `AMOUNT` controls engine advance or hold intensity. It is not a fixed "1x to 4x" ratio.
- `PITCH` controls pitch-rate in semitones independently of `AMOUNT`.
- `TRG` arms the engine. With `TRG` off, the wet path is dry passthrough.
- `ALIGN` delays the dry path only for FFT1/FFT2 latency alignment.
- `PDC` only reports FFT1/FFT2 latency to the host; it does not change the DSP path.

## Engines

### STRETCH

WSOLA-based elastic time stretching with overlap search and crossfade between segments.

- Best for more direct, time-domain stretching.
- Supports reverse.
- `AMOUNT` reduces analysis advance toward freeze.
- Near neutral settings can fall back to a unity-safe path instead of forcing continuous resegmentation.

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
- `WINDOW` is mapped to power-of-two FFT sizes internally.
- Full reverse hold is handled as a direct reverse read so `100%` behaves consistently with nearby values.

### FFT2

Spectral-hold engine built on the same FFT framework.

- Best for frozen or semi-frozen spectral textures.
- Supports reverse.
- `AMOUNT` increases hold intensity rather than moving an analysis hop toward zero.
- Uses its own stored window value and maps it to power-of-two FFT sizes internally.

## Interface

STRE-TR uses the same text-first horizontal bar language as the rest of the series.

- Left view: `AMOUNT`, `PITCH`, `JIT`, `GRAIN`, `ENGINE`, `WINDOW`, `STYLE`
- Expanded IO view: `INPUT`, `OUTPUT`, `TILT`, `PAN`, `MIX`, `LIM`
- Bottom controls: routing, limiter mode, invert modes, mix mode, filter/tilt position
- Toggle rows: `ALIGN`, `PDC`, `RVS`, `TRG`
- Chaos row in the expanded view: `CHSF`, `CHSD`
- Right-click numeric prompt on bars and supported controls
- `WINDOW` is edited from its bar only; it does not open a numeric prompt
- `PDC` tooltip shows `MAX WIN`, and right-click opens the FFT max-window prompt
- Gear icon for the info/graphics popup
- Resizable editor width with new instances opening at `360 x 752`; width and graphics state persist, height stays fixed

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

### PITCH (-24st to +24st)

Pitch-rate control centered at `1.0x`.

- `0.5` internal = `1.0x`
- `0.0` internal = `-24st` / `0.25x`
- `1.0` internal = `+24st` / `4x`

It is smoothed per sample.

### JIT (0-100%)

Organic jitter/instability for the active stretch engine.

- Acts as its own depth control rather than a secondary `AMOUNT` multiplier
- Adds deterministic pitch-rate drift in all engines
- Adds safe grain-length and grain-anchor movement in `GRAIN`
- Keeps non-pitch grain motion bounded around neutral settings to avoid discontinuities
- Uses a stronger calibrated depth in `STRETCH` and `GRAIN`, while `FFT1`/`FFT2` keep their own spectral jitter scale
- Uses deterministic drift and smoothed sample-and-hold sources

### GRAIN (1-500 ms)

Grain length for the `GRAIN` engine.

- Only active for `ENGINE = GRAIN`
- Display is dimmed and disabled in other engines
- In non-`GRAIN` engines, the label remains visible but the numeric value is hidden

### ENGINE

Selects the active engine:

- `0` = `STRETCH`
- `1` = `GRAIN`
- `2` = `FFT1`
- `3` = `FFT2`

### WINDOW (16-8192)

Shared UI slot with independent stored values per engine.

- `STRETCH` uses the smoothed value directly
- `GRAIN` uses the smoothed value directly up to a safe effective maximum of `2048`
- `FFT1` and `FFT2` snap it to valid power-of-two FFT sizes, with a minimum effective FFT size of `64`
- `WINDOW` has no numeric prompt; FFT windows are selected as stepped values
- `MAX WIN` is configured from the `PDC` right-click prompt and clamps FFT1/FFT2 window choices
- Default `MAX WIN` is `2048`

### STYLE

Stereo behavior of the wet signal:

- `MONO`: collapses wet output to mono
- `STEREO`: normal stereo behavior
- `WIDE`: per-engine decorrelation plus extra side boost
- `DUAL`: independent left/right trajectories where supported

### INPUT (-INF to +24 dB)

Pre-engine gain.
The fader floor is -144 dB, displayed as -INF; 0 dB is centered on the control.

### OUTPUT (-INF to +24 dB)

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

- Active in `STRETCH`, `GRAIN`, `FFT1`, and `FFT2`

### ALIGN

Delays the dry path by the FFT1/FFT2 latency budget so dry and wet remain time-aligned in FFT engines.

`STRETCH` and `GRAIN` do not use `ALIGN`.

### PDC

Reports FFT1/FFT2 latency to the host when compensation is enabled.

- `ON`: host latency compensation follows the FFT latency budget
- `OFF`: no latency is reported
- `STRETCH` and `GRAIN`: no latency is reported
- PDC does not add internal padding or change the engine behavior
- Right-click opens the `MAX WIN` prompt, which sets the maximum FFT latency budget used by `PDC` and `ALIGN`

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

### LIM (-36 to 0 dB)

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
- `STRETCH`: WSOLA with overlap search, segment crossfade, and unity-safe bypass near neutral motion
- `GRAIN`: up to `64` grains with Hann envelopes and deterministic trigger/loop state
- `FFT1`: phase vocoder with freeze reached by reducing FFT analysis advance, plus signed reverse phase tracking
- `FFT2`: spectral hold built on the FFT engine, with hold intensity controlled by `AMOUNT` and signed reverse phase tracking
- `JIT`: deterministic per-channel drift/S&H modulation, mapped to `PITCH`/pitch-rate and safe granular read geometry, with stronger `STRETCH`/`GRAIN` depth than the FFT engines
- Wet filter: HP/LP biquads with periodic coefficient updates
- Tilt: first-order wet tilt
- Chaos: Hermite-interpolated random targets with drift
- Limiter: dual-stage, stereo-linked leveler + brickwall design
- Safety stage: hard clip at about `+48 dBFS`
- Developer diagnostics and CSV trace dumps are opt-in and intended for debug builds, not normal release behavior

## Smoothing

STRE-TR currently smooths the user-facing continuous controls that matter for fast GUI movement:

- `INPUT`
- `OUTPUT`
- `MIX`
- `SEND DRY LEVEL`
- `SEND WET LEVEL`
- `LIM`
- `WINDOW`
- `AMOUNT` -> engine speed/hold behavior
- `PITCH` -> pitch rate
- `JIT` -> engine jitter depth
- `PAN`

Filter, tilt, and chaos subsystems also have their own internal smoothing/update logic.

## Notes

- `ALIGN` and `PDC` are FFT-only and are intentionally independent from the sound of the engine
- `TRG` is central to the creative workflow of this plugin; with `TRG` off the wet path stays clean
- `WINDOW` uses one UI slot but stores independent values for `STRETCH`, `GRAIN`, `FFT1`, and `FFT2`
- `FFT1` and `FFT2` quantize `WINDOW` to power-of-two FFT sizes internally, while `STRETCH` uses the smoothed value directly and `GRAIN` caps its effective window at `2048`

## Changelog

### v1.4

- Added the current limiter, routing, chaos, and UI workflow
- Kept `STRETCH`, `GRAIN`, `FFT1`, and `FFT2` under the same editor
- Smoothed `SEND DRY/WET` and `LIM` to match the rest of the series
- Hardened `PDC` so it reports latency without changing the underlying engine behavior
- Made `WINDOW` state independent per engine while keeping a single compact UI slot
- Stabilized `AMOUNT`/`PITCH` automation consistency across `STRETCH`, `GRAIN`, `FFT1`, and `FFT2`
- Hardened FFT output normalization and FFT1 freeze transitions to avoid edge-case automation clicks on large windows
- Added `JIT` as a deterministic organic-motion control for pitch drift and granular instability, with calibrated `STRETCH`/`GRAIN` depth and separate FFT scaling
- Improved FFT reverse behavior, including FFT1 full-reverse hold and signed reverse phase tracking
- Fixed `DUAL` pitch mapping in FFT routes so each channel uses its own pitch rate consistently
- Fixed the `GRAIN` size row so it is enabled only for the `GRAIN` engine
- Capped the effective `GRAIN` window at `2048` and set the default FFT `MAX WIN` budget to `2048`
- Cleaned documentation and removed stale or incorrect legacy descriptions
- Release hardening: developer traces are now opt-in and kept out of normal release behavior
