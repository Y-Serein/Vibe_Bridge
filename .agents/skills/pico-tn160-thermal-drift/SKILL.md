---
name: pico-tn160-thermal-drift
description: Diagnose and close pico_tn160/TN160/PT160 thermal drift, startup path dependence, cross-board transfer, and absolute-temperature errors using blackbody/ice-point paired PNG+CSV, FFC/NUC events, raw ADU, anchor, mean_diff, NTC, VTEMP, OFF/TABLE/STATE/adaptive models, and Type-C UVC versus UART/MaixCam2 paths. Use whenever the user mentions cold/hot start, RST, temperature drift, 50°C high/low, ice-point error, FFC spikes, NTC jumping, STATE/PWL saturation, MaixCam2 trend logs, K_cold/K_shutter, changing boards, or asks for a firmware test. Always apply the evidence-and-test-value gate before changing thermal compensation or FFC parameters.
---

# pico_tn160 thermal drift

Drive the thermal path toward stability, accuracy, and cross-board transfer without regressing UVC, FFC, calibration storage, or MaixCam2. Hardware feedback is primary evidence. User hardware time is expensive: do not ask for a firmware test unless the candidate has a numerical prediction and a result that can falsify it.

## Current project override — 2026-09-07 architecture reset

This section supersedes the older startup-time and generic relative-NTC guidance below. Use `HANDOFF.md` for the exact live files and measurements.

- Retire the current `TC(0x4354)` control law `A*exp(-uptime/tau)` and its one-shot `boot_ntc > threshold` gate. Real data showed `startup_model_valid=1` but `active=0` for the entire run because training boot NTC was about 2027, the generated gate was 1963, and test boot NTC was 1861. Do not repair this by changing the gate, amplitude, or tau.
- The product assumption approved for the next candidate is per-device `error=f(current_ntc)`: compensation is evaluated every frame from the current NTC and is independent of cold start, warm start, RST, and uptime. Keep that assumption explicitly falsifiable; do not silently add startup classification.
- Basic V1 is unconditional product functionality. Missing, late, incomplete, skipped, or failed drift learning must fall open to `0C -> 50C -> base V1 write/readback -> normal imaging`. Never disable basic calibration or production write because an optional drift observable/model is unavailable.
- The intended optional station path is `0C -> 50C -> hold 50C fixed -> manually heat the board until Delta NTC >= 3C -> fit -> write/readback`. Treat `Delta NTC >= 3C` as the currently approved operator progress condition. Do not reintroduce 24 samples, 120 raw ADU, replay error, cold-start origin, or NUC continuity as hidden UI/write gates without a new state-table approval.
- Inside the learned NTC interval, interpolate continuously. Outside it, clamp to the nearest learned boundary and continue imaging; do not disable compensation for the whole boot and do not extrapolate without bounds. Report learned min/max and runtime hit/clamp state in diagnostics.
- Preserve absolute calibration with an explicit zero/reference: prefer an inspectable form such as `comp = K * (current_ntc - ntc_zero)`, bounded by the learned interval and a declared ADU limit. Keep FFC spatial offset and global NTC anchor separate so the same correction is not baked twice.
- Audit whether existing TNC1 `TN(reference_adu, k_q8, limit_adu)` can express the approved model before changing the package. If interval min/max cannot fit in the existing eight bytes, report the protocol/compatibility impact and obtain separate approval; never repurpose bytes silently.
- Evidence anchor: `tn160_diagnostic_20260907_155751.csv` at fixed 50C, excluding the final target removal, drifted 48.8 -> 44.4C from 5-125 s (P-P 5.1C); stored TC never activated. Training NTC was 1918-2010 while runtime NTC was 1685-1825, so the domains did not overlap.
- A same-trajectory linear replay on 155751 produced `comp_adu = 1.319 * (NTC_ADU - 1853)`, P-P about 1.21C, endpoint change +0.31C, and 50C RMSE about 0.25C. An 8-ADU PWL improved P-P only to about 1.13C, so start with linear, not PWL. These are expressiveness numbers, not qualification.
- `tn160_diagnostic_20260907_160820.csv` without TC was better after 60 s (about 49.1-49.8C, endpoint -0.6C, P-P 1.6C), but it used a different serial and firmware build. Treat base V1 as the safe rollback, not as a strict A/B pass.
- Qualify with one device held out from its own fit: fixed 50C learning, then cold power-on and hot RST. Pass only if >=6 s P-P <=1.5C, endpoint change <=0.5C, and steady 50C error <=1C. If any gate fails, stop the single-NTC linear model instead of tuning constants; require a second measured state that can distinguish the residual.
- Before editing, provide a state table covering optional-learning skip/failure, button enablement, package fields, interval/clamp behavior, FFC interaction, diagnostics, write/readback, and rollback. User-visible behavior and any protocol change require explicit confirmation.

