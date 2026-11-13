Every command in the NarRTOS documentation is typed into a terminal. VS Code is a free editor that shows the project files and a terminal in the same window. This page takes someone with no development tools on their computer to the point where they can paste the first command from Quick Start.

Followed in order it takes 10-15 minutes. At the end VS Code is installed, a terminal is open inside it, git is ready and the project files are on your computer. From there you continue in the **Quick Start** section of the NarRTOS Wear documentation.

## What you need

- A computer running Windows, macOS or Linux

- An internet connection (two programs will be downloaded)

- Your computer's password may be asked for during installation

## Installation

### 1. Download and install VS Code

Go to [code.visualstudio.com](https://code.visualstudio.com) and press the big **Download** button; the site detects your operating system itself.

- **Windows:** Run the downloaded `.exe`. Leave "Add to PATH" ticked among the wizard's options (it already is). VS Code opens when it finishes.

- **macOS:** Open the downloaded `.zip`; drag the **Visual Studio Code** icon that appears into your Applications folder and open it from there. On first launch, answer *Open* to the "downloaded from the internet" warning.

- **Linux (Ubuntu / Debian):** Download the `.deb` file, double-click it and choose *Install*. Or in a terminal: `sudo apt install ./code_*.deb`

When it opens you see a "Welcome" tab. The interface is in English; this page gives menu names exactly as they appear there.

### 2. Open a terminal inside VS Code

From the top menu choose **Terminal → New Terminal**. A panel opens at the bottom of the window; this is where you will type the commands. On Windows the panel runs PowerShell, on Mac zsh; the commands in this documentation are the same in both.

![New Terminal in the Terminal menu](GORSEL: vscode-new-terminal.png)

*macOS: from the menu at the top of the screen, **Terminal → New Terminal**.*

![The Terminal menu on Windows and Linux](GORSEL: vscode-win-new-terminal.png)

*On Windows and Linux the menu strip sits inside the window, at the top: **Terminal → New Terminal** (illustration).*

You type a command, press Enter, wait while it runs, and a new prompt appears when it finishes. The prompt ends in `>` on Windows and `%` or `$` on Mac and Linux; you type the command after that sign.

### 3. Install Git

Git is the tool that downloads the project files from the internet; the first command in Quick Start (`git clone`) uses it. First check whether it is already there:

```
git --version
```

If you see a line like `git version 2.x.x`, it is installed; go to step 4. If not:

- **Windows:** Download and run the installer from [git-scm.com/download/win](https://git-scm.com/download/win). Press *Next* on every screen; the defaults are right. When it finishes, **close VS Code completely and open it again**; otherwise the terminal will not see the new program.

- **macOS:** When you type `git --version`, the Mac opens a "command line developer tools" installation window. Choose *Install*; it downloads a few hundred MB and takes a few minutes. If no window appeared, type `xcode-select --install` in the terminal.

- **Linux:** `sudo apt install git`

Once the installation is done, run `git --version` again and make sure you see the version line.

### 4. Choose where the project will live

When the terminal opens you are in your home folder (`C:\Users\yourname` on Windows, `/Users/yourname` on Mac). The project you download lands inside whatever folder you are in at that moment. To keep things tidy, create a folder and enter it:

```
mkdir Projects
cd Projects
```

The first command creates the folder, the second enters it. The prompt now ends in `Projects`; that is how you can always tell where you are.

### 5. Paste the first command from Quick Start

Open the Quick Start section of the NarRTOS Wear documentation. The first line in the code box starts with `git clone`; copy that line and paste it into the terminal: Ctrl + V on Windows (right-click also pastes in PowerShell), ⌘ + V on Mac. Press Enter.

A few lines starting with `Cloning into ...` scroll past and it ends with `done.`. The project is now inside the `Projects` folder. The second command in Quick Start (the one starting with `cd`) takes you into that folder; paste it too and press Enter.

### 6. Open the folder in VS Code

From the top menu choose **File → Open Folder**, select the new folder inside Projects and press *Open*. VS Code may ask "Do you trust the authors of the files in this folder?"; if it does, answer **Yes, I trust the authors**. The project files appear on the left: `src`, `docs`, `platformio.ini` and the rest.

![Open Folder in the File menu](GORSEL: vscode-open-folder.png)

*macOS: **File → Open Folder…***

![The File menu on Windows and Linux](GORSEL: vscode-win-open-folder.png)

*On Windows and Linux: **File → Open Folder…**, in the menu strip inside the window (illustration).*

A "Do you want to install the recommended 'PlatformIO IDE' extension from PlatformIO for this repository?" bubble appears at the bottom right. Close it with the cross rather than pressing *Install* or *Show Recommendations*; this guide installs PlatformIO through the command-line route on the Getting Started page, no extension is needed.

![Extension recommendation bubble](GORSEL: vscode-eklenti-onerisi.png)

*The bubble that appears at the bottom right when the folder opens. Close it with the cross.*

Opening the folder may have closed the terminal at the bottom. Open it again with **Terminal → New Terminal**. This time the terminal opens directly inside the project folder, no `cd` needed; you can see the folder name in the prompt.

![Project open with the terminal at the bottom](GORSEL: vscode-explorer.png)

*The project open: file tree on the left, terminal at the bottom. The prompt shows the folder name `NarRTOS-Wear`.*

### 7. From here on

You now have: VS Code installed, a terminal open inside it, git ready, the project downloaded and open. You have completed the first two commands of Quick Start. The remaining two (`pio run ...` and `pio device monitor`) require PlatformIO to be installed.

If you have never installed it, go to the **Getting Started** page. Its step 1 (open a terminal) and step 5 (download the project files) are already done; continue from step 2 (Python). Every command there goes into this VS Code terminal.

## Where people get stuck on the first try

| What you see | Cause and fix |
|---|---|
| Installed git on Windows but `git` is "not recognized" | VS Code was open before the installation. Close it completely and open it again |
| No installation window appeared on the Mac | Type `xcode-select --install` in the terminal |
| Ctrl + V does not paste into the terminal | Paste with a right-click, or try Ctrl + Shift + V |
| `fatal: repository ... not found` | The address was copied incompletely. Select the line in Quick Start from start to end and copy it again |
| I do not know where I am | Look at the folder name at the end of the prompt. To list what is inside, use `ls` on Mac/Linux and `dir` on Windows |
| I answered *No* to the "Trust" question by mistake | Open the folder again with File → Open Folder; this time answer *Yes* |
| The terminal panel disappeared | Open it again with Terminal → New Terminal |

<div style="margin:40px 0 0"><a href="/en/document-content/narrtos-wear-6" style="display:flex;align-items:center;gap:22px;flex-wrap:wrap;border:1px solid #12A0AD;background:rgba(18,160,173,.07);border-radius:14px;padding:26px 28px;text-decoration:none;color:inherit"><div><div style="font-size:12.5px;font-weight:800;letter-spacing:1.4px;text-transform:uppercase;color:#12A0AD">Next step</div><div style="font-size:22px;font-weight:800;margin-top:4px;color:inherit">Quick Start</div><div style="font-size:15.5px;color:inherit;opacity:.72;margin-top:4px">Build the project and upload it to the board with four commands</div></div><div style="margin-left:auto;font-size:32px;color:#12A0AD">→</div></a></div>
