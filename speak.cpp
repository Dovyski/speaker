// ────────────────────────────────────────────────────────────────────────────
// speak.exe — self-contained text-to-speech with an on-screen pulsing orb.
//
// Two ways to run, same binary:
//
//   speak.exe --serve                 resident daemon: loads the model once and
//                                     serves upstream PocketTTS.cpp's HTTP API
//   speak.exe "Hello world."          speaks; uses the daemon if one is up
//                                     (~100 ms to first audio), otherwise loads
//                                     the model in-process (~5 s) and warms a
//                                     daemon in the background for next time
//
// The engine is upstream's single-file runtime, compiled into this binary: we
// use both its ptt_* C API (local synthesis) and its TTSServer (daemon mode).
// PTT_SHARED_LIB drops its main(). It must be included first — it sets NOMINMAX
// and pulls in winsock2.h before windows.h.
// ────────────────────────────────────────────────────────────────────────────

#define WIN32_LEAN_AND_MEAN
#include "pocket_tts.cpp"

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int    kSampleRate  = 24000;
constexpr size_t kEnvBlock    = 256;   // frames per amplitude-envelope entry
constexpr int    kDefaultPort = 8123;

// Silence appended to the render stream after the last real sample. Without it
// the device stops the moment the final sample is consumed, which clips the
// audible tail of the last word.
constexpr size_t kTailSilenceFrames = kSampleRate / 4;   // 250 ms

// Shared state between the audio writer and the orb renderer.
std::atomic<uint64_t> g_play_pos{0};      // frames handed to the speakers
std::atomic<bool>     g_audio_done{false};
std::mutex            g_env_mtx;
std::vector<float>    g_env;             // peak amplitude per kEnvBlock frames

// ── timing ──────────────────────────────────────────────────────────────────

LARGE_INTEGER g_t0{}, g_qpf{};

void ClockStart() {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_t0);
}

double MsSinceStart() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_t0.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
}

// ── Orb overlay ─────────────────────────────────────────────────────────────

constexpr float kPi = 3.14159265358979f;

int g_orb_size = 220;

enum class OrbStyle { Aurora, Dot };
OrbStyle g_orb_style = OrbStyle::Aurora;

struct Rgb { float r, g, b; };

// "dot" style: the original solid core plus halo
constexpr Rgb kCoreIn  {1.00f, 0.86f, 0.78f};   // hot centre
constexpr Rgb kCoreOut {0.85f, 0.42f, 0.26f};   // core rim
constexpr Rgb kGlow    {0.88f, 0.48f, 0.31f};   // outer halo

// "aurora" style: a luminous rim whose colours travel around the ring
constexpr Rgb kEmber {1.00f, 0.30f, 0.13f};     // red-orange
constexpr Rgb kHot   {1.00f, 0.96f, 0.93f};     // white-hot
constexpr Rgb kAzure {0.16f, 0.52f, 1.00f};     // blue
constexpr Rgb kCyan  {0.36f, 0.86f, 1.00f};     // cyan

float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

float SmoothStep(float edge0, float edge1, float x) {
    float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.f - 2.f * t);
}