## Current project override — 2026-08-06 session close

This override takes precedence over the historical four-state V2 guidance below. Read
`HANDOFF.md` and `C_context/MEMORY.md` first; they record the live artifact identity.

- The four-temperature Flash V2 map route is retired: its CRC-valid activation
  broke real-device imaging, and V1 calibration did not repair it. Do not
  reactivate, extend, or offer it as the long-term thermal solution without a
  separately authorized new project with imaging-safety evidence.
- The source selector `THERMAL_COMP_MODEL_RELATIVE_NTC` with `282/256`, limit
  `220 ADU`, and UF2 `94319ded…c53423` is a failed candidate. A 50°C blackbody
  measured roughly 47°C at 10 s, 44.4°C at 60 s, then about 45°C. Do not make
  a follow-up constant tweak; roll back to a known V1-only controller first.
- A log used for the failed fit began at 402 s. A cold-start model may be fit
  or tested only with data that starts at `elapsed_s <= 5` and covers its
  controlled interval (currently at least 0–180 s). Late steady data cannot
  be extrapolated to 10–60 s startup.
- Split the qualification: first real 0°C/50°C V1 calibration/steady absolute
  error; then fixed 50°C cold-start and hot-RST dynamics. The pass gates are
  >=6 s peak-to-peak <=1.5°C, endpoint drift <=0.5°C, and 50°C steady error
  <=1°C. No current candidate passes all three.
- V1 calibration must directly use `0xCC`; it must not require V2, F3/F4, or
  a `TNVW` transaction. Repackage and validate the Windows GUI separately.
- After a candidate misses its declared gate, stop that model class. Request
  a new observable or change to a per-device state model; never issue a
  sequence of same-assumption parameter patches.

## Start with identity and context

Read completely:

1. `AGENTS.md`
2. `HANDOFF.md`
3. `C_context/MEMORY.md`
4. `../C_context/KNOWN_FAILURES.md`
5. `docs/design/tn160_thermal_architecture_optimization_plan.md`
6. Latest relevant paired PNG/CSV

The project root has no `CLAUDE.md`; do not invent it. Run:

```bash
python3 /home/slam/tools/agent_preflight.py --project pico-tn160
```

Lock down before interpreting a curve:

- Board ID and whether boards/calibration data were changed.
- Actual target and reference instrument.
- Cold power-on, hot power-on, RST, or scene change.
- Exact UF2 path, SHA256, source selector, and `total_comp` identity.
- Logger start and RST time; whether CSV starts at `NUC count=0/1`.
- PNG/CSV non-empty, same name/timestamp/window.
- Type-C UVC or UART/MaixCam2, including power/thermal environment.

Continue safe read-only analysis when information is incomplete, but label conclusions provisional. Ask only for the smallest missing evidence that changes the decision.

## Audit artifact identity before diagnosing a regression

When the question is “the old firmware was stable; why does v4 drift?”, first
test whether the two artifacts actually contain the same controller. Do not fit
a new thermal model until this read-only identity gate is complete.

Create one comparison table for the old successful artifact, the failing test
artifact, and any proposed candidate. Resolve, or explicitly mark unknown:

- UF2 path, SHA256, byte size, and build timestamp.
- Source revision or recoverable source snapshot.
- `THERMAL_COMP_MODEL` selector and every compile-time gate that changes
  `total_comp`.
- Meaning of the telemetry compensation fields in that build.
- Board, calibration payload, host/power path, and log captured with the SHA.

Treat `THERMAL_COMP_MODEL_OFF` as an open-loop diagnostic artifact. Its curve
measures raw plant drift; it cannot prove that a compensated baseline, FFC, or
sensor regressed. OFF data may still identify a candidate controller, but the
only valid causal statement is “this is the response with compensation off.”

If a diagnostic build changed the selector, multiplexed telemetry, calibration
path, or FFC behavior, consider the test firmware itself a candidate cause.
Compare source/artifact identity before researching extra physics or requesting
new hardware data. If the prior low-drift artifact cannot be identified, report
that as the blocker; do not assume equivalence and do not build a replacement
solely to explain the regression.

After an identity mismatch explains the apparent regression, separate two
questions:

