OBS BLE Sensors - live heart rate from Bluetooth LE sensors in OBS Studio
=========================================================================

Requirements
  - Windows 10 (1903+) or Windows 11, 64-bit, with Bluetooth LE
  - OBS Studio 31 or newer (tested with OBS 32.2.2)
  - A standard Bluetooth LE heart-rate sensor (Heart Rate Service 0x180D),
    e.g. a chest strap or an arm band.

Install
  1. Close OBS Studio.
  2. Double-click Install.cmd
     (or copy the "obs-ble-sensors" folder into C:\ProgramData\obs-studio\plugins\).
  3. Start OBS.

Use
  1. Sources > + > BLE Heart Rate > OK.
  2. Put the sensor on (most chest straps only wake up when worn).
  3. Click "Scan / Manage Sensors...", select your sensor, click
     "Use selected sensor", give it a name, done.

  From now on the source connects by itself every time OBS starts and shows
  your BPM only while fresh, valid readings arrive. Whenever the sensor is off,
  out of range, not worn or silent, the source is completely transparent.

Uninstall
  Close OBS and double-click Uninstall.cmd
  (or delete C:\ProgramData\obs-studio\plugins\obs-ble-sensors).

More information: https://github.com/bask209/obs-ble-sensors
