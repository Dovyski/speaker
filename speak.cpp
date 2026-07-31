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
#include <windowsx.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <dwmapi.h>
#include <tlhelp32.h>

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
#include <unordered_map>
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
// Set by clicking the orb, read by the audio writer: playback holds where it is
// until the next click. The orb is the only way in, so there is nothing to do
// here when running with --no-orb.
std::atomic<bool>     g_paused{false};
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
constexpr Rgb kSteel {0.60f, 0.72f, 0.88f};     // cold, dimmed: the paused ring

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

// True for points inside the ring, which is the only part that reacts to a click.
// The overlay is a square window but mostly empty, so everything outside this
// radius is reported as transparent and the click goes to whatever is underneath.
bool InsideOrb(HWND hwnd, POINT screen_pt) {
    POINT p = screen_pt;
    ScreenToClient(hwnd, &p);
    const float c  = g_orb_size * 0.5f;
    const float dx = p.x + 0.5f - c, dy = p.y + 0.5f - c;
    // A shade wider than the rim (0.29 S) so the target is comfortable to hit.
    const float hit = g_orb_size * 0.36f;
    return dx * dx + dy * dy <= hit * hit;
}

LRESULT CALLBACK OrbWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        // Everything but the ring itself is click-through.
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            return InsideOrb(hwnd, pt) ? HTCLIENT : HTTRANSPARENT;
        }

        case WM_SETCURSOR:
            // IDC_HAND is an ANSI-typed macro without UNICODE defined.
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;

        // Click to pause, click again to resume.
        case WM_LBUTTONDOWN:
            g_paused.store(!g_paused.load());
            return 0;
    }
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
//
// `pause` (0..1) is the third drive, and it reads as the orb holding its breath:
// the ring contracts a little, cools from ember to a dim steel blue, and — since
// the caller stops advancing `time` — comes to a near standstill.
void ComposeAurora(uint32_t* pixels, int S, float level, float voice, float fade,
                   float time, float pause) {
    g_geom.Ensure(S);

    const float R0         = S * 0.29f * (1.f + 0.09f * level) * (1.f - 0.055f * pause);
    // Grows superlinearly with the voice: barely rippling when quiet, properly
    // turbulent when loud, and exactly 0 — a true circle — in silence.
    const float wobble     = (0.095f + 0.19f * voice) * voice * R0;
    const float churn      = time * (1.f + 1.1f * voice);   // faster when loud
    const float spin       = time * 0.55f;           // the outline orbits
    const float rotation   = time * 0.33f;           // the colours drift round
    const float rim_sigma  = (1.7f + 1.0f * level) * (1.f - 0.20f * pause);
    const float glow_sigma = (8.5f + 7.0f * level) * (1.f - 0.28f * pause);
    const float gain       = (0.72f + 0.45f * level) * (1.f - 0.24f * pause);

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
        colour[a] = Mix(RimColour(std::fmod((th - rotation) / (2 * kPi) + 2.f, 1.f)),
                        kSteel, 0.62f * pause);
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

// Source-over composite of one premultiplied pixel onto another.
inline uint32_t BlendOver(uint32_t src, uint32_t dst) {
    const uint32_t sa = src >> 24;
    if (sa >= 254) return src;
    const auto ch = [&](int shift) {
        const uint32_t v = ((src >> shift) & 0xFF) +
                           (((dst >> shift) & 0xFF) * (255 - sa) + 127) / 255;
        return std::min(v, 255u);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Two soft luminous bars at the centre — the one glyph everyone reads as "held".
// Drawn over whatever the style composed, so both styles pause alike.
void OverlayPauseGlyph(uint32_t* pixels, int S, float pause, float fade) {
    if (pause <= 0.01f) return;
    const float cx = S * 0.5f, cy = S * 0.5f;
    const float hh   = S * 0.075f;    // half height of a bar
    const float hw   = S * 0.017f;    // half width
    const float gap  = S * 0.026f;    // half gap between the two bars
    const float soft = std::max(1.f, S * 0.007f);
    const Rgb   tint = Mix(kHot, kCyan, 0.22f);
    // Eases in as it appears, so the bars grow out of the middle.
    const float grow = SmoothStep(0.f, 1.f, pause);

    const int y0 = std::max(0, static_cast<int>(cy - hh - soft - 1));
    const int y1 = std::min(S, static_cast<int>(cy + hh + soft + 2));
    for (int y = y0; y < y1; ++y) {
        const float dy = std::fabs(y + 0.5f - cy);
        const float ay = 1.f - SmoothStep(hh * grow - soft, hh * grow + soft, dy);
        if (ay <= 0.f) continue;
        for (int x = 0; x < S; ++x) {
            const float dx = std::fabs(x + 0.5f - cx);
            const float ax = 1.f - SmoothStep(hw - soft, hw + soft,
                                              std::fabs(dx - (gap + hw)));
            const float a = ax * ay * pause * fade;
            if (a <= 0.004f) continue;
            const size_t i = static_cast<size_t>(y) * S + x;
            pixels[i] = BlendOver(Pack(tint, a), pixels[i]);
        }
    }
}

void ComposeOrb(uint32_t* pixels, float level, float voice, float fade, float time,
                float pause) {
    if (g_orb_style == OrbStyle::Aurora) {
        ComposeAurora(pixels, g_orb_size, level, voice, fade, time, pause);
    } else {
        ComposeDot(pixels, g_orb_size, level * (1.f - 0.4f * pause), fade);
    }
    OverlayPauseGlyph(pixels, g_orb_size, pause, fade);
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

    // No WS_EX_TRANSPARENT: the ring has to receive clicks to be pausable. Clicks
    // outside it are handed on via WM_NCHITTEST, so the square stays click-through.
    // WS_EX_NOACTIVATE keeps the click from stealing focus from the user's window.
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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

    float voice = 0.f, level = 0.f, fade = 0.f, pause = 0.f;
    float anim = 0.f, breath_phase = 0.f;
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
        // Pause: eased, so the transition is a settling rather than a switch.
        pause += ((g_paused.load() ? 1.f : 0.f) - pause) * 0.16f;

        // Voice: the audio envelope alone, so silence really is silence and the
        // outline settles into a perfect circle. Decays a little slower than it
        // rises, otherwise the ring snaps flat between syllables. Forced to zero
        // while paused — the playback position is frozen, possibly mid-syllable,
        // so the envelope there would otherwise hold the outline distorted.
        const float voice_target = Clamp01(target * 1.6f) * (1.f - pause);
        voice += (voice_target - voice) * (voice_target > voice ? 0.35f : 0.12f);

        // Level: keep it alive between words with a slow breath, slower when held.
        breath_phase += 0.09f - 0.055f * pause;
        const float breath = 0.10f + 0.06f * std::sin(breath_phase);
        level += (std::max(voice, breath) - level) * 0.35f;

        // Animation clock: nearly stops while paused, so the ring coasts to a
        // standstill instead of freezing on the spot or spinning on regardless.
        anim += (1.f - 0.94f * pause) / 60.f;

        if (g_audio_done.load()) closing = true;
        fade += ((closing ? 0.f : 1.f) - fade) * (closing ? 0.12f : 0.22f);
        if (closing && fade < 0.01f) break;

        ComposeOrb(pixels, level, voice, fade, anim, pause);
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
    bool               source_done = false, started = false, chunk_is_tail = false;
    bool               paused = false;

    while (true) {
        // Paused (the orb was clicked): stop the device, which keeps whatever is
        // already in its buffer and its position, and stop pulling from the
        // source. Start() picks up exactly where it left off. Nothing else in the
        // loop runs, so the playback position the orb reads stays put too.
        const bool want_pause = g_paused.load();
        if (want_pause != paused) {
            if (want_pause) client->Stop(); else client->Start();
            paused = want_pause;
            std::fprintf(stderr, "speak: %s\n", paused ? "paused" : "resumed");
        }
        if (paused) { Sleep(16); continue; }

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
                source_done   = true;
                chunk_is_tail = true;
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
                    // Only real samples count as having spoken — the trailing
                    // silence must not make an empty synthesis look successful.
                    if (!started && !chunk_is_tail) {
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

// Writes a square of premultiplied BGRA pixels to a 32-bit BMP, flattened over
// a dark backdrop. Used to eyeball/regression-check the visuals without a screen
// recorder — the overlays themselves are invisible to GDI capture.
void WriteBmp(const std::string& path, std::vector<uint32_t> px, int S) {
    const uint32_t data_bytes = static_cast<uint32_t>(px.size() * 4);
    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize    = fh.bfOffBits + data_bytes;
    BITMAPINFOHEADER ih{};
    ih.biSize     = sizeof(ih);
    ih.biWidth    = S;
    ih.biHeight   = -S;  // top-down
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

void DumpOrbFrame(const std::string& path, float level, float voice, float time,
                  float pause = 0.f) {
    std::vector<uint32_t> px(static_cast<size_t>(g_orb_size) * g_orb_size);
    ComposeOrb(px.data(), level, voice, 1.0f, time, pause);
    WriteBmp(path, std::move(px), g_orb_size);
}

// ── Pointing at a window ────────────────────────────────────────────────────
// A second, non-interactive overlay: rings that expand out of a point and fade,
// a few times over, so an agent can show *which* window it was that just spoke.
// Nothing here touches the model or the audio device, so a pointing call costs
// no more than starting the process.

int g_point_size = 320;

constexpr float kRingLife  = 1.90f;   // seconds one ring takes to expand and die
constexpr float kRingGap   = 0.80f;   // seconds between successive rings

float PointerDuration(int pulses) {
    return (std::max(1, pulses) - 1) * kRingGap + kRingLife;
}

// One frame: `pulses` rings born kRingGap apart, each expanding out of the
// centre and thinning as it goes, over a hot core that marks the exact spot.
// Perfectly circular by design — this is a pointer, not a voice.
void ComposePointer(uint32_t* px, int S, float t, int pulses) {
    g_geom.Ensure(S);
    const size_t n = static_cast<size_t>(S) * S;
    std::memset(px, 0, n * 4);

    const float total = PointerDuration(pulses);
    // Fade in fast, and out over the last stretch so nothing snaps off-screen.
    const float fade = std::min(Clamp01(t / 0.08f), Clamp01((total - t) / 0.22f));
    if (fade <= 0.f) return;

    const float R_max = S * 0.46f;

    // Core: brightest as each ring is born, so the spot itself keeps blinking.
    float core = 0.f;
    for (int i = 0; i < pulses; ++i) {
        const float u = (t - i * kRingGap) / kRingLife;
        if (u < 0.f || u > 1.f) continue;
        core = std::max(core, (1.f - u) * (1.f - u));
    }
    if (core > 0.f) {
        const float r    = S * 0.030f * (1.f + 0.35f * core);
        const float halo = S * 0.105f;
        for (size_t i = 0; i < n; ++i) {
            const float d = g_geom.dist[i];
            if (d > halo) continue;
            const float dot = 1.f - SmoothStep(r - 1.2f, r + 1.2f, d);
            const float glow = d > r ? 0.42f * std::pow(1.f - (d - r) / (halo - r), 3.f) : 0.f;
            const float a = Clamp01(dot + (1.f - dot) * glow) * core * fade;
            if (a <= 0.004f) continue;
            px[i] = BlendOver(Pack(Mix(kHot, kEmber, 1.f - dot), a), px[i]);
        }
    }

    // Rings, oldest (widest) first so the newest one reads on top.
    for (int i = 0; i < pulses; ++i) {
        const float u = (t - i * kRingGap) / kRingLife;
        if (u < 0.f || u > 1.f) continue;

        // Ease-out: leaves the centre fast, then coasts outward.
        const float R     = R_max * (0.05f + 0.95f * (1.f - std::pow(1.f - u, 2.2f)));
        const float sigma = 2.1f + 2.4f * u;
        const float gain  = std::pow(1.f - u, 1.4f) * fade;
        const Rgb   col   = Mix(kHot, kEmber, SmoothStep(0.f, 0.85f, u));

        // A thin bright line riding a wider glow, so the ring still reads over a
        // busy window instead of disappearing into the text behind it.
        const float glow_sigma = 4.5f * sigma;
        for (size_t j = 0; j < n; ++j) {
            const float dr = std::fabs(g_geom.dist[j] - R);
            if (dr > 4.f * glow_sigma) continue;
            const float line = g_geom.Gauss(dr / sigma);
            const float a = Clamp01((line + 0.38f * g_geom.Gauss(dr / glow_sigma)) * gain);
            if (a <= 0.004f) continue;
            px[j] = BlendOver(Pack(Mix(col, kHot, 0.7f * line), a), px[j]);
        }
    }
}

// The overlay window itself. Fully click-through (unlike the orb, there is
// nothing here to click) and gone the moment the last ring dies.
void PointerOverlay(POINT centre, int size, int pulses) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClaudeSpeakPointer";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, size, size, nullptr, nullptr,
        wc.hInstance, nullptr);
    if (!hwnd) return;

    HDC screen = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem_dc, dib);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    const POINT pos{centre.x - size / 2, centre.y - size / 2};
    const float total = PointerDuration(pulses);
    LARGE_INTEGER qpf{}, t0{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const float t = float(now.QuadPart - t0.QuadPart) / float(qpf.QuadPart);
        if (t >= total) break;

        ComposePointer(static_cast<uint32_t*>(bits), size, t, pulses);
        SIZE  wnd_size{size, size};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, nullptr, const_cast<POINT*>(&pos), &wnd_size, mem_dc,
                            &src, 0, &blend, ULW_ALPHA);
        Sleep(16);
    }

    SelectObject(mem_dc, old);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen);
    DestroyWindow(hwnd);
}

// ── finding the window to point at ──────────────────────────────────────────

struct WindowTarget {
    HWND        hwnd = nullptr;
    RECT        rect{};
    std::string title;
    std::string process;
    DWORD       pid = 0;
};

// Window rectangles and overlay placement are only in the same coordinate space
// if this thread is per-monitor aware; otherwise Windows silently scales both for
// a system-DPI-aware process and the rings land off-target on a scaled monitor.
// Set per *thread*, so the orb (bottom-right of the primary work area) keeps the
// process-wide awareness it was written against.
void MakeThreadDpiAware() {
    using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto set = reinterpret_cast<Fn>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
}

bool WindowIsCloaked(HWND hwnd) {
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked))) &&
           cloaked != 0;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::unordered_map<DWORD, std::pair<DWORD, std::string>> ProcessTable() {
    std::unordered_map<DWORD, std::pair<DWORD, std::string>> table;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return table;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        table[e.th32ProcessID] = {e.th32ParentProcessID,
                                  LowerAscii(Utf8(e.szExeFile))};
    }
    CloseHandle(snap);
    return table;
}