1. Can the known low-drift controller be recovered and transferred across the
   required boards and power paths?
2. Is a new controller still justified by a measured product-gate failure?

Answer the first before spending hardware time on the second. A newly compiled
candidate is only a testable alternative, not evidence that the old method
failed or that the root cause is closed.

## Reuse settled evidence before analyzing

Treat `HANDOFF.md` and `C_context/MEMORY.md` as the index of work already completed. Start by listing their frozen conclusions and the exact decision still open. Do not reload and recompute the same logs unless at least one of these changed:

- Data identity or an additional physical observable.
- Firmware SHA, model equation, or selector.
- Target, board, startup path, or reference setup.
- Acceptance criterion or the question being answered.

The 2026-08-04 four-dataset result is settled evidence: first-order held-out peak-to-peak was `1.29/1.64/1.95/2.05°C`, and second-order was `1.35/1.30/1.80/1.96°C` for v1, v2 Maix, v3, and v2 CH346. Extra VTEMP/NTC-rate/quadratic/interaction/dual-RC terms did not make every dataset meet `≤1.5°C`. Do not rerun these calculations and present the same finding as progress. The next action must change observability, choose per-device learning, or end the hypothesis.

## Follow the closed loop

`目标 → 状态 → 误差 → 控制动作 → 反馈 → 修正 → 验证 → 沉淀`

Before a firmware edit, report:

- Numeric goal and acceptance criteria.
- Current evidence-backed state.
- Independent error classes.
- One control law or diagnostic variable.
- Offline predicted start/end/mean/peak-to-peak where data permits.
- Failure mode, rollback, and shortest informative test.

Wait for explicit confirmation before changing thermal or FFC firmware. A previously approved model does not authorize a materially different model found later.

## Freeze authorization and the execution budget

Hardware time is part of the product cost. Before proposing any code, diagnostic firmware, or capture, publish one bounded execution contract containing:

1. Total remaining steps, including every conditional branch already foreseeable.
2. Maximum number of user hardware rounds.
3. Exact boards, host/power path, action, and duration per round.
4. Numerical start-to-result prediction and all acceptance gates.
5. Failure conclusion, rollback, and hard-stop action.

For the current 50°C closure, the standing ceiling is four total steps and at most two user hardware rounds. Full `0–50°C` absolute accuracy is a separately approved fifth multipoint step. Never hide it inside the 50°C budget.

Interpret authorization literally. A question such as “只制作诊断固件吗？” asks for an answer; it does not authorize an edit. A challenge, paraphrase, or general acceptance of a strategy also does not authorize a particular firmware, capture, flash semantic, or hardware run. Proceed only after an explicit statement such as “同意执行第 N 步” or “允许修改 X / 烧录 SHA Y”. Quote the authorized scope in the pre-edit report. If no such statement exists, stay at the read-only decision layer.

Do not say “一次就够” or “最后一次” while another collection, diagnostic revision, tuning round, or qualification test remains possible. If new evidence later reveals that the frozen budget is insufficient, report the earlier estimate as wrong, explain why, and ask whether to redefine scope; do not silently roll the plan forward.

## Enforce the test-value gate

Do not send a candidate to hardware merely because it is plausible. A worthwhile test must have:

1. A measurable causal mechanism in current logs or source.
2. A numerical predicted improvement.
3. A threshold that declares the hypothesis failed.
4. A result that distinguishes it from the next hypothesis.
5. A single default UF2 with unambiguous identity.

If the data do not expose the proposed variable—such as closed-shutter per-frame values when evaluating discard count—state that the effect cannot be calculated. Withdraw the test or first create a short diagnostic capture. Do not consume a 3–10 minute hardware run on an unbounded guess.

When a candidate misses its promised threshold, stop that hypothesis. Do not extend `2→4→6→8` or repeatedly tune small constants without new evidence.

A diagnostic capture is not automatically justified by “more fields.” State which two hypotheses its fields distinguish, what result selects each branch, and why one agreed capture is enough. If the new observable cannot decide within that capture, do not build the diagnostic firmware. Once the agreed diagnostic candidate fails, do not produce a second diagnostic version unless the user explicitly reopens the execution budget.

## Separate error classes

Never collapse these into one “temperature problem”:

1. Absolute error: stable reading minus target.
2. Time drift: start/end, slope, mean, and peak-to-peak under fixed target.
3. Persistent FFC change: settled pre/post difference aligned to an FFC.
4. Transient post-FFC sample: a dip/spike that recovers.
5. Startup calibration interval: before first accepted FFC.
6. Scene or reference change: blackbody movement, environment change, or target settling.

