# STRE-TR PDC Algorithm-Change Click Postmortem

Read this file before touching anything related to:

- `PDC`
- `ALIGN`
- `algorithm` changes
- `GRAIN <-> FFT1` transitions
- `FFT1 <-> FFT2` transitions
- any attempt to remove clicks in those cases

## Base state that must be checked before editing

1. Confirm `git status`.
2. Confirm the real base commit.
3. Do not assume the tree is clean because "it was reverted".
4. Do not implement anything if there is still doubt about what code is actually on disk.

Historical good base:

- commit: `4df7ca3`
- message: `pdc rev,align with trg`

## Real problem we are chasing

Clicks when changing `algorithm` with `PDC ON`, especially in:

- `GRAIN -> FFT1`
- `FFT1 -> GRAIN`
- `FFT1 -> FFT2`

With `PDC OFF`, those changes sound noticeably cleaner or directly fine.

## What has been tried and why it did not work

### 1. `TRG` smoothing on final output

Attempts included:

- global output duck
- old/new crossfade on final output
- variants with final-output history

Why it failed:

- it attacked the wrong layer
- it pumped the entire output, not the real engine handoff
- it could introduce "note", overlap, or extra artifacts
- it did not solve the `PDC` problem in algorithm changes

Conclusion:

- do not start from final-output smoothing for this issue

### 2. Heuristic `TRG` crossfade as `old -> new`

Why it failed:

- it extended the overlap
- it exposed a "note" or a continuous click
- it did not behave like the `window` duck/fade

Conclusion:

- do not reuse that idea for `algorithm change`

### 3. "More ms" without closing the cause

Longer hold/fade values were tried.

Why it failed:

- the problem was not only duration
- the jump came from internal timing inconsistencies
- increasing ms only masked or stretched the artifact

Conclusion:

- do not use "let's raise the ms" as the first response

### 4. `PDC ON` fixed only at host-report level

The reported latency was fixed in some attempts.

Why it was not enough:

- reported latency is one thing
- real internal latency is another
- if `GRAIN` still runs at `0` and FFT runs with a different pad/latency frame, the click stays

Conclusion:

- never separate:
  - reported latency
  - real internal latency
  - output pad behavior

### 5. `output pad` without its own transition

Correct diagnosis that was found:

- with `PDC ON`, the `fftOutputPadLen` jump happened before `engineFade`
- so the fade arrived too late

This was a real problem.

### 6. Generic `engineFade` for FFT cases

Problem found:

- generic `engineFade` uses a very weak old reference
- in several iterations it was only a frozen sample or an insufficient approximation
- that may pass with `PDC OFF`, but becomes more audible with `PDC ON`

Conclusion:

- for `FFT1 <-> FFT2`, do not assume the generic `engineFade` is good enough

### 7. Mixing too many hypotheses in one iteration

This happened repeatedly:

- touching `TRG`
- touching `PDC`
- touching `pad`
- touching `engineFade`
- touching transition priorities

Why it was bad:

- when there was no clear improvement, it was impossible to isolate which layer failed
- traceability was lost

Conclusion:

- one iteration = one hypothesis = one layer

## Firm facts learned so far

### A. With `PDC ON`, the problem is not only "missing crossfade"

It also involves:

- reported latency
- real internal latency
- `fftOutputPadLen`
- pad ordering relative to the fade

### B. `window` and `algorithm` are not the same kind of transition

`window`:

- happens inside the same FFT engine
- already has a specific fade path

`algorithm`:

- changes engine
- may change FFT size
- may imply reseed/reset
- with `PDC ON`, may also change the internal timing frame

Conclusion:

- do not expect them to behave the same unless they use the same transition path

### C. `FFT1 <-> FFT2` deserves its own treatment

Reason:

- it is an algorithm change
- but it is still FFT -> FFT
- so there may be an FFT-specific transition that is better than the generic one

Before touching it again, verify with data:

- whether `fftOutputFade` is really armed
- whether `engineFade` overrides it
- what the real runtime values are for:
  - `requestedFftSize`
  - `stft_.activeFftSize`
  - `fftOutputPadLen`
- `engineFadePos_`
- `fftOutputFadePos_`

### D. The current dominant artifact does not look like an active fade artifact

Latest dump-based findings:

- the largest measured deltas have been showing up with:
  - `engineFadePos = 0/0`
  - `fftOutputFadePos = 0/0`
  - `fftStartupWarmupRemaining = 0`
