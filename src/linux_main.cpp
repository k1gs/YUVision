#define GL_GLEXT_PROTOTYPES

#include <SDL3/SDL.h>
#include <GL/gl.h>
#include <alsa/asoundlib.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void Fail(const std::string& message) { throw std::runtime_error(message); }

int Ioctl(int fd, unsigned long request, void* value) {
    int result;
    do {
        result = ioctl(fd, request, value);
    } while (result < 0 && errno == EINTR);
    return result;
}

struct VideoDevice {
    std::string path;
    std::string name;
};

std::vector<VideoDevice> EnumerateVideoDevices() {
    std::vector<VideoDevice> result;
    for (int index = 0; index < 64; ++index) {
        const std::string path = "/dev/video" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability caps{};
        if (Ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0) {
            const uint32_t flags = (caps.capabilities & V4L2_CAP_DEVICE_CAPS)
                ? caps.device_caps : caps.capabilities;
            if ((flags & V4L2_CAP_VIDEO_CAPTURE) && (flags & V4L2_CAP_STREAMING)) {
                result.push_back({path, reinterpret_cast<const char*>(caps.card)});
            }
        }
        close(fd);
    }
    return result;
}

class V4l2Capture {
public:
    struct Frame {
        const uint8_t* data = nullptr;
        size_t bytes = 0;
        uint32_t index = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bytesPerLine = 0;
        uint64_t serial = 0;
    };

    ~V4l2Capture() { Close(); }

    void Open(const std::string& path) {
        Close();
        fd_ = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) Fail("Could not open " + path + ": " + std::strerror(errno));

        v4l2_capability caps{};
        if (Ioctl(fd_, VIDIOC_QUERYCAP, &caps) < 0) Fail("VIDIOC_QUERYCAP failed");
        const uint32_t flags = (caps.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? caps.device_caps : caps.capabilities;
        if (!(flags & V4L2_CAP_VIDEO_CAPTURE) || !(flags & V4L2_CAP_STREAMING)) {
            Fail(path + " is not a streaming capture device");
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = 1920;
        format.fmt.pix.height = 1080;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (Ioctl(fd_, VIDIOC_S_FMT, &format) < 0) Fail("VIDIOC_S_FMT(YUYV) failed");
        if (format.fmt.pix.width != 1920 || format.fmt.pix.height != 1080 ||
            format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            Fail("Capture device did not accept native 1920x1080 YUYV");
        }
        width_ = format.fmt.pix.width;
        height_ = format.fmt.pix.height;
        bytesPerLine_ = std::max(format.fmt.pix.bytesperline, width_ * 2u);

        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parameters.parm.capture.timeperframe.numerator = 1;
        parameters.parm.capture.timeperframe.denominator = 60;
        if (Ioctl(fd_, VIDIOC_S_PARM, &parameters) < 0) {
            std::cerr << "Warning: device rejected explicit 60 fps request\n";
        } else {
            const auto& interval = parameters.parm.capture.timeperframe;
            if (interval.numerator == 0 || interval.denominator != interval.numerator * 60u)
                std::cerr << "Warning: device negotiated a frame rate other than 60 fps\n";
        }

        v4l2_requestbuffers request{};
        request.count = 2;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (Ioctl(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
            Fail("VIDIOC_REQBUFS failed");
        }
        buffers_.resize(request.count);
        for (uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (Ioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) Fail("VIDIOC_QUERYBUF failed");
            void* address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd_, static_cast<off_t>(buffer.m.offset));
            if (address == MAP_FAILED) Fail("mmap capture buffer failed");
            buffers_[index] = {address, buffer.length};
            if (Ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) Fail("VIDIOC_QBUF failed");
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Ioctl(fd_, VIDIOC_STREAMON, &type) < 0) Fail("VIDIOC_STREAMON failed");
        streaming_ = true;
        started_ = Clock::now();
        fpsWindow_ = started_;
    }

    bool TakeLatest(Frame& frame) {
        int latest = -1;
        size_t latestBytes = 0;
        while (true) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (Ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN) break;
                Fail("VIDIOC_DQBUF failed: " + std::string(std::strerror(errno)));
            }
            if (latest >= 0) {
                Queue(static_cast<uint32_t>(latest));
                ++dropped_;
            }
            latest = static_cast<int>(buffer.index);
            latestBytes = buffer.bytesused;
            ++received_;
            ++fpsFrames_;
        }
        if (latest < 0) return false;
        const size_t required = static_cast<size_t>(bytesPerLine_) * height_;
        if (latestBytes < required) {
            Queue(static_cast<uint32_t>(latest));
            Fail("Short V4L2 YUYV frame");
        }
        const auto now = Clock::now();
        const double elapsed = Seconds(now - fpsWindow_);
        if (elapsed >= 1.0) {
            captureFps_ = static_cast<double>(fpsFrames_) / elapsed;
            fpsFrames_ = 0;
            fpsWindow_ = now;
        }
        frame = {static_cast<const uint8_t*>(buffers_[latest].address), latestBytes,
                 static_cast<uint32_t>(latest), width_, height_, bytesPerLine_, received_};
        return true;
    }

    void Release(const Frame& frame) { Queue(frame.index); }
    double CaptureFps() const { return captureFps_; }
    uint64_t Received() const { return received_; }
    uint64_t Dropped() const { return dropped_; }

