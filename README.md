# YUVision

YUVision is a low-latency Windows and Linux viewer for USB HDMI capture cards. It is built for
native 1920x1080 60 fps YUY2 capture from the ezcap331 / ezcap CAM LINK 4K.

The app reconstructs 4:2:2 chroma and converts Rec.709 YUY2 to RGB on the GPU. It keeps only the
newest frame, so slow rendering drops old frames instead of increasing latency.

![YUY2 chroma reconstruction comparison](Compare.png)

### More comparisons

![Game UI chroma comparison](game_cmp.png)

![Red and yellow UI comparison](hs_cmp.png)

## Features

- Native YUY2 capture: Media Foundation on Windows, V4L2 on Linux
- Original luma-guided, bilinear, bicubic, conservative, and adaptive chroma modes
- Manual chroma offset and Limited / Full range selection
- Native-resolution GPU conversion followed by sharp bicubic window scaling
- Borderless fullscreen and a VSync toggle
- Low-latency 48 kHz audio through WASAPI on Windows or ALSA on Linux
- Capture, render, drop, queue, timestamp, format, and audio diagnostics

## Build

### Windows

Requirements: Windows 10/11 x64, Visual Studio 2022, and a Windows SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is `build\Release\YUVision.exe`. No third-party runtime libraries are required.

### Linux

Requirements: a C++20 compiler, SDL3, OpenGL, ALSA development files, and V4L2 headers.

```bash
make -f GNUmakefile
./yuvision --list-devices
./yuvision --shader-test
./yuvision --video /dev/video0
```

Use `--audio-in <ALSA_PCM>`, `--audio-out <ALSA_PCM>`, or `--no-audio` when automatic ezcap
audio selection is not suitable. A CMake build is also supported when CMake and pkg-config are
installed.

The Linux MVP uses a compact keyboard-driven UI. Its V4L2 path requests native 1920x1080 YUYV at
60 fps, drains all ready buffers on each iteration, and displays only the newest frame. SDL3 and
OpenGL 3.3 run the same reconstruction modes as the Windows D3D11 shaders. ALSA capture and
playback are non-blocking and use a bounded queue.

## Controls

- `F11` or `Alt+Enter`: borderless fullscreen
- `V`: VSync
- `A`: sharp bicubic window scaling
- `1` to `6`: select a chroma reconstruction mode
- `Left` / `Right`: adjust chroma offset (`Shift` uses larger steps)
- `Down` / `Up`: adjust edge threshold (`Shift` uses larger steps)
- `S`: split screen, bilinear on the left and selected mode on the right
- `R`: Limited / Full input range
- `O`: debug overlay (Windows)
- Menu or right click: select capture mode and audio devices (Windows)

On Linux, select devices with the command-line options shown above. Live capture/render/drop and
audio-queue statistics are shown in the window title.

YUY2 is currently the implemented video path. NV12 and XRGB native modes may be listed by the
device, but remain disabled. On Windows, the diagnostic log is written to
`%TEMP%\YUVision.log`.

Technical details are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/RESEARCH.md](docs/RESEARCH.md).