Rgb Mix(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Colour around the rim, u in 0..1. Ember and azure sit opposite each other with
// white-hot arcs between them, which is what gives the ring its two-tone look.
Rgb RimColour(float u) {
    static const Rgb stops[] = {
        kEmber, Mix(kEmber, kHot, 0.45f), kHot, kAzure, kCyan, Mix(kAzure, kHot, 0.35f),
    };
    constexpr int n = static_cast<int>(sizeof(stops) / sizeof(stops[0]));
    const float x  = u * n;
    const int   i0 = static_cast<int>(x) % n;
    float       t  = x - std::floor(x);
    t = t * t * (3.f - 2.f * t);                 // ease the hand-off between stops
    return Mix(stops[i0], stops[(i0 + 1) % n], t);
}

// Per-pixel polar coordinates never change, so they are computed once, together
// with a lookup table for the Gaussian falloff used by the rim and its glow.
struct OrbGeometry {
    int                  size = 0;
    std::vector<float>   dist;
    std::vector<int16_t> angle;
    std::vector<float>   gauss;

    static constexpr int kAngles   = 512;
    static constexpr int kGaussLut = 512;

    void Ensure(int s) {
        if (gauss.empty()) {
            gauss.resize(kGaussLut);
            for (int i = 0; i < kGaussLut; ++i) {
                const float x = 4.f * i / (kGaussLut - 1);
                gauss[i] = std::exp(-x * x);
            }
        }
        if (size == s) return;
        size = s;
        dist.resize(static_cast<size_t>(s) * s);
        angle.resize(static_cast<size_t>(s) * s);
        const float c = s * 0.5f;
        for (int y = 0; y < s; ++y) {
            for (int x = 0; x < s; ++x) {
                const float dx = x + 0.5f - c, dy = y + 0.5f - c;
                const size_t i = static_cast<size_t>(y) * s + x;
                dist[i] = std::sqrt(dx * dx + dy * dy);
                float a = std::atan2(dy, dx) / (2 * kPi);
                if (a < 0.f) a += 1.f;
                angle[i] = static_cast<int16_t>(
                    std::min(kAngles - 1, static_cast<int>(a * kAngles)));
            }
        }
    }

    float Gauss(float x) const {   // x >= 0
        if (x >= 4.f) return 0.f;
        return gauss[static_cast<int>(x * (kGaussLut - 1) / 4.f)];
    }
};

OrbGeometry g_geom;

LRESULT CALLBACK OrbWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Writes one premultiplied-alpha BGRA pixel.
inline uint32_t Pack(const Rgb& c, float a) {
    const auto ch = [a](float v) {
        return static_cast<uint32_t>(Clamp01(v) * a * 255.f + 0.5f);
    };
    return (static_cast<uint32_t>(a * 255.f + 0.5f) << 24) |
           (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

// "aurora": a luminous ring. The rim is a thin white-hot line riding on a wide
// coloured glow, and the whole thing spins slowly.
//
// Two separate drives: `voice` is the actual audio envelope and is the only thing
// that distorts the outline — silence means a perfect circle. `level` is voice or
// the idle breath, whichever is larger, and drives size and brightness so the orb
// still looks alive between words.
void ComposeAurora(uint32_t* pixels, int S, float level, float voice, float fade,
                   float time) {
    g_geom.Ensure(S);

    const float R0         = S * 0.29f * (1.f + 0.09f * level);
    // Grows superlinearly with the voice: barely rippling when quiet, properly
    // turbulent when loud, and exactly 0 — a true circle — in silence.
    const float wobble     = (0.085f + 0.13f * voice) * voice * R0;
    const float churn      = time * (1.f + 1.1f * voice);   // faster when loud
    const float spin       = time * 0.55f;           // the outline orbits
    const float rotation   = time * 0.33f;           // the colours drift round
    const float rim_sigma  = 1.7f + 1.0f * level;    // the hot line itself
    const float glow_sigma = 8.5f + 7.0f * level;    // coloured halo either side
    const float gain       = 0.72f + 0.45f * level;

    // Out-of-phase harmonics: circular enough to read as a ring, irregular enough
    // not to look machine-drawn. The high ones are scaled by the voice, so loud
    // passages get sharp kinks where quiet ones only get broad lobes. Amplitudes
    // are normalized, otherwise they occasionally align and the ring turns into a
    // star instead of a wobbling circle.
    const float h1 = 1.00f, h2 = 0.62f, h3 = 0.45f;
    const float h4 = 0.60f * voice, h5 = 0.f;   // 11θ dropped: reads as a starfish
    const float norm = 1.f / (h1 + h2 + h3 + h4 + h5);

    static std::vector<float> radius;
    static std::vector<Rgb>   colour;
    radius.resize(OrbGeometry::kAngles);
    colour.resize(OrbGeometry::kAngles);
    for (int a = 0; a < OrbGeometry::kAngles; ++a) {
        const float th = 2 * kPi * a / OrbGeometry::kAngles;
        const float ph = th - spin;   // harmonics ride the spin, so bumps travel
        radius[a] = R0 + wobble * norm *
                             (h1 * std::sin(3 * ph + 1.10f * churn) +
                              h2 * std::sin(5 * ph - 0.80f * churn) +
                              h3 * std::sin(2 * ph + 0.47f * churn) +
                              h4 * std::sin(7 * ph + 1.90f * churn) +
                              h5 * std::sin(11 * ph - 2.40f * churn));
        colour[a] = RimColour(std::fmod((th - rotation) / (2 * kPi) + 2.f, 1.f));
    }

    const size_t n = static_cast<size_t>(S) * S;
    for (size_t i = 0; i < n; ++i) {
        const float d  = g_geom.dist[i];
        const float R  = radius[g_geom.angle[i]];
        const float dr = d - R;

        const float rim  = g_geom.Gauss(std::fabs(dr) / rim_sigma);
        const float glow = 0.62f * g_geom.Gauss(std::fabs(dr) / glow_sigma);
        // Light bleeding inward, so the inside is tinted rather than empty.
        const float bleed = dr < 0.f ? 0.20f * g_geom.Gauss(-dr / (0.5f * R)) : 0.f;

        float alpha = Clamp01((rim + glow + bleed) * gain);
        if (alpha <= 0.004f) { pixels[i] = 0; continue; }
        pixels[i] = Pack(Mix(colour[g_geom.angle[i]], kHot, 0.85f * rim), alpha * fade);
    }
}

// "dot": the original solid core with a soft halo.
void ComposeDot(uint32_t* pixels, int S, float level, float fade) {
    const float cx = S * 0.5f, cy = S * 0.5f;
    const float r  = 20.f + 12.f * level;              // solid core radius
    const float R  = r + 34.f + 22.f * level;          // halo radius

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);

            const float core = 1.f - SmoothStep(r - 1.5f, r + 0.75f, d);
            float halo = 0.f;
            if (d > r && d < R) {
                const float u = 1.f - (d - r) / (R - r);
                halo = 0.55f * u * u * u;
            }
            const float a = Clamp01(core + (1.f - core) * halo);
            if (a <= 0.f) { pixels[y * S + x] = 0; continue; }

            // core: hot centre → rim, then rim → halo colour outside the core
            const float t = std::min(1.f, d / std::max(r, 1.f));
            Rgb c = Mix(kCoreIn, kCoreOut, t);
            if (core < 1.f) c = Mix(c, kGlow, 1.f - core);

            pixels[y * S + x] = Pack(c, a * fade);
        }
    }
}