- that means the loudest discontinuity is probably not being created while a fade is still running
- it is more likely happening in the first fully audible output of the new FFT path under `PDC ON`

Conclusion:

- stop assuming the next fix is "another fade tweak"
- verify the FFT output path itself first

### E. The current `prevWetPostOutputL` reference is not valid for `GRAIN -> FFT1`

What was learned:

- `fftPrevWetPostOutputL_` is only updated in FFT engines
- so it cannot be trusted as the real previous-output edge when the source engine was `GRAIN`

Conclusion:

- for `GRAIN -> FFT1`, add and use a true global last-output reference
- do not reason from FFT-only history as if it represented all engines

### F. The zero-gain notch in `engineFade` is real, but not sufficient as the main explanation

What was learned:

- the current generic `engineFade` formula contains a real `old=0 / new=0` handoff moment
- this was confirmed in the dump
- however, removing or compensating for that notch did not remove the audible click

## Current reset point

Current codebase reset:

- `PluginProcessor.cpp` and `PluginProcessor.h` restored to `HEAD`
- base commit remains `4df7ca3`
- Release build confirmed clean after restore

Meaning:

- all experimental click fixes have been dropped
- the only thing that must be preserved from the failed iterations is the diagnosis, not the code

## Next-phase rules

From this point:

1. do not patch `engineFade` first
2. do not patch `TRG`
3. do not patch `PDC` reporting first
4. do not add heavy dump instrumentation to Release

The next valid phase must be:

- one narrow measurement path
- one narrow hypothesis
- one narrow implementation

## Next hypothesis to test from the clean base

Best current hypothesis:

- the dominant click under `PDC ON` during algorithm change is created in the FFT output path before or at the output pad handoff
- not in the generic final-output fade

Most important transitions:

- `GRAIN -> FFT1`
- `FFT1 -> GRAIN`
- `FFT1 -> FFT2`
- `FFT2 -> FFT1`

Priority rule:

- treat `FFT entry under PDC ON` as the hottest path
- only after that, verify if `FFT -> GRAIN` is the same family or another one

Conclusion:

- keep it documented as a real bug in the generic fade
- but do not keep treating it as the dominant cause of the `PDC ON` algorithm-change click

## Mandatory rules for the next iteration

### Rule 1

Before editing:

- check `git status`
- check `git log -1`
- check the real diff against the base

### Rule 2

Before editing:

- state the exact hypothesis in the work note
- state which layer is being touched:
  - `pad`
  - `engineFade`
  - `fftOutputFade`
  - `TRG`
  - other

### Rule 3

Do not mix in one iteration:

- `TRG`
- `PDC`
- `engineFade`
- `fftOutputFade`

unless code review already proves they belong to the same causal point.

### Rule 4

If an iteration "does absolutely nothing" perceptually:

- do not keep adding ms
- do not improvise another fade
- stop and review the exact render order

For this issue specifically:

- do not patch `engineFade` again before new instrumentation proves the discontinuity is still born there

### Rule 5

If touching `FFT1 <-> FFT2` with `PDC ON`, verify first:

- where `fftOutputFade` is armed
- where `engineFade` is armed
- in what order they render
- whether one blocks the other

## What to review first next time

1. Is the tree really clean, or are there still local leftovers?
2. Does the remaining click belong to:
   - `GRAIN -> FFT1`
   - `FFT1 -> GRAIN`
   - `FFT1 -> FFT2`
   - all of them?
3. With `PDC ON`:
   - is internal latency really fixed now?
   - does the pad still jump?
4. In `FFT1 <-> FFT2`, does the residual come from:
   - fade priority
   - FFT reseed/reset
   - runtime values that were assumed but not verified?
5. In `GRAIN -> FFT1` and `FFT1 <-> FFT2`, where is the first bad sample born:
   - STFT normalized read
   - post-pad
   - post-engineFade
   - final wet output

## Minimum protocol before the next implementation

1. Read this file.
2. Confirm the real repo state.
3. Pick only one case:
   - `GRAIN -> FFT1`
   - `FFT1 -> GRAIN`
   - `FFT1 -> FFT2`
4. Pick only one hypothesis.
5. If the hypothesis is still "fade issue", prove first that the large delta happens while the fade is active.
5. Implement only one layer.
6. Compile.
7. Test.
8. If there is no clear improvement, stop and review.

## Final reminder

The goal is not "another patch".

The goal is:

- isolate the real cause
- touch only the correct layer
- stop repeating iterations that do not change anything audible