private:
    struct Buffer { void* address = nullptr; size_t length = 0; };

    void Queue(uint32_t index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (Ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) Fail("VIDIOC_QBUF(requeue) failed");
    }

    void Close() {
        if (fd_ >= 0 && streaming_) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            Ioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        for (const auto& buffer : buffers_) {
            if (buffer.address && buffer.address != MAP_FAILED) munmap(buffer.address, buffer.length);
        }
        buffers_.clear();
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        streaming_ = false;
    }

    int fd_ = -1;
    bool streaming_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bytesPerLine_ = 0;
    std::vector<Buffer> buffers_;
    uint64_t received_ = 0;
    uint64_t dropped_ = 0;
    uint32_t fpsFrames_ = 0;
    double captureFps_ = 0.0;
    Clock::time_point started_{};
    Clock::time_point fpsWindow_{};
};

std::vector<std::pair<std::string, std::string>> EnumeratePcms() {
    std::vector<std::pair<std::string, std::string>> result;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return result;
    for (void** item = hints; item && *item; ++item) {
        char* rawName = snd_device_name_get_hint(*item, "NAME");
        char* rawDescription = snd_device_name_get_hint(*item, "DESC");
        if (rawName) result.emplace_back(rawName, rawDescription ? rawDescription : "");
        free(rawName);
        free(rawDescription);
    }
    snd_device_name_free_hint(hints);
    return result;
}

std::string FindEzcapAudioInput() {
    std::string fallback;
    for (const auto& [name, description] : EnumeratePcms()) {
        const std::string text = Lower(name + " " + description);
        if (text.find("ezcap") == std::string::npos &&
            text.find("cam link") == std::string::npos) continue;
        if (name.rfind("plughw:", 0) == 0) return name;
        if (fallback.empty()) fallback = name;
    }
    return fallback;
}

class AudioLoop {
public:
    ~AudioLoop() { Stop(); }

    void Start(std::string input, std::string output) {
        if (input.empty()) return;
        input_ = std::move(input);
        output_ = std::move(output);
        running_ = true;
        thread_ = std::thread(&AudioLoop::Run, this);
    }

    void Stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    uint32_t BufferedFrames() const { return bufferedFrames_.load(); }
    std::string Error() const {
        std::scoped_lock lock(errorMutex_);
        return error_;
    }

private:
    void SetError(const std::string& message) {
        std::scoped_lock lock(errorMutex_);
        error_ = message;
    }

