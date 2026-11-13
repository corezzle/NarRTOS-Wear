This page was written for someone who has never compiled embedded software before. Follow it in order and you will end up with NarRTOS running on the board in your hand. The first time takes 20-30 minutes, most of it one-off program installations. The second time you will be done in two commands.

## What you need

|  |  |
|---|---|
| **A board** | One of the supported ESP32-S3 boards (see Hardware and Board Variants) |
| **A USB cable** | With the right connector for your board, and one that **carries data** |
| **A computer** | Windows, macOS or Linux; all three work |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">The cable trap</div><div style="line-height:1.6;color:inherit">Some phone charging cables carry power only, not data. With a cable like that the board never shows up on the computer and you will assume it is broken. If the board does not appear, swapping the cable is the easiest thing to try and the one that most often works.</div></div>

## 1. Open a terminal

The whole setup is done from a window where you type commands. That window is called a terminal.

- **macOS:** open Spotlight (⌘ + space), type `Terminal` and press Enter

- **Windows:** type `PowerShell` into the Start menu and press Enter

- **Linux:** Ctrl + Alt + T

Into the window that opens you will type the commands one at a time, pressing Enter after each. While a command is running, wait; when it finishes the prompt comes back.

![Spotlight with Terminal typed](GORSEL: spotlight-terminal.png)

*macOS: type `Terminal` into Spotlight; the first result is the Terminal app.*

![PowerShell in the Windows Start search](GORSEL: windows-start-powershell.png)

*Windows: type `PowerShell` into the Start menu; the first result is Windows PowerShell (illustration).*

## 2. Check whether Python is installed

The build tool we will use is written in Python, so Python has to be there first. Type this into the terminal:

```
python3 --version
```

