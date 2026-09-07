# TN160 Android UVC case study

Use this reference when the current task resembles the July 2026 Tony USB Camera failure.

## Goal

Make one formal TN160 firmware work without manual configuration on Windows, Linux, and Android while preserving the full-speed Bulk `160×120 YUY2@10fps` baseline.

## Observed Android sequence

1. The phone parsed `160×120 YUY2` and a test `640×480 MJPEG` profile.
2. The Tony App automatically selected `640×480 MJPEG`.
3. The original App repeatedly stopped/recreated a `640×480` camera handler through message `4386` and later dereferenced a null handler.
4. The crash happened within milliseconds, so it was not caused by the firmware's possible three-second pre-FFC data window.
5. A coexist diagnostic App disabled the `4386` restart and unsafe delayed callback.
6. The first diagnostic APK opened and immediately closed before USB/UVC code ran.
7. Android crash buffer showed `ClassNotFoundException: com.serenegiant.usbcamera.App`.
8. Android SDK `dexdump` found the real packaging defect: `Out-of-order string_ids: 'com.tencent.smtt.sdk.QbSdk' then 'com.diag.usbcamera.molink...'`.
9. Directly replacing `com.tony...` with equal-length `com.diag...` changed lexicographic order inside the DEX string table and invalidated `classes2.dex`.
10. Restoring the DEX hardcoded package string, recalculating DEX checksums, zipaligning, and signing produced a valid coexist APK.
11. The fixed App cold-started, stayed alive, entered `MainActivity`, received `USB_DEVICE_ATTACHED`, and recognized `359f:4202`.
12. The user manually selected `160×120` and confirmed that the diagnostic App displayed an image.

## What this proves

- Android USB enumeration works.
- The fixed diagnostic App is stable enough for UVC testing.
- The `160×120` path can produce an Android-visible frame.
- VC status endpoint/UVC 1.1 changes are not the remaining primary error.
- Immediate App crashes are not caused by FFC startup latency.
- The strongest remaining product issue is automatic format/resolution selection.
- After rebuilding with `UVC_MJPEG640_STATIC_TEST=OFF`, the Huawei-channel `Next Camera` automatically displayed the image on power-up and remained stable for about five minutes.
- The user also verified the same OFF UF2 on Windows and macOS.
- The original Tony App is excluded from product acceptance because its lifecycle/handler race is a client defect.

## What this does not prove

- A fresh App state automatically selects `160×120`.
- The original Tony App no longer crashes.
- Linux/V4L2/guvcview still pass with the latest UF2; only the historical Linux baseline is recorded.
- The image is sustained at 10fps for a meaningful duration.
- A real `640×480 MJPEG` product mode is feasible or necessary.
- The current uncommitted temperature/NUC changes are safe on hardware.

## Artifact identities

- Original APK: `docs/usb_camera_base.apk`
- Fixed diagnostic APK: `out/android/tony_uvc_diag_fixed.apk`
- Fixed diagnostic APK SHA256: `ca86a63df8381a0195d96e0d88115e86a8c9efe719ed696a2908d98ab52727fd`
- Invalid diagnostic APK SHA256: `faab593797c0a9ec9197d028819980003a1bc4134dedb3ceac9f42a6d56e2d3f`
- Static MJPEG test UF2 SHA256: `1eb002b871c392f9e1964d327b8f073f5b31eb59fbda9247490df82b69ec4dcd`
- Current OFF UF2 SHA256: `0a9cf317db0b5e61c446ab8769f499c244cda807e560d47451d43640224ceeb0`
- Android-test pre-MJPEG formal backup SHA256: `80dea1dc42a251b2f80aefd43eafa55de8562794f091103e1e9dfb0054e849a1`

## Current follow-up

1. Preserve the current OFF UF2 and its SHA256 as the UVC baseline.
2. Continue the uncommitted `src/main.c` temperature/NUC work without changing UVC descriptors, endpoint, payload, or frame format.
3. Build and record a new UF2 hash, then perform cold-start and runtime FFC hardware checks; mark results explicitly as verified or unverified.
4. Before claiming the latest artifact passes all four platforms, repeat Linux/V4L2 and guvcview capture on that exact UF2.