    static bool Configure(snd_pcm_t* pcm) {
        return snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 20000) >= 0;
    }

    void Run() {
        snd_pcm_t* capture = nullptr;
        snd_pcm_t* playback = nullptr;
        if (snd_pcm_open(&capture, input_.c_str(), SND_PCM_STREAM_CAPTURE,
                         SND_PCM_NONBLOCK) < 0) {
            SetError("Could not open ALSA capture device " + input_);
            running_ = false;
            return;
        }
        if (snd_pcm_open(&playback, output_.c_str(), SND_PCM_STREAM_PLAYBACK,
                         SND_PCM_NONBLOCK) < 0) {
            snd_pcm_close(capture);
            SetError("Could not open ALSA playback device " + output_);
            running_ = false;
            return;
        }
        if (!Configure(capture) || !Configure(playback)) {
            snd_pcm_close(playback);
            snd_pcm_close(capture);
            SetError("ALSA endpoints do not accept 48 kHz stereo S16");
            running_ = false;
            return;
        }

        constexpr uint32_t channels = 2;
        constexpr uint32_t ringFrames = 4096;
        constexpr uint32_t maxBuffered = 1920;
        std::array<int16_t, ringFrames * channels> ring{};
        std::array<int16_t, 256 * channels> input{};
        std::array<int16_t, 256 * channels> output{};
        uint32_t read = 0, write = 0, buffered = 0;

        while (running_) {
            const snd_pcm_sframes_t captured = snd_pcm_readi(capture, input.data(), 256);
            if (captured > 0) {
                for (snd_pcm_sframes_t frame = 0; frame < captured; ++frame) {
                    if (buffered >= maxBuffered) {
                        read = (read + 1) % ringFrames;
                        --buffered;
                    }
                    for (uint32_t channel = 0; channel < channels; ++channel) {
                        ring[static_cast<size_t>(write) * channels + channel] =
                            input[static_cast<size_t>(frame) * channels + channel];
                    }
                    write = (write + 1) % ringFrames;
                    ++buffered;
                }
            } else if (captured < 0 && captured != -EAGAIN) {
                snd_pcm_recover(capture, static_cast<int>(captured), 1);
            }

            if (buffered > 0) {
                const uint32_t wanted = std::min<uint32_t>(buffered, 256);
                for (uint32_t frame = 0; frame < wanted; ++frame) {
                    const uint32_t index = (read + frame) % ringFrames;
                    output[static_cast<size_t>(frame) * channels] =
                        ring[static_cast<size_t>(index) * channels];
                    output[static_cast<size_t>(frame) * channels + 1] =
                        ring[static_cast<size_t>(index) * channels + 1];
                }
                const snd_pcm_sframes_t written = snd_pcm_writei(playback, output.data(), wanted);
                if (written > 0) {
                    read = (read + static_cast<uint32_t>(written)) % ringFrames;
                    buffered -= static_cast<uint32_t>(written);
                } else if (written < 0 && written != -EAGAIN) {
                    snd_pcm_recover(playback, static_cast<int>(written), 1);
                }
            }
            bufferedFrames_ = buffered;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        snd_pcm_drop(capture);
        snd_pcm_drop(playback);
        snd_pcm_close(playback);
        snd_pcm_close(capture);
        bufferedFrames_ = 0;
    }

    std::string input_;
    std::string output_ = "default";
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> bufferedFrames_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};

constexpr const char* kVertexShader = R"glsl(#version 330 core
out vec2 uv;
void main() {
    vec2 position;
    if (gl_VertexID == 0) position = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) position = vec2(3.0, -1.0);
    else position = vec2(-1.0, 3.0);
    uv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

constexpr const char* kFragmentCommon = R"glsl(#version 330 core
in vec2 uv;
out vec4 fragColor;
uniform sampler2D packedYuy2;
uniform sampler2D convertedRgb;
uniform vec2 sourceSize;
uniform vec2 outputSize;
uniform float chromaOffset;
uniform float edgeThreshold;
uniform int chromaMode;
uniform int limitedRange;
uniform int splitScreen;
uniform int sharpScale;