void ComposeOrb(uint32_t* pixels, float level, float voice, float fade, float time) {
    if (g_orb_style == OrbStyle::Aurora) {
        ComposeAurora(pixels, g_orb_size, level, voice, fade, time);
    } else {
        ComposeDot(pixels, g_orb_size, level, fade);
    }
}

// Pushes the composed frame to the layered window, parked just inside the
// bottom-right corner of the work area (above the taskbar).
void PushOrb(HWND hwnd, HDC mem_dc) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT pos{work.right - g_orb_size - 24, work.bottom - g_orb_size - 24};
    SIZE  size{g_orb_size, g_orb_size};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &pos, &size, mem_dc, &src, 0, &blend,
                        ULW_ALPHA);
}

// Runs the whole overlay lifetime on its own thread: fade in, animate while
// audio plays, fade out, tear down.
void OrbThread() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OrbWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClaudeSpeakOrb";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, g_orb_size, g_orb_size, nullptr,
        nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;

    HDC screen = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_orb_size;
    bi.bmiHeader.biHeight      = -g_orb_size;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem_dc, dib);
    auto* pixels = static_cast<uint32_t*>(bits);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    float voice = 0.f, level = 0.f, fade = 0.f;
    bool  closing = false;
    for (int frame = 0;; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Loudness of the block the speakers are playing right now.
        const size_t idx = static_cast<size_t>(g_play_pos.load()) / kEnvBlock;
        float target = 0.f;
        {
            std::lock_guard<std::mutex> lock(g_env_mtx);
            if (idx < g_env.size()) target = g_env[idx];
        }
        // Voice: the audio envelope alone, so silence really is silence and the
        // outline settles into a perfect circle. Decays a little slower than it
        // rises, otherwise the ring snaps flat between syllables.
        const float voice_target = Clamp01(target * 1.6f);
        voice += (voice_target - voice) * (voice_target > voice ? 0.35f : 0.12f);

        // Level: keep it alive between words with a slow breath.
        const float breath = 0.10f + 0.06f * std::sin(frame * 0.09f);
        level += (std::max(voice, breath) - level) * 0.35f;

        if (g_audio_done.load()) closing = true;
        fade += ((closing ? 0.f : 1.f) - fade) * (closing ? 0.12f : 0.22f);
        if (closing && fade < 0.01f) break;

        ComposeOrb(pixels, level, voice, fade, frame / 60.f);
        PushOrb(hwnd, mem_dc);
        Sleep(16);
    }

    SelectObject(mem_dc, old);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen);
    DestroyWindow(hwnd);
}

// ── PCM sources ─────────────────────────────────────────────────────────────
// Playback does not care where samples come from: the local in-process engine
// and the HTTP daemon both look like "hand me the next chunk".

struct PcmSource {
    virtual ~PcmSource() = default;
    // Fills `out` with the next chunk. Returns false once the stream is over.
    virtual bool Next(std::vector<float>* out) = 0;
};

