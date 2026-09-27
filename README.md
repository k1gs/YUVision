# 331Viewer-YUY2Fix

Minimal Windows 10/11 live viewer for a UVC HDMI capture card. The current MVP targets native
1920x1080 60 fps YUY2 from ezcap331 / ezcap CAM LINK 4K, performs chroma reconstruction and
Rec.709 conversion in one D3D11 pixel shader, and deliberately keeps only the newest captured
frame.

## Build

Use an x64 Visual Studio 2022 developer prompt:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

No third-party runtime dependencies are used. A Windows 10/11 SDK is required.

## Controls

- `F11` / `Alt+Enter`: borderless fullscreen
- `V`: VSync on/off (tearing is used when supported)
- `1` / `2` / `3` / `4` / `5` / `6`: nearest / bilinear / bicubic / original luma-guided /
  conservative luma-guided / adaptive blend
- `Left` / `Right`: chroma offset by 0.05 pixels
- `Shift+Left` / `Shift+Right`: chroma offset by 0.25 pixels
- `Down` / `Up`: edge threshold by 0.01 (`Shift`: 0.025)
- `S`: split-screen comparison, bilinear on the left and selected mode on the right
- `R`: Limited / Full input range
- `O`: overlay on/off
- Right click or the menu bar: select capture/audio devices and video mode

The selected video mode is always a native media type reported by the capture device. The app
does not ask Media Foundation to insert a converter. YUY2 is implemented by the MVP; other
enumerated subtypes are shown for diagnostics but disabled until their GPU upload path exists.

If capture cannot produce a frame, the viewer shows the exact startup/callback state instead of a
blank client area. The same information and the native media-type list are written to
`%TEMP%\331Viewer-YUY2Fix.log`.

## Known MVP boundaries

- UVC YUY2 normally arrives in system memory, so one CPU-to-GPU upload per displayed frame is
  unavoidable in this path. There is no CPU RGB conversion and no intermediate RGB texture.
- The edge-aware modes are spatial-only reconstruction, not an AI upscaler. They cannot recreate
  chroma detail that the capture device never sampled.
- Original pre-adaptive luma-guided is the default. Conservative and adaptive modes remain
  opt-in experiments and do not replace it.
- WASAPI uses a bounded audio ring. If the endpoints cannot both open as 48 kHz stereo float,
  audio reports an error instead of silently accepting an unknown format.
- Manual audio sync offset and the `IAudioClient3` minimum-period path are deferred until the
  actual ezcap audio endpoint is measured; the MVP never delays video to compensate.
- Exact device behavior and end-to-end latency must be validated on the ezcap331 hardware.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the buffer and latency model and
[docs/RESEARCH.md](docs/RESEARCH.md) for the capture/API research and source links.