int PairCount() { return max(1, int(sourceSize.x) / 2); }
vec4 Pair(int pairX, int y) {
    return texelFetch(packedYuy2, ivec2(clamp(pairX, 0, PairCount() - 1),
                     clamp(y, 0, int(sourceSize.y) - 1)), 0);
}
float LumaAt(int x, int y) {
    x = clamp(x, 0, int(sourceSize.x) - 1);
    vec4 pairValue = Pair(x / 2, y);
    return (x & 1) != 0 ? pairValue.b : pairValue.r;
}
vec2 ChromaAt(int pairX, int y) { return Pair(pairX, y).ga; }
float PairLuma(int pairX, int y) {
    vec4 pairValue = Pair(pairX, y);
    return 0.5 * (pairValue.r + pairValue.b);
}
float CubicWeight(float x) {
    x = abs(x);
    if (x < 1.0) return 1.5*x*x*x - 2.5*x*x + 1.0;
    if (x < 2.0) return -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0;
    return 0.0;
}
vec2 BicubicChroma(int base, int y, float f) {
    vec2 sum = vec2(0.0);
    float weightSum = 0.0;
    for (int i = -1; i <= 2; ++i) {
        float weight = CubicWeight(float(i) - f);
        sum += ChromaAt(base + i, y) * weight;
        weightSum += weight;
    }
    vec2 result = sum / max(weightSum, 1e-5);
    return clamp(result, min(ChromaAt(base, y), ChromaAt(base + 1, y)),
                  max(ChromaAt(base, y), ChromaAt(base + 1, y)));
}
float EdgeConfidence(int x, int y, int base, float targetY, out vec2 edgeCandidate) {
    float centerLeft = abs(targetY - LumaAt(x - 1, y));
    float centerRight = abs(LumaAt(x + 1, y) - targetY);
    float primaryGradient = max(centerLeft, centerRight);
    float competingGradient = min(centerLeft, centerRight);
    float outerGradient = max(abs(LumaAt(x - 1, y) - LumaAt(x - 2, y)),
                              abs(LumaAt(x + 2, y) - LumaAt(x + 1, y)));
    int leftX = int(floor(float(base * 2) - chromaOffset + 0.5));
    int rightX = int(floor(float((base + 1) * 2) - chromaOffset + 0.5));
    float leftMatch = abs(targetY - LumaAt(leftX, y));
    float rightMatch = abs(targetY - LumaAt(rightX, y));
    edgeCandidate = leftMatch <= rightMatch ? ChromaAt(base, y) : ChromaAt(base + 1, y);
    float strength = smoothstep(edgeThreshold, edgeThreshold * 2.5, primaryGradient);
    float complexity = max(competingGradient, outerGradient);
    float isolation = 1.0 - clamp(complexity / max(primaryGradient, 1e-5), 0.0, 1.0);
    float matchConfidence = smoothstep(0.0, edgeThreshold, abs(leftMatch - rightMatch));
    float contrast = abs(LumaAt(leftX, y) - LumaAt(rightX, y));
    float agreement = smoothstep(edgeThreshold * 0.5, edgeThreshold * 2.0, contrast);
    return clamp(strength * isolation * matchConfidence * (0.35 + 0.65 * agreement), 0.0, 1.0);
}
vec2 ReconstructChroma(float sourceX, int y, float targetY, int mode) {
    float coordinate = (sourceX + chromaOffset) * 0.5;
    if (mode == 0) return ChromaAt(int(floor(coordinate + 0.5)), y);
    int base = int(floor(coordinate));
    float fraction = fract(coordinate);
    vec2 bilinear = mix(ChromaAt(base, y), ChromaAt(base + 1, y), fraction);
    if (mode == 1) return bilinear;
    if (mode == 2) return BicubicChroma(base, y, fraction);
    if (mode == 3) {
        vec2 guidedSum = vec2(0.0);
        float guidedWeight = 0.0;
        for (int j = -1; j <= 2; ++j) {
            int candidate = base + j;
            float spatial = exp2(-1.35 * abs(float(candidate) - coordinate));
            float difference = abs(targetY - PairLuma(candidate, y));
            float weight = spatial * (0.035 + exp2(-28.0 * difference));
            guidedSum += ChromaAt(candidate, y) * weight;
            guidedWeight += weight;
        }
        return guidedSum / max(guidedWeight, 1e-5);
    }
    vec2 edgeCandidate;
    float confidence = EdgeConfidence(int(floor(sourceX + 0.5)), y, base, targetY,
                                      edgeCandidate);
    if (mode == 5) return mix(bilinear, edgeCandidate, confidence);
    vec2 smoothChroma = mix(bilinear, BicubicChroma(base, y, fraction), 0.25);
    float hardEdge = smoothstep(0.45, 0.8, confidence);
    return mix(smoothChroma, mix(edgeCandidate, bilinear, 0.08), hardEdge);
}
vec3 ToRgb(float y, vec2 chroma) {
    float cb, cr;
    if (limitedRange != 0) {
        y = (y - 16.0/255.0) * (255.0/219.0);
        cb = (chroma.x - 128.0/255.0) * (255.0/224.0);
        cr = (chroma.y - 128.0/255.0) * (255.0/224.0);
    } else {
        cb = chroma.x - 128.0/255.0;
        cr = chroma.y - 128.0/255.0;
    }
    return vec3(y + 1.5748*cr,
                y - 0.187324*cb - 0.468124*cr,
                y + 1.8556*cb);
}
vec3 SampleSource(vec2 pixel, int mode) {
    ivec2 sourcePixel = ivec2(clamp(floor(pixel + 0.5), vec2(0.0), sourceSize - 1.0));
    float y = LumaAt(sourcePixel.x, sourcePixel.y);
    return clamp(ToRgb(y, ReconstructChroma(pixel.x, sourcePixel.y, y, mode)), 0.0, 1.0);
}
vec4 CatmullRomWeights(float f) {
    float f2 = f*f, f3 = f2*f;
    return vec4(-0.5*f + f2 - 0.5*f3,
                1.0 - 2.5*f2 + 1.5*f3,
                0.5*f + 2.0*f2 - 1.5*f3,
                -0.5*f2 + 0.5*f3);
}
vec3 SampleConverted(ivec2 pixel) {
    return texelFetch(convertedRgb, clamp(pixel, ivec2(0), ivec2(sourceSize) - 1), 0).rgb;
}
vec3 SampleConvertedBicubic(vec2 pixel) {
    ivec2 base = ivec2(floor(pixel));
    vec4 wx = CatmullRomWeights(fract(pixel.x));
    vec4 wy = CatmullRomWeights(fract(pixel.y));
    vec3 result = vec3(0.0);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            result += SampleConverted(base + ivec2(i - 1, j - 1)) * wx[i] * wy[j];
    vec3 lo = min(min(SampleConverted(base), SampleConverted(base + ivec2(1, 0))),
                  min(SampleConverted(base + ivec2(0, 1)), SampleConverted(base + ivec2(1))));
    vec3 hi = max(max(SampleConverted(base), SampleConverted(base + ivec2(1, 0))),
                  max(SampleConverted(base + ivec2(0, 1)), SampleConverted(base + ivec2(1))));
    return clamp(result, lo, hi);
}
)glsl";