// In-process synthesis: runs the engine on a worker thread and queues the chunks
// it emits. We drive pocket_tts::PocketTTS directly rather than through its ptt_*
// C API because that API cannot pass EOS settings, and the tail of the last word
// depends on them.
class EngineSource : public PcmSource {
public:
    EngineSource(pocket_tts::PocketTTS* tts, std::string text, std::string voice) {
        worker_ = std::thread([this, tts, text = std::move(text),
                                     voice = std::move(voice)]() mutable {
            try {
                tts->stream(text, voice, [this](const float* s, size_t n) {
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        if (abort_) return false;
                        queue_.emplace_back(s, s + n);
                    }
                    cv_.notify_one();
                    return true;
                });
            } catch (const std::exception& e) {
                error_ = e.what();
            }
            {
                std::lock_guard<std::mutex> lock(mtx_);
                done_ = true;
            }
            cv_.notify_one();
        });
    }

    ~EngineSource() override {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            abort_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool Next(std::vector<float>* out) override {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    const std::string& error() const { return error_; }

private:
    std::thread                          worker_;
    std::mutex                           mtx_;
    std::condition_variable              cv_;
    std::deque<std::vector<float>>       queue_;
    std::string                          error_;
    bool                                 done_  = false;
    bool                                 abort_ = false;
};

// ── HTTP client for the resident daemon ─────────────────────────────────────

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

bool WinsockInit() {
    static bool ready = false;
    if (ready) return true;
    WSADATA wsa;
    ready = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    return ready;
}

// Connects to 127.0.0.1:port, giving up after timeout_ms. A refused connection
// (nothing listening) fails immediately, which is the common cold-start case.
SOCKET ConnectLocal(int port, int timeout_ms) {
    if (!WinsockInit()) return INVALID_SOCKET;

    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;

    u_long nonblocking = 1;
    ioctlsocket(fd, FIONBIO, &nonblocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    fd_set write_set, error_set;
    FD_ZERO(&write_set); FD_SET(fd, &write_set);
    FD_ZERO(&error_set); FD_SET(fd, &error_set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int ready = select(0, nullptr, &write_set, &error_set, &tv);

    if (ready <= 0 || FD_ISSET(fd, &error_set)) {
        closesocket(fd);
        return INVALID_SOCKET;
    }

    u_long blocking = 0;
    ioctlsocket(fd, FIONBIO, &blocking);
    const int one = 1;   // no Nagle: we want the first audio chunk immediately
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
               sizeof(one));
    const int recv_timeout = 120000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
    return fd;
}

bool SendAll(SOCKET fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Streams `POST /tts` from the daemon, decoding chunked transfer-encoding into
// float samples. Partial chunks are handed to playback as soon as they land, so
// audio starts on the first few kilobytes rather than the first full chunk.
class DaemonSource : public PcmSource {
public:
    ~DaemonSource() override {
        if (fd_ != INVALID_SOCKET) closesocket(fd_);
    }

    // Returns nullptr if no daemon answered (caller falls back to local).
    static std::unique_ptr<DaemonSource> Post(int port, const std::string& text,
                                              const std::string& voice,
                                              int connect_timeout_ms,
                                              std::string* err) {
        SOCKET fd = ConnectLocal(port, connect_timeout_ms);
        if (fd == INVALID_SOCKET) { if (err) *err = "no daemon on port"; return nullptr; }

        const std::string body = "{\"text\":\"" + JsonEscape(text) +
                                 "\",\"voice\":\"" + JsonEscape(voice) + "\"}";
        const std::string req =
            "POST /tts HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;

        if (!SendAll(fd, req)) {
            closesocket(fd);
            if (err) *err = "daemon closed the connection";
            return nullptr;
        }

        auto src = std::unique_ptr<DaemonSource>(new DaemonSource(fd));
        if (!src->ReadHeaders(err)) return nullptr;
        return src;
    }

    bool Next(std::vector<float>* out) override {
        out->clear();
        while (out->empty()) {
            if (chunk_left_ == 0) {
                std::string line;
                if (!ReadLine(&line)) return false;
                if (line.empty()) continue;                  // trailing CRLF
                chunk_left_ = std::strtoul(line.c_str(), nullptr, 16);
                if (chunk_left_ == 0) return false;           // terminator
            }
            // Take whatever of this chunk has arrived, floats-aligned.
            while (buf_.size() < 4 && chunk_left_ >= 4) {
                if (!ReadMore()) return !out->empty();
            }
            size_t take = std::min<size_t>(buf_.size(), chunk_left_);
            take -= take % 4;
            if (take == 0) {
                if (!ReadMore()) return !out->empty();
                continue;
            }
            const size_t n = take / 4;
            out->resize(n);
            std::memcpy(out->data(), buf_.data(), take);
            buf_.erase(0, take);
            chunk_left_ -= take;
        }
        return true;
    }

private:
    explicit DaemonSource(SOCKET fd) : fd_(fd) {}

    bool ReadMore() {
        char tmp[16384];
        const int n = recv(fd_, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf_.append(tmp, static_cast<size_t>(n));
        return true;
    }

    bool ReadLine(std::string* line) {
        for (;;) {
            const size_t nl = buf_.find("\r\n");
            if (nl != std::string::npos) {
                *line = buf_.substr(0, nl);
                buf_.erase(0, nl + 2);
                return true;
            }
            if (!ReadMore()) return false;
        }
    }

    bool ReadHeaders(std::string* err) {
        std::string line;
        if (!ReadLine(&line)) { if (err) *err = "no response"; return false; }
        if (line.find(" 200") == std::string::npos) {
            if (err) *err = "daemon said: " + line;
            return false;
        }
        for (;;) {
            if (!ReadLine(&line)) { if (err) *err = "truncated headers"; return false; }
            if (line.empty()) return true;   // end of headers
        }
    }

    SOCKET      fd_ = INVALID_SOCKET;
    std::string buf_;
    size_t      chunk_left_ = 0;
};

// GET /health — cheap liveness probe.
bool DaemonAlive(int port, int timeout_ms) {
    SOCKET fd = ConnectLocal(port, timeout_ms);
    if (fd == INVALID_SOCKET) return false;
    const bool sent = SendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                  "Connection: close\r\n\r\n");
    bool ok = false;
    if (sent) {
        char buf[256]{};
        const int n = recv(fd, buf, sizeof(buf) - 1, 0);
        ok = n > 0 && std::string(buf, n).find(" 200") != std::string::npos;
    }
    closesocket(fd);
    return ok;
}

// ── WASAPI playback ─────────────────────────────────────────────────────────

// Renders mono 24 kHz float audio pulled from `src` to the default device,
// recording an amplitude envelope and the live playback position as it goes.
// Returns false on a fatal audio error.
bool PlayStream(PcmSource* src, std::vector<float>* recorded, double* first_audio_ms) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioRenderClient*  render     = nullptr;
    bool ok = false;

    auto cleanup = [&] {
        if (render) render->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator),
                               reinterpret_cast<void**>(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client)))) {
        std::fprintf(stderr, "speak: no audio output device\n");
        cleanup();
        return false;
    }

    WAVEFORMATEX wf{};
    wf.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = kSampleRate;
    wf.wBitsPerSample  = 32;
    wf.nBlockAlign     = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    // Let the audio engine resample/upmix our 24 kHz mono float stream.
    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    5'000'000 /* 500ms */, 0, &wf, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "speak: audio init failed (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        cleanup();
        return false;
    }

    UINT32 buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void**>(&render)))) {
        cleanup();
        return false;
    }
    client->Start();

    uint64_t           written = 0;
    std::vector<float> chunk;
    size_t             chunk_pos = 0;
    bool               source_done = false, started = false;

    while (true) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        g_play_pos.store(written > padding ? written - padding : 0);

        if (chunk_pos >= chunk.size() && !source_done) {
            chunk.clear();
            chunk_pos = 0;
            if (src->Next(&chunk) && !chunk.empty()) {
                std::lock_guard<std::mutex> lock(g_env_mtx);
                for (size_t i = 0; i < chunk.size(); i += kEnvBlock) {
                    float peak = 0.f;
                    const size_t end = std::min(i + kEnvBlock, chunk.size());
                    for (size_t j = i; j < end; ++j) peak = std::max(peak, std::fabs(chunk[j]));
                    g_env.push_back(peak);
                }
                if (recorded) recorded->insert(recorded->end(), chunk.begin(), chunk.end());
            } else {
                // Follow the last sample with silence, so the device drains the
                // real tail instead of stopping on top of it.
                source_done = true;
                chunk.assign(kTailSilenceFrames, 0.f);
            }
        }

        if (chunk_pos < chunk.size()) {
            const UINT32 space = buffer_frames - padding;
            if (space > 0) {
                const UINT32 n = std::min<UINT32>(space,
                                     static_cast<UINT32>(chunk.size() - chunk_pos));
                BYTE* dst = nullptr;
                if (SUCCEEDED(render->GetBuffer(n, &dst))) {
                    std::memcpy(dst, chunk.data() + chunk_pos, n * sizeof(float));
                    render->ReleaseBuffer(n, 0);
                    written   += n;
                    chunk_pos += n;
                    if (!started) {
                        started = true;
                        if (first_audio_ms) *first_audio_ms = MsSinceStart();
                    }
                }
                continue;   // keep the buffer topped up before sleeping
            }
        } else if (source_done) {
            if (padding == 0) { ok = started; break; }   // fully drained
        }
        Sleep(4);
    }

    client->Stop();
    cleanup();
    return ok;
}