Blackbody, fixed room scene, skin/contact thermometer, and ice point have different evidence quality. Never infer absolute accuracy from an unknown fixed scene.

## Analyze CSV quantitatively

Check:

- `elapsed_s`, row count, gaps, start/end/span.
- `center_c`, `center_roi_c`: start/end/min/max/mean/peak-to-peak.
- `center_raw_adu`, `center_roi_adu`.
- `ntc_adu`, `vtemp_adu`, `smooth_vtemp`, `boot_ntc`, `boot_delta`.
- `anchor`, `total_comp`, `eff_anchor`.
- `nuc_decision`, `nuc_count`, `nuc_dg`, `nuc_dv`, `nuc_dn`.
- `mean_diff`, `smooth_low`, `smooth_high`.

### Align FFC events correctly

For every `nuc_count` transition calculate both:

- Immediate adjacent-row difference.
- Median or mean of at least three samples before versus three after.

Then inspect whether the output recovers over the next samples. Never sum adjacent-row differences and call them cumulative FFC drift without proving persistence.

The 2026-07-31 correction is a permanent lesson: adjacent rows suggested cold/hot cumulative `-2.2/-1.3°C`, while three-sample windows showed hot persistent mean about `-0.008°C/event`. That invalidated the closed-shutter `2→4` test.

If CENTER moves continuously while FFC events are accepted and anchor is stable, do not blame FFC. If `total_comp` stays exactly at a limit while raw ADU moves, treat model saturation/lost observability as the primary failure.

Do not treat plotted FPA/VTEMP around 100–130°C as physical FPA Celsius. Use it only for deltas, scheduling, smoothing, and plausibility.

## Convert observations into compensation

Use the firmware's exact T⁴ conversion when replaying candidates:

```text
f = (corrected_adu - eff_anchor) / mean_diff
T_K^4 = T_low_K^4 + f × (T_high_K^4 - T_low_K^4)
```

Do not rely on one generic ADU/°C value across 0–50°C. A local approximation is allowed only when labeled and calculated from the same range.

For cross-board OFF data:

- Reconstruct the anchor correction required to make the fixed blackbody equal its true temperature.
- Accumulate `nuc_dg` only at accepted `nuc_count` transitions.
- Define `K_shutter` as required anchor ADU per shutter-mean ADU.
- Define `K_cold` as residual required anchor ADU per NTC ADC code after subtracting the shutter term, only on the cold-warming path.

## Choose the smallest defensible model

Use this decision order:

1. Wrong artifact/test identity: stop tuning.
2. Missing startup/FFC data: fix capture, not model.
3. Persistent bad FFC: inspect freshness, closed discard, 8-frame candidate, offset-map regional change, and guards.
4. Saturation: extend only with measured anchors.
5. Monotonic single-path residual versus NTC: bounded fixed-point linear/PWL diagnostic.
6. Same NTC needs different cold/hot correction: direction, `dNTC/dt`, hysteresis, or second state.
7. Cross-board product: relative per-board references plus cross-board coefficients; do not ship absolute single-board NTC knots.
8. 50°C stable but 0°C wrong: inspect gain/`mean_diff` or multipoint calibration.

Prefer inspectable bounded fixed-point arithmetic. Preserve older selectors for rollback, but do not build multiple artifacts unless the user requests regression.

## Cross-board and interface transfer

Do not call a model universal from one board. For B1/B2/B3:

1. Same OFF firmware SHA.
2. Same fixed 50°C target.
3. Logger starts before power-on; cold 180 s, immediate hot RST 180 s.
4. `total_comp=0`, preferably `NUC count=0/1` at start.
5. Fit each board separately.
6. Use two-board mean coefficients to predict the held-out third board; rotate three times.

Suggested batch-fixed gates:

- `K_cold` within about `±0.15` of batch mean.
- `K_shutter` about `0.85–1.15`.
- Held-out cold/hot peak-to-peak `≤1.5°C`.
- 50°C steady error `≤1°C`.
- Endpoint drift `≤0.5°C`.

If the spread fails, use per-board learned coefficients; do not average them into a false universal value.

Type-C UVC and UART/MaixCam2 share `binary_calib_frame()`, so implement compensation there. Still separate:

- Algorithm-path compatibility.
- UART diagnostic visibility.
- Bulk UVC transport compatibility.
- Different power/installation thermal environment.

After Maix cross-board validation, run one representative Type-C UVC and thermal-environment regression. A generic UVC stream does not provide the Maix diagnostic CSV.