constexpr const char* kConvertMain = R"glsl(
void main() {
    vec2 sourceUv = vec2(uv.x, 1.0 - uv.y);
    vec2 pixel = sourceUv * sourceSize - 0.5;
    int mode = splitScreen != 0 && sourceUv.x < 0.5 ? 1 : chromaMode;
    fragColor = vec4(SampleSource(pixel, mode), 1.0);
}
)glsl";

constexpr const char* kScaleMain = R"glsl(
void main() {
    float sourceAspect = sourceSize.x / sourceSize.y;
    float outputAspect = outputSize.x / outputSize.y;
    vec2 scale = vec2(1.0);
    if (outputAspect > sourceAspect) scale.x = sourceAspect / outputAspect;
    else scale.y = outputAspect / sourceAspect;
    vec2 centered = (uv - 0.5) / scale + 0.5;
    if (any(lessThan(centered, vec2(0.0))) || any(greaterThan(centered, vec2(1.0)))) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 pixel = centered * sourceSize - 0.5;
    vec2 sourcePerOutput = sourceSize / max(outputSize * scale, vec2(1.0));
    vec3 rgb = sharpScale != 0 && any(greaterThan(sourcePerOutput, vec2(1.001)))
        ? SampleConvertedBicubic(pixel)
        : SampleConverted(ivec2(floor(pixel + 0.5)));
    if (splitScreen != 0 && abs(uv.x - 0.5) < 1.25 / outputSize.x)
        rgb = vec3(0.85);
    fragColor = vec4(rgb, 1.0);
}
)glsl";

GLuint CompileShader(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        glDeleteShader(shader);
        Fail("OpenGL shader compilation failed: " + log);
    }
    return shader;
}

