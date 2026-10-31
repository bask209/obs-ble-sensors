# Test results (v1.0.0, 2026-10-31)

Environment: Windows 11 Pro 25H2 (10.0.26200), MSVC 14.44 (VS 2022 Build Tools), Windows SDK
10.0.26100, CMake 4.4.4, OBS SDK 31.1.1, tested in **OBS Studio 32.2.2**. Bluetooth adapter: Qualcomm
FastConnect 7800.

Nothing below is claimed beyond what was actually run.

## 1. Automated unit tests: 49/49 passed

`ctest --test-dir build_tests -C Release` (`tests/`), run 5 times in a row with no flaky failures.

| Area | Tests |
| --- | --- |
| Parser (0x2A37) | UINT8, UINT16, out-of-range, zero, malformed/truncated (5 variants), contact status (supported/detected/lost), energy + RR intervals, odd trailing RR byte, Polar H10-style packet |
| Visibility / state machine | not configured, every connecting state hidden, valid shows, stale after timeout (2/5/15 s), disconnect hides immediately, contact lost/regained, zero BPM, malformed does not refresh, late packet after disconnect ignored, new connection needs new data, packet during subscription kept, unsupported/BT off/error hidden, clock going backwards |
| Backoff, registry | exponential growth/cap/reset with jitter; add/rename/forget/dedupe, throttled `noteConnected` persistence, load sanitizing |
| BleManager + mock backend | 72 BPM, UINT16, malformed never shows, sudden disconnect → hidden immediately → auto-reconnect → new data required, sensor off/on, no notifications (watchdog reconnect), data stops → hidden within timeout, contact lost, device not found, unsupported device, **two sources share one connection**, switching device has no stale data, Bluetooth off/on, auto-reconnect disabled + manual reconnect, advertisement wakes reconnect early, scan listener lifecycle, release during connection attempt is non-blocking, **shutdown while connecting** (<1 s, clean), manager destroyed with live subscriptions, subscribe/release stress |

## 2. Build

- `cmake --preset windows-ci-x64` + Release build with **warnings as errors**: succeeded, 0 warnings.
- `obs-ble-sensors.dll` (Release, x64) imports only `obs.dll`, `obs-frontend-api.dll`, Qt6
  Core/Gui/Widgets (shipped with OBS), the MSVC runtime, and Windows system libraries.
- The release DLL contains no simulated-backend code (checked by string search, also enforced in CI).
- Packages: `obs-ble-sensors-1.0.0-windows-x64-setup.exe` (Inno Setup; silent install + uninstall verified in a scratch folder: correct files, Apps entry registered and removed) and the `.zip`.

## 3. OBS integration (real OBS 32.2.2, isolated portable copy, driven by obs-websocket)

Driver: `tests/integration/obs_itest.mjs`. It takes `GetSourceScreenshot` PNGs every second and counts
non-transparent pixels. Raw logs: `docs/itest-*.log`. Development build with the simulated sensor
(40 s cycle: 20 s data, 10 s silent, 5 s off, 5 s back on). Video set to 1920x1080 at **60 FPS**.

| Acceptance test | Result |
| --- | --- |
| Plugin loads; `ble_heart_rate_source` registered (shows as "BLE Heart Rate") | ✅ |
| **A** source without sensor | ✅ 420x96, 0 visible pixels (stable size, fully transparent) |
| **C** valid data shows | ✅ "♥ 70 BPM" rendered (see `docs/example.png`) |
| **D** notifications stop | ✅ hidden ~5 s after the last packet (default timeout) |
| **E** sensor off / disconnect | ✅ transparent while off |
| **F** sensor back on | ✅ reconnected automatically, visible again only after new data |
| **L** two sources, same sensor | ✅ both live; log shows a single connection per cycle |
| **K** switch to unsupported device | ✅ transparent on the next sample |
| **G** switch back | ✅ no stale value shown; waits for fresh data |
| **B/J** close OBS (graceful), restart | ✅ `device_key` and transform (321,123) restored; auto-reconnected; BPM shown only after the first valid reading; registry `devices.json` loaded, updated atomically (`.bak` present) |
| **H** clean shutdown | ✅ every shutdown logged "BLE manager stopped", "plugin unloaded", **"Number of memory leaks: 0"** (dev and release DLLs, including while the real backend was retrying) |
| **I** performance | ✅ 60.0 FPS, **0 skipped frames out of 2908**, average frame render 0.18 ms, OBS CPU 0.4 %. Streaming and recording were *not* started. |
| Release DLL + real WinRT backend, absent sensor | ✅ transparent for 20 s, 60 FPS, 0 skipped frames, clean shutdown |

## 4. Real Bluetooth hardware (WinRT backend via `ble-probe`)

