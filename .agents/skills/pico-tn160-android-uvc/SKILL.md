---
name: pico-tn160-android-uvc
description: Diagnose and close pico_tn160 Android UVC compatibility issues across APK startup crashes, USB attach/permission, descriptor parsing, format or resolution selection, Probe/Commit, first-frame/no-frame failures, and Windows/Linux/macOS regressions. Use this skill whenever working with TN160, Thermal160, VID:PID 359f:4202, Tony USB Camera, Huawei app-store Android camera clients, Android logcat/APK analysis, YUY2/MJPEG negotiation, or four-platform no-configuration UVC validation.
---

# pico_tn160 Android UVC

Drive the task to one formal firmware that works without manual host configuration on Windows, Linux, macOS, and Android. Separate host-App defects from firmware/protocol defects before changing USB descriptors.

## Read context first

Read these files before acting:

1. `AGENTS.md`
2. `HANDOFF.md`
3. `C_context/MEMORY.md`
4. `../C_context/KNOWN_FAILURES.md`
5. `references/case-study.md` when symptoms involve the Tony App, `640×480 MJPEG`, diagnostic APK repacking, or manual `160×120` success

Run the read-only preflight and confirm that it resolves the project root to `/home/slam/dev/pico_tn160`; report any mismatch before editing.

## Preserve the baseline

- Preserve full-speed Bulk UVC.
- Preserve `160×120 YUY2@10fps` unless evidence explicitly requires another real output mode.
- Preserve Video EP `0x81` Bulk/64 bytes and TinyUSB payload/epbuf 256.
- Do not return to historical ISO descriptor/payload guessing.
- Keep `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4` in formal firmware.
- Use `tools/pico-sdk-2.2.0/`; do not reference removed SDK copies.
- Treat Windows, Linux/V4L2/guvcview, macOS, and Android as separate regression gates after any UVC change.

## Define the closed loop

Before modifying anything, state:

- Target host-visible behavior.
- Exact firmware/APK currently under test, including SHA256 when available.
- Current position in the Android state ladder.
- Observed error and evidence.
- One controlled variable for the next test.
- Minimum feedback that will accept or reject the hypothesis.

Use: target → state → error → control action → feedback → correction → verification → handoff.

## Locate the failure layer

Advance through this ladder and stop at the first missing transition:

1. APK installs.
2. Application process starts and remains alive.
3. Expected Activity becomes resumed.
4. Android receives `USB_DEVICE_ATTACHED` for `359f:4202`.
5. USB permission is granted.
6. App logs `Camera open` or equivalent.
7. App parses advertised formats and frames.
8. App selects format, resolution, and interval.
9. Host sends Probe.
10. Host sends Commit.
11. Firmware starts frame transfer.
12. Host receives the first complete frame.
13. Frames continue at the expected cadence.

Do not investigate FFC, TinyUSB transfer scheduling, or payload headers before the App reliably reaches the relevant lower layer.

## Diagnose Android App failures

Use short, single-purpose ADB queries because Windows `adb.exe` through WSL can block:

- `adb devices -l`
- `adb logcat -d -b crash -v threadtime`
- `adb shell dumpsys activity exit-info <package>`
- `adb shell pidof <package>`
- `adb shell dumpsys activity activities`
- A bounded/filtering logcat query around the connection timestamp

Distinguish:

- Process/Application crash before Activity: APK, DEX, manifest, multidex, signature, or dependency problem.
- Activity/App NPE before `Camera open`: host client lifecycle problem.
- USB attach without `Camera open`: permission, intent routing, Activity, or App state.
- Camera open without Probe/Commit: format parser or host selection problem.
- Probe/Commit without frames: firmware format, scheduling, payload, or first-frame timing problem.

Confirm that the hardware is physically connected before interpreting absence of USB logs.

## Handle diagnostic APKs safely

Explain the purpose, package affected, original-App impact, and rollback before installing or repacking an APK.

For any DEX modification:

1. Avoid direct binary package-name replacement. Equal-length replacement can still violate sorted `string_ids`.
2. Recompute the DEX SHA-1 and Adler32 after bytecode changes.
3. Run Android SDK `dexdump`; reject any verification warning.
4. Rebuild the APK without stale signatures.
5. Run zipalign before signing.
6. Verify APK v1/v2/v3 signatures.
7. Install with a complete transfer when diagnosing loader problems, for example `adb install --no-streaming -r`.
8. Launch without the USB device first; verify process survival and no new crash entry.
9. Connect the device only after the diagnostic App is proven stable.

Keep the original App installed unless the user explicitly authorizes replacement. Prefer a coexist package, but leave DEX hardcoded package strings unchanged unless a proper rebuild tool can reconstruct the DEX string table.

## Diagnose format and resolution selection

Compare three facts:

- What the descriptor advertises.
- What the App actually selects.
- What the firmware can continuously produce.

Treat manual success after selecting `160×120` as evidence that enumeration and the YUY2 data path work. Do not accept manual selection as the final product solution.

For no-configuration compatibility:

- Advertise only real, continuously supported modes; or
- Implement every advertised compatibility profile as a real sustained stream.

Do not leave a static MJPEG test profile enabled in formal firmware. A test profile can change an App's automatic format choice even when the original YUY2 mode is correct.

Before claiming automatic compatibility, clear only the diagnostic App's stored settings or reinstall it, then retest from a fresh state without touching resolution or format controls.

If the user specifies Huawei Consumer/AppGallery or another non-Google distribution channel, search and validate clients through that channel. Do not substitute Google Play availability for the user's actual installation path. A generic client that automatically displays the image is stronger product evidence than a special diagnostic App that requires manual settings.

## Control firmware changes

Before changing descriptors, locate:

- `CMakeLists.txt`
- `src/usb_descriptors.c`
- `src/usb_descriptors.h`
- `src/tusb_config.h`
- Probe/Commit and frame-transfer paths in `src/main.c`

Verify source configuration, CMake Cache, and UF2 SHA256 independently. A Cache value of `OFF` does not prove the default UF2 was rebuilt.

Use the fixed build command:

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

Do not label a test UF2 as formal. Keep firmware artifacts only under `/home/slam/dev/pico_tn160/build/` and give each test artifact a descriptive filename and SHA256.

## Validate the result

Require all applicable checks:

### Android

- Fresh App state with no remembered `160×120` selection.
- Plug/open without manual configuration.
- No Java/native crash.
- Correct automatic format/resolution selection.
- First frame and sustained live frames.
- Test the stable diagnostic/generic App separately from the original Tony App.

### Windows

- Device enumerates normally.
- Capture five seconds at `160×120 YUY2@10fps` and observe about 50 frames.

### macOS

- Confirm the device enumerates as a camera.
- Confirm a normal camera client displays sustained frames without manual format configuration.

### Linux/WSL

- Use the previous known-good `v4l2-ctl` command on `/dev/video0`.
- Require continuous `DQBUF`.
- Require `bytesused=38400` per frame.
- Require five-frame output size `192000` bytes.
- Retest guvcview at 10fps.

Mark all unavailable hardware checks as “未上板验证”; never replace them with “应该可以”. Historical Linux success does not prove the latest UF2 still passes; label it separately until the current artifact is captured again.

## Separate committed UVC work from later thermal work

When `src/main.c` is marked modified after a UVC commit, do not assume the file was omitted. Compare both views:

- `git show <commit> -- src/main.c` — what the commit contains.
- `git diff HEAD -- src/main.c` — changes made after the commit.

Keep unvalidated NTC/FFC/NUC changes out of a UVC compatibility commit. Build and hardware-test thermal changes separately, then commit them independently if they pass.

## Communicate with the user

- Answer causal questions directly; do not interpret “为什么” as “不要做”.
- Report the current outcome before describing tooling.
- If a diagnostic route consumes time without new feedback, stop and choose the smallest test that separates the most hypotheses.
- Ask the user for only one minimal hardware action at a time, with the expected discriminating result.
- Update `HANDOFF.md` at the end of a long session with current status, failures, exact artifacts, next 3–5 actions, and unverified risks. When the user explicitly asks to preserve lessons, also update `C_context/MEMORY.md`; do not silently replace user changes or stage generated APK/androguard output.
