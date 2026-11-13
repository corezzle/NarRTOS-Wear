## Building and flashing

| Symptom | Cause / fix |
|---|---|
| Checksum error while flashing (`Failed to write to target RAM`) | Some USB-serial bridges are unstable at 921600 → lower `upload_speed` to **460800** |
| `pio` not found | PlatformIO Core is not installed: `pip install platformio` |
| Port not found | Is the USB-serial driver (e.g. CH34x) installed? Check with `ls /dev/cu.*` |
| The wrong board is being built | Pass the `-e <env>` parameter: `pio run -e esp32s3_lcd128` |
| My change does not show up on the device | Clean first, then build: `pio run -e esp32s3_wearable -t clean`, then the same command with `-t upload` |

## Serial monitor

| Symptom | Cause / fix |
|---|---|
| No output at all in the monitor (0 bytes) | The reading tool may be driving the DTR/RTS lines incorrectly → open the port with `DTR=false` and issue the reset pulse with RTS |
| The device restarts on every connection | Known behaviour of macOS + the CH34x bridge. Open the port **once** and work within that session |
| Logs are garbled/meaningless | The baud rate must be 115200 |

A working Python example for the DTR/RTS problem:

```
import serial, time
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = "/dev/cu.usbserial-XXXX", 115200, 1
ser.dtr = False; ser.rts = False      # açılışta hatları ASSERT ETME
ser.open()
ser.setDTR(False)                     # GPIO0 = high -> normal boot
ser.setRTS(True);  time.sleep(0.15)   # EN = low  -> reset'te tut
ser.setRTS(False)                     # bırak -> cihaz app'i boot eder
while True:
    print(ser.readline().decode("utf-8", "replace"), end="")
```

## Boot

| Symptom | Cause / fix |
|---|---|
| Touch I2C error at boot | May be normal — the device becomes ready late; the driver is registered anyway and recovers on the first touch |
| "PMIC not found" warning | Expected behaviour on a board without a PMIC; the system continues to boot |
| RTC not found | The internal clock is used; it is synchronised over NTP |
| Display is on but there is no image | An error may have occurred before the brightness ramp started; check the display driver and `ui_service` lines in the boot logs |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Fault tolerance principle</div><div style="line-height:1.6;color:inherit">The system continues to boot even when peripheral init fails. A single faulty peripheral never leaves the device unable to start.</div></div>

## WiFi

| Symptom | Cause / fix |
|---|---|
| Does not connect at boot | The user preference in NVS may be OFF (deliberate behaviour) — enable it from Settings |
| Endless `retry 1/5 … 5/5` | The network is out of range or the password is wrong; reconnect from Settings → WiFi |
| The connection drops during a scan | Expected — an active scan briefly interrupts the connection (see Telemetry and Positioning) |

## The telemetry stream has stopped

Check in this order:

1. **Did the server's IP change?** The most common cause. When the development machine's DHCP address changes, the device POSTs to a dead address. Permanent fix: a **static DHCP reservation** on the router. Temporary fix: update `OTA_BACKEND_HOST` in the build flag and re-flash.

2. **Is the server up?** `curl http://<ip>:3001/api/wear-os/health`

3. **Is the device powered on and on WiFi?** Check the serial log.

## Command interface

| Symptom | Cause / fix |
|---|---|
| I type a command and get no reply | The device may be mid-boot — wait for the `CmdSvc: init OK` line |
| `app current` returned `id=-1` | The application transition did not complete; the animation never finishes while the display is off (send `wake` before the command) |
| "Peer removed pairing information" over BLE | Bond mismatch → forget the device on the phone, restart the app, connect again |
| `set rail` → `NOT_SUPPORTED` | The board has no PMIC — an expected, honest error |

## UI / layout

| Symptom | Cause / fix |
|---|---|
| Icons look out of proportion on the small screen | Preserve the icon-to-circle ratio (a 24 px symbol in a 40 px circle → 16 px in a 26 px circle) |
| Row spacing is far too wide | The list `STEP` should be the row height + ~8 px; do not carry the large-screen value over |
| The title collides with the clock | On a round display the usable width at the top edge is narrow — stack elements vertically rather than side by side |
| Text overflows / gets clipped | Declare the size constants in the variant block (see Application Development) |

## Getting help

If the problem persists: open an issue and include the serial boot log, the build environment you used (`-e ...`) and your board model.
