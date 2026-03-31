# STRE-TR v1.4

<br/><br/>

STRE-TR is a real-time time-stretching audio effect with three selectable DSP engines — WSOLA, Granular, and FFT Phase Vocoder.  
It combines elastic time-stretch, pitch shifting, and spectral freezing with filter shaping, chaos modulation, and a minimal CRT-inspired interface.

## Concept

STRE-TR treats time-stretching as a playable instrument. The AMOUNT control maps a continuous 1×–4× stretch ratio across all engines, while MOD independently scales pitch from ×0.0625 to ×16 without affecting stretch rate.

Three engines offer different trade-offs:
- **STRETCH (WSOLA)** — time-domain overlap-add with cross-correlation search. Transparent, elastic stretching with preserved transients. Trigger-gated.
- **GRAIN** — polyphonic granular synthesis with up to 64 simultaneous Hann-windowed grains. Textural, stochastic stretching. Trigger-gated.
- **FFT** — STFT phase vocoder with Laroche & Dolson phase correction. Spectral time-freezing. Always active — at 100% the signal freezes on the last analysis frame.

Trigger mode gates STRETCH and GRAIN engines: when TRG is OFF, the engine advances through the input buffer; when ON, it freezes at the current position and resets on each rising edge.

## Interface

STRE-TR uses a text-based UI with horizontal bar sliders. All controls are visible at once — no pages, tabs, or hidden menus.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: TRG, RVS, ALIGN, PDC. Click to enable/disable.
- **Collapsible INPUT/OUTPUT/MIX section**: Click the toggle bar (triangle) to swap between main parameters and the INPUT, OUTPUT, MIX controls. State persists across sessions.
- **Filter bar**: Visible in the INPUT/OUTPUT/MIX section. Click to open the HP/LP filter configuration prompt with frequency, slope, and enable/disable controls for each filter.
- **Gear icon** (top-right): Opens the info popup with version, credits, and a link to Graphics settings.
- **Graphics popup**: Toggle CRT post-processing effect and switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

The value column to the right of each slider shows the current state in context:
- AMOUNT shows percentage.
- MOD shows the pitch multiplier.
- GRAIN shows milliseconds (dimmed when engine ≠ GRAIN).
- ENGINE shows STRETCH/GRAIN/FFT.
- WINDOW shows sample count.
- STYLE shows MONO/STEREO/WIDE/DUAL.
- INPUT/OUTPUT show dB values.
- MIX shows percentage.

## Parameters

### AMOUNT (0–100%)

Time-stretch ratio. Maps linearly: 0% = 1× (no stretch), 50% = 2×, 100% = 4×.

For the FFT engine, Amount controls the analysis hop: `analysisHop = synthesisHop × (1 − Amount/100)`. At 100%, no new analysis frames are read — the output freezes on the last spectral frame.

For WSOLA and Granular, higher values increase the stretch ratio applied to segment/grain playback.

### MOD (×0.0625–×16.0)

Pitch multiplier applied independently of time-stretch.  
Stored as 0–1 internally: 0 = ×0.0625 (−4 octaves), 0.5 = ×1.0 (no shift), 1.0 = ×16 (barrel shift).  
Smoothed per-sample for glitch-free sweeps.

### GRAIN (1–500 ms)

Grain size for the Granular engine. Controls the length of individual grains in milliseconds.  
Larger values produce smoother stretching; smaller values produce more granular, glitchy textures.  
This slider is dimmed and disabled when ENGINE is not set to GRAIN.

Default: 100 ms. Skew: 0.35 (lower range has finer resolution).

### ENGINE

Selects the active DSP engine:
- **STRETCH** (0): WSOLA — time-domain cross-correlation overlap. Best for transparent stretching.
- **GRAIN** (1): Granular synthesis — polyphonic Hann-windowed grains. Best for textural effects.
- **FFT** (2): Phase vocoder — STFT spectral processing. Best for spectral freeze and pitched content.

Default: STRETCH.

### WINDOW

Segment/FFT size shared by all engines:

| Index | Samples |
|-------|---------|
| 0     | 128     |
| 1     | 256     |
| 2     | 512     |
| 3     | 1024    |
| 4     | 2048    |
| 5     | 4096    |
| 6     | 8192    |

Larger windows give better frequency resolution (smoother FFT, less noise) but higher latency and slower transient response. Smaller windows track transients better but introduce more spectral artifacts.

