# Architecture and latency model

## Why Media Foundation

Media Foundation exposes the native types advertised by a UVC source, supports an asynchronous
Source Reader and has an explicit switch that disables converters. DirectShow is retained as a
future compatibility fallback only: it is a legacy API and would not remove the UVC driver's own
buffering. At startup the application enumerates the actual device types and selects an exact
YUY2 1920x1080 60/1 type where available.

Source Reader attributes:

- `MF_LOW_LATENCY = TRUE`
- `MF_READWRITE_DISABLE_CONVERTERS = TRUE`
- `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING = FALSE`
- `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = FALSE`
- one outstanding asynchronous `ReadSample`

This proves that the subtype delivered to the application is native YUY2; it does not prove how
the capture-card firmware generated YUY2 internally.

## Frame lifetime

1. The Media Foundation callback locks the sample, copies its pitched YUY2 rows into a reusable
   tightly packed pending slot, records the MF timestamp and immediately requests the next sample.
2. There is only one pending slot. If it is occupied, the new frame replaces it and increments
   `dropped`; old frames never form a queue.
3. The render thread swaps the pending storage into its local frame, maps one dynamic D3D11
   texture and uploads the packed bytes.
4. The first GPU pass reconstructs chroma and converts Rec.709 YCbCr into one native-resolution
   RGB texture. This keeps reconstruction independent from the window size.
5. The second GPU pass scales that completed RGB frame into the swapchain back buffer. Both passes
   run in the same D3D11 frame; the intermediate texture is never copied through the CPU and does
   not add a frame queue.
6. The flip-discard swapchain has two buffers but `SetMaximumFrameLatency(1)` limits queued work.

The explicit application queue therefore has depth 0 or 1. Potentially hidden queues remain in
the capture firmware, UVC kernel driver, Media Foundation capture source, Windows compositor and
display electronics; the overlay's frame age helps measure their combined visible effect but is
not a photon-to-photon latency measurement.

## Chroma reconstruction

YUY2 stores `Y0 Cb Y1 Cr` for every two horizontal luma samples. The shader treats chroma sample
positions as a phase that can be moved by `-1.5..+1.5` luma pixels. This is deliberately manual:
the byte format does not carry a reliable per-device siting calibration.

- Nearest: diagnostic baseline.
- Bilinear: two neighboring 4:2:2 chroma samples.
- Bicubic: four samples with a Catmull-Rom kernel.
- Original luma-guided: the exact pre-adaptive four-candidate joint-bilateral reconstruction. It
  remains the default because hardware comparison showed materially better perceived chroma
  reconstruction than the stricter confidence model.
- Conservative luma-guided: uses only the two chroma candidates surrounding the phase-corrected
  sample position. A thresholded confidence test measures the primary luma gradient, competing
  nearby gradients, candidate contrast and how clearly target luma matches one side. Only an
  isolated, high-confidence edge switches to almost-nearest chroma; smooth areas use a restrained
  bilinear/bicubic blend.
- Adaptive blend: computes `lerp(bilinear, edge_candidate, confidence)`. Obvious isolated edges
  retain the edge-selected chroma, while thin, textured or ambiguous detail falls back toward
  bilinear. This remains an opt-in experimental mode.

`Edge threshold` is adjustable from 0.01 to 0.20 in normalized luma units. Split-screen evaluates
the exact same frame with bilinear on the left and the selected reconstruction on the right, so
mode and threshold changes can be assessed without temporal or scene differences.

The method is intentionally conservative. Correlated luma/chroma edges are common in game UI,
but luma is not a mathematical oracle for color; the live modes and offset control exist so the
user can compare against bilinear on the same scene.

## Presentation and cadence

The swapchain uses flip-discard and the frame-latency waitable object. With VSync on, each new
60 fps capture frame is presented at the next display refresh. On a 120 Hz display this naturally
holds each source frame for two refreshes. With VSync off, `DXGI_PRESENT_ALLOW_TEARING` is used
only when the OS reports support. Buffering duplicate frames to manufacture cadence is avoided.

## Audio

WASAPI opens a selected capture endpoint and selected render endpoint at 48 kHz stereo float in
shared, event-driven mode with a requested 20-ms endpoint buffer. A bounded ring absorbs endpoint
scheduling jitter and is drained as soon as render space is available. Its hard cap is about
40 ms; on overflow it discards oldest frames. This is the only
multi-sample queue in the application and cannot grow over time. Video is never delayed to follow
audio. A manual sync-offset control is deferred until measured device behavior is available.