// Every top-level window a human could point at: visible, titled, real size, not
// a DWM-cloaked ghost (background store apps leave those behind), and not one of
// ours. Optionally narrowed to one process.
std::vector<WindowTarget> EnumTargets(DWORD only_pid = 0) {
    struct Ctx {
        DWORD                     only_pid;
        std::vector<WindowTarget> out;
    } ctx{only_pid, {}};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto& ctx = *reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (ctx.only_pid && pid != ctx.only_pid) return TRUE;
        if (pid == GetCurrentProcessId()) return TRUE;

        wchar_t cls[64]{};
        GetClassNameW(hwnd, cls, 64);
        if (!std::wcscmp(cls, L"ClaudeSpeakOrb") || !std::wcscmp(cls, L"ClaudeSpeakPointer"))
            return TRUE;

        wchar_t title[512]{};
        if (GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;

        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return TRUE;
        if (r.right - r.left < 80 || r.bottom - r.top < 60) return TRUE;
        if (WindowIsCloaked(hwnd)) return TRUE;

        WindowTarget t;
        t.hwnd  = hwnd;
        t.rect  = r;
        t.title = Utf8(title);
        t.pid   = pid;
        ctx.out.push_back(std::move(t));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    const auto table = ProcessTable();
    for (auto& t : ctx.out) {
        const auto it = table.find(t.pid);
        if (it != table.end()) t.process = it->second.second;
    }
    return ctx.out;
}

std::string TargetsJson(const std::vector<WindowTarget>& targets) {
    std::string out = "[";
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        if (i) out += ",";
        out += "{\"hwnd\":" + std::to_string(reinterpret_cast<uintptr_t>(t.hwnd)) +
               ",\"pid\":" + std::to_string(t.pid) +
               ",\"process\":\"" + JsonEscape(t.process) + "\"" +
               ",\"title\":\"" + JsonEscape(t.title) + "\"" +
               ",\"x\":" + std::to_string(t.rect.left) +
               ",\"y\":" + std::to_string(t.rect.top) +
               ",\"w\":" + std::to_string(t.rect.right - t.rect.left) +
               ",\"h\":" + std::to_string(t.rect.bottom - t.rect.top) + "}";
    }
    return out + "]";
}

