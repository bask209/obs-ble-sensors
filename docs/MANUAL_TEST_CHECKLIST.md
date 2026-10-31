# Manual verification checklist (real BLE hardware)

Use a standard heart-rate strap (e.g. Polar H10, Wahoo TICKR) and, for K, any non-heart-rate BLE
device. Record pass/fail, the OBS version and the sensor model. Useful aids:
- OBS log: Help → Log Files → Show Current Log (lines start with `[obs-ble-sensors]`).
- `ble-probe.exe` (from `build_tests\Release`) to scan and watch the live state outside OBS.
- To make transparency easy to see, put a solid Color Source behind the BLE Heart Rate source.

| # | Steps | Expected | Pass |
|---|---|---|---|
| 0 | Install with Install.cmd, start OBS | Log shows `plugin loaded successfully`; Sources → + lists **BLE Heart Rate**; Tools → Plugin Manager lists the plugin | ☐ |
| A | Add a BLE Heart Rate source, no sensor selected | Nothing visible; selecting the source shows a stable bounding box; status "No sensor selected" | ☐ |
| — | Scan / Manage Sensors with the strap worn | Dialog opens immediately; strap appears within ~10 s as "Heart-rate sensor"; UI stays responsive; OBS preview keeps running | ☐ |
| — | Use selected sensor → name it "Chest strap" | Dropdown shows "Chest strap"; BPM appears within a few seconds | ☐ |
| C | Compare with the sensor's own app or watch | Same BPM (±1); default style "♥ 82 BPM"; transparent background | ☐ |
| D | Take the strap off (or wet electrodes dry) | Disappears within the timeout (5 s); status says no skin contact or waiting for data | ☐ |
| E | Put the strap back on, then remove its battery or walk out of range | Disappears no later than the timeout, immediately once Windows reports the disconnect; status "Disconnected - will reconnect automatically" / "Looking for the sensor..." | ☐ |
| F | Put the battery back / return in range | Reconnects without any click, shows BPM after the first valid packet | ☐ |
| B | Close OBS, reopen | Sensor still saved (dialog shows it even when off); reconnects by itself | ☐ |
| J | Scene Collection → switch away and back | Source, position, transform, filters and selected sensor preserved | ☐ |
| G | Save a second sensor, switch the source to it | Old BPM disappears at once; new sensor's BPM appears only after its first packet | ☐ |
| L | Add a second BLE Heart Rate source using the same sensor | Both show the same BPM; only one connection (log shows one "connected" line) | ☐ |
| K | Select a non-heart-rate BLE device (Show all Bluetooth devices) | Output stays transparent; status "This device does not provide standard Bluetooth heart-rate data" | ☐ |
| — | Turn Bluetooth off in Windows, then on | Hidden while off (status "Bluetooth is turned off"); reconnects after it is on | ☐ |
| — | Sleep the PC with OBS open, wake it | Reconnects automatically within ~30 s of the strap being worn | ☐ |
| — | Change style: each display style, font, color, outline | Rendering updates; source does not jump; still hidden when no data | ☐ |
| — | Stale timeout 2 s and 15 s | Hides after about 2 s / 15 s without data | ☐ |
| H | Close OBS while the dialog is scanning, and again while a sensor is connecting (switched on just before) | OBS exits normally, no crash dialog; log ends with `plugin unloaded` and no crash | ☐ |
| I | Stream (or record) at 1080p60 for 10 minutes with the source live | Stats dock: no increase in render-lag/skipped frames compared with the source hidden | ☐ |
| — | Uninstall.cmd | Plugin folder removed; OBS starts normally; sources show as missing plugin | ☐ |
| — | Watch in heart-rate broadcast mode (pair it in Windows first if it uses a private address) | Appears as "Paired with Windows" or a heart-rate device; BPM shown; reconnects after the watch restarts broadcasting | ☐ |
| — | Smart bike / treadmill with a strap paired to it (FTMS) | BPM from the machine shown; hidden when the machine reports no heart rate | ☐ |
| — | Bluetooth pulse oximeter (PLX) | Pulse rate shown; hidden when the finger is removed | ☐ |
| — | Hide the scene containing the source for >30 s, show it again | Log shows "connection closed", then reconnects; BPM back within a few seconds | ☐ |