GLuint CreateProgram(const std::string& fragmentMain) {
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER,
                                          std::string(kFragmentCommon) + fragmentMain);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        glDeleteProgram(program);
        Fail("OpenGL program link failed: " + log);
    }
    return program;
}

class Renderer {
public:
    ~Renderer() {
        if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
        if (convertedTexture_) glDeleteTextures(1, &convertedTexture_);
        if (packedTexture_) glDeleteTextures(1, &packedTexture_);
        if (conversionProgram_) glDeleteProgram(conversionProgram_);
        if (scalingProgram_) glDeleteProgram(scalingProgram_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (context_) SDL_GL_DestroyContext(context_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void Initialize() {
        if (!SDL_Init(SDL_INIT_VIDEO)) Fail(std::string("SDL_Init: ") + SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window_ = SDL_CreateWindow("YUVision — waiting for YUYV",
                                   1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_) Fail(std::string("SDL_CreateWindow: ") + SDL_GetError());
        context_ = SDL_GL_CreateContext(window_);
        if (!context_) Fail(std::string("SDL_GL_CreateContext: ") + SDL_GetError());
        if (!SDL_GL_SetSwapInterval(1)) std::cerr << "Warning: VSync unavailable\n";
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        conversionProgram_ = CreateProgram(kConvertMain);
        scalingProgram_ = CreateProgram(kScaleMain);
    }

    void Upload(const V4l2Capture::Frame& frame) {
        if (!packedTexture_ || width_ != frame.width || height_ != frame.height) {
            width_ = frame.width;
            height_ = frame.height;
            if (!packedTexture_) glGenTextures(1, &packedTexture_);
            glBindTexture(GL_TEXTURE_2D, packedTexture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width_ / 2),
                         static_cast<GLsizei>(height_), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            if (!convertedTexture_) glGenTextures(1, &convertedTexture_);
            glBindTexture(GL_TEXTURE_2D, convertedTexture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width_),
                         static_cast<GLsizei>(height_), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            if (!framebuffer_) glGenFramebuffers(1, &framebuffer_);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   convertedTexture_, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                Fail("OpenGL conversion framebuffer is incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glBindTexture(GL_TEXTURE_2D, packedTexture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(frame.bytesPerLine / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width_ / 2),
                        static_cast<GLsizei>(height_), GL_RGBA, GL_UNSIGNED_BYTE, frame.data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    void Render() {
        if (!packedTexture_) return;
        glBindVertexArray(vao_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
        glUseProgram(conversionProgram_);
        SetCommonUniforms(conversionProgram_, static_cast<float>(width_),
                          static_cast<float>(height_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, packedTexture_);
        glUniform1i(glGetUniformLocation(conversionProgram_, "packedYuy2"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        int outputWidth = 1, outputHeight = 1;
        SDL_GetWindowSizeInPixels(window_, &outputWidth, &outputHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, outputWidth, outputHeight);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(scalingProgram_);
        SetCommonUniforms(scalingProgram_, static_cast<float>(outputWidth),
                          static_cast<float>(outputHeight));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, convertedTexture_);
        glUniform1i(glGetUniformLocation(scalingProgram_, "convertedRgb"), 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        SDL_GL_SwapWindow(window_);

        ++renderFrames_;
        const auto now = Clock::now();
        const double elapsed = Seconds(now - renderWindow_);
        if (elapsed >= 1.0) {
            renderFps_ = static_cast<double>(renderFrames_) / elapsed;
            renderFrames_ = 0;
            renderWindow_ = now;
        }
    }

    void SetTitle(const std::string& title) { SDL_SetWindowTitle(window_, title.c_str()); }
    SDL_Window* Window() const { return window_; }
    double RenderFps() const { return renderFps_; }
    void SetVsync(bool value) { vsync_ = value; SDL_GL_SetSwapInterval(value ? 1 : 0); }
    bool Vsync() const { return vsync_; }
    void ToggleFullscreen() { fullscreen_ = !fullscreen_; SDL_SetWindowFullscreen(window_, fullscreen_); }
    void SetMode(int value) { chromaMode_ = std::clamp(value, 0, 5); }
    void ToggleSplit() { splitScreen_ = !splitScreen_; }
    void ToggleRange() { limitedRange_ = !limitedRange_; }
    void ToggleScale() { sharpScale_ = !sharpScale_; }
    void AdjustOffset(float value) { chromaOffset_ = std::clamp(chromaOffset_ + value, -1.5f, 1.5f); }
    void AdjustThreshold(float value) { edgeThreshold_ = std::clamp(edgeThreshold_ + value, 0.01f, 0.2f); }
    int Mode() const { return chromaMode_; }
    float Offset() const { return chromaOffset_; }
    bool Limited() const { return limitedRange_; }

private:
    void SetCommonUniforms(GLuint program, float outputWidth, float outputHeight) {
        glUniform2f(glGetUniformLocation(program, "sourceSize"),
                    static_cast<float>(width_), static_cast<float>(height_));
        glUniform2f(glGetUniformLocation(program, "outputSize"), outputWidth, outputHeight);
        glUniform1f(glGetUniformLocation(program, "chromaOffset"), chromaOffset_);
        glUniform1f(glGetUniformLocation(program, "edgeThreshold"), edgeThreshold_);
        glUniform1i(glGetUniformLocation(program, "chromaMode"), chromaMode_);
        glUniform1i(glGetUniformLocation(program, "limitedRange"), limitedRange_ ? 1 : 0);
        glUniform1i(glGetUniformLocation(program, "splitScreen"), splitScreen_ ? 1 : 0);
        glUniform1i(glGetUniformLocation(program, "sharpScale"), sharpScale_ ? 1 : 0);
    }

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    GLuint vao_ = 0;
    GLuint packedTexture_ = 0;
    GLuint convertedTexture_ = 0;
    GLuint framebuffer_ = 0;
    GLuint conversionProgram_ = 0;
    GLuint scalingProgram_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool vsync_ = true;
    bool fullscreen_ = false;
    bool limitedRange_ = true;
    bool splitScreen_ = false;
    bool sharpScale_ = true;
    int chromaMode_ = 3;
    float chromaOffset_ = 0.0f;
    float edgeThreshold_ = 0.04f;
    uint32_t renderFrames_ = 0;
    double renderFps_ = 0.0;
    Clock::time_point renderWindow_ = Clock::now();
};

struct Options {
    std::string video;
    std::string audioInput;
    std::string audioOutput = "default";
    bool noAudio = false;
    bool listDevices = false;
    bool shaderTest = false;
};

Options ParseOptions(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        auto value = [&]() -> std::string {
            if (++index >= argc) Fail("Missing value after " + std::string(arg));
            return argv[index];
        };
        if (arg == "--video") result.video = value();
        else if (arg == "--audio-in") result.audioInput = value();
        else if (arg == "--audio-out") result.audioOutput = value();
        else if (arg == "--no-audio") result.noAudio = true;
        else if (arg == "--list-devices") result.listDevices = true;
        else if (arg == "--shader-test") result.shaderTest = true;
        else if (arg == "--help") {
            std::cout << "YUVision Linux\n"
                      << "  --video /dev/videoN\n  --audio-in ALSA_PCM\n"
                      << "  --audio-out ALSA_PCM\n  --no-audio\n  --list-devices\n"
                      << "  --shader-test\n";
            std::exit(0);
        } else Fail("Unknown argument: " + std::string(arg));
    }
    return result;
}

void ListDevices() {
    std::cout << "V4L2 capture devices:\n";
    for (const auto& device : EnumerateVideoDevices())
        std::cout << "  " << device.path << "  " << device.name << '\n';
    std::cout << "ALSA PCM devices:\n";
    for (const auto& [name, description] : EnumeratePcms())
        std::cout << "  " << name << "  " << description << '\n';
}

void RunShaderTest() {
    constexpr uint32_t width = 640;
    constexpr uint32_t height = 360;
    std::vector<uint8_t> packed(static_cast<size_t>(width) * height * 2u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; x += 2) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 2u;
            const bool red = x > width / 4 && x < width * 3 / 4 &&
                             y > height / 4 && y < height * 3 / 4;
            packed[offset + 0] = red ? 81 : static_cast<uint8_t>(16 + 180 * x / width);
            packed[offset + 1] = red ? 90 : 128;
            packed[offset + 2] = red ? 81 : static_cast<uint8_t>(16 + 180 * (x + 1) / width);
            packed[offset + 3] = red ? 240 : 128;
        }
    }

    Renderer renderer;
    renderer.Initialize();
    V4l2Capture::Frame frame{};
    frame.data = packed.data();
    frame.bytes = packed.size();
    frame.width = width;
    frame.height = height;
    frame.bytesPerLine = width * 2u;
    renderer.Upload(frame);
    renderer.Render();
    glFinish();
    std::cout << "OpenGL YUYV shader test passed\n";
}

std::string SelectVideo(const std::vector<VideoDevice>& devices) {
    if (devices.empty()) return {};
    for (const auto& device : devices) {
        const std::string name = Lower(device.name);
        if (name.find("ezcap") != std::string::npos || name.find("cam link") != std::string::npos)
            return device.path;
    }
    return devices.front().path;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseOptions(argc, argv);
        if (options.listDevices) {
            ListDevices();
            return 0;
        }
        if (options.shaderTest) {
            RunShaderTest();
            return 0;
        }
        const auto devices = EnumerateVideoDevices();
        const std::string video = options.video.empty() ? SelectVideo(devices) : options.video;
        if (video.empty()) Fail("No V4L2 streaming capture device found; use --list-devices");

        V4l2Capture capture;
        capture.Open(video);
        Renderer renderer;
        renderer.Initialize();
        AudioLoop audio;
        if (!options.noAudio) {
            const std::string input = options.audioInput.empty()
                ? FindEzcapAudioInput() : options.audioInput;
            if (input.empty()) std::cerr << "No ezcap ALSA input found; video will run without audio\n";
            else audio.Start(input, options.audioOutput);
        }

        bool running = true;
        auto titleUpdate = Clock::now();
        while (running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) continue;
                const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                switch (event.key.key) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_F11: renderer.ToggleFullscreen(); break;
                case SDLK_RETURN:
                    if ((event.key.mod & SDL_KMOD_ALT) != 0) renderer.ToggleFullscreen();
                    break;
                case SDLK_V: renderer.SetVsync(!renderer.Vsync()); break;
                case SDLK_A: renderer.ToggleScale(); break;
                case SDLK_R: renderer.ToggleRange(); break;
                case SDLK_S: renderer.ToggleSplit(); break;
                case SDLK_1: renderer.SetMode(0); break;
                case SDLK_2: renderer.SetMode(1); break;
                case SDLK_3: renderer.SetMode(2); break;
                case SDLK_4: renderer.SetMode(3); break;
                case SDLK_5: renderer.SetMode(4); break;
                case SDLK_6: renderer.SetMode(5); break;
                case SDLK_LEFT: renderer.AdjustOffset(shift ? -0.25f : -0.05f); break;
                case SDLK_RIGHT: renderer.AdjustOffset(shift ? 0.25f : 0.05f); break;
                case SDLK_DOWN: renderer.AdjustThreshold(shift ? -0.025f : -0.01f); break;
                case SDLK_UP: renderer.AdjustThreshold(shift ? 0.025f : 0.01f); break;
                default: break;
                }
            }

            V4l2Capture::Frame frame;
            if (capture.TakeLatest(frame)) {
                renderer.Upload(frame);
                capture.Release(frame);
                renderer.Render();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const auto now = Clock::now();
            if (Seconds(now - titleUpdate) >= 0.5) {
                std::string title = "YUVision  Capture " + std::to_string(capture.CaptureFps()) +
                    " fps  Render " + std::to_string(renderer.RenderFps()) +
                    " fps  Dropped " + std::to_string(capture.Dropped()) +
                    "  1920x1080 YUYV" +
                    "  Mode " + std::to_string(renderer.Mode() + 1) +
                    "  Offset " + std::to_string(renderer.Offset()) +
                    (renderer.Limited() ? "  Limited" : "  Full") +
                    "  AudioQ " + std::to_string(audio.BufferedFrames());
                const std::string audioError = audio.Error();
                if (!audioError.empty()) title += "  AUDIO ERROR: " + audioError;
                renderer.SetTitle(title);
                titleUpdate = now;
            }
        }
        audio.Stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "YUVision: " << error.what() << '\n';
        return 1;
    }
}