// Where the caller lives. A tool child of an agent gets its own conhost, so the
// console is no help — but the process tree still reaches the terminal that hosts
// the session, so walk up it and take the first ancestor that owns windows.
// Stops at the shell/service layer: pointing at Program Manager or a pile of
// File Explorer windows would be worse than admitting we do not know.
std::vector<WindowTarget> AncestorTargets(std::string* who) {
    const auto table = ProcessTable();
    DWORD pid = GetCurrentProcessId();
    for (int depth = 0; depth < 16; ++depth) {
        const auto it = table.find(pid);
        if (it == table.end()) break;
        pid = it->second.first;
        const auto parent = table.find(pid);
        if (!pid || parent == table.end()) break;

        const std::string& name = parent->second.second;
        if (name == "explorer.exe" || name == "services.exe" || name == "svchost.exe" ||
            name == "wininit.exe" || name == "winlogon.exe" || name == "system") {
            break;
        }
        auto found = EnumTargets(pid);
        if (!found.empty()) {
            if (who) *who = name + " (pid " + std::to_string(pid) + ")";
            return found;
        }
    }
    return {};
}

// What to point at, in the order a caller means it: explicit point, explicit
// window, title match, or "wherever I am".
struct PointRequest {
    bool        have_at = false;
    POINT       at{};
    HWND        hwnd    = nullptr;
    std::string title;
    int         pulses  = 3;
    int         size    = 0;      // 0 = g_point_size
};

