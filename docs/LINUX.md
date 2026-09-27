# Linux build and run guide

The Linux build uses V4L2 for native YUYV capture, SDL3 with OpenGL 3.3 for presentation, and
ALSA for low-latency audio passthrough. It currently targets x86-64 desktop Linux.

## Current scope

- Fixed capture request: 1920x1080, 60 fps, `V4L2_PIX_FMT_YUYV`.
- Latest-frame-wins video path with two V4L2 MMAP buffers.
- The same six chroma reconstruction modes as the Windows build.
- Rec.709 Limited / Full conversion, chroma offset, edge threshold, and split screen.
- Non-blocking 48 kHz stereo S16 ALSA capture and playback.
- Keyboard controls and compact statistics in the window title.

The Linux MVP does not yet have the native device-selection menus or the full on-frame debug
overlay available on Windows. Select capture and audio devices on the command line.

## Install build dependencies

### Fedora Workstation

```bash
sudo dnf install gcc-c++ make cmake pkgconf-pkg-config \
  SDL3-devel alsa-lib-devel libglvnd-devel kernel-headers \
  v4l-utils alsa-utils
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake pkgconf sdl3 alsa-lib libglvnd \
  v4l-utils alsa-utils
```

For another distribution, install a C++20 compiler plus development packages that provide these
pkg-config modules:

```bash
pkg-config --modversion sdl3 alsa gl
```

The build also needs the userspace Linux headers containing `linux/videodev2.h`.

## Build

The small GNU Make build is the shortest route:

```bash
make -f GNUmakefile
```

The resulting executable is `./yuvision`. To use CMake instead:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The CMake executable is `build/yuvision`.

## Verify the graphics path

Run the hardware-independent shader test first:

```bash
./yuvision --shader-test
```

It creates an OpenGL 3.3 context, uploads a synthetic packed-YUYV frame, runs both shader passes,
presents once, and waits for GPU completion. Success prints:

```text
OpenGL YUYV shader test passed
```

## Find the capture card

Connect the ezcap331 before starting YUVision, then inspect the V4L2 modes:

```bash
v4l2-ctl --list-devices
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

The selected device must advertise `YUYV` at 1920x1080 and 60 fps. YUVision deliberately rejects
a driver-selected compressed or converted format instead of silently accepting it.

List the video and ALSA names visible to YUVision:

```bash
./yuvision --list-devices
arecord -L
aplay -L
```

## Run

YUVision automatically prefers a V4L2 device and ALSA input whose description contains `ezcap`
or `CAM LINK`:

```bash
./yuvision
```

Explicit device selection is safer when several capture devices are connected:

```bash
./yuvision --video /dev/video0 \
  --audio-in plughw:CARD=ezcap,DEV=0 \
  --audio-out default
```

Run video only while diagnosing audio:

```bash
./yuvision --video /dev/video0 --no-audio
```

Use the exact PCM names printed by `./yuvision --list-devices` or `arecord -L`; the example card
name is not guaranteed to match every ezcap firmware revision.

## Controls

- `Esc`: exit.
- `F11` or `Alt+Enter`: borderless fullscreen.
- `V`: toggle VSync.
- `A`: toggle sharp bicubic window scaling.
- `1` to `6`: nearest, bilinear, bicubic, original luma-guided, conservative, adaptive.
- `Left` / `Right`: chroma offset by 0.05 px; hold `Shift` for 0.25 px.
- `Down` / `Up`: edge threshold by 0.01; hold `Shift` for 0.025.
- `S`: split screen, bilinear on the left and the selected mode on the right.
- `R`: Limited / Full input range.

## Troubleshooting

### No V4L2 device appears

Check that the kernel detected the USB device and created a video node:

```bash
lsusb
ls -l /dev/video*
dmesg --level=err,warn
```

If `/dev/video0` exists but cannot be opened, check its ACL with `getfacl /dev/video0`. On a normal
Fedora Workstation login, udev/logind normally grants the active local user access. Do not run the
viewer as root merely to bypass a missing device ACL.

### Device or resource busy

Only one application may be able to stream from the card. Close OBS, browsers, camera utilities,
and any previous YUVision process, then retry.

### Native YUYV mode is rejected

Confirm the exact mode with `v4l2-ctl --list-formats-ext`. A node exposing only MJPEG or a metadata
interface is not the desired native YUYV capture node.

### Video works but audio is missing

First verify the capture PCM independently:

```bash
arecord -D plughw:CARD=ezcap,DEV=0 -f S16_LE -c 2 -r 48000 -d 5 /tmp/ezcap.wav
aplay /tmp/ezcap.wav
```

Then pass the working PCM name through `--audio-in`. PipeWire-based desktops still expose the ALSA
`default` output used by YUVision, but an explicit `--audio-out` may be used if needed.

### Shader test cannot create a context

Confirm that the desktop session has working OpenGL 3.3 support:

```bash
glxinfo -B
```

On a remote shell, ensure the graphical session is available. Software rendering may run the
test, but it is not suitable for a low-latency 1080p60 preview.

