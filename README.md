# QScreenRec 0.3.3

Cross-platform (Linux, macOS, Windows) screen recorder built on Qt 6 Widgets + Qt Multimedia.
Records the entire screen, a dragged-out area of a screen, or a single window to a video file,
with system audio ("what you hear") or any audio input device.

## Requirements

- Qt **6.8 or newer** with the **Qt Multimedia** module (FFmpeg backend, the default)
- CMake 3.21+, a C++17 compiler
- Linux: `libpulse-dev` (system-audio capture) and `libpipewire-0.3-dev` (capture on Wayland);
  the build still works without either, minus that feature

## Build

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt dir, e.g. ~/Qt/6.8.3/macos>
    cmake --build build --config Release

## Installers

Run CPack in the build directory after building; it produces a self-contained package
(Qt libraries and plugins bundled):

    cd build && cpack -C Release

| Platform | Output | Needs |
|----------|--------|-------|
| Windows | `qscreenrec-0.3.3-windows-x64.exe` (NSIS setup, Start-menu entry, uninstaller) + portable `.zip` | NSIS on PATH |
| macOS | `qscreenrec-0.3.3-macos.dmg` (drag QScreenRec to Applications) | Xcode command-line tools |
| Linux | `qscreenrec-0.3.3-linux-x86_64.deb`, `.rpm` (if `rpmbuild` exists), portable `.tar.gz` | `dpkg`, optionally `rpm` |

The Linux packages install to `/opt/qscreenrec` and add `/usr/bin/qscreenrec` plus a menu entry.

**All three at once:** `.github/workflows/installers.yml` builds every installer on GitHub's
Windows, macOS and Linux runners. Push the project to a GitHub repository, then run *Installers*
from the Actions tab (files appear as artifacts). To publish, tick **Publish a GitHub release** in
the Run workflow dialog: the workflow creates the tag `v<version>` from the version in
`CMakeLists.txt` and a release with the installers attached. Pushing a tag such as `v0.3.3` does
the same.

Signing is not set up: the macOS app is unsigned/un-notarized (first launch: right-click → Open,
or sign and notarize with your Developer ID) and the Windows setup is unsigned (SmartScreen warns).
Before publishing, set the maintainer address in `CPACK_PACKAGE_CONTACT` (CMakeLists.txt).

## Use

1. Choose **Entire screen**, **Area of the screen** (click *Select area…* and drag), or **Window**
   (choose from the list, or click *Pick…* and click the window's live preview).
2. Choose the **Audio** source: system audio, no audio, or an input device (microphone, line-in).
3. Pick the folder, container format, quality and frame rate.
4. **Record** (Ctrl+R). The window minimizes after the start delay; stop from the tray icon
   (single click) or restore the window and press **Stop**. **Pause/Resume** leaves no gap in the file.

Files are named `Recording yyyy-MM-dd HH.mm.ss.<ext>`. In area mode a red frame marks the
recorded rectangle; it sits just outside the area and is not part of the video.

### Command line

    qscreenrec --list                                    # screens, windows, codecs, audio inputs
    qscreenrec -o demo.mp4 -d 10 -a system               # whole screen 1 + system audio, 10 s
    qscreenrec -o demo.mp4 -r 100,100,800,600 -f 60      # area x,y,w,h (logical px), 60 fps
    qscreenrec -o demo.mp4 -w "KiCad" -a mic -d 30       # window by title substring + microphone

## Audio

| Source | How |
|--------|-----|
| System audio, Linux | monitor of the default output via PulseAudio / PipeWire-pulse |
| System audio, Windows | WASAPI loopback of the default output device |
| System audio, macOS | ScreenCaptureKit (macOS 13+; uses the Screen Recording permission) |
| Input device | Qt Multimedia `QAudioInput` (macOS asks for Microphone permission on first use) |

One source per recording; system audio and a microphone are not mixed together.

## Platform notes

- **macOS**: grant *Screen Recording* permission (System Settings → Privacy & Security) on first
  run, then restart the app. H.264 via VideoToolbox. On macOS 12 system audio is unavailable.
- **Windows**: H.264 via Media Foundation / hardware encoders.
- **Linux/X11**: window list needs `XDG_SESSION_TYPE=x11` (set by normal desktop sessions).
  The video codec depends on your hardware: the official Qt has no software H.264 encoder, so
  H.264 is only used when the GPU can encode it through VA-API. Otherwise recording falls back to
  MPEG-4 Part 2, which VLC and mpv play but browsers and QuickTime do not.
- **Linux/Wayland**: applications cannot grab the screen or list windows, so capture goes through
  the desktop's screen-sharing portal (xdg-desktop-portal + PipeWire). When recording starts the
  system shows its own dialog and you click the screen or window to share; the window list and
  *Pick…* are therefore not shown. For area recording, pick the same screen you selected the area
  on. `qscreenrec --list` prints which path is in use.
- **Linux, software H.264**: if your Qt's FFmpeg uses libx264 (distribution Qt packages), Qt encodes
  screen content as H.264 High 4:4:4, which some players (QuickTime, browsers) refuse. The
  installers built by the workflow use the official Qt and are not affected.
- **MKV**: Qt 6.8's FFmpeg backend cannot write software-encoded H.264 into Matroska (the
  recording fails to start with a message suggesting MP4). MP4 is the safe default.