| Check | Result |
| --- | --- |
| Advertisement scanning | ✅ 18 nearby BLE devices discovered in 20 s; names and HR-service flags parsed |
| Connect + GATT discovery on a real **non-heart-rate** device (Govee thermometer) | ✅ reported as **Unsupported** ("no standard Heart Rate service"), output transparent |
| Absent address | ✅ stays in "Searching", retries with backoff, transparent |
| Bug found and fixed by this test | The first version polled with C++/WinRT `wait_for()`, which re-assigns the `Completed` delegate and failed with "A delegate was assigned when not allowed". Replaced with a single Completed handler plus an event. |

## 5. NOT verified (no hardware or UI automation available)

- A **real heart-rate sensor** (no HR strap was in range). Live BPM over real Bluetooth, the
  CCCD subscription on real hardware, and the real-sensor parts of acceptance tests B, C, D, E, F, G
  and L remain to be done with `docs/MANUAL_TEST_CHECKLIST.md`.
- Clicking through the Qt **Scan / Manage Sensors** dialog and the properties panel by hand. It
  compiles and is wired to the same manager APIs tested above, but nobody operated it interactively.
- Windows Bluetooth toggled off/on and sleep/wake with a real device (covered only by mock tests and
  the reconnect watchdog design).
- Streaming and recording running at the same time (only rendering at 60 FPS was measured).
- Install.cmd / Uninstall.cmd against the system-wide `C:\ProgramData\obs-studio\plugins` folder. The
  same DLL and layout were loaded from a portable OBS instead, so the system OBS installation was not
  modified.
- The Linux unit-test job runs only in CI.

## 6. Update: safety, resource use and new renderer

Run after the Bluetooth safety hardening, the resource optimization and the new renderer
(animated heart, contrast halo, inverted numbers):

- Unit tests: **55/55 passed** (3 consecutive runs). New tests cover: unsupported devices are never
  retried automatically, only after an explicit request; no background scanning by default; the
  background scan (when enabled) is passive and drops unrelated devices before parsing; silent
  sensors back off; heartbeat pulse shape and BPM timing; number/heart text parts.
- Real Bluetooth (`ble-probe`, absent sensor, 20 s): **≈0.16 % of one core** without background
  scanning vs ≈1.7 % with it. That measurement is why background scanning is off by default.
- Real OBS 32.2.2 (portable, simulated sensor, 1080p60):
  - The heart visibly pulses (frame-to-frame size changes).
  - Inverted digits are black over white, white over black and light over grey, split exactly at
    a background edge (see `docs/example.png`). The fixed-color mode works.
  - 0 skipped frames, average frame render 0.26–0.31 ms.
  - No blank frame across 160 captures spanning about 8 BPM changes (a one-frame flicker on BPM
    change was found and fixed).
  - Hide/show behaviour is unchanged. Clean shutdown with 0 memory leaks.
- Found and fixed during this round: the first halo (12 offset copies) also darkened the area under
  the digits, which turned the inversion into plain white. It is now a single-pass ring that never
  draws under the glyph.

Still not verified: a real heart-rate strap, clicking through the dialog by hand, the invert mode
under the OBS GPU-scaled preview with filters applied (documented as unsupported).

## 7. Final review round (v1.0.0)

- Unit tests: **69/69**, also under **AddressSanitizer** with 0 errors. New tests: FTMS bike,
  treadmill, rower and cross-trainer heart rate; PLX pulse rate (SFLOAT, NaN); 2.1 M fuzzed
  packets over all formats; random packets through the state machine; device-name sanitizing;
  hostile identity keys; first reading needs confirmation; silent-device back-off; adapter states;
  paired-device listing; event logging.
- Real Bluetooth (`ble-probe`): adapter state "ready"; paired devices listed (phone, game
  controller). Govee thermometer: cached, then uncached discovery, then **Unsupported**, never
  retried. Absent address keeps searching. Galaxy Watch5 Pro: the cached discovery found its
  heart-rate service and reached the subscription step. The watch did not answer at that moment
  (not broadcasting, or its private address had rotated), and the new private-address guidance
  was shown.
- User's OBS log (v1.0.0-beta.1, Galaxy Watch): 19 min stable live data, then a reconnect every
  ≈22 s while the watch sent no data. This led to the confirmation rule and the silent-device
  back-off.
- Real OBS 32.2.2 (portable, dev build): pause-when-hidden released the hidden source's link after
  exactly 30 s while a visible source kept its shared link. Pre-baked halo renders identically.
  0 skipped frames out of 7,567 at 1080p60; no flicker across 160 consecutive captures. Clean
  shutdown with 0 memory leaks.
- Release DLL: Control Flow Guard, CET-compatible, ASLR (high entropy) and DEP confirmed with `dumpbin`.

## 8. Release 1.0.1

The first `1.0.0` tag build failed in CI: the Windows AddressSanitizer job caught a
**use-after-scope in a unit test**. A test's logging callback wrote into a list declared after the
connection manager, so a worker thread could log into it after it went out of scope. The plugin's own
callback logs to OBS and is unaffected. The test was fixed, the callback lifetime rule was documented,
and the ASan suite then passed **20 consecutive runs** locally (plus 10 normal runs). Nothing was
released from `1.0.0`; the same code plus this fix ships as `1.0.1`.