// ── misc helpers ────────────────────────────────────────────────────────────

std::string Utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string ExePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return Utf8(path);
}

std::string ExeDir() {
    const std::string s = ExePath();
    const size_t cut = s.find_last_of("\\/");
    return cut == std::string::npos ? "." : s.substr(0, cut);
}

void WriteWav(const std::string& path, const std::vector<float>& samples) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "speak: cannot write %s\n", path.c_str()); return; }
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t rate = kSampleRate;
    const uint16_t fmt = 3, channels = 1, bits = 32, block = 4;
    const uint32_t byte_rate = rate * block;
    const uint32_t riff_size = 36 + data_bytes;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff_size, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmt_size = 16;
    std::fwrite(&fmt_size, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f);      std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);     std::fwrite(&byte_rate, 4, 1, f);
    std::fwrite(&block, 2, 1, f);    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);    std::fwrite(&data_bytes, 4, 1, f);
    std::fwrite(samples.data(), 1, data_bytes, f);
    std::fclose(f);
}

// Renders a single orb frame to a 32-bit BMP, flattened over a dark backdrop.
// Used to eyeball/regression-check the visuals without a screen recorder.
void DumpOrbFrame(const std::string& path, float level, float voice, float time) {
    std::vector<uint32_t> px(static_cast<size_t>(g_orb_size) * g_orb_size);
    ComposeOrb(px.data(), level, voice, 1.0f, time);

    const uint32_t data_bytes = static_cast<uint32_t>(px.size() * 4);
    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize    = fh.bfOffBits + data_bytes;
    BITMAPINFOHEADER ih{};
    ih.biSize     = sizeof(ih);
    ih.biWidth    = g_orb_size;
    ih.biHeight   = -g_orb_size;  // top-down
    ih.biPlanes   = 1;
    ih.biBitCount = 32;

    for (auto& p : px) {  // flatten premultiplied pixel over #22272E
        const float a = ((p >> 24) & 0xFF) / 255.f;
        const auto  ch = [&](int shift, int bg) {
            return static_cast<uint32_t>(std::min(255.f,
                       ((p >> shift) & 0xFF) + (1.f - a) * bg));
        };
        p = 0xFF000000u | (ch(16, 0x22) << 16) | (ch(8, 0x27) << 8) | ch(0, 0x2E);
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "speak: cannot write %s\n", path.c_str()); return; }
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(px.data(), 1, data_bytes, f);
    std::fclose(f);
}

