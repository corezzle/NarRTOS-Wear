<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">If you want the ready-made build without compiling anything</div><div style="line-height:1.6;color:inherit">The <strong>Corezzle Imager</strong> desktop app writes the ready-made NarRTOS build onto the board: plug in the board, pick your board, press the button. Step by step: <a href="/en/document-content/narrtos-wear-corezzle-imager-ile-yukleme-12">Installing with Corezzle Imager</a>. The steps below are only for people who want to build and change the code themselves.</div></div>

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Before you start the setup</div><div style="line-height:1.6;color:inherit">If VS Code is not installed on your computer, install it first: <a href="/en/document-content/vs-code-kurulumu-10">VS Code Setup</a>. If VS Code is installed but you do not know how to download and open the project inside it, the <a href="/en/document-content/narrtos-wear-vs-code-kurulumu-11">VS Code Setup for NarRTOS</a> page shows step by step where to paste the first command below. Then come back here.</div></div>

If PlatformIO is already installed on your computer and the board is plugged in, four commands are enough:

```
git clone https://github.com/corezzle/NarRTOS-Wear.git
cd NarRTOS-Wear
pio run -e esp32s3_wearable -t upload
pio device monitor
```

In order: download the project, enter its folder, build and upload to the board, listen to the device's output.

![The four commands in the terminal](GORSEL: terminal-quick-start.gif)

*The four commands end to end: download, enter the folder, build and upload, boot log. Output shortened.*

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">First time setting this up?</div><div style="line-height:1.6;color:inherit">These four commands assume the required programs are already installed. If you have never done this before, start from <strong>Getting Started</strong>. Every step is covered one by one, from installing Python and PlatformIO to plugging in the cable.</div></div>
