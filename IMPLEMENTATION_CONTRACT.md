# STRE-TR Implementation Contract

This file is mandatory context before changing STRE-TR.

If a requested change touches `PDC`, `ALIGN`, `WINDOW`, `MAX WINDOW`,
`GRAIN`, `STRETCH`, `FFT1`, `FFT2`, `TRG`, `RVS`, `AMOUNT`, `PITCH`, or
`JIT`, read this file first. If anything is not 100% clear, ask before
editing.

## Non-Negotiable Rules

1. Do not infer product decisions from DSP fixes.
   - Do not disable, dim, hide, rename, reorder, or restyle controls unless the user explicitly requests it.
   - DSP behavior and UI behavior are separate decisions.

2. One implementation pass must touch one causal layer.
   - Do not combine PDC, ALIGN, fades, trigger behavior, jitter, and UI changes in the same fix unless the user explicitly approves the combined scope.

3. Do not use heuristic masking as a first response.
   - Do not add arbitrary fades, ducks, delays, or extra milliseconds before proving the discontinuity source.

4. Debug dumps are opt-in.
   - Do not enable release dumps unless explicitly requested.
   - If dumps are enabled temporarily, disable them before release hardening.

5. Before editing, state:
   - exact hypothesis,
   - exact files/layers to touch,
   - exact things that will not be touched.

## Engine Semantics

STRE-TR has four engines:

- `STRETCH` = WSOLA time-domain stretch.
- `GRAIN` = granular engine with internal look-behind and Hann grains.
- `FFT1` = phase-vocoder style FFT engine.
- `FFT2` = spectral-hold FFT engine.

`TRG OFF` means the wet path should behave as clean passthrough.

`RVS` is valid for all four engines.

## PDC And ALIGN Contract

This is the most important rule in the plugin.

### PDC

`PDC` is FFT-only.

- `PDC ON + FFT1/FFT2`: report FFT latency to the host.
- `PDC ON + STRETCH/GRAIN`: report `0` latency to the host.
- `PDC OFF`: report `0` latency to the host.

`GRAIN` and `STRETCH` must never affect `setLatencySamples()`.

Internal look-behind is not host latency.

### ALIGN

`ALIGN` is FFT-only.

- `ALIGN ON + FFT1/FFT2`: delay the dry path by the same FFT latency budget used for PDC.
- `ALIGN ON + STRETCH/GRAIN`: do not delay the dry path.
- `ALIGN OFF`: do not delay the dry path.

`GRAIN` and `STRETCH` must never affect `dryDelayLen_`.

### FFT Latency Budget

FFT latency is based on `MAX WINDOW`, not on `GRAIN`, not on `STRETCH`,
and not on the current non-FFT window.

Current intended formula:

```cpp
fftLatency = maxFftWindow + recommendedFftSynthHop (maxFftWindow);
```

`TRG` must not gate the reported FFT latency. If the active engine is FFT1 or
FFT2 and `PDC` is on, the host latency should be stable for that FFT budget.

### Required Verification

After touching PDC/ALIGN, search for:

```text
setLatencySamples
reportedLatency =
dryDelayLen_ =
grainWetLatency
stretchWetLatency
engineWetLatency
computeGrainLookBehind
```

Expected result:

- The only runtime `setLatencySamples()` route depends on FFT latency only.
- `dryDelayLen_` depends on FFT latency only.
- `computeGrainLookBehind()` may exist for GRAIN internals, but not for PDC/ALIGN.

## WINDOW And MAX WINDOW Contract

`WINDOW` uses one UI slot but stores independent values per engine.

- `STRETCH`: uses its own stored continuous/smoothed window value.
- `GRAIN`: uses its own stored continuous/smoothed window value.
- `FFT1`: snaps to valid powers of two.
- `FFT2`: snaps to valid powers of two.

`MAX WINDOW` affects FFT1/FFT2 only.

- It clamps FFT1/FFT2 window values.
- It defines the fixed FFT PDC/ALIGN budget.
- It must not clamp or alter STRETCH/GRAIN windows.

If `MAX WINDOW` is reduced below the current FFT1/FFT2 window, the active FFT
window must be clamped to the new maximum.

## GRAIN Contract

`GRAIN` controls grain length in milliseconds.

- Range: `1..500 ms`.
- It is only active for `ENGINE = GRAIN`.
- It may use internal look-behind.
- Internal look-behind is musical state, not PDC.
- Changing `GRAIN` must not change host latency.
- Changing `GRAIN` must not change dry alignment.

The current safe approach for fast `GRAIN` movement is smoothing grain size
before converting it to samples. This smoothing is allowed only inside the
GRAIN engine path.

