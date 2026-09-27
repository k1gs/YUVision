# Research notes

## ezcap331 native formats

The manufacturer's current ezcap331 specification lists UVC output as YUY2, NV12 and RGB. Its
published maxima are:

- 3840x2160: NV12 30 fps
- 2560x1440: NV12 60 fps, YUY2 50 fps
- 1920x1080: NV12 120 fps, YUY2 60 fps, XRGB 30 fps
- 1280x720: NV12/YUY2/XRGB 60 fps

Source: [ezcap331 CAM LINK 4K product specification](https://www.ezcap.com/ezcap331).

These are product-level maxima, not proof of the exact media-type table exposed by a particular
firmware/USB link. YUVision therefore enumerates `GetNativeMediaType` and only selects the
exact native entry returned by the connected unit. The MVP accepts YUY2; it shows but disables
unimplemented native NV12/RGB entries.

## Capture API decision

Microsoft marks DirectShow as legacy and recommends Media Foundation capture for new Windows
code. More importantly for this viewer, Source Reader exposes both `MF_LOW_LATENCY` and
`MF_READWRITE_DISABLE_CONVERTERS`. The latter explicitly disables the normally allowed
uncompressed format conversions. Video-processing attributes remain false so no hidden software
YUV-to-RGB conversion is requested.

- [Source Reader attributes](https://learn.microsoft.com/en-us/windows/win32/medfound/source-reader-attributes)
- [MF video processing warning for Direct3D display](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-source-reader-enable-video-processing)
- [DirectShow legacy guidance](https://learn.microsoft.com/en-us/windows/win32/directshow/choosing-the-right-renderer)

This does not promise literal zero-copy. An uncompressed UVC frame normally reaches this desktop
pipeline in system memory. The MVP makes that cost explicit: one pitched sample copy into the
latest-frame slot and one upload into a packed GPU texture. A D3D manager is intentionally not
advertised to Source Reader because no transform/decoder is needed and the application's custom
shader needs the original packed YUY2 bytes.

## Presentation and audio

DXGI's flip model avoids the older blit presentation path. Microsoft documents the frame-latency
waitable object and maximum frame latency of one as the low-latency configuration, while tearing
must be feature-checked before use.

- [DXGI flip-model guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)
- [Frame-latency waitable object](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)

For audio, Windows 10 can expose sub-10-ms shared-mode periods through `IAudioClient3`, but actual
minimums depend on the endpoint driver. The MVP currently uses event-driven shared WASAPI at a
requested 20-ms endpoint buffer plus a bounded application ring; moving to `IAudioClient3`'s
minimum reported period is the next measured optimization, not an assumed win.

- [Windows low-latency audio guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio)

## Reconstruction choice

4:2:2 retains luma at every horizontal pixel and chroma at half horizontal resolution. The fast
luma-guided shader is a four-candidate joint-bilateral interpolation: distance controls the base
weight, while difference between target full-resolution luma and each candidate pair's average
luma suppresses cross-edge mixing. It is spatial-only and fixed-cost. Bilinear and bicubic remain
available because luma and chroma edges are not always correlated.

The byte layout alone cannot establish how this particular capture firmware filtered or phased
its HDMI chroma. The shader begins from nominal co-sited 4:2:2 and exposes a continuous manual
phase adjustment. The user's previously observed approximately -1-pixel improvement is a useful
starting hypothesis, not a hard-coded device fact.
