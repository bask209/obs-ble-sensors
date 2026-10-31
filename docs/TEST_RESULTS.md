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
- Package: `release/obs-ble-sensors-1.0.0-windows-x64.zip` (standard OBS plugin layout plus Install/Uninstall scripts).

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
