# PowerPilot

An idle-detection auto-shutdown timer for Windows 10/11. Native Win32 (C++), no framework, no installer — just download the `.exe` and run it.

## Why PowerPilot?

Most auto-shutdown timers on Windows just count down from when you launch them, regardless of whether you're actually still at your desk. PowerPilot is different:

- The countdown **automatically resets** on any keyboard/mouse activity — your PC won't shut down mid-session just because a timer expired.
- Shutdown is a **real full power-off** (bypasses Windows' Fast Startup hybrid shutdown), not a disguised hibernate — equivalent to a manual Shift+Shutdown.
- Status is shown through a **draggable, transparent text overlay** you can resize, not an intrusive popup or notification.
- Under 50 KB, near-zero idle RAM/CPU — pure Win32 API, no .NET/Electron.
- UI available in **English and Indonesian**, switchable anytime from the main window.

## Usage

1. Download `PowerPilot.exe` from [Releases](../../releases).
2. Run it directly — no installation needed.
3. Set the idle duration (minutes) and overlay text size, then click **Start**.
4. The main window auto-minimizes; a countdown overlay appears in the top-left corner — drag it anywhere you like.
5. Right-click the overlay to stop at any time.

## ⚠️ Windows SmartScreen / Smart App Control

Since this binary isn't signed with a paid certificate, Windows may warn you the first time you run it:

- **SmartScreen**: click *More info* → *Run anyway*.
- **Smart App Control** (if enabled on your system): this will **block the app entirely**, with no "run anyway" option. You'll need to temporarily disable it via *Windows Security → App & browser control → Smart App Control* to run unsigned apps like this one. Read the trade-offs before turning it off.

This isn't a sign that PowerPilot is unsafe — it's the standard experience for any small/independent app that hasn't gone through Microsoft's paid code-signing process.

## ⚠️ Usage Warning

- Shutdown executes **with no confirmation delay** once the countdown hits 0. Save your work before leaving your PC with the timer running.
- The app does not check for unsaved documents before shutting down.
- Use at your own risk (see [LICENSE](LICENSE)).

## Building from Source

Requires MinGW-w64 (cross-compiling from Linux) or MSYS2 (native Windows):

```bash
g++ -O2 -s -static -static-libgcc -static-libstdc++ -mwindows -municode \
    -fno-exceptions -fno-rtti main.cpp -o PowerPilot.exe \
    -luser32 -lkernel32 -lgdi32 -ladvapi32 -lshcore
```

## License

MIT — see [LICENSE](LICENSE).
