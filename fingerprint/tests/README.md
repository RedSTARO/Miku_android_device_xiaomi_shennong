# Shennong fingerprint regression tests

The tests compile the production `Session.cpp`, `FingerprintEngine.cpp`,
`LockoutTracker.cpp`, `SessionTimer.h` and Android's `WorkerThread.cpp`. A fake
Goodix function table replaces the physical sensor. The host runner supplies
small Android binder, configuration and logging shims; it does not replace the
operation state machine, callback conversion, cancellation, timers or lockout
logic. Host success is not a device/TEE/HBM validation.

From the Android source root:

```sh
python3 device/xiaomi/shennong/fingerprint/tests/run_host_tests.py
python3 device/xiaomi/shennong/fingerprint/tests/run_host_tests.py --sanitize address
python3 device/xiaomi/shennong/fingerprint/tests/run_host_tests.py --sanitize thread
```

The address configuration also enables UBSan. A test waits for the actual
10-second production lockout timeout. The other timer/queue tests use barriers,
not assumptions about thread scheduling. No files under `out/` are touched.

The native build uses the same `SessionTest.cpp` with real AIDL/NDK headers and
libraries instead of host shims:

```sh
m shennong_fingerprint_test
adb root
adb shell mkdir -p /data/nativetest64/vendor/shennong_fingerprint_test
adb push out/target/product/shennong/data/nativetest64/vendor/shennong_fingerprint_test/shennong_fingerprint_test /data/nativetest64/vendor/shennong_fingerprint_test/
adb shell chmod 0755 /data/nativetest64/vendor/shennong_fingerprint_test/shennong_fingerprint_test
adb shell /data/nativetest64/vendor/shennong_fingerprint_test/shennong_fingerprint_test
```

Run the binary from the vendor test directory. This tree's
`system/linkerconfig/contents/configuration/baseconfig.cc` maps
`/data/nativetest64/vendor` to the vendor linker configuration, which can load the
test's vendor-private shared libraries. The generic `/data/local/tmp` directory
does not select that configuration.

Covered cases:

- Initialized state and unavailable HAL: deterministic `HW_UNAVAILABLE` rather
  than dereferencing a missing device.
- Cancellation before start, while running, repeated cancellation and stale
  cancellation signals after success/a subsequent request.
- A synchronous vendor cancellation callback must not finish the operation
  before the vendor's cancel function has returned.
- Close waits for cancellation acknowledgment. Old-session callbacks and
  pointer-up calls cannot modify a new session's hardware state.
- Vendor cancellation failure makes the engine unavailable; it cannot start a
  new operation while the old hardware operation may still be active.
- Full 64-bit authenticator IDs and batched template enumeration/removal.
- Successful matches reset failure counts; counters are isolated by Android user.
- Enrollment HAT version/zero initialization, exact MAC length, and cancellation.
- Concurrent start/close admission and a delayed old-session close cleanup.
- Live timeout delivery, destruction of an old session/callback before its
  timeout, timer cancellation/restart, and absence of cross-user timeout effects.

## Vendor ABI evidence

This adapter targets the checked-in OS2.0.217 blobs. It is not the standard
legacy fingerprint 2.1 C ABI despite that module version number.

| Blob | SHA-256 |
|---|---|
| `vendor/xiaomi/shennong/proprietary/odm/lib64/hw/fingerprint.goodix_fod.so` | `d77c55b884f2c8971744e8e904a1cab325a2fbbb57196f1bc6ca0a7f4744d750` |
| `vendor/xiaomi/shennong/proprietary/odm/lib64/libgf_hal.so` | `2d916158d7cce24340d78b5be65dfe0e197e35c379234a853034700768239fbc` |

Disassembly of these exact binaries with `llvm-objdump -d` establishes:

- Module cancel at `0x24a8` passes `true` (`0x24d0`) to
  `goodix::FingerprintCore::cancel(bool)`. In `libgf_hal.so`, `0x32fc8` selects
  error 5 and invokes the error callback at `0x32fd0`, **before** issuing the TA
  command at `0x33008` and returning at `0x33090`. The wrapper therefore queues
  vendor notifications on its worker and acknowledges cancellation only after
  this function returns. It does not synthesize an early cancellation response.
- Generate challenge at `0x2fc8`, get authenticator ID at `0x3140`, and invalidate
  authenticator ID at `0x31e0` deliver their 64-bit value via message payload
  offset 8, then explicitly return zero at `0x304c`, `0x31c4`, and `0x3264`.
  Their return register is status, not the challenge/authenticator ID.
- Enumerate at `0x24f8` and remove at `0x2690` each zero one message buffer and
  write up to five nonzero IDs consecutively at payload offsets 0, 4, 8, 12, 16
  (message offsets 8 through 24), then notify **once**, at `0x265c` and `0x28b8`.
  They do not send legacy `(fid, remaining_templates)` iterator messages.
  Enumeration returns its operation status at `0x2670`; remove explicitly
  returns zero at `0x28cc`, reporting successfully removed IDs in the array.

Recheck these functions if the firmware blobs change. The tests reproduce the
observed synchronous cancellation and fixed-array message layouts. They cannot
prove compatibility with another blob revision.

## Validation in this workspace

The 13-case production-code host suite passed with AddressSanitizer and
UndefinedBehaviorSanitizer. ThreadSanitizer compilation succeeded, but the test
process failed before running the suite. A separate minimal program containing
only a thread creation/join failed with exit 66 and
`FATAL: ThreadSanitizer: unexpected memory mapping` on this WSL host. TSan is
**unverified**, not passed; no sanitizer checks were disabled to obtain a pass.
The smoke source and recorded output are in
`out/shennong-onpaper-fixes/fingerprint-tsan-smoke.cpp` and
`fingerprint-tsan-smoke.log` under the Android source root. The ASan/UBSan result
is recorded alongside them in `fingerprint-host-asan-ubsan.log`.

## Remaining device checks

The wrapper still uses the existing software lockout policy; this change does
not claim to add secure-world verification of `resetLockout` HATs. Physical
sensor initialization, enrollment/recognition quality, cryptographic operations
with the actual TEE, display illumination timing, screen-off behavior and vendor
failure/recovery paths still require a real device.
