# MouseKeep

Windows x64 CLI that moves the mouse periodically and prevents idle sleep. Written in C; the standalone executable is **8.5 KiB** and requires no installation, administrator rights, or separate runtime.

## Usage

Download `mousekeep.exe` and run it in PowerShell:

```powershell
.\mousekeep.exe 30       # Start in background; move every 30 seconds
.\mousekeep.exe status
.\mousekeep.exe stop
.\mousekeep.exe run 30   # Foreground; Ctrl+C to stop
```

The default interval is 60 seconds; valid intervals are 1–86400 whole seconds. Stop and start again to change it. Add the executable's folder to PATH to use `mousekeep` from any directory.

One worker runs per Windows user and session. It alternates small left/right movements, skips movement while a mouse button is held, and blocks between intervals. It also keeps the system and display awake until stopped. Explicit sleep, lid-close actions, and enforced locking still apply.

Measured memory: approximately **5.3 MiB working set** and **1.14 MiB private allocation**; usage varies by system.

## Build

Install portable [Zig](https://ziglang.org/download/) (tested with 0.15.2), then:

```powershell
.\build.ps1 -Compiler C:\zig\zig.exe
```
