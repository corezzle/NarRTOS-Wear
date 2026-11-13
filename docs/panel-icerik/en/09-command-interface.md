The device can be driven from outside with text commands — for testing, demos, automation and remote control. A single **transport-agnostic parser core** is fed from two channels; the commands contain no new screen code, they are a bridge to the existing internal APIs.

```
USB seri (UART0 RX) ──┐        yanıtlar reply-sink ile geri döner:
BLE "NarRTOS CMD"   ──┴─► parser ──► USB'den gelen -> seriye (printf)
                                     BLE'den gelen -> telefona (notify)
```

## Command set

| Command | Effect |
|---|---|
| `help` / `info` | Command list / status JSON (version, uptime, heap, wifi, active application) |
| `app launch <name\|id>` | Opens an application (via `EVENT_APP_SWITCH` — on the LVGL task, thread-safe) |
| `app back` / `current` / `list` | Back • active application • name→id table |
| `swipe left\|right\|up\|down` | Gesture injection — navigating between screens |
| `brightness 0-255` | Display brightness |
| `set wifi\|bt on\|off` | WiFi preference (NVS, persistent) + connect/disconnect • BLE advertise/stop |
| `get wifi\|bt` / `toggle wifi\|bt` | Read state / invert it |
| `set rail <name> on\|off` / `get rails` | AXP2101 power rails (aldo1-4, dcdc2-4, bldo1-2) |
| `wake` / `reboot` | Wake the display / restart |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit"><code>set rail</code> only works on a board that has a PMIC; with no PMIC the command returns an honest error. <code>DCDC1</code> (the system rail) is deliberately absent from the list — turning it off kills the device.</div></div>

## Using it over USB

Type lines into a serial terminal (115200):

```
pio device monitor
# sonra:
help
app launch weather
brightness 30
```

Replies come back with a `[cmd]` prefix:

```
> app launch weather
[cmd] OK: launch weather (id=6)
> brightness 30
[cmd] OK: brightness=30
> get wifi
[cmd] OK: wifi connected=yes pref=on
```

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit">⚠ <strong>macOS + CH34x bridge:</strong> opening the port resets the device every single time. Open the port <strong>once</strong> and work within that session; do not reconnect for each command.</div></div>

## Using it from a phone (BLE — no dedicated app required)

```
GATT servisi "NarRTOS CMD":
  Service : 4E010063-6D64-004E-4152-52544F530001
  RX      : ...530002  (Write / WriteNoRsp)  <- UTF-8 komut satırı yaz
  TX      : ...530003  (Notify)              <- yanıtlar buraya düşer
```

**With nRF Connect:**

1. Connect to "NarRTOS Watch"

2. Enable **Notify** on the `...530003` characteristic (so you can see the replies)

3. Write the command to `...530002` as **UTF-8 text**: `app launch weather`

4. The application opens on the watch; the reply arrives as a notification

BLE does not suffer from the port-reset problem USB has; commands can be sent even while the device is on your wrist.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit">⚠ Sending <code>set bt off</code> <strong>from the phone</strong> cuts your own connection too. If the pairing breaks (iOS "Peer removed pairing information"): forget the device on the phone → connect again.</div></div>

## Architecture notes

- **Thread safety:** Opening an application does not call `app_manager_launch` directly — it publishes `EVENT_APP_SWITCH`, and `app_manager` opens it on its own LVGL task. The back operation is handed to the LVGL task with `lv_async_call`.

- **Waking the display:** `power_service_user_activity` is called before the command; while the display is off the transition animation does not complete.

- **Concurrency:** USB and BLE can send commands at the same time — the parser serialises them with a mutex.

- **Files:** `services/command_service.{h,cpp}` (core + USB), `services/ble_cmd_service.{h,cpp}` (GATT transport).

## Adding a new transport

A small adapter that feeds the line into `command_service_exec_ex(line, reply_fn, ctx)` is all it takes — the parser and the command set work unchanged (e.g. an HTTP endpoint, an MQTT topic).