## Design

    QScreenCapture / QWindowCapture ─▶ QVideoSink ─┐
    PortalCapture (Wayland: portal + PipeWire) ────┴▶ Recorder::onFrame ─▶ QVideoFrameInput ─┐
    SystemAudio backend ─▶ Recorder::onSystemAudio ─▶ QAudioBufferInput ────────────────────┼▶ QMediaRecorder
    QAudioInput (input device) ─────────────────────────────────────────────────────────────┘

`Recorder::onFrame` throttles to the chosen frame rate, re-timestamps (so pauses leave no gap)
and, in area mode, crops each frame. `Recorder::onSystemAudio` keeps the pushed audio continuous
and locked to the same clock: silence is inserted when a backend delivers nothing (WASAPI
loopback is silent when nothing plays) and late bursts are dropped.

| File | Purpose |
|------|---------|
| `src/Recorder.*` | capture pipeline, A/V clock, encoder settings |
| `src/audio/SystemAudio.h` | Qt-free interface for system-audio capture |
| `src/audio/SystemAudio_{pulse,wasapi}.cpp`, `SystemAudio_mac.mm` | per-platform backends (`_none.cpp` = stub) |
| `src/PortalCapture.*` | Linux/Wayland capture: ScreenCast portal (D-Bus) and PipeWire video stream |
| `src/WindowPicker.*` | window chooser with live thumbnails |
| `src/RegionSelector.*` | drag-to-select overlay and the on-screen area frame |
| `src/MainWindow.*` | UI, countdown, tray icon, settings |
| `src/main.cpp` | entry point, dark gradient theme, command-line mode |
| `packaging/` | icons, macOS Info.plist, Windows resource file, Linux desktop entry and package scripts |

## Changes

- **0.3.3** – no application changes; the Installers workflow gained a "Publish a GitHub release"
  option for one-click releases.
- **0.3.2** – codec fallback made robust: it no longer depends on the recorder's duration being
  zero and it absorbs the extra errors a single failed encoder start can raise (the
  "Could not initialize encoder" popup).
- **0.3.1** – when a video encoder cannot start (e.g. hardware H.264 through VA-API on a GPU
  without encode support), recording automatically restarts with the next codec (H.264 → H.265 →
  MPEG-4 → Motion JPEG) and remembers the choice; Linux runs through X11/XWayland by default, which
  removes the missing "wayland" plugin warning.
- **0.3.0** – Wayland support: screen, area and window capture through the desktop portal (the
  system picker selects the window); visual window picker (*Pick…*) on X11/Windows/macOS; untitled
  windows are listed; the area-selection overlay no longer turns the screen black under Wayland;
  dark gradient theme.
- **0.2.1** – first version with installers built and published by the GitHub workflow; macOS
  link fix (Qt 6.8 references the AGL framework that current Xcode SDKs no longer ship); CI uses
  the `macos-latest` runner; the Windows installer bundles the MSVC runtime.
- **0.2.0** – audio recording (system audio or input device); CPack installers for Windows, macOS
  and Linux plus a GitHub Actions workflow; H.264/AAC now chosen explicitly when available
  (0.1.0 left the video codec to Qt, which could pick HEVC); `--list` shows codecs and audio inputs.
- **0.1.0** – screen / area / window recording, pause, tray control, command-line mode.

## Not in this version

Mixing system audio with a microphone, cursor-capture options, global hotkeys, code signing.