Default: 1024 (index 3).

### STYLE

Stereo shaping mode applied to the processed signal:
- **MONO** (0): Collapse L+R to mono: `out = (L + R) × 0.5`.
- **STEREO** (1): Pass through unchanged.
- **WIDE** (2): Exaggerate stereo width: mid unchanged, side × 1.5.
- **DUAL** (3): Independent left/right processing.

Default: STEREO.

### INPUT (−100 to 0 dB)

Pre-effect gain. Displays "−INF" at −80 dB or below.  
Skew: 2.5 (finer resolution near 0 dB).

### OUTPUT (−100 to +24 dB)

Post-effect gain applied to the wet signal.  
Skew: 3.23.

### MIX (0–100%)

Dry/wet balance. 0% = fully dry, 100% = fully wet.  
Default: 100%.

### HP/LP FILTER

High-pass and low-pass filters applied to the wet signal, accessible via the filter bar in the IO section.

- **HP FREQ (20–20 000 Hz)**: High-pass cutoff frequency. Default: 250 Hz.
- **LP FREQ (20–20 000 Hz)**: Low-pass cutoff frequency. Default: 2000 Hz.
- **HP SLOPE / LP SLOPE**: 6 dB/oct (one-pole), 12 dB/oct (second-order Butterworth), or 24 dB/oct (two cascaded second-order Butterworth stages). Default: 12 dB/oct.
- **HP / LP toggles**: Enable or disable each filter independently.

### TILT (−6 to +6 dB)

Spectral tilt applied to the wet signal. A first-order symmetric shelf filter pivoted at 1 kHz.  
Positive values boost highs and cut lows; negative values cut highs and boost lows.  
Default: 0 dB.

### PAN (L–C–R)

Equal-power stereo panning. 0 = hard left, 0.5 = center, 1.0 = hard right.  
Default: center.

### TRG (Trigger)

Gate mode for STRETCH and GRAIN engines.  
- OFF: Engine advances through the input buffer continuously.
- ON: Freezes at the current buffer position. Edge-triggered — resets read position on each OFF→ON transition.

The FFT engine is always active and does not use the trigger.

### RVS (Reverse)

Reverses the read direction in the input buffer:
- **WSOLA**: Analysis hop direction reversed.
- **Granular**: Grains spawn and play backward.
- **FFT**: Analysis read position decrements.

### ALIGN (default: ON)

Dry/wet phase alignment. When ON, the dry signal is delayed by the FFT engine's processing latency (equal to the FFT window size) so dry and wet are time-coherent. When OFF, the dry signal is undelayed — useful as a creative effect at intermediate MIX values.

### PDC (default: ON)

Plugin Delay Compensation. When ON, reports the FFT engine's latency (equal to the FFT window size in samples) to the DAW, which compensates by shifting the plugin's output forward in time. When OFF, no latency is reported.

### MODE IN / MODE OUT

Signal encoding applied before (Mode In) and after (Mode Out) the stretch engine:
- **L+R** (0): Stereo pass-through.
- **MID** (1): Extract mid: `(L + R) × 0.707`.
- **SIDE** (2): Extract side: `(L − R) × 0.707`.

### SUM BUS

Output routing after Mode Out:
- **ST** (0): Normal stereo output.
- **→M** (1): Sum to mid, output mid to both channels.
- **→S** (2): Difference to side, output side to both channels.

### CHAOS

Two independent random modulation subsystems applied to the wet signal:

**CHAOS D (Delay)**: Modulates delay time and gain via smooth random LFO. Creates tape-like wobble and detuning.
- **Enable**: Toggle (default: OFF).
- **Amount (0–100%)**: Max delay ≈ 50 ms, gain swing up to ±3 dB. Default: 50%.
- **Speed (0.01–100 Hz)**: Random target change rate with 5 ms smoothing. Default: 5 Hz.

**CHAOS F (Filter)**: Modulates HP/LP filter cutoff frequencies via smooth random LFO. Creates evolving tonal movement.
- **Enable**: Toggle (default: OFF).
- **Amount (0–100%)**: Max ±2 octave shift. Default: 50%.
- **Speed (0.01–100 Hz)**: Random target change rate with 10 ms smoothing. Default: 5 Hz.

### LIM THRESHOLD (−36 to 0 dB)