// Resolves the request to a screen point. On failure fills `err` with something
// the caller can act on, and `candidates` with the JSON list to choose from.
bool ResolvePoint(const PointRequest& req, POINT* out, std::string* err,
                  std::string* candidates) {
    if (req.have_at) { *out = req.at; return true; }

    const auto centre = [](const WindowTarget& t) {
        return POINT{(t.rect.left + t.rect.right) / 2, (t.rect.top + t.rect.bottom) / 2};
    };

    if (req.hwnd) {
        if (!IsWindow(req.hwnd)) { *err = "no such window"; return false; }
        RECT r{};
        if (!GetWindowRect(req.hwnd, &r)) { *err = "window has no rectangle"; return false; }
        *out = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        return true;
    }

    if (!req.title.empty()) {
        const std::string needle = LowerAscii(req.title);
        std::vector<WindowTarget> hits;
        for (auto& t : EnumTargets()) {
            if (LowerAscii(t.title).find(needle) != std::string::npos) hits.push_back(t);
        }
        if (hits.empty()) { *err = "no window title contains '" + req.title + "'"; return false; }
        if (hits.size() > 1) {
            *err = "'" + req.title + "' matches " + std::to_string(hits.size()) + " windows";
            if (candidates) *candidates = TargetsJson(hits);
            return false;
        }
        *out = centre(hits[0]);
        return true;
    }

    // A classic console (conhost) has a real window of its own, and a process
    // attached to one can just ask. Under a ConPTY terminal — Windows Terminal,
    // VS Code, an agent's tool shell — this is a hidden pseudo-console instead, so
    // it fails the visibility test and we fall through to the process tree.
    if (HWND console = GetConsoleWindow()) {
        RECT r{};
        if (IsWindowVisible(console) && !WindowIsCloaked(console) &&
            GetWindowRect(console, &r) && r.right - r.left >= 80 && r.bottom - r.top >= 60) {
            *out = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            return true;
        }
    }

    std::string who;
    auto found = AncestorTargets(&who);
    if (found.empty()) {
        *err = "could not tell which window this call came from — pass --title, "
               "--hwnd or --at";
        return false;
    }
    if (found.size() > 1) {
        // The common case: one Windows Terminal process owning a window per
        // session. Nothing in the process tree says which one, so say so rather
        // than pointing at an arbitrary sibling.
        *err = who + " owns " + std::to_string(found.size()) +
               " windows — pass --title to say which";
        if (candidates) *candidates = TargetsJson(found);
        return false;
    }
    *out = centre(found[0]);
    return true;
}

