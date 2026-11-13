This page was written for someone who has never installed firmware before. Start to finish it takes 10 minutes, 2-3 of which is the writing itself. If you want to change the code or build it yourself, the **Getting Started** page takes you there; it is not needed for this page.

## What you need

|  |  |
|---|---|
| **A board** | One of the NarRTOS Wear boards: the AMOLED with the rectangular screen, or the round 240×240 LCD |
| **A USB cable** | With the right connector for your board, and one that **carries data** |
| **A computer** | Windows or macOS; an internet connection (the release files are downloaded) |
| **Corezzle Imager** | Corezzle's desktop app; installed below |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">The cable trap</div><div style="line-height:1.6;color:inherit">Some phone charging cables carry power only, not data. With a cable like that the board never shows up on the computer. If the board is not in the list, the first thing to try is another cable.</div></div>

## Installation

### 1. Download and install Corezzle Imager

Get the file for your operating system from Corezzle's download page.

- **Windows:** Run the downloaded installer. If Windows shows "Windows protected your PC", choose *More info* → *Run anyway*; the warning appears because the app is new. The installation completes by itself and the app opens.

- **macOS:** Open the downloaded `.dmg`, drag the Corezzle Imager icon into the Applications folder. On first launch, answer *Open* to the "downloaded from the internet" warning.

![The Corezzle Imager start screen](GORSEL: imager-01-welcome.png)

*The screen you see when the app opens. Steps in the left strip, NEXT at the bottom.*

### 2. Plug the board into the computer

Connect it with the USB cable; a small light on the board should come on. The board has a CH343 USB chip; most computers recognise it by themselves. If not, install the manufacturer's CH34x driver and restart the computer. You only need this if the board does not show up in step 5.

### 3. Pick your board (Device)

Open the app, press **NEXT**. In the board list click **NarRTOS Wear (ESP32-S3)**, then **NEXT**.

![Device step with NarRTOS Wear selected](GORSEL: imager-02-device.png)

*Device step: NarRTOS Wear (ESP32-S3) selected, tick mark on the right.*

### 4. Pick the release (OS)

Choose according to your board:

- **NarRTOS Wear · 410×502 AMOLED**: the board with the rectangular screen.

- **NarRTOS Wear · 240×240 round LCD**: the board with the round screen.

Picking the wrong one does no harm; the write finishes, the screen stays dark, and you write again with the right one. Then **NEXT**.

![OS step with the two releases](GORSEL: imager-03-os.png)

*OS step: two releases, the round LCD selected.*

### 5. Pick the USB port and write (USB Port)

Your board appears in the list as **USB Single Serial** (with an address such as `wchusbserial…` or `COM3` underneath). Click it. If it is not there, change the cable and press **Refresh List**; if it is still missing, install the driver from step 2.

Tick the checkbox at the bottom: the board's flash will be fully erased and rewritten (that is what a first install should do). Answer *OK* to the question, then **FLASH!**, then *OK* again.

![USB Port step with the port selected and the box ticked](GORSEL: imager-04-usb-port.png)

*USB Port step: the board selected as "USB Single Serial", checkbox ticked, FLASH! enabled.*

### 6. Wait (Writing)

The screen shows *Downloading*, *Connecting to the board*, *Erasing flash and writing* and a percentage in turn; it takes 1-2 minutes. Do not unplug the cable meanwhile. At the end it says *Restarting the board* and the **Done** screen appears.

![Writing step with the percentage running](GORSEL: imager-05-writing.png)

*Writing step: summary box and progress bar. The first 30-40 seconds are the erase; the percentage runs after that.*

### 7. What happens on the board

The board restarts itself; after 4-5 seconds the backlight comes on and the watch face appears. Swipe right on the screen to open the application menu. Later updates the device downloads itself (OTA); you use the Imager only for the first install or a reset.

![Done screen](GORSEL: imager-06-done.png)

*Done screen: writing finished, the board restarts. CLOSE quits, FLASH ANOTHER goes back for another board.*

## Where people get stuck on the first try

| What you see | Cause and fix |
|---|---|
| USB Port list is empty, "No USB serial port found" | Change the cable first (the most common cause), then another USB socket, then the CH34x driver. Press *Refresh List* after each try |
| Other ports are listed but not your board | Only the **USB Single Serial** row is the board; do not pick the others |
| "Expected an ESP32-S3 board, found …" | The port you picked is not the NarRTOS board; pick the right port |
| Writing stopped halfway, "Flashing failed" | The cable may have moved or been unplugged; make sure it is in and press *RETRY* |
| Writing finished but the screen is dark | Either the wrong release was picked (step 4) or the board did not restart: unplug and replug the cable. If that does not help, write again with the right release |
| Windows: "Windows protected your PC" | *More info* → *Run anyway*. The standard warning shown because the app is new |
| macOS: "cannot be opened because Apple cannot check it" | Right-click the app and choose *Open*; once is enough |

<div style="margin:40px 0 0"><a href="/en/document-content/narrtos-wear-6" style="display:flex;align-items:center;gap:22px;flex-wrap:wrap;border:1px solid #12A0AD;background:rgba(18,160,173,.07);border-radius:14px;padding:26px 28px;text-decoration:none;color:inherit"><div><div style="font-size:12.5px;font-weight:800;letter-spacing:1.4px;text-transform:uppercase;color:#12A0AD">Next step</div><div style="font-size:22px;font-weight:800;margin-top:4px;color:inherit">NarRTOS Wear</div><div style="font-size:15.5px;color:inherit;opacity:.72;margin-top:4px">Get to know the device: interface, apps, command interface, updates</div></div><div style="margin-left:auto;font-size:32px;color:#12A0AD">→</div></a></div>