Peak limiter threshold. Sets the ceiling above which the limiter engages.
At 0 dB (default) the limiter acts as a transparent safety net. Lower values compress the signal harder.

### LIM MODE

Limiter insertion point:
- **NONE**: Limiter disabled.
- **WET**: Limiter applied to the wet signal only (after processing, before dry/wet mix).
- **GLOBAL**: Limiter applied to the final output (after output gain and dry/wet mix).

The limiter is a dual-stage transparent peak limiter:
- **Stage 1 (Leveler)**: 2 ms attack, 10 ms release — catches sustained overs.
- **Stage 2 (Brickwall)**: Instant attack, 100 ms release — catches transient peaks.

Stereo-linked gain reduction ensures consistent imaging.

## Technical Details

### DSP Architecture
- **Input buffer**: 262 144-sample circular buffer (~5.9 s at 44.1 kHz) with bitwise AND wrapping.
- **WSOLA**: Cross-correlation overlap search with Hann-windowed crossfade at 25% overlap. Analysis hop scaled by stretch ratio and pitch rate.
- **Granular**: Up to 64 simultaneous grains. Hann envelope per grain. Spawn interval = grain size / density. Polyphonic normalization by 1/√(active count).
- **FFT Phase Vocoder**: Hann window, 75% overlap (synthesis hop = FFT size / 4). Laroche & Dolson 1999 phase correction with instantaneous frequency estimation. Pitch shift via linear bin interpolation. OLA reconstruction with 2/3 normalization.
- **Smoothing**: One-pole EMA per sample for gain, mix, pan, and delay parameters. Snap-to-target when within ε.
- **Wet filter**: Biquad HP/LP (Transposed Direct Form II). Coefficients updated every 32 samples.
- **Tilt EQ**: First-order symmetric shelf at 1 kHz. Tolerance-based coefficient update.
- **Chaos**: Sample-and-hold random modulation with exponential smoothing. Per-block precomputation.
- **Safety limiter**: Hard clip at +48 dBFS (±251.19) on all output. Catches NaN/Inf runaways without engaging during normal operation.

### State Persistence
- All parameters saved via JUCE AudioProcessorValueTreeState.
- UI state (window size, palette, CRT toggle, IO section expanded/collapsed) persisted in the plugin state.
- Parameter IDs are stable across versions for preset compatibility.

### Performance
- Zero-allocation audio thread. All buffers pre-allocated in `prepareToPlay`.
- Lock-free atomic parameter reads.
- Gain/mix smoothing snaps to target within ε to avoid unnecessary EMA in steady state.
- Filter coefficient update uses per-block interval (every 32 samples).

### Build
- JUCE Framework, C++17, VST3 format.
- Visual Studio 2022 (MSBuild, x64 Release).
- Dependencies: JUCE modules only (no third-party libraries). Uses `juce::dsp::FFT` for the phase vocoder engine.

## Changelog

### v1.4
- Initial release with three DSP engines: WSOLA (STRETCH), Granular (GRAIN), FFT Phase Vocoder (FFT).
- AMOUNT (0–100%) maps 1×–4× stretch ratio. MOD independently shifts pitch ×0.0625–×16.
- FFT engine is always active; STRETCH and GRAIN are trigger-gated.
- ENGINE selector with automatic UI dimming (GRAIN slider disabled when engine ≠ GRAIN).
- WINDOW selector: 128–8192 samples shared across all engines.
- STYLE modes: MONO, STEREO, WIDE, DUAL.
- HP/LP biquad filters with 6/12/24 dB/oct slopes.
- TILT EQ (−6 to +6 dB) — first-order spectral tilt on wet signal.
- PAN with equal-power law.
- CHAOS engine with two independent targets: CHAOS D (delay/gain modulation) and CHAOS F (filter modulation).
- Mode In / Mode Out / Sum Bus signal routing (L+R, MID, SIDE).
- ALIGN and PDC enabled by default for phase-coherent dry/wet mixing.
- RVS (reverse) mode across all three engines.
- Safety hard-limiter at +48 dBFS.
- Added dual-stage transparent peak limiter with LIM THRESHOLD (−36 to 0 dB) and LIM MODE (NONE/WET/GLOBAL). Stereo-linked gain reduction with 2 ms/10 ms leveler + instant/100 ms brickwall stages.
- CRT post-processing overlay with scanlines, chromatic aberration, barrel distortion, noise, and vignette.
- Resizable UI with persistent window size.