// Runs a pointing request to completion. Must be called on a DPI-aware thread.
bool Point(const PointRequest& req, std::string* err, std::string* candidates) {
    POINT centre{};
    if (!ResolvePoint(req, &centre, err, candidates)) return false;
    PointerOverlay(centre, req.size > 0 ? req.size : g_point_size,
                   std::max(1, req.pulses));
    return true;
}

// ── the pointing endpoint ───────────────────────────────────────────────────
// Upstream's TTSServer has its routes hardcoded and lives in a file CMake
// downloads at a pinned SHA, so this is our own tiny listener next to it, on the
// following port. Loopback only: it moves things on the user's screen.
//
// A request must name its target (title, hwnd or x/y). Ancestry resolution is
// meaningless here — the caller is at the other end of a socket, and the daemon's
// own process tree says nothing about it.

std::atomic<bool> g_pointing{false};

// Numbers, to go with upstream's json_get_string. Enough for a flat object of
// integers, which is all this endpoint takes.
bool JsonGetNumber(const std::string& json, const std::string& key, double* out) {
    const std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] == '"') return false;
    char* end = nullptr;
    const double v = std::strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return false;
    *out = v;
    return true;
}

void PointHttpRespond(SOCKET fd, int status, const std::string& body) {
    const char* text = status == 200 ? "OK" : (status == 404 ? "Not Found"
                                            : (status == 409 ? "Conflict" : "Bad Request"));
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + body;
    SendAll(fd, resp);
}