// ── options ─────────────────────────────────────────────────────────────────

struct Options {
    std::string text;
    std::string voice       = "alba.wav";
    std::string save_path;
    std::string models_dir;
    std::string voices_dir;
    std::string dump_orb;       // render one orb frame to this BMP and exit
    std::string orb_preview;    // render a strip of orb frames to <prefix>N.bmp
    bool   show_orb    = true;
    bool   timing      = false;
    bool   auto_serve  = true;   // warm a daemon in the background on a cold call
    bool   use_daemon  = true;   // try the daemon first
    float  temperature = 0.7f;
    float  eos_threshold = -4.0f;
    int    eos_extra   = 4;      // upstream default (-1, auto) clips the last word
    int    threads     = 0;
    int    port        = kDefaultPort;
    int    keepalive   = 60;     // daemon: seconds between warm-up nudges, 0 = off
};

pocket_tts::Config BuildConfig(const Options& opt) {
    pocket_tts::Config cfg;
    cfg.models_dir       = opt.models_dir;
    cfg.voices_dir       = opt.voices_dir;
    cfg.tokenizer_path   = opt.models_dir + "\\tokenizer.model";
    cfg.precision        = "int8";
    cfg.temperature      = opt.temperature;
    cfg.num_threads      = opt.threads;
    cfg.eos_threshold    = opt.eos_threshold;
    cfg.eos_extra_frames = opt.eos_extra;
    return cfg;
}

// ── daemon ──────────────────────────────────────────────────────────────────

std::string DaemonMutexName(int port) {
    return "Local\\speaker-daemon-" + std::to_string(port);
}

// True if a daemon for this port exists — including one still loading models,
// which is what stops a burst of cold calls from each spawning their own.
bool DaemonRegistered(int port) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, Wide(DaemonMutexName(port)).c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

std::string PidFilePath(int port) {
    wchar_t buf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
    const std::string dir = Utf8(buf) + "\\speaker";
    CreateDirectoryW(Wide(dir).c_str(), nullptr);
    return dir + "\\daemon-" + std::to_string(port) + ".pid";
}

// Loads the model once, warms it up, then serves upstream's HTTP API forever.
int RunDaemon(const Options& opt) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, Wide(DaemonMutexName(opt.port)).c_str());
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "speak: a daemon is already running on port %d\n", opt.port);
        return 0;
    }

    const pocket_tts::Config cfg = BuildConfig(opt);

    try {
        std::fprintf(stderr, "speak: loading models from %s\n", opt.models_dir.c_str());
        pocket_tts::PocketTTS tts(cfg);
        std::fprintf(stderr, "speak: loaded in %.2fs, warming up...\n", MsSinceStart() / 1000);
        tts.warmup();

        // Prime the voice too: conditioning a cold voice costs hundreds of ms,
        // and we would rather pay that here than on the first real request.
        try {
            tts.stream("Ready.", opt.voice, [](const float*, size_t) { return true; });
        } catch (const std::exception& e) {
            std::fprintf(stderr, "speak: voice '%s' not primed (%s)\n",
                         opt.voice.c_str(), e.what());
        }
        std::fprintf(stderr, "speak: ready in %.2fs\n", MsSinceStart() / 1000);

        const std::string pid_file = PidFilePath(opt.port);
        if (!pid_file.empty()) {
            if (FILE* f = std::fopen(pid_file.c_str(), "w")) {
                std::fprintf(f, "%lu\n", GetCurrentProcessId());
                std::fclose(f);
            }
        }

        pocket_tts::TTSServer server(tts, opt.port);
        if (!server.start()) return 1;

        // Keepalive: an idle daemon gets slow again (CPU clocks down, its working
        // set gets paged out), turning ~100 ms calls into ~500 ms ones. So nudge
        // it periodically with a throwaway word. This goes through our own HTTP
        // endpoint rather than calling the engine directly, so the server
        // serializes it against real requests instead of racing them.
        if (opt.keepalive > 0) {
            std::thread([opt] {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(opt.keepalive));
                    std::string err;
                    auto src = DaemonSource::Post(opt.port, "Ok.", opt.voice, 500, &err);
                    if (!src) continue;
                    std::vector<float> discard;
                    while (src->Next(&discard)) {}
                }
            }).detach();
        }

        server.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "speak: daemon failed: %s\n", e.what());
        return 1;
    }
    return 0;
}

