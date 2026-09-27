# YUVision

YUVision is a low-latency Windows viewer for USB HDMI capture cards. It is built for native
1920x1080 60 fps YUY2 capture from the ezcap331 / ezcap CAM LINK 4K.

The app reconstructs 4:2:2 chroma and converts Rec.709 YUY2 to RGB in a D3D11 shader. It keeps
only the newest frame, so slow rendering drops old frames instead of increasing latency.

![YUY2 chroma reconstruction comparison](Compare.png)

### More comparisons

![Game UI chroma comparison](game_cmp.png)

![Red and yellow UI comparison](hs_cmp.png)

## Features

- Native Media Foundation capture with hidden format conversion disabled
- Original luma-guided, bilinear, bicubic, conservative, and adaptive chroma modes
- Manual chroma offset and Limited / Full range selection
- Native-resolution GPU conversion followed by sharp bicubic window scaling
- Borderless fullscreen, VSync toggle, and tearing when supported
- Low-latency 48 kHz WASAPI audio
- Capture, render, drop, queue, timestamp, format, and audio diagnostics

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022, and a Windows SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is `build\Release\YUVision.exe`. No third-party runtime libraries are required.

## Controls

- `F11` or `Alt+Enter`: borderless fullscreen
- `V`: VSync
- `A`: sharp bicubic window scaling
- `1` to `6`: select a chroma reconstruction mode
- `Left` / `Right`: adjust chroma offset (`Shift` uses larger steps)
- `Down` / `Up`: adjust edge threshold (`Shift` uses larger steps)
- `S`: split screen, bilinear on the left and selected mode on the right
- `R`: Limited / Full input range
- `O`: debug overlay
- Menu or right click: select capture mode and audio devices

YUY2 is currently the implemented video path. NV12 and XRGB native modes may be listed by the
device, but remain disabled. The diagnostic log is written to `%TEMP%\YUVision.log`.

Technical details are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/RESEARCH.md](docs/RESEARCH.md).