If you see a version number such as `Python 3.9.6`, it is installed; go to step 3. If you get an error like `command not found`, download and install it from [python.org/downloads](https://www.python.org/downloads/).

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Watch out during installation on Windows</div><div style="line-height:1.6;color:inherit">Make sure you tick the <strong>"Add Python to PATH"</strong> box at the bottom of the installer window. If you do not, Python is installed but the terminal cannot find it, and the next steps will fail.</div></div>

On Windows the command may be `python` rather than `python3`; try both.

## 3. Install PlatformIO

PlatformIO is the tool that turns source code into a file you can load onto the board. It downloads the compiler and the ESP32 toolchain itself, so you do not install those separately.

```
pip3 install platformio
```

The installation can take a few minutes. When it finishes, verify it:

```
pio --version
```

You should see output such as `PlatformIO Core, version 6.x.x`.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">If it says <code>pio: command not found</code></div><div style="line-height:1.6;color:inherit">The program is installed but the terminal does not know where. You can carry on by calling it with its full path: on macOS and Linux <code>~/.platformio/penv/bin/pio</code>, on Windows <code>%USERPROFILE%\Scripts\pio.exe</code>. Use that path in place of <code>pio</code> in every command from here on.</div></div>

![Checking Python and installing PlatformIO](GORSEL: terminal-setup.gif)

*Steps 2 and 3 in the terminal: Python version, PlatformIO installation and verification. Your version numbers may differ.*

## 4. Install the USB driver

On the board there is a small chip that converts USB into the signal the board understands. Your computer needs a driver to talk to that chip.

Most computers already have it. **Try step 6 first.** If the board shows up, skip this step. If it does not, install the driver for the chip on your board; the most common family is CH340/CH341 and the driver can be downloaded from the manufacturer's site. Restart the computer afterwards.

## 5. Download the project files

```
git clone https://github.com/corezzle/NarRTOS-Wear.git
cd NarRTOS-Wear
```

The first line copies the project onto your computer, the second enters that folder. Every command from here on is run from inside this folder.

If you get `git: command not found`, git is not installed. If you would rather not install it, use **Code → Download source code → zip** on the GitLab page, unpack the folder, then enter that folder in the terminal.

## 6. Plug in the board and confirm it is seen

Connect the board to the computer with the USB cable. A small light on the board should come on.

Confirm the computer can see it:

```
pio device list
```

In the output you should see a line such as `/dev/cu.usbserial-...` (macOS), `/dev/ttyUSB0` (Linux) or `COM3` (Windows). That is the board's address.

If nothing appears, try in this order: **a different USB cable** (the most common cause), a different USB port, then the driver installation in step 4.

![pio device list output](GORSEL: terminal-device-list.gif)

*When the board is seen, `pio device list` prints a line like this. The address (`/dev/cu.usbserial-…`, `COM3`) will differ on your machine.*

## 7. Build and upload to the board

A single command both builds and uploads:

```
pio run -e esp32s3_wearable -t upload
```

**The first build takes a while.** The compiler and libraries have to be downloaded, so it takes a few minutes. Long lines scroll past on screen; that is normal. Later builds take about 15 seconds if you do not clean first.

If you see `SUCCESS` at the end, it worked; the board restarts itself and NarRTOS comes up.

Later, when you change the code and upload again, clean first, then build:

```
pio run -e esp32s3_wearable -t clean
pio run -e esp32s3_wearable -t upload
```

The first command deletes the files produced by the previous build, the second compiles from scratch and uploads to the board. That way you avoid the "my change did not show up on the device" problem caused by stale files. A clean build takes a few minutes, like the first one.

If your board has the 1.28-inch round display, change the environment name in the middle of the command:

```
pio run -e esp32s3_lcd128 -t upload
```

Which board maps to which name is listed on the Hardware and Board Variants page.

![Building and uploading to the board](GORSEL: terminal-build.gif)

*The build and upload flow. Long lines scroll past, `SUCCESS` appears at the end and the board restarts.*

## 8. See that it works

A watch face should appear on the screen. Swipe right with your finger and the application menu opens.

If you want to see what the device is doing internally, open the serial monitor:

```
pio device monitor
```

The boot log starts scrolling. While the monitor is open you can type `help` and press Enter to drive the device with commands (see Command Interface). Press Ctrl + C to leave the monitor.

![Boot log in the serial monitor](GORSEL: terminal-monitor.gif)

*The boot log in the serial monitor. The last line shows the interface has started; press Ctrl + C to leave.*

## Where people get stuck on the first try

| What you see | Cause and fix |
|---|---|
| `pio: command not found` | PlatformIO is installed but the terminal cannot find it; use the full path from step 3 |
| `pio device list` comes back empty | Most likely a charge-only cable; change the cable first, then install the driver |
| Checksum error while uploading | Some USB bridges are unstable at high speed (see Troubleshooting) |
| Nothing at all in the monitor | The wrong speed may be selected; `pio device monitor` sets the right speed itself |
| Display is on but there is no image | It may have been built for the wrong board; check the environment name in step 7 |

For anything not on this list, see the Troubleshooting page.

## What happens on the first boot?

1. System services come up in order (NVS → I2C → PMIC → event bus → …)

2. Screen brightness ramps up smoothly from 0 (the visual "power-on" moment)

3. The watch face (WatchFace) loads; touch and buttons become active

4. If the user preference for WiFi is enabled, it connects to the stored network

---

## Build environments

The `-e esp32s3_wearable` part in the commands above says which board to build for. The supported environments:

| Env | Board | Key settings |
|---|---|---|
| `esp32s3_wearable` | ESP32-S3R8 + CO5300 AMOLED | 8 MB flash QIO, Octal PSRAM, upload 921600 |
| `esp32s3_lcd128` | 240×240 round LCD board | 16 MB flash, Quad PSRAM, upload 460800, 240×240 |

Each environment has its own `sdkconfig.<env>` file (ESP-IDF settings such as PSRAM type and flash size). The partition layout is built around A/B OTA: two application partitions (app0/app1) plus NVS and an assets partition.

## Configuration

The single central configuration file is `src/core/os_config.h`: version, pin map, task core/priority assignments, display buffer, external service addresses.

Every definition is `#ifndef`-guarded, meaning it can be overridden from `platformio.ini` with a build flag:

```
build_flags =
    '-DOTA_BACKEND_HOST="http://192.168.1.50:3001"'
```

## Next steps

- Understand how the system works → Architecture

- Write your own application → Application Development

- Drive the device remotely → Command Interface

- Stuck? → Troubleshooting