void HandlePointRequest(SOCKET fd) {
    const pocket_tts::HttpRequest req = pocket_tts::HttpRequest::parse(fd);

    if (req.method == "GET" && (req.path == "/health" || req.path == "/")) {
        PointHttpRespond(fd, 200, "{\"ok\":true,\"service\":\"speak-pointer\"}");
        return;
    }
    if (req.method == "GET" && req.path == "/targets") {
        PointHttpRespond(fd, 200, TargetsJson(EnumTargets()));
        return;
    }
    if (req.method != "POST" || req.path != "/point") {
        PointHttpRespond(fd, 404,
                         "{\"ok\":false,\"error\":\"try GET /targets or POST /point\"}");
        return;
    }

    PointRequest pr;
    pr.title = pocket_tts::json_get_string(req.body, "title");
    double v = 0;
    if (JsonGetNumber(req.body, "hwnd", &v))
        pr.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(v));
    double x = 0, y = 0;
    if (JsonGetNumber(req.body, "x", &x) && JsonGetNumber(req.body, "y", &y)) {
        pr.have_at = true;
        pr.at = POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
    }
    if (JsonGetNumber(req.body, "pulses", &v)) pr.pulses = static_cast<int>(v);
    if (JsonGetNumber(req.body, "size", &v))   pr.size   = static_cast<int>(v);

    if (!pr.have_at && !pr.hwnd && pr.title.empty()) {
        PointHttpRespond(fd, 400,
            "{\"ok\":false,\"error\":\"name a target: title, hwnd, or x and y\"}");
        return;
    }
    // One overlay at a time: two animations on top of each other read as noise.
    bool expected = false;
    if (!g_pointing.compare_exchange_strong(expected, true)) {
        PointHttpRespond(fd, 409, "{\"ok\":false,\"error\":\"already pointing\"}");
        return;
    }

    std::string err, candidates;
    const bool ok = Point(pr, &err, &candidates);
    g_pointing.store(false);

    if (ok) {
        PointHttpRespond(fd, 200, "{\"ok\":true}");
    } else {
        std::string body = "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"";
        if (!candidates.empty()) body += ",\"candidates\":" + candidates;
        PointHttpRespond(fd, 400, body + "}");
    }
}