## Preserve safety and compatibility

Do not alter during a thermal-model iteration:

- full-speed Bulk UVC `160×120 YUY2@10fps`.
- Video EP `0x81`, Bulk 64 bytes, TinyUSB payload/epbuf 256.
- `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`.
- Flash layout/data semantics without separate approval.
- UART/MaixCam2 telemetry length and fields.
- GPIO, PIO, DMA, descriptors, or realtime scheduling.

FFC baseline:

- Discard two closed-shutter settling frames.
- Directly average eight shutter frames.
- Discard at least four frames after reopening, up to twelve with stability guards.
- Preserve sanity, VTEMP/NTC plausibility, jump, retry, abort, capture epoch, and UVC hold.

The remembered `4/12` values belong to open-shutter recovery, not closed-shutter capture. Eight full 160×120 `uint16` frames need about 153.6 KiB, versus the current reused workspace around 76.8 KiB; do not introduce per-pixel max/min removal without a memory plan.

## Gate per-device four-state Flash calibration

Treat factory complexity as part of the control law, not a later UI concern. A productizable per-device flow must be one fixture setup and one button: the worker chooses states, pairs each accepted FFC with its map and RAW stack, computes, writes, verifies, and reports PASS/FAIL. Do not require operators to select T0–T3 or continuously occupy a precision blackbody.

Before accepting a residual equation, substitute these boundary cases into the exact firmware path: startup before first FFC, immediately after FFC where `T=Tffc`, every knot, interpolation between knots, extrapolation outside knots, and hot RST. Reject a law that algebraically cancels the measured error at the moment the error was observed. The 2026-08-06 correction is permanent: a residual measured after a matched FFC cannot be corrected by `Residual(T)-Residual(Tffc)` because that term is zero immediately after FFC. Use independently justified absolute open-path spatial residual and global anchor layers.

Separate the fixtures:

- Drift/spatial learning uses a fixed high-emissivity field; its temperature may be unknown.
- Absolute-temperature qualification uses a precision blackbody briefly at approved points.

The drift field need not be geometrically flat. A matte black plate, curved hood, or lined cavity is acceptable when one material fills the field of view and its radiance, pose, and distance remain stable. An ordinary room scene is a lower-quality fallback because mixed materials, reflections, and independent temperature changes can be written into the pixel maps.

Do not demand a windless laboratory when the factory has HVAC. Steady representative airflow is acceptable. Shield the optical target from direct or cyclic gusts with a short hood while leaving the core body in its real production thermal environment. Reject sudden airflow or target motion, not normal stable air conditioning.

Freeze time and failure semantics before implementation. For the current four-state flow: target uptime `6/60/120/300 s`, use the third accepted FFC for the first safe 32-frame pair, complete capture/compute/Flash/readback within `360 s`, and never silently extend a failure to ten minutes. Save the expensive capture even when Flash or CRC fails, mark the unit failed, and preserve the V1 fallback.

## Build and artifact discipline

User preference overrides historical multi-artifact practice:

1. Build only one firmware per iteration.
2. Use only `/home/slam/dev/pico_tn160/build/pico_tn160.elf.uf2`.
3. Do not copy to `out/`; the project `out/` was explicitly deleted.
4. Do not automatically restore TABLE/PWL or build a regression artifact.
5. Report selector, sole variable, command/result, text/data/bss, UF2 bytes, and SHA256.
6. Keep source on the tested selector until the user requests another mode.

Never commit/push unless explicitly requested. Preserve unrelated dirty files.

## Verification strategy

Use short engineering gates before expensive qualification:

- Diagnostic/cross-board screen: cold 180 s + hot RST 180 s.
- Candidate must meet its numerical replay prediction within declared tolerance.
- Only after the engineering gate passes: 50°C cold/hot at least five minutes and 0°C at least five minutes.
- UVC changes or final shared thermal model: build, Maix diagnostic test, then representative Windows/Linux/guvcview or prior `v4l2-ctl` regression.

Always label “未上板验证” until hardware feedback exists.

## Report and handoff

Before editing:

```markdown
### 目标
### 当前状态
### 误差分解
### 单一控制动作与数字预测
### 风险、回退与失败线
### 最短验证矩阵
```

After editing, report changed files, exact artifact identity, build result, unchanged protected paths, hardware status, and one next test.

At a long-session boundary update `HANDOFF.md`. Update `C_context/MEMORY.md` when the user asks to record or durable workflow knowledge changes.