// Starts a detached daemon so the *next* call is instant. Fire and forget.
bool SpawnDaemon(const Options& opt) {
    std::string cmd = "\"" + ExePath() + "\" --serve --port " + std::to_string(opt.port) +
                      " --voice \"" + opt.voice + "\"" +
                      " --models-dir \"" + opt.models_dir + "\"" +
                      " --voices-dir \"" + opt.voices_dir + "\"" +
                      " --eos-extra " + std::to_string(opt.eos_extra) +
                      " --keepalive " + std::to_string(opt.keepalive);
    if (opt.threads) cmd += " --threads " + std::to_string(opt.threads);

    std::wstring wcmd = Wide(cmd);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                                   DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok == TRUE;
}

int StopDaemon(const Options& opt) {
    const std::string pid_file = PidFilePath(opt.port);
    DWORD pid = 0;
    if (FILE* f = std::fopen(pid_file.c_str(), "r")) {
        unsigned long v = 0;
        if (std::fscanf(f, "%lu", &v) == 1) pid = static_cast<DWORD>(v);
        std::fclose(f);
    }
    if (!pid) {
        std::fprintf(stderr, "speak: no daemon recorded for port %d\n", opt.port);
        return 1;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        std::fprintf(stderr, "speak: daemon %lu is not running\n", pid);
        std::remove(pid_file.c_str());
        return 1;
    }
    const bool killed = TerminateProcess(h, 0) == TRUE;
    CloseHandle(h);
    std::remove(pid_file.c_str());
    std::printf("%s daemon %lu on port %d\n", killed ? "stopped" : "could not stop",
                pid, opt.port);
    return killed ? 0 : 1;
}

int DaemonStatus(const Options& opt) {
    const bool alive = DaemonAlive(opt.port, 300);
    std::printf("port %d: %s\n", opt.port,
                alive ? "daemon ready"
                      : (DaemonRegistered(opt.port) ? "daemon starting up"
                                                    : "no daemon"));
    return alive ? 0 : 1;
}

void Usage() {
    std::fprintf(stderr,
        "speak — text to speech with an on-screen orb\n\n"
        "  speak [options] \"text to speak\"\n"
        "  speak --serve [--port N]      run the resident daemon (fast speech)\n"
        "  speak --status | --stop       inspect or stop the daemon\n\n"
        "  --voice <name|path>   voice sample (default: alba.wav)\n"
        "  --save <file.wav>     also save the audio\n"
        "  --no-orb              skip the on-screen indicator\n"
        "  --orb-style <s>       aurora (default) or dot\n"
        "  --orb-size <px>       orb square size (default 220)\n"
        "  --dump-orb <f.bmp>    render one orb frame to a BMP and exit\n"
        "  --orb-preview <pfx>   render a strip of orb frames and exit\n"
        "  --keepalive <sec>     daemon: nudge itself every N seconds so it stays\n"
        "                        fast when idle (default 60)\n"
        "  --no-keepalive        daemon: let it go cold between calls\n"
        "  --local               never use the daemon; synthesize in-process\n"
        "  --no-auto-serve       do not start a daemon in the background\n"
        "  --timing              report time to first audio\n"
        "  --port <n>            daemon port (default 8123)\n"
        "  --temperature <f>     sampling temperature (default 0.7)\n"
        "  --eos-extra <n>       extra frames after end-of-speech (default 4,\n"
        "                        -1 = upstream auto; raise if words get clipped)\n"
        "  --eos-threshold <f>   end-of-speech threshold (default -4.0, lower =\n"
        "                        later cutoff)\n"
        "  --threads <n>         thread budget (0 = half the cores)\n"
        "  --models-dir <dir>    ONNX models (default: <exe dir>/models)\n"
        "  --voices-dir <dir>    voice samples (default: <exe dir>/voices)\n");
}

}  // namespace