// Serves the endpoint until the process ends. Its own thread, so a long animation
// cannot stall speech and a busy engine cannot stall the animation.
void RunPointServer(int port) {
    MakeThreadDpiAware();
    if (!WinsockInit()) return;

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never off this machine
    addr.sin_port        = htons(static_cast<unsigned short>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        std::fprintf(stderr, "speak: pointing endpoint could not take port %d\n", port);
        closesocket(fd);
        return;
    }
    std::fprintf(stderr, "speak: pointing endpoint on http://127.0.0.1:%d/point\n", port);

    for (;;) {
        SOCKET client = accept(fd, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        HandlePointRequest(client);
        closesocket(client);
    }
    closesocket(fd);
}

// ── options ─────────────────────────────────────────────────────────────────

struct Options {
    std::string text;
    std::string voice       = "jarvis.wav";
    std::string save_path;
    std::string models_dir;
    std::string voices_dir;
    std::string dump_orb;       // render one orb frame to this BMP and exit
    std::string orb_preview;    // render a strip of orb frames to <prefix>N.bmp
    std::string point_preview;  // render a strip of pointer frames and exit
    bool   point        = false;   // point at a window (before or instead of speaking)
    bool   list_targets = false;
    bool   point_server = true;    // daemon: serve the pointing endpoint
    int    point_port   = 0;       // 0 = speech port + 1
    PointRequest point_req;
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

        // The pointing endpoint rides along on the next port. Separate listener,
        // separate thread, no model involved.
        if (opt.point_server) {
            const int point_port = opt.point_port ? opt.point_port : opt.port + 1;
            std::thread(RunPointServer, point_port).detach();
        }

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
    if (opt.point_port) cmd += " --point-port " + std::to_string(opt.point_port);
    if (!opt.point_server) cmd += " --no-point-server";

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
        "  speak --status | --stop       inspect or stop the daemon\n"
        "  speak --point [--title T]     ring out a window so a human can find it\n"
        "  speak --list-targets          the windows --title can match, as JSON\n\n"
        "  Click the orb to pause while speaking, click again to resume.\n\n"
        "  --voice <name|path>   voice sample (default: jarvis.wav)\n"
        "  --save <file.wav>     also save the audio\n"
        "  --no-orb              skip the on-screen indicator\n"
        "  --orb-style <s>       aurora (default) or dot\n"
        "  --orb-size <px>       orb square size (default 220)\n"
        "  --dump-orb <f.bmp>    render one orb frame to a BMP and exit\n"
        "  --orb-preview <pfx>   render a strip of orb frames and exit\n"
        "  --point               point at a window: expanding rings, then gone.\n"
        "                        Alone it only points; with text it speaks first.\n"
        "                        With no target given, the window of the calling\n"
        "                        session is used, and it is an error (exit 3) if\n"
        "                        that cannot be told apart from its siblings.\n"
        "  --title <substr>      point at the window whose title contains this\n"
        "  --hwnd <n>            point at this window handle\n"
        "  --at <x,y>            point at a screen position\n"
        "  --pulses <n>          rings to send out (default 3)\n"
        "  --point-size <px>     pointer square size (default 320)\n"
        "  --point-preview <pfx> render a strip of pointer frames and exit\n"
        "  --list-targets        list pointable windows as JSON and exit\n"
        "  --point-port <n>      daemon: pointing endpoint port (default port+1)\n"
        "  --no-point-server     daemon: do not serve the pointing endpoint\n"
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
        else if (a == "--point")       opt.point = true;
        else if (a == "--list-targets") opt.list_targets = true;
        else if (a == "--title")       { opt.point_req.title = next("--title"); opt.point = true; }
        else if (a == "--hwnd") {
            opt.point_req.hwnd = reinterpret_cast<HWND>(
                static_cast<uintptr_t>(std::strtoull(next("--hwnd").c_str(), nullptr, 0)));
            opt.point = true;
        }
        else if (a == "--at") {
            const std::string v = next("--at");
            const size_t comma = v.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "speak: --at wants x,y\n");
                return 2;
            }
            opt.point_req.have_at = true;
            opt.point_req.at = POINT{std::atol(v.c_str()), std::atol(v.c_str() + comma + 1)};
            opt.point = true;
        }
        else if (a == "--pulses")      opt.point_req.pulses = std::max(1, std::atoi(next("--pulses").c_str()));
        else if (a == "--point-size")  g_point_size = std::max(80, std::atoi(next("--point-size").c_str()));
        else if (a == "--point-preview") opt.point_preview = next("--point-preview");
        else if (a == "--point-port")  opt.point_port = std::atoi(next("--point-port").c_str());
        else if (a == "--no-point-server") opt.point_server = false;
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
        // Frame 0 is silence, so the "perfect circle when idle" case is visible;
        // the last two are the paused state easing in and fully held.
        constexpr int kFrames = 8;
        constexpr float kVoices[kFrames] = {0.f, 0.25f, 0.6f, 1.f, 0.55f, 0.f, 0.f, 0.f};
        constexpr float kPauses[kFrames] = {0.f, 0.f,   0.f,  0.f, 0.f,   0.f, 0.5f, 1.f};
        for (int i = 0; i < kFrames; ++i) {
            const float t = i * 0.7f;
            const float v = kVoices[i];
            DumpOrbFrame(opt.orb_preview + std::to_string(i) + ".bmp",
                         std::max(v, 0.13f), v, t, kPauses[i]);
        }
        std::printf("wrote %d frames to %s0..%d.bmp\n", kFrames,
                    opt.orb_preview.c_str(), kFrames - 1);
        return 0;
    }
    if (!opt.point_preview.empty()) {
        // A strip across the life of the animation: the core alone, then the first
        // ring leaving, then two rings in flight, then the tail.
        constexpr int kFrames = 8;
        const int  S = g_point_size;
        const int  pulses = std::max(1, opt.point_req.pulses);
        const float total = PointerDuration(pulses);
        for (int i = 0; i < kFrames; ++i) {
            std::vector<uint32_t> px(static_cast<size_t>(S) * S);
            ComposePointer(px.data(), S, total * (i + 0.5f) / kFrames, pulses);
            WriteBmp(opt.point_preview + std::to_string(i) + ".bmp", std::move(px), S);
        }
        std::printf("wrote %d frames to %s0..%d.bmp\n", kFrames,
                    opt.point_preview.c_str(), kFrames - 1);
        return 0;
    }
    if (opt.list_targets) {
        MakeThreadDpiAware();
        std::printf("%s\n", TargetsJson(EnumTargets()).c_str());
        return 0;
    }
    if (stop)   return StopDaemon(opt);
    if (status) return DaemonStatus(opt);
    if (serve)  return RunDaemon(opt);

    // Pointing on its own: no model, no audio device, nothing to wait for.
    if (opt.point && opt.text.empty()) {
        MakeThreadDpiAware();
        std::string err, candidates;
        if (Point(opt.point_req, &err, &candidates)) return 0;
        std::fprintf(stderr, "speak: %s\n", err.c_str());
        if (!candidates.empty()) std::printf("%s\n", candidates.c_str());
        return 3;
    }
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

    if (!ok) {
        if (!engine_error.empty()) {
            std::fprintf(stderr, "speak: synthesis failed: %s\n", engine_error.c_str());
        } else {
            std::fprintf(stderr, "speak: no audio was produced (voice '%s')\n",
                         opt.voice.c_str());
        }
    }
    if (opt.timing) {
        std::fprintf(stderr, "speak: first audio in %.0f ms (%s)\n", first_audio_ms,
                     used_daemon ? "daemon" : "local");
    }
    if (!ok && !daemon_err.empty() && opt.use_daemon) {
        std::fprintf(stderr, "speak: daemon path failed (%s)\n", daemon_err.c_str());
    }

    // Spoke and pointing too: point afterwards, so the orb has finished and the
    // rings are the only thing moving. A failure to point is reported but does not
    // turn a successful utterance into a failed call.
    if (opt.point) {
        MakeThreadDpiAware();
        std::string err, candidates;
        if (!Point(opt.point_req, &err, &candidates)) {
            std::fprintf(stderr, "speak: spoke, but could not point (%s)\n", err.c_str());
            if (!candidates.empty()) std::printf("%s\n", candidates.c_str());
        }
    }
    return ok ? 0 : 1;
}
