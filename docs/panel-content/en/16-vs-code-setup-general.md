Visual Studio Code (VS Code for short) is Microsoft's free, open-source code editor. It runs on Windows, macOS and Linux and brings your files, a terminal and extensions together in one window. This page covers installing it from scratch and the few things worth knowing on first launch.

The installation takes 10 minutes and needs no prior knowledge. At the end VS Code is open, working inside a folder, and ready for you to use the terminal.

## What VS Code is, and what it is not

VS Code is a text editor: it opens code files, colours them and helps you edit them. It does not compile or run programs itself; extensions and the tools you run in the terminal do that. That is why it is lightweight and usable for every language.

Do not confuse it with **Visual Studio**: that is a much larger, separate Microsoft program that runs only on Windows. This page is about VS Code.

## Installation

Go to [code.visualstudio.com](https://code.visualstudio.com). The big **Download** button detects your operating system itself; then follow the section for your system.

![The code.visualstudio.com download page](GORSEL: vscode-download.png)

*The download page. The big button is named after your system: macOS here, Windows or Linux on yours.*

### Windows

- Run the downloaded `.exe`; accept the licence and leave the install location as it is.

- On the **Select Additional Tasks** screen tick these: *Add "Open with Code" action to Windows Explorer file context menu*, the *directory* variant on the same line, *Register Code as an editor for supported file types* and *Add to PATH* (already ticked). This lets you right-click any folder and open it in VS Code.

- **Install**, then **Finish**. VS Code opens.

### macOS

- Double-click the downloaded `.zip`; the **Visual Studio Code** application appears.

- Drag that icon into your **Applications** folder. Leaving it in Downloads causes trouble with updates.

- Open it from Launchpad. Answer *Open* to the "downloaded from the internet" warning.

- If you will use it often, right-click the Dock icon and choose *Options → Keep in Dock*.

- To open it from a terminal with the `code` command: inside VS Code press ⇧⌘P, type `Shell Command: Install 'code' command in PATH` and press Enter. From then on, typing `code .` in any terminal opens the current folder in VS Code.

### Linux

- **Ubuntu / Debian:** download the `.deb` and run `sudo apt install ./code_*.deb` in a terminal (or double-click the file and choose *Install*). This also adds Microsoft's package repository; later updates arrive through `apt`.

- **Fedora / RHEL:** download the `.rpm` and run `sudo dnf install ./code-*.rpm`.

- **Distributions with Snap:** `sudo snap install code --classic`

## First launch

You are greeted by the **Welcome** tab. You can pick a dark or light theme here; to change it later use *File → Preferences → Theme → Color Theme*.

![The VS Code Welcome tab](GORSEL: vscode-welcome.png)

*First launch: Explorer on the left, the Welcome tab in the middle.*

The interface is in English. Most documentation and tutorials use the English menu names, so leaving it in English at first makes them easier to follow. If you still want it in your own language, it takes a few steps:

1. In the vertical strip on the far left of the window, click the icon that looks like four squares; that is the **Extensions** view. Ctrl + Shift + X (⇧⌘X on Mac) also opens it.

2. Type the name of your language followed by `Language Pack` into the search box at the top, for example `Turkish Language Pack`.

3. The list shows **Turkish Language Pack for Visual Studio Code** (publisher: Microsoft). Press the blue **Install** button next to it; it takes a few seconds.

4. A question appears at the bottom right: "Would you like to change Visual Studio Code's display language to Turkish and restart?". Press **Change Language and Restart**. VS Code closes and reopens in that language.

![The Turkish Language Pack extension page](GORSEL: vscode-extensions-tr.png)

*The extension page: publisher Microsoft, the blue Install button.*

![The Change Language and Restart bubble](GORSEL: vscode-change-language.png)

*The question that appears at the bottom right right after installation. Press the button and VS Code reopens in Turkish.*

If the question did not appear: press Ctrl + Shift + P (⇧⌘P on Mac), type `Configure Display Language`, press Enter and pick the language from the list; VS Code asks to restart.

Once you have switched, use the translated names for the English menu items on this page; for example **Terminal → New Terminal** becomes **Terminal → Yeni Terminal** in Turkish. To switch back, run the same command and choose **English**.

Updates download by themselves. When "Restart to update" appears at the bottom right, restart; it takes a few seconds.

## Get to know the window

![The regions of the VS Code window](GORSEL: vscode-window.png)

*The six regions of the window. The numbers follow the same order as the table below.*

| Area | Where | What it does |
|---|---|---|
| **Activity Bar** | The vertical strip on the far left | Switches between views: Explorer (files), Search, Source Control (git), Run and Debug, Extensions |
| **Sidebar** | The panel next to the strip | Contents of the selected view; in Explorer, the file tree of the open folder |
| **Editor** | The large area in the middle | Files open as tabs; can be split side by side |
| **Panel** | The bottom section | Terminal, Problems (errors), Output |
| **Status Bar** | The thin strip at the very bottom | Line/column, file type, git branch, error count |
| **Command Palette** | Ctrl + Shift + P (⇧⌘P on Mac) | Reach any command by typing its name; faster than hunting through menus |

## Open a folder

VS Code works with folders, not files. Choose **File → Open Folder** and select your project folder. VS Code may ask "Do you trust the authors of the files in this folder?"; for your own projects and things downloaded from sources you know, answer **Yes, I trust the authors**. The Explorer on the left lists the folder's contents; clicking a file opens it in the middle.

![Open Folder in the File menu](GORSEL: vscode-open-folder.png)

*macOS: **File → Open Folder…***

![The File menu on Windows and Linux](GORSEL: vscode-win-open-folder.png)

*On Windows and Linux: **File → Open Folder…**, in the menu strip inside the window (illustration).*

Return to folders you opened before with *File → Open Recent*. To open a single file, use *File → Open File*.

## Use the terminal

**Terminal → New Terminal** opens a terminal in the bottom panel. The shortcut is Ctrl + ` (also Ctrl + ` on Mac); if your keyboard has no ` key, use the menu. The terminal starts inside the folder that is currently open, so you type commands without going anywhere first.

![New Terminal in the Terminal menu](GORSEL: vscode-new-terminal.png)

*macOS: from the menu at the top of the screen, **Terminal → New Terminal**.*

![The Terminal menu on Windows and Linux](GORSEL: vscode-win-new-terminal.png)

*On Windows and Linux the menu strip sits inside the window, at the top: **Terminal → New Terminal** (illustration).*

It runs PowerShell on Windows, zsh on macOS and bash on Linux. The **+** at the top right opens a second terminal, the bin icon closes one. To change the default shell, press the ▾ arrow next to **+** and choose *Select Default Profile*.

## Extensions

Language support, themes, tool integrations: everything arrives as an extension. Click the **Extensions** icon in the Activity Bar (Ctrl + Shift + X), type the name into the search box and press **Install**. The official extension for the programming language you use (Python, C/C++, Go...) is enough to begin with; install the rest as the need arises.

If a project folder contains a `.vscode/extensions.json` file, VS Code asks "Do you want to install the recommended … extension for this repository?" at the bottom right on opening. Do whatever the project's own documentation says; if unsure, close it with the cross, you can always install later.

![Extension recommendation bubble](GORSEL: vscode-extension-prompt.png)

*The recommendation bubble. Install installs, Show Recommendations opens the list, the cross closes it.*

## Together with Git

The Source Control view, and most projects, expect **git** to be installed on your computer. If VS Code shows a "Git not found" warning:

- **Windows:** download the installer from [git-scm.com/download/win](https://git-scm.com/download/win) and press *Next* on every screen.

- **macOS:** type `xcode-select --install` in the terminal; Apple's command line tools include git.

- **Linux:** `sudo apt install git`

After installing, close VS Code completely and open it again; otherwise it will not see the new program.

## Common problems

| What you see | Fix |
|---|---|
| Mac: "cannot be opened because Apple cannot check it for malicious software" | Right-click the app and choose *Open* (once is enough). Failing that, System Settings → Privacy & Security → *Open Anyway* |
| Windows: the `code` command is not recognised in the terminal | *Add to PATH* was not ticked during installation, or the terminal was already open. Reopen the terminal; if it is still missing, run the installer again and tick the option |
| Mac: no `code` command | Inside VS Code, ⇧⌘P → *Shell Command: Install 'code' command in PATH* |
| The terminal opens but not with the shell I expected | The ▾ arrow in the terminal panel → *Select Default Profile* |
| "Git not found" warning | Install git as above and restart VS Code |
| Text is too small or too large | View → Appearance → Zoom In / Zoom Out (Ctrl + = and Ctrl + -) |
| Notifications keep appearing at the bottom right | The bell icon at the far right of the Status Bar → *Do Not Disturb Mode* |
| I closed a panel by accident and cannot find it | Pick the view from the View menu, or type its name into the Command Palette |