int main() {
    ClockStart();
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return 1;

    Options opt;
    opt.models_dir = ExeDir() + "\\models";
    opt.voices_dir = ExeDir() + "\\voices";
    bool serve = false, stop = false, status = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = Utf8(wargv[i]);
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "speak: %s needs a value\n", name);
                std::exit(2);
            }
            return Utf8(wargv[++i]);
        };
        if (a == "--voice")            opt.voice = next("--voice");
        else if (a == "--save")        opt.save_path = next("--save");
        else if (a == "--models-dir")  opt.models_dir = next("--models-dir");
        else if (a == "--voices-dir")  opt.voices_dir = next("--voices-dir");
        else if (a == "--temperature") opt.temperature = std::strtof(next("--temperature").c_str(), nullptr);
        else if (a == "--eos-extra")   opt.eos_extra = std::atoi(next("--eos-extra").c_str());
        else if (a == "--keepalive")   opt.keepalive = std::atoi(next("--keepalive").c_str());
        else if (a == "--no-keepalive") opt.keepalive = 0;
        else if (a == "--eos-threshold") opt.eos_threshold = std::strtof(next("--eos-threshold").c_str(), nullptr);
        else if (a == "--threads")     opt.threads = std::atoi(next("--threads").c_str());
        else if (a == "--port")        opt.port = std::atoi(next("--port").c_str());
        else if (a == "--no-orb")      opt.show_orb = false;
        else if (a == "--timing")      opt.timing = true;
        else if (a == "--local")       opt.use_daemon = false;
        else if (a == "--no-auto-serve") opt.auto_serve = false;
        else if (a == "--serve")       serve = true;
        else if (a == "--stop")        stop = true;
        else if (a == "--status")      status = true;
        else if (a == "--dump-orb")    opt.dump_orb = next("--dump-orb");
        else if (a == "--orb-preview") opt.orb_preview = next("--orb-preview");
        else if (a == "--orb-size")    g_orb_size = std::max(60, std::atoi(next("--orb-size").c_str()));
        else if (a == "--orb-style") {
            const std::string style = next("--orb-style");
            if (style == "aurora")   g_orb_style = OrbStyle::Aurora;
            else if (style == "dot") g_orb_style = OrbStyle::Dot;
            else {
                std::fprintf(stderr, "speak: unknown orb style '%s' (aurora|dot)\n",
                             style.c_str());
                return 2;
            }
        }
        else if (a == "-h" || a == "--help") { Usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "speak: unknown option %s\n", a.c_str());
            return 2;
        } else {
            if (!opt.text.empty()) opt.text += " ";
            opt.text += a;
        }
    }
    LocalFree(wargv);

    if (!opt.dump_orb.empty()) {
        DumpOrbFrame(opt.dump_orb, 0.65f, 0.65f, 0.f);
        return 0;
    }
    if (!opt.orb_preview.empty()) {
        // A strip across time and loudness, to review the look without a capture.
        // Frame 0 is silence, so the "perfect circle when idle" case is visible.
        constexpr int kFrames = 6;
        constexpr float kVoices[kFrames] = {0.f, 0.25f, 0.6f, 1.f, 0.55f, 0.f};
        for (int i = 0; i < kFrames; ++i) {
            const float t = i * 0.7f;
            const float v = kVoices[i];
            DumpOrbFrame(opt.orb_preview + std::to_string(i) + ".bmp",
                         std::max(v, 0.13f), v, t);
        }
        std::printf("wrote %d frames to %s0..%d.bmp\n", kFrames,
                    opt.orb_preview.c_str(), kFrames - 1);
        return 0;
    }
    if (stop)   return StopDaemon(opt);
    if (status) return DaemonStatus(opt);
    if (serve)  return RunDaemon(opt);
    if (opt.text.empty()) { Usage(); return 2; }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::fprintf(stderr, "speak: COM init failed\n");
        return 1;
    }

    // Fast path: a warm daemon already holds the model in memory.
    std::unique_ptr<PcmSource> source;
    std::string daemon_err;
    if (opt.use_daemon) {
        source = DaemonSource::Post(opt.port, opt.text, opt.voice, 300, &daemon_err);
    }

    // Cold path: synthesize here, and warm a daemon for next time.
    const bool used_daemon = source != nullptr;
    std::unique_ptr<pocket_tts::PocketTTS> engine;
    if (!source) {
        if (opt.use_daemon && opt.auto_serve && !DaemonRegistered(opt.port)) {
            SpawnDaemon(opt);
        }
        try {
            engine = std::make_unique<pocket_tts::PocketTTS>(BuildConfig(opt));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "speak: could not load models from %s (%s)\n",
                         opt.models_dir.c_str(), e.what());
            CoUninitialize();
            return 1;
        }
        source = std::make_unique<EngineSource>(engine.get(), opt.text, opt.voice);
    }

    std::thread orb;
    if (opt.show_orb) orb = std::thread(OrbThread);

    std::vector<float> recorded;
    double first_audio_ms = 0;
    const bool ok = PlayStream(source.get(), opt.save_path.empty() ? nullptr : &recorded,
                               &first_audio_ms);

    std::string engine_error;
    if (auto* es = dynamic_cast<EngineSource*>(source.get())) engine_error = es->error();
    source.reset();          // closes the socket / joins the generator thread
    if (ok && !opt.save_path.empty()) WriteWav(opt.save_path, recorded);

    g_audio_done.store(true);
    if (orb.joinable()) orb.join();

    engine.reset();
    CoUninitialize();

    if (!ok && !engine_error.empty()) {
        std::fprintf(stderr, "speak: synthesis failed: %s\n", engine_error.c_str());
    }
    if (opt.timing) {
        std::fprintf(stderr, "speak: first audio in %.0f ms (%s)\n", first_audio_ms,
                     used_daemon ? "daemon" : "local");
    }
    if (!ok && !daemon_err.empty() && opt.use_daemon) {
        std::fprintf(stderr, "speak: daemon path failed (%s)\n", daemon_err.c_str());
    }
    return ok ? 0 : 1;
}