If clicks appear below `10 ms`, investigate:

- grain-size smoothing,
- grain spawn interval,
- active grain length changes,
- grain reset paths,
- unity bypass transitions.

Do not solve GRAIN clicks by touching PDC/ALIGN.

## STRETCH Contract

`STRETCH` is WSOLA/time-domain.

- It may use internal overlap/search/look-ahead-like state.
- That state is not host latency.
- It must not affect `setLatencySamples()`.
- It must not affect `dryDelayLen_`.

Do not treat WSOLA internal segment timing as host PDC.

## FFT1 / FFT2 Contract

FFT1 and FFT2 are the only engines allowed to participate in PDC/ALIGN.

- Their effective FFT window is power-of-two.
- Their latency budget uses `MAX WINDOW`.
- FFT1 and FFT2 must remain stable when current `WINDOW` changes below the max.

Any FFT click fix must follow `PDC_ALGO_CLICK_POSTMORTEM.md`.

Do not patch generic fades first. Prove the layer:

- FFT output path,
- output pad,
- FFT resize/reseed,
- engine fade,
- final wet output.

## AMOUNT And PITCH Contract

`AMOUNT` controls engine advance/freeze/hold intensity.

- `STRETCH`, `GRAIN`, `FFT1`: `0%` normal advance, `100%` freeze.
- `FFT2`: `0%` minimal hold, `100%` strongest hold.

`PITCH` is the pitch-rate parameter.

- Internal range remains normalized `0..1`.
- UI displays `-24st..+24st`.
- Center `0.5` = `1.0x`.
- Internal mapping:

```cpp
pitchRate = exp2 ((pitch - 0.5f) * 4.0f);
```

Do not reintroduce `MOD` naming in STRE-TR unless the user explicitly asks.

## JIT Contract

`JIT` is deterministic organic instability.

- It must not create clicks at `0%`.
- It must not alter PDC/ALIGN.
- It must preserve deterministic seeds unless explicitly redesigned.
- Per-engine mappings may differ, but the structure should remain maintainable.

Current intent:

- `GRAIN`: pitch drift plus safe grain-length/anchor motion.
- `STRETCH`: pitch-rate motion only unless explicitly approved otherwise.
- `FFT1/FFT2`: pitch-rate motion only unless explicitly approved otherwise.

If changing JIT, compare against `JITTER_GRA_STRE_AUDIT.html` when available.

## UI/UX Contract

Do not change GUI interaction as a side effect of DSP work.

Examples requiring explicit approval:

- disabling controls outside a mode,
- changing alpha/color of controls,
- changing click/right-click behavior,
- changing prompt text,
- changing labels or short labels,
- reordering controls,
- changing tooltip semantics.

Current known UI rules:

- `GRAIN` slider is enabled only for the GRAIN engine.
- FFT windows should move in discrete/equidistant power-of-two steps.
- `PDC` right-click opens `MAX WINDOW`.
- `ALIGN` short label is `ALN`.
- `PITCH` short label is `PCH`.

## Diagnostics Contract

Before adding dump variables:

1. Define the exact discontinuity being measured.
2. Define the expected numeric signature.
3. Keep dump code compile-gated.
4. Do not leave release auto-dumps enabled.

## Mandatory Pre-Edit Checklist

Run or inspect:

```powershell
git -C STRE-TR status --short
git -C STRE-TR log -1 --oneline
```

For PDC/ALIGN work:

```powershell
rg -n "setLatencySamples|reportedLatency =|dryDelayLen_|grainWetLatency|stretchWetLatency|engineWetLatency|computeGrainLookBehind" STRE-TR\Source\PluginProcessor.cpp
```

For UI work:

```powershell
rg -n "alignButton|pdcButton|pdcDisplay|setAlpha|setEnabled|mouseDown|Tooltip" STRE-TR\Source\PluginEditor.cpp STRE-TR\Source\PluginEditor.h
```

## Mandatory Done Checklist

At minimum:

```powershell
git -C STRE-TR diff --check
```

Compile at least:

```powershell
MSBuild STRE-TR.sln /t:STRE-TR_SharedCode /p:Configuration=Release /p:Platform=x64
```

Then report:

- files changed,
- exact invariant verified,
- what was not touched,
- whether release dump/debug paths remain disabled.

## If Context Conflicts

Priority order:

1. User's latest explicit instruction.
2. This implementation contract.
3. `PDC_ALGO_CLICK_POSTMORTEM.md` for PDC/click/FFT-transition work.
4. Current source code.
5. README.

If two sources conflict, stop and ask.
