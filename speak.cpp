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
#include <ctime>
#include <deque>
#include <functional>
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
// Set by clicking the caption's ×, read by the renderer. The frame keeps its size
// — the window's bitmap is allocated once — so a dismissed card simply stops
// being drawn, leaving the orb where it was.
std::atomic<bool>     g_cap_closed{false};
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

// ── text encoding ───────────────────────────────────────────────────────────
// Arguments arrive as UTF-16 and are carried around as UTF-8; the caption and the
// window APIs need them back as UTF-16.

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

int CaptionStripWidth();
// Defined with the socket helpers, called from the overlay loop below: one
// datagram per frame telling a daemon's panel how loud this utterance is.
void BeaconSend(float level, bool active);
bool InsideCaptionClose(HWND hwnd, POINT screen_pt);

// True for points inside the ring, which is the only part that reacts to a click.
// The overlay is a square window (wider with a caption) but mostly empty, so
// everything outside this radius is reported as transparent and the click goes to
// whatever is underneath.
bool InsideOrb(HWND hwnd, POINT screen_pt) {
    POINT p = screen_pt;
    ScreenToClient(hwnd, &p);
    const float c  = g_orb_size * 0.5f;
    const float dx = p.x + 0.5f - c - CaptionStripWidth(), dy = p.y + 0.5f - c;
    // A shade wider than the rim (0.29 S) so the target is comfortable to hit.
    const float hit = g_orb_size * 0.36f;
    return dx * dx + dy * dy <= hit * hit;
}

LRESULT CALLBACK OrbWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        // Everything but the ring and the caption's × is click-through.
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            return (InsideOrb(hwnd, pt) || InsideCaptionClose(hwnd, pt)) ? HTCLIENT
                                                                        : HTTRANSPARENT;
        }

        case WM_SETCURSOR:
            // IDC_HAND is an ANSI-typed macro without UNICODE defined.
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;

        // The × dismisses the caption; anywhere else that reaches us is the ring,
        // where a click pauses and another resumes.
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            if (InsideCaptionClose(hwnd, pt)) g_cap_closed.store(true);
            else                              g_paused.store(!g_paused.load());
            return 0;
        }
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

// ── Caption ─────────────────────────────────────────────────────────────────
// A toast to the left of the orb: an icon, a title and one short line of
// context, so the orb says *what* it is about and not merely that something
// spoke. It is a flat Bootstrap-style card — solid variant colour, rounded
// corners, a soft drop shadow, ink chosen light or dark for contrast — rather
// than a second glowing object competing with the ring. The card is built once
// (the text cannot change mid-utterance) and blended into every frame with the
// orb's own fade, so the two arrive and leave together.

std::string g_cap_title, g_cap_text;

constexpr int kCapMaxWidth   = 360;  // panel width ceiling, at the default orb size
constexpr int kCapGap        = 16;   // between the panel edge and the orb square
constexpr int kCapPad        = 16;
constexpr int kCapRadius     = 10;
constexpr int kCapShadow     = 20;   // room around the panel for its drop shadow
constexpr int kCapLineGap    = 3;    // between title and body
constexpr int kCapTitlePx    = 16;
constexpr int kCapBodyPx     = 14;
constexpr int kCapTitleLines = 2;
constexpr int kCapBodyLines  = 3;
constexpr int kCapIcon       = 26;   // icon disc diameter
constexpr int kCapIconGap    = 13;   // icon to text column
constexpr int kCapClose      = 11;   // the ×, corner to corner
constexpr int kCapCloseGap   = 14;   // text column to the ×
constexpr int kCapStroke     = 2;    // icon and × line weight

// Not fully opaque: a hair of translucency sits the card on the desktop instead
// of on top of it, without costing any legibility.
constexpr float kCapFillAlpha  = 0.97f;
constexpr float kCapShadowA    = 0.34f;
constexpr float kCapBodyFade   = 0.26f;   // body ink mixed back towards the fill
constexpr float kCapCloseA     = 0.62f;
constexpr Rgb   kCapDarkInk  {0.09f, 0.11f, 0.14f};
constexpr Rgb   kCapLightInk {1.00f, 1.00f, 1.00f};

enum class CapIcon { None, Check, Info, Warn, Ban, Dot };

// The Bootstrap defaults, in Bootstrap's own colours. `dark_ink` is the contrast
// choice their own utilities make for each background.
struct CapVariant {
    const char* name;
    Rgb         bg;
    bool        dark_ink;
    CapIcon     icon;
};

constexpr CapVariant kCapVariants[] = {
    {"primary",   {0.05f, 0.43f, 0.99f}, false, CapIcon::Info},   // #0d6efd
    {"secondary", {0.42f, 0.46f, 0.49f}, false, CapIcon::Info},   // #6c757d
    {"success",   {0.10f, 0.53f, 0.33f}, false, CapIcon::Check},  // #198754
    {"danger",    {0.86f, 0.21f, 0.27f}, false, CapIcon::Ban},    // #dc3545
    {"warning",   {1.00f, 0.76f, 0.03f}, true,  CapIcon::Warn},   // #ffc107
    {"info",      {0.05f, 0.79f, 0.94f}, true,  CapIcon::Info},   // #0dcaf0
    {"light",     {0.97f, 0.98f, 0.98f}, true,  CapIcon::Info},   // #f8f9fa
    {"dark",      {0.13f, 0.15f, 0.16f}, false, CapIcon::Info},   // #212529
};

// Neutral by default: `light` is the one to live with all day, where a green or
// red card would claim a meaning the caller never asked for. Note that on a dark
// desktop a near-white card is the *loudest* of the eight — which is the point
// for a notification, and why `dark` is there for when it should recede.
const CapVariant* g_cap_variant = &kCapVariants[6];
// -1 keeps the variant's own icon; anything else overrides it.
int g_cap_icon = -1;
// Whole-card alpha, 0..1: the fill, the shadow, the text, all of it. 1 is a solid
// toast; lower lets the desktop through and sits it further back.
float g_cap_opacity = 1.f;

const CapVariant* FindCapVariant(const std::string& name) {
    for (const CapVariant& v : kCapVariants) {
        if (name == v.name) return &v;
    }
    return nullptr;
}

bool ParseCapIcon(const std::string& name, CapIcon* out) {
    if (name == "none")       *out = CapIcon::None;
    else if (name == "check") *out = CapIcon::Check;
    else if (name == "info")  *out = CapIcon::Info;
    else if (name == "warn")  *out = CapIcon::Warn;
    else if (name == "ban")   *out = CapIcon::Ban;
    else if (name == "dot")   *out = CapIcon::Dot;
    else return false;
    return true;
}

CapIcon CaptionIcon() {
    return g_cap_icon < 0 ? g_cap_variant->icon : static_cast<CapIcon>(g_cap_icon);
}

Rgb CapInk()  { return g_cap_variant->dark_ink ? kCapDarkInk : kCapLightInk; }
// The muted second line: the same ink pulled back towards the card, which keeps
// the pair tonal instead of introducing a third grey.
Rgb CapBodyInk() { return Mix(CapInk(), g_cap_variant->bg, kCapBodyFade); }
// Barely there on a coloured card, but it is what gives the near-white ones an
// edge to end on.
Rgb CapBorder() {
    return Mix(g_cap_variant->bg, g_cap_variant->dark_ink ? kCapDarkInk : kCapLightInk,
               0.13f);
}

bool HaveCaption() { return !g_cap_title.empty() || !g_cap_text.empty(); }

// Everything on the card is sized off the orb, so --orb-size scales the pair.
int CapScale(int v) {
    return std::max(1, static_cast<int>(std::lround(v * g_orb_size / 220.0)));
}

// Coverage mask: alpha per pixel, no colour of its own. GDI cannot draw into an
// alpha channel, so the text is drawn white on black and its luminance becomes
// the alpha.
struct TextMask {
    int                  w = 0, h = 0;
    std::vector<uint8_t> a;
};

// `extra` adds DrawText format bits — DT_RIGHT for the subtitle, nothing for the
// caption, which is what keeps the two callers on one rasterizer.
TextMask RenderText(const std::wstring& text, int height_px, bool bold, int max_w,
                    int max_lines, UINT extra = 0) {
    TextMask mask;
    if (text.empty() || max_w <= 0) return mask;

    HDC screen = GetDC(nullptr);
    HDC dc     = CreateCompatibleDC(screen);
    // ANTIALIASED_QUALITY rather than the ClearType default: subpixel antialiasing
    // would leave colour fringes once luminance is reinterpreted as alpha.
    HFONT font = CreateFontW(-height_px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);

    const UINT kFlags = (DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX) | extra;
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    RECT measure{0, 0, max_w, 0};
    DrawTextW(dc, text.c_str(), -1, &measure, kFlags | DT_CALCRECT);
    // Anything past max_lines is simply cut: a caption is a toast, not a paragraph.
    const int w = std::max(1, std::min<int>(measure.right, max_w));
    const int h = std::max(1, std::min<int>(measure.bottom, max_lines * tm.tmHeight));

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void*   bits = nullptr;
    HBITMAP dib  = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old_bmp = SelectObject(dc, dib);
        std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{0, 0, w, h};
        DrawTextW(dc, text.c_str(), -1, &r, kFlags);
        GdiFlush();

        mask.w = w;
        mask.h = h;
        mask.a.resize(static_cast<size_t>(w) * h);
        const auto* px = static_cast<const uint32_t*>(bits);
        for (size_t i = 0; i < mask.a.size(); ++i) {
            mask.a[i] = static_cast<uint8_t>((px[i] >> 8) & 0xFF);   // grey: any channel
        }
        SelectObject(dc, old_bmp);
        DeleteObject(dib);
    }

    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return mask;
}

// Geometry only, rasterized once; the colours are applied per frame so the card
// can fade with the orb. Everything drawn on it — text, icon, × — is a coverage
// mask over the same card-sized grid, which makes compositing one pass.
struct CaptionCard {
    int                  w = 0, h = 0;   // panel plus the shadow margin around it
    std::vector<float>   dist;           // signed distance to the panel edge, <0 inside
    std::vector<uint8_t> title_ink, body_ink, icon_ink, close_ink;
    RECT                 close_hit{};    // the ×'s click target, in card coordinates
};

CaptionCard g_card;
int g_cap_panel_w = 0;   // the card itself, without the margin

// Width the caption adds to the left of the orb square, 0 when there is none. The
// right-hand shadow margin is allowed to overlap the orb square, so the gap is
// measured from the card edge.
int CaptionStripWidth() {
    return g_card.w ? g_cap_panel_w + CapScale(kCapShadow) + CapScale(kCapGap) : 0;
}

void StampMask(std::vector<uint8_t>* layer, int layer_w, int layer_h,
               const TextMask& m, int x0, int y0) {
    for (int y = 0; y < m.h; ++y) {
        const int cy = y0 + y;
        if (cy < 0 || cy >= layer_h) continue;
        for (int x = 0; x < m.w; ++x) {
            const int cx = x0 + x;
            if (cx < 0 || cx >= layer_w) continue;
            (*layer)[static_cast<size_t>(cy) * layer_w + cx] =
                m.a[static_cast<size_t>(y) * m.w + x];
        }
    }
}

// Distance from a point to a line segment: every stroke of every glyph below is
// one of these, so the icons are resolution-independent and antialias for free.
float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0.f ? Clamp01((wx * vx + wy * vy) / len2) : 0.f;
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

// Rasterizes a shape given as a signed-distance function over a box, into a
// coverage mask. `sdf` is called in box-local pixel coordinates.
template <class Sdf>
void StampSdf(std::vector<uint8_t>* layer, int layer_w, int layer_h, int x0, int y0,
              int bw, int bh, Sdf sdf) {
    for (int y = -1; y <= bh; ++y) {
        const int cy = y0 + y;
        if (cy < 0 || cy >= layer_h) continue;
        for (int x = -1; x <= bw; ++x) {
            const int cx = x0 + x;
            if (cx < 0 || cx >= layer_w) continue;
            const float a = 1.f - SmoothStep(-0.6f, 0.6f, sdf(x + 0.5f, y + 0.5f));
            if (a <= 0.002f) continue;
            uint8_t& dst = (*layer)[static_cast<size_t>(cy) * layer_w + cx];
            dst = std::max(dst, static_cast<uint8_t>(a * 255.f + 0.5f));
        }
    }
}

// A ringed glyph, the shape every variant's icon shares: an outlined circle with
// one or two strokes inside it, drawn to fill a box of `d` pixels.
void StampCapIcon(CaptionCard* card, CapIcon icon, int x0, int y0, int d) {
    if (icon == CapIcon::None || d <= 0) return;
    const float w = std::max(1.f, static_cast<float>(CapScale(kCapStroke)));
    const float c = d * 0.5f, r = c - w * 0.5f - 0.5f;
    const float s = static_cast<float>(d);
    StampSdf(&card->icon_ink, card->w, card->h, x0, y0, d, d,
             [=](float px, float py) {
        const float ring = std::fabs(std::sqrt((px - c) * (px - c) + (py - c) * (py - c)) - r)
                           - w * 0.5f;
        float inner = 1e9f;
        switch (icon) {
            case CapIcon::Check:
                inner = std::min(SegDist(px, py, 0.28f * s, 0.52f * s, 0.44f * s, 0.68f * s),
                                 SegDist(px, py, 0.44f * s, 0.68f * s, 0.74f * s, 0.34f * s))
                        - w * 0.5f;
                break;
            case CapIcon::Info:
                inner = std::min(SegDist(px, py, c, 0.45f * s, c, 0.72f * s) - w * 0.5f,
                                 SegDist(px, py, c, 0.30f * s, c, 0.30f * s) - w * 0.62f);
                break;
            case CapIcon::Warn:
                inner = std::min(SegDist(px, py, c, 0.27f * s, c, 0.56f * s) - w * 0.5f,
                                 SegDist(px, py, c, 0.71f * s, c, 0.71f * s) - w * 0.62f);
                break;
            case CapIcon::Ban: {
                const float k = r * 0.66f * 0.7071f;
                inner = SegDist(px, py, c - k, c - k, c + k, c + k) - w * 0.5f;
                break;
            }
            case CapIcon::Dot:
                inner = SegDist(px, py, c, c, c, c) - d * 0.17f;
                break;
            default:
                break;
        }
        return std::min(ring, inner);
    });
}

void BuildCaptionCard() {
    g_card = {};
    g_cap_panel_w = 0;
    if (!HaveCaption()) return;

    const int pad       = CapScale(kCapPad);
    const int icon      = CaptionIcon() == CapIcon::None ? 0 : CapScale(kCapIcon);
    const int icon_gap  = icon ? CapScale(kCapIconGap) : 0;
    const int close     = CapScale(kCapClose);
    const int close_gap = CapScale(kCapCloseGap);
    const int max_inner = CapScale(kCapMaxWidth) - 2 * pad - icon - icon_gap -
                          close_gap - close;
    const TextMask title = RenderText(Wide(g_cap_title), CapScale(kCapTitlePx), true,
                                      max_inner, kCapTitleLines);
    const TextMask body  = RenderText(Wide(g_cap_text), CapScale(kCapBodyPx), false,
                                      max_inner, kCapBodyLines);
    const int gap     = (title.h && body.h) ? CapScale(kCapLineGap) : 0;
    const int inner_w = std::max(title.w, body.w);
    const int inner_h = title.h + gap + body.h;
    if (inner_w <= 0 || inner_h <= 0) return;

    const int margin  = CapScale(kCapShadow);
    const int panel_w = pad + icon + icon_gap + inner_w + close_gap + close + pad;
    const int panel_h = std::max(inner_h, icon) + 2 * pad;
    g_cap_panel_w = panel_w;
    g_card.w = panel_w + 2 * margin;
    g_card.h = panel_h + 2 * margin;
    const size_t n = static_cast<size_t>(g_card.w) * g_card.h;
    g_card.dist.resize(n);
    g_card.title_ink.assign(n, 0);
    g_card.body_ink.assign(n, 0);
    g_card.icon_ink.assign(n, 0);
    g_card.close_ink.assign(n, 0);

    // Signed distance to a rounded rectangle: negative inside. One expression then
    // gives the antialiased silhouette, the hairline on its edge and the shadow
    // cast below it — exactly how the ring is drawn from its own radius.
    const float radius = static_cast<float>(CapScale(kCapRadius));
    const float hw = panel_w * 0.5f, hh = panel_h * 0.5f;
    const float ccx = g_card.w * 0.5f, ccy = g_card.h * 0.5f;
    for (int y = 0; y < g_card.h; ++y) {
        for (int x = 0; x < g_card.w; ++x) {
            const float dx = x + 0.5f - ccx, dy = y + 0.5f - ccy;
            const float qx = std::max(std::fabs(dx) - (hw - radius), 0.f);
            const float qy = std::max(std::fabs(dy) - (hh - radius), 0.f);
            g_card.dist[static_cast<size_t>(y) * g_card.w + x] =
                std::sqrt(qx * qx + qy * qy) - radius;
        }
    }

    const int text_x = margin + pad + icon + icon_gap;
    const int text_y = margin + (panel_h - inner_h) / 2;
    StampMask(&g_card.title_ink, g_card.w, g_card.h, title, text_x, text_y);
    StampMask(&g_card.body_ink, g_card.w, g_card.h, body, text_x, text_y + title.h + gap);
    StampCapIcon(&g_card, CaptionIcon(), margin + pad, margin + (panel_h - icon) / 2, icon);

    // The × rides on the title's own centre line rather than the card's, which is
    // what keeps it looking hung off the first line when the body wraps.
    const float cw    = std::max(1.f, CapScale(kCapStroke) * 0.9f);
    const int   close_x = margin + panel_w - pad - close;
    const int   close_y = text_y + (title.h ? title.h : inner_h) / 2 - close / 2;
    StampSdf(&g_card.close_ink, g_card.w, g_card.h, close_x, close_y, close, close,
             [=](float px, float py) {
        const float e = static_cast<float>(close) - 0.5f;
        return std::min(SegDist(px, py, 0.5f, 0.5f, e, e),
                        SegDist(px, py, e, 0.5f, 0.5f, e)) - cw * 0.5f;
    });
    // Generous around the mark itself: this is a small target on a card the user
    // is not looking at when they reach for it.
    const int slop = CapScale(9);
    g_card.close_hit = RECT{close_x - slop, close_y - slop,
                            close_x + close + slop, close_y + close + slop};
}

// ── Subtitle ────────────────────────────────────────────────────────────────
// The same strip of screen as the caption toast, but bare: no card, no icon, no
// title, no ×. Just the line itself, white with a dark contour so it stays
// readable over whatever the desktop happens to be showing underneath. Where the
// toast is a notification, this is a caption in the film sense — the words that
// go with the voice — so it is deliberately the quieter of the two.
//
// Built once, like the card, and composited with the same `fade`: it arrives and
// leaves with the orb.

std::string g_sub_text;

constexpr int   kSubPx      = 19;   // a touch above the toast's body line
constexpr int   kSubLines   = 3;
constexpr int   kSubHalo    = 2;    // contour radius, in pixels at the default size
constexpr int   kSubDrop    = 2;    // how far the soft shadow falls
constexpr float kSubHaloA   = 0.78f;
constexpr float kSubDropA   = 0.42f;
constexpr int   kSubStackGap = 4;   // between the toast and the subtitle below it
constexpr Rgb   kSubInk   {1.00f, 1.00f, 1.00f};
constexpr Rgb   kSubShade {0.02f, 0.03f, 0.05f};

bool HaveSubtitle() { return !g_sub_text.empty(); }

// Geometry-only again: `ink` is the glyph coverage, `halo` the dilated silhouette
// that becomes the contour, `drop` the same silhouette pushed downwards.
struct SubtitleBlock {
    int                  w = 0, h = 0;
    std::vector<uint8_t> ink, halo, drop;
};

SubtitleBlock g_sub;

void BuildSubtitle() {
    g_sub = {};
    if (!HaveSubtitle()) return;

    const int halo = std::max(1, CapScale(kSubHalo));
    const int drop = std::max(1, CapScale(kSubDrop));
    const int pad  = halo + drop;   // room for the contour and the shadow
    const TextMask text = RenderText(Wide(g_sub_text), CapScale(kSubPx), true,
                                     CapScale(kCapMaxWidth) - 2 * pad, kSubLines,
                                     DT_RIGHT);
    if (text.w <= 0 || text.h <= 0) return;

    g_sub.w = text.w + 2 * pad;
    g_sub.h = text.h + 2 * pad;
    const size_t n = static_cast<size_t>(g_sub.w) * g_sub.h;
    g_sub.ink.assign(n, 0);
    g_sub.halo.assign(n, 0);
    g_sub.drop.assign(n, 0);
    StampMask(&g_sub.ink, g_sub.w, g_sub.h, text, pad, pad);

    // Contour: the maximum coverage inside a small disc around each pixel. A
    // dilation rather than a blur, so thin strokes keep a solid edge instead of
    // dissolving into grey.
    const int   r  = halo;
    const float r2 = static_cast<float>(r * r) + 0.25f;
    for (int y = 0; y < g_sub.h; ++y) {
        for (int x = 0; x < g_sub.w; ++x) {
            uint8_t best = 0;
            for (int dy = -r; dy <= r && best < 255; ++dy) {
                const int sy = y + dy;
                if (sy < 0 || sy >= g_sub.h) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy > r2) continue;
                    const int sx = x + dx;
                    if (sx < 0 || sx >= g_sub.w) continue;
                    best = std::max(best, g_sub.ink[static_cast<size_t>(sy) * g_sub.w + sx]);
                    if (best == 255) break;
                }
            }
            g_sub.halo[static_cast<size_t>(y) * g_sub.w + x] = best;
        }
    }

    // Shadow: the contour again, a couple of rows down. Cheap, and it is what
    // lifts the line off a busy background.
    for (int y = g_sub.h - 1; y >= drop; --y) {
        std::memcpy(&g_sub.drop[static_cast<size_t>(y) * g_sub.w],
                    &g_sub.halo[static_cast<size_t>(y - drop) * g_sub.w], g_sub.w);
    }
}

// Width the subtitle claims to the left of the orb square, gap included.
int SubtitleStripWidth() { return g_sub.w ? g_sub.w + CapScale(kCapGap) : 0; }

int FrameWidth()  {
    return std::max(CaptionStripWidth(), SubtitleStripWidth()) + g_orb_size;
}
int FrameHeight() { return g_orb_size; }

// Dims a premultiplied pixel — all four channels scale together.
inline uint32_t ScaleAlpha(uint32_t p, float f) {
    const auto ch = [&](int shift) {
        return static_cast<uint32_t>(((p >> shift) & 0xFF) * f + 0.5f);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Where the card and the subtitle sit in the frame. Both hang off the orb: they
// are right-aligned against it and the stack as a whole is centred on it, so with
// only one of the two present this is exactly the old placement. With both, the
// toast keeps the top and the subtitle sits under it.
int OverlayStackHeight() {
    const int gap = (g_card.h && g_sub.h) ? CapScale(kSubStackGap) : 0;
    return g_card.h + gap + g_sub.h;
}

int OverlayStackTop(int H) { return std::max(0, (H - OverlayStackHeight()) / 2); }

int CaptionOriginY(int H) { return OverlayStackTop(H); }

int SubtitleOriginY(int H) {
    return OverlayStackTop(H) +
           (g_card.h ? g_card.h + CapScale(kSubStackGap) : 0);
}

// The strip is as wide as the wider of the two, so each block is pushed right
// until it touches the gap in front of the orb.
int CaptionOriginX(int W) {
    return W - g_orb_size - CapScale(kCapGap) - CapScale(kCapShadow) - g_cap_panel_w;
}

int SubtitleOriginX(int W) { return W - g_orb_size - CapScale(kCapGap) - g_sub.w; }

// The one clickable thing on the card. Once it has been used the card is gone, so
// the region stops claiming clicks and the desktop underneath gets them back.
bool InsideCaptionClose(HWND hwnd, POINT screen_pt) {
    // A card at zero opacity is not drawn, so it must not swallow clicks either.
    if (!g_card.w || g_cap_closed.load() || g_cap_opacity <= 0.004f) return false;
    POINT p = screen_pt;
    ScreenToClient(hwnd, &p);
    p.x -= CaptionOriginX(FrameWidth());
    p.y -= CaptionOriginY(FrameHeight());
    const RECT& r = g_card.close_hit;
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// Draws the caption into the frame: drop shadow, solid fill, hairline edge, icon,
// text, ×. Deliberately static — a toast that pulsed along with the ring would
// read as two things throbbing at each other. Only `fade` is shared, so the pair
// still arrives and leaves as one object.
void OverlayCaption(uint32_t* frame, int W, int H, float fade) {
    fade *= g_cap_opacity;
    if (!g_card.w || fade <= 0.004f || g_cap_closed.load()) return;

    const Rgb   fill   = g_cap_variant->bg;
    const Rgb   border = CapBorder();
    const Rgb   ink    = CapInk();
    const Rgb   body   = CapBodyInk();
    constexpr Rgb kShadowInk {0.01f, 0.02f, 0.04f};
    const float sigma  = std::max(1.f, CapScale(kCapShadow) * 0.52f);
    const int   drop   = CapScale(5);       // the shadow falls below the card
    const int   y0     = CaptionOriginY(H);
    const int   x0     = CaptionOriginX(W);

    for (int y = 0; y < g_card.h; ++y) {
        const int fy = y0 + y;
        if (fy < 0 || fy >= H) continue;
        for (int x = 0; x < g_card.w; ++x) {
            const int fx = x0 + x;
            if (fx < 0 || fx >= W) continue;
            const size_t ci = static_cast<size_t>(y) * g_card.w + x;
            const float  d  = g_card.dist[ci];
            const float  inside = 1.f - SmoothStep(-0.7f, 0.7f, d);

            // The same silhouette, sampled a few rows up, is the shape of the
            // shadow: one distance field, two uses.
            float shadow = 0.f;
            const int sy = y - drop;
            if (sy >= 0) {
                const float sd = std::max(g_card.dist[static_cast<size_t>(sy) * g_card.w + x],
                                          0.f);
                shadow = kCapShadowA * std::exp(-(sd / sigma) * (sd / sigma));
            }
            if (inside <= 0.004f && shadow <= 0.004f) continue;

            uint32_t p = 0;
            if (shadow > 0.004f) p = Pack(kShadowInk, Clamp01(shadow));
            if (inside > 0.004f) {
                p = BlendOver(Pack(fill, inside * kCapFillAlpha), p);
                // A band a pixel or two wide just inside the edge. Invisible on the
                // saturated variants; on `light` it is the only thing separating the
                // card from a pale desktop behind it.
                const float edge = inside * (1.f - SmoothStep(-1.8f, -0.3f, d));
                if (edge > 0.004f) p = BlendOver(Pack(border, edge * kCapFillAlpha), p);
            }
            if (const uint8_t a = g_card.icon_ink[ci])  p = BlendOver(Pack(ink, a / 255.f), p);
            if (const uint8_t a = g_card.title_ink[ci]) p = BlendOver(Pack(ink, a / 255.f), p);
            if (const uint8_t a = g_card.body_ink[ci])  p = BlendOver(Pack(body, a / 255.f), p);
            if (const uint8_t a = g_card.close_ink[ci])
                p = BlendOver(Pack(ink, a / 255.f * kCapCloseA), p);

            const size_t i = static_cast<size_t>(fy) * W + fx;
            frame[i] = BlendOver(ScaleAlpha(p, fade), frame[i]);
        }
    }
}

// The subtitle: shadow, contour, then the words. Three coverage masks over the
// same grid, so this is one pass with no card underneath it.
void OverlaySubtitle(uint32_t* frame, int W, int H, float fade) {
    if (!g_sub.w || fade <= 0.004f) return;

    const int y0 = SubtitleOriginY(H);
    const int x0 = SubtitleOriginX(W);

    for (int y = 0; y < g_sub.h; ++y) {
        const int fy = y0 + y;
        if (fy < 0 || fy >= H) continue;
        for (int x = 0; x < g_sub.w; ++x) {
            const int fx = x0 + x;
            if (fx < 0 || fx >= W) continue;
            const size_t si = static_cast<size_t>(y) * g_sub.w + x;
            const uint8_t ink = g_sub.ink[si];
            const uint8_t halo = g_sub.halo[si];
            const uint8_t shade = g_sub.drop[si];
            if (!ink && !halo && !shade) continue;

            uint32_t p = 0;
            if (shade) p = Pack(kSubShade, shade / 255.f * kSubDropA);
            if (halo)  p = BlendOver(Pack(kSubShade, halo / 255.f * kSubHaloA), p);
            if (ink)   p = BlendOver(Pack(kSubInk, ink / 255.f), p);

            const size_t i = static_cast<size_t>(fy) * W + fx;
            frame[i] = BlendOver(ScaleAlpha(p, fade), frame[i]);
        }
    }
}

// The whole overlay: the orb on the right, the caption card centred against it on
// the left. Without a caption this is exactly the orb, at exactly its old size.
void ComposeFrame(uint32_t* frame, float level, float voice, float fade, float time,
                  float pause) {
    const int S = g_orb_size, W = FrameWidth();
    if (W == S) {
        ComposeOrb(frame, level, voice, fade, time, pause);
        return;
    }

    static std::vector<uint32_t> orb;
    orb.resize(static_cast<size_t>(S) * S);
    ComposeOrb(orb.data(), level, voice, fade, time, pause);

    std::memset(frame, 0, static_cast<size_t>(W) * S * 4);
    for (int y = 0; y < S; ++y) {
        std::memcpy(frame + static_cast<size_t>(y) * W + (W - S),
                    orb.data() + static_cast<size_t>(y) * S,
                    static_cast<size_t>(S) * 4);
    }

    OverlayCaption(frame, W, S, fade);
    OverlaySubtitle(frame, W, S, fade);
}

// Pushes the composed frame to the layered window, parked just inside the
// bottom-right corner of the work area (above the taskbar). The orb keeps that
// corner whether or not there is a caption — the card grows leftwards.
void PushOrb(HWND hwnd, HDC mem_dc) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int W = FrameWidth(), H = FrameHeight();
    POINT pos{work.right - W - 24, work.bottom - H - 24};
    SIZE  size{W, H};
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
    const int W = FrameWidth(), H = FrameHeight();
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, W, H, nullptr,
        nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;

    HDC screen = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;   // top-down
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

        ComposeFrame(pixels, level, voice, fade, anim, pause);
        PushOrb(hwnd, mem_dc);
        // The panel in the target window glows off the same envelope the ring
        // does, so the two move together rather than merely coinciding.
        if ((frame & 1) == 0) BeaconSend(voice, !closing);
        Sleep(16);
    }

    // Say so, rather than leaving the daemon to notice the datagrams stopped:
    // the gap is the fallback for a client that died, not the normal ending.
    for (int i = 0; i < 3; ++i) BeaconSend(0.f, false);

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
// ── the speaking beacon ─────────────────────────────────────────────────────
// When speech is aimed at a window (`--title`, `--hwnd`), the panel parked in
// that window's corner should say so — glowing with the voice, and showing the
// caption the utterance carries. The awkward part is *where the two live*: the
// panel belongs to the resident daemon, while the audio, the amplitude envelope
// and the caption all belong to the one-shot client process that is playing.
//
// So the client broadcasts. One fixed-size datagram per frame to loopback, each
// carrying the whole state — target, level, caption — so the protocol has no
// setup, no teardown and no session: a gap in the datagrams *is* the end of the
// utterance, which also means a client killed mid-sentence cannot leave a panel
// glowing forever. Upstream's TTSServer has its routes hardcoded in a file CMake
// downloads at a pinned SHA, so a field on POST /tts was never an option; and a
// 60 Hz stream of tiny HTTP requests through the pointing listener would have
// blocked /panel behind it.

constexpr uint32_t kBeaconMagic   = 0x424B5053;   // "SPKB"
constexpr uint32_t kBeaconVersion = 1;

#pragma pack(push, 1)
struct SpeakBeacon {
    uint32_t magic   = kBeaconMagic;
    uint32_t version = kBeaconVersion;
    uint64_t hwnd    = 0;      // the window the speech is aimed at
    float    level   = 0.f;    // live amplitude, 0..1 — the orb's own value
    uint32_t active  = 0;      // 0 while fading out, so the end is explicit too
    char     caption_title[128]{};
    char     caption[192]{};
};
#pragma pack(pop)

// Set by the client once its target is resolved; 0 means "say nothing".
std::atomic<uint64_t> g_beacon_hwnd{0};
int                   g_beacon_port = 0;

void BeaconCopy(char* dst, size_t n, const std::string& src) {
    const size_t len = std::min(n - 1, src.size());
    std::memcpy(dst, src.data(), len);
    dst[len] = 0;
}

// One unconnected UDP socket for the life of the process. Nothing listening is
// not an error: the panel is optional scenery.
void BeaconSend(float level, bool active) {
    const uint64_t target = g_beacon_hwnd.load();
    if (!target || g_beacon_port <= 0) return;

    static SOCKET fd = INVALID_SOCKET;
    if (fd == INVALID_SOCKET) {
        if (!WinsockInit()) return;
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd == INVALID_SOCKET) return;
    }
    SpeakBeacon msg;
    msg.hwnd   = target;
    msg.level  = level;
    msg.active = active ? 1u : 0u;
    BeaconCopy(msg.caption_title, sizeof(msg.caption_title), g_cap_title);
    BeaconCopy(msg.caption, sizeof(msg.caption), g_cap_text);

    sockaddr_in to{};
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port        = htons(static_cast<unsigned short>(g_beacon_port));
    sendto(fd, reinterpret_cast<const char*>(&msg), sizeof(msg), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

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

// Writes a block of premultiplied BGRA pixels to a 32-bit BMP, flattened over
// a dark backdrop. Used to eyeball/regression-check the visuals without a screen
// recorder — the overlays themselves are invisible to GDI capture.
void WriteBmp(const std::string& path, std::vector<uint32_t> px, int w, int h) {
    const uint32_t data_bytes = static_cast<uint32_t>(px.size() * 4);
    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize    = fh.bfOffBits + data_bytes;
    BITMAPINFOHEADER ih{};
    ih.biSize     = sizeof(ih);
    ih.biWidth    = w;
    ih.biHeight   = -h;  // top-down
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
    const int W = FrameWidth(), H = FrameHeight();
    std::vector<uint32_t> px(static_cast<size_t>(W) * H);
    ComposeFrame(px.data(), level, voice, 1.0f, time, pause);
    WriteBmp(path, std::move(px), W, H);
}

// ── Pointing at a window ────────────────────────────────────────────────────
// A second, non-interactive overlay: rings that expand out of a point and fade,
// a few times over, so an agent can show *which* window it was that just spoke.
// Nothing here touches the model or the audio device, so a pointing call costs
// no more than starting the process.

int g_point_size = 320;

// How long a ring lives, and how long after it the next one is born. Tunable at
// runtime (--point-duration) so the gesture can be paced without a rebuild; the
// two keep their ratio, since that is what makes the rings read as a sequence
// rather than as one thick pulse.
constexpr float kRingLife = 1.90f;
constexpr float kRingGap  = 0.80f;

float g_ring_life = kRingLife;
float g_ring_gap  = kRingGap;

// The one colour the pointer is built from: rings are born white-hot and settle
// into it as they expand, and the core dot's halo takes it too. Ember by default,
// so pointing looks like the rest of the app — set it to say something else
// ("red" for a problem, say) without touching the code.
Rgb g_point_colour = kEmber;

// #rrggbb, rrggbb, r,g,b in 0..255, or one of a few names.
bool ParseColour(const std::string& text, Rgb* out) {
    std::string s;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (s.empty()) return false;

    static const struct { const char* name; Rgb rgb; } kNames[] = {
        {"ember",   kEmber},                   {"red",     {1.00f, 0.14f, 0.10f}},
        {"orange",  {1.00f, 0.48f, 0.05f}},    {"amber",   {1.00f, 0.72f, 0.12f}},
        {"yellow",  {1.00f, 0.90f, 0.20f}},    {"green",   {0.24f, 0.90f, 0.36f}},
        {"cyan",    kCyan},                    {"azure",   kAzure},
        {"blue",    {0.20f, 0.40f, 1.00f}},    {"violet",  {0.62f, 0.36f, 1.00f}},
        {"magenta", {1.00f, 0.24f, 0.72f}},    {"pink",    {1.00f, 0.45f, 0.70f}},
        {"white",   kHot},                     {"steel",   kSteel},
    };
    for (const auto& n : kNames) {
        if (s == n.name) { *out = n.rgb; return true; }
    }

    if (s.find(',') != std::string::npos) {
        int v[3]{};
        if (std::sscanf(s.c_str(), "%d,%d,%d", &v[0], &v[1], &v[2]) != 3) return false;
        *out = {Clamp01(v[0] / 255.f), Clamp01(v[1] / 255.f), Clamp01(v[2] / 255.f)};
        return true;
    }

    if (s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 || s.find_first_not_of("0123456789abcdef") != std::string::npos) {
        return false;
    }
    const auto byte = [&](size_t i) {
        return static_cast<float>(std::stoi(s.substr(i, 2), nullptr, 16)) / 255.f;
    };
    *out = {byte(0), byte(2), byte(4)};
    return true;
}

float PointerDuration(int pulses) {
    return (std::max(1, pulses) - 1) * g_ring_gap + g_ring_life;
}

// Stretches or compresses the animation to `total` seconds for this many pulses.
void SetPointerDuration(float total, int pulses) {
    if (total <= 0.f) return;
    g_ring_life = kRingLife;
    g_ring_gap  = kRingGap;
    const float scale = std::min(20.f, std::max(0.05f, total / PointerDuration(pulses)));
    g_ring_life *= scale;
    g_ring_gap  *= scale;
}

// One frame: `pulses` rings born g_ring_gap apart, each expanding out of the
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
        const float u = (t - i * g_ring_gap) / g_ring_life;
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
            px[i] = BlendOver(Pack(Mix(kHot, g_point_colour, 1.f - dot), a), px[i]);
        }
    }

    // Rings, oldest (widest) first so the newest one reads on top.
    for (int i = 0; i < pulses; ++i) {
        const float u = (t - i * g_ring_gap) / g_ring_life;
        if (u < 0.f || u > 1.f) continue;

        // Ease-out: leaves the centre fast, then coasts outward.
        const float R     = R_max * (0.05f + 0.95f * (1.f - std::pow(1.f - u, 2.2f)));
        const float sigma = 2.1f + 2.4f * u;
        const float gain  = std::pow(1.f - u, 1.4f) * fade;
        // Takes the colour early — a ring that only tints once it is nearly gone
        // reads as white whatever it was asked to be.
        const Rgb   col   = Mix(kHot, g_point_colour, SmoothStep(0.f, 0.30f, u));

        // A thin bright line riding a wider glow, so the ring still reads over a
        // busy window instead of disappearing into the text behind it.
        const float glow_sigma = 4.5f * sigma;
        for (size_t j = 0; j < n; ++j) {
            const float dr = std::fabs(g_geom.dist[j] - R);
            if (dr > 4.f * glow_sigma) continue;
            const float line = g_geom.Gauss(dr / sigma);
            const float a = Clamp01((line + 0.38f * g_geom.Gauss(dr / glow_sigma)) * gain);
            if (a <= 0.004f) continue;
            px[j] = BlendOver(Pack(Mix(col, kHot, 0.40f * line), a), px[j]);
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

// The image name for one pid, cached. Enumerating windows is cheap; taking a
// TH32CS_SNAPPROCESS snapshot of the entire machine to find out who owns them is
// not, and the panel's resolver does this twice a second for as long as the
// daemon lives. A pid's image name cannot change, so the only reason to expire
// the entry at all is pid reuse after a process dies — thirty seconds is plenty
// of margin for a mapping whose worst failure is mislabelling a window.
std::string ProcessNameFor(DWORD pid) {
    struct Entry { std::string name; DWORD at; };
    static std::mutex                          mtx;
    static std::unordered_map<DWORD, Entry>    cache;
    const DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(pid);
        if (it != cache.end() && now - it->second.at < 30000) return it->second.name;
    }
    std::string name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH]{};
        DWORD   n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            std::string full = Utf8(buf);
            const size_t slash = full.find_last_of("\\/");
            name = LowerAscii(slash == std::string::npos ? full : full.substr(slash + 1));
        }
        CloseHandle(h);
    }
    {
        std::lock_guard<std::mutex> lock(mtx);
        cache[pid] = Entry{name, now};
    }
    return name;
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

    for (auto& t : ctx.out) t.process = ProcessNameFor(t.pid);
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
    float       duration = 0.f;   // 0 = the built-in pacing
    bool        have_colour = false;
    Rgb         colour{};         // only read when have_colour
};

// Applies a request's look — pacing and colour — and hands back the ring count.
// Both the live overlay and --point-preview go through here, so a previewed frame
// is the frame that would be drawn. Colour is reset rather than left behind: the
// daemon serves many callers, and each gets the default unless it asks otherwise.
int ApplyPointStyle(const PointRequest& req) {
    const int pulses = std::max(1, req.pulses);
    SetPointerDuration(req.duration, pulses);
    g_point_colour = req.have_colour ? req.colour : kEmber;
    return pulses;
}

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
// The *window* a pointing request names, for the caller that wants the handle
// rather than the centre of it. Same rule pointing uses — a unique
// contains-match, case-insensitive — and the same refusal to guess: an ambiguous
// title gives nothing rather than an arbitrary sibling.
HWND ResolveTargetHwnd(const PointRequest& req) {
    if (req.hwnd) return IsWindow(req.hwnd) ? req.hwnd : nullptr;
    if (req.title.empty()) return nullptr;
    const std::string needle = LowerAscii(req.title);
    HWND              found  = nullptr;
    for (const WindowTarget& t : EnumTargets()) {
        if (LowerAscii(t.title).find(needle) == std::string::npos) continue;
        if (found) return nullptr;
        found = t.hwnd;
    }
    return found;
}

bool Point(const PointRequest& req, std::string* err, std::string* candidates) {
    POINT centre{};
    if (!ResolvePoint(req, &centre, err, candidates)) return false;
    const int pulses = ApplyPointStyle(req);
    PointerOverlay(centre, req.size > 0 ? req.size : g_point_size, pulses);
    return true;
}

// ── The attention panel ─────────────────────────────────────────────────────
// A small card parked inside the bottom-right corner of *one* terminal window,
// listing what the session running in it is working on: pull requests, issues,
// working directories. Where the orb says "something spoke" and the pointer says
// "over there", this says "and here is what it is about" — and it stays, because
// the answer to "what was I doing in this window" is wanted long after the voice
// has finished.
//
// It is the caption toast's material — the same rounded card, fonts, light fill
// and dark ink — so the two read as one product. Three things make it a
// different object all the same: it is bound to a window rather than to the
// screen, it lives until told otherwise rather than for one utterance, and it is
// *interactive*: rows highlight under the pointer and open in the browser or in
// Explorer when clicked. So, unlike the pointer, it does not set
// WS_EX_TRANSPARENT — and unlike the orb, several can be alive at once, one per
// terminal.

// ── a JSON value, for the one endpoint that takes nested data ───────────────
// Upstream's json_get_string only reaches top-level strings, and /panel takes an
// array of objects. A small recursive-descent parser rather than more regex.

struct JsonVal {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool                                         b   = false;
    double                                       num = 0;
    std::string                                  str;
    std::vector<JsonVal>                         arr;
    std::vector<std::pair<std::string, JsonVal>> obj;

    const JsonVal* Find(const std::string& key) const {
        if (t != T::Obj) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    std::string GetStr(const std::string& key) const {
        const JsonVal* v = Find(key);
        return (v && v->t == T::Str) ? v->str : std::string();
    }
    // Numbers arrive as numbers from a sane producer and as strings from a shell
    // one-liner, so take either.
    long GetNum(const std::string& key, long fallback = 0) const {
        const JsonVal* v = Find(key);
        if (!v) return fallback;
        if (v->t == T::Num) return static_cast<long>(v->num);
        if (v->t == T::Str) return std::atol(v->str.c_str());
        return fallback;
    }
};

void Utf8Append(std::string* out, unsigned cp) {
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct JsonParser {
    const std::string& s;
    size_t             i = 0;

    explicit JsonParser(const std::string& text) : s(text) {}

    void Space() {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }
    bool Lit(const char* word) {
        const size_t n = std::strlen(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    bool Hex4(unsigned* out) {
        if (i + 4 > s.size()) return false;
        unsigned cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s[i + k];
            cp <<= 4;
            if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
        }
        i += 4;
        *out = cp;
        return true;
    }
    bool String(std::string* out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        for (;;) {
            if (i >= s.size()) return false;
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { *out += c; continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"':  *out += '"';  break;
                case '\\': *out += '\\'; break;
                case '/':  *out += '/';  break;
                case 'b':  *out += '\b'; break;
                case 'f':  *out += '\f'; break;
                case 'n':  *out += '\n'; break;
                case 'r':  *out += '\r'; break;
                case 't':  *out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(&cp)) return false;
                    // Surrogate pair: the producer writes JSON from a UTF-16 world.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
                        s[i] == '\\' && s[i + 1] == 'u') {
                        const size_t save = i;
                        i += 2;
                        unsigned lo = 0;
                        if (Hex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i = save;
                        }
                    }
                    Utf8Append(out, cp);
                    break;
                }
                default: return false;
            }
        }
    }
    bool Value(JsonVal* out, int depth = 0) {
        if (depth > 32) return false;   // a malformed body must not eat the stack
        Space();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '"') {
            out->t = JsonVal::T::Str;
            return String(&out->str);
        }
        if (c == '{') {
            ++i;
            out->t = JsonVal::T::Obj;
            Space();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                Space();
                std::string key;
                if (!String(&key)) return false;
                Space();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                JsonVal v;
                if (!Value(&v, depth + 1)) return false;
                out->obj.emplace_back(std::move(key), std::move(v));
                Space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            out->t = JsonVal::T::Arr;
            Space();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                JsonVal v;
                if (!Value(&v, depth + 1)) return false;
                out->arr.push_back(std::move(v));
                Space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == 't' || c == 'f') {
            out->t = JsonVal::T::Bool;
            if (Lit("true"))  { out->b = true;  return true; }
            if (Lit("false")) { out->b = false; return true; }
            return false;
        }
        if (c == 'n') {
            out->t = JsonVal::T::Null;
            return Lit("null");
        }
        char* end = nullptr;
        const double v = std::strtod(s.c_str() + i, &end);
        if (!end || end == s.c_str() + i) return false;
        i = static_cast<size_t>(end - s.c_str());
        out->t   = JsonVal::T::Num;
        out->num = v;
        return true;
    }
};

bool JsonParse(const std::string& text, JsonVal* out) {
    JsonParser p(text);
    if (!p.Value(out)) return false;
    p.Space();
    return true;
}

// ── PNG out, for --panel-preview ────────────────────────────────────────────
// The BMPs the orb and pointer previews write are fine for a strip of frames
// committed to a repo, but a panel preview is a single image a human or an agent
// opens to look at, so: PNG. No zlib here and no wish to link one — a zlib
// stream made entirely of *stored* deflate blocks is legal, so the whole encoder
// is a CRC, an Adler and some framing. The files are a few hundred KB, which for
// a preview nobody ships is a fair trade for zero dependencies.

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool     ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PngBe32(std::string* out, uint32_t v) {
    out->push_back(static_cast<char>((v >> 24) & 0xFF));
    out->push_back(static_cast<char>((v >> 16) & 0xFF));
    out->push_back(static_cast<char>((v >> 8) & 0xFF));
    out->push_back(static_cast<char>(v & 0xFF));
}

void PngChunk(std::string* out, const char* tag, const std::string& body) {
    PngBe32(out, static_cast<uint32_t>(body.size()));
    std::string payload(tag, 4);
    payload += body;
    *out += payload;
    PngBe32(out, Crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
}

// Premultiplied BGRA in, 24-bit PNG out, flattened over the same #22272E the BMP
// dumps use — the panel lives on a dark terminal, so that is roughly what it
// looks like in place.
void WritePng(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    std::string raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);   // filter: none
        for (int x = 0; x < w; ++x) {
            const uint32_t p = px[static_cast<size_t>(y) * w + x];
            const float    a = ((p >> 24) & 0xFF) / 255.f;
            const auto     ch = [&](int shift, int bg) {
                return static_cast<uint8_t>(std::min(255.f,
                           ((p >> shift) & 0xFF) + (1.f - a) * bg));
            };
            raw.push_back(static_cast<char>(ch(16, 0x22)));   // R
            raw.push_back(static_cast<char>(ch(8, 0x27)));    // G
            raw.push_back(static_cast<char>(ch(0, 0x2E)));    // B
        }
    }

    std::string z;
    z.push_back(0x78);   // zlib: deflate, 32K window
    z.push_back(0x01);   // no preset dictionary
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0;;) {
        const uint16_t n    = static_cast<uint16_t>(std::min<size_t>(65535, raw.size() - off));
        const bool     last = static_cast<size_t>(off) + n >= raw.size();
        z.push_back(static_cast<char>(last ? 1 : 0));   // stored block, final or not
        z.push_back(static_cast<char>(n & 0xFF));
        z.push_back(static_cast<char>((n >> 8) & 0xFF));
        z.push_back(static_cast<char>(~n & 0xFF));
        z.push_back(static_cast<char>((~n >> 8) & 0xFF));
        z.append(raw, off, n);
        off += n;
        if (last) break;
    }
    PngBe32(&z, (b << 16) | a);

    std::string ihdr;
    PngBe32(&ihdr, static_cast<uint32_t>(w));
    PngBe32(&ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(2);   // colour type: truecolour
    ihdr.push_back(0);   // deflate
    ihdr.push_back(0);   // adaptive filtering
    ihdr.push_back(0);   // no interlace

    std::string out("\x89PNG\r\n\x1a\n", 8);
    PngChunk(&out, "IHDR", ihdr);
    PngChunk(&out, "IDAT", z);
    PngChunk(&out, "IEND", std::string());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "speak: cannot write %s\n", path.c_str()); return; }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
}

// ── what a panel is made of ─────────────────────────────────────────────────

// The state a row reports, in the order that decides which one a collapsed pill
// shows: the pill carries the *worst* of them, so a red mark on a one-line pill
// is enough to know that window wants attention.
enum class PanStatus {
    Unknown = 0, Closed, Merged, Draft, Open, Approved, ChangesRequested, ChecksFailing
};

const char* PanStatusName(PanStatus s) {
    static const char* kNames[] = {"unknown", "closed",   "merged",
                                   "draft",   "open",     "approved",
                                   "changes_requested",   "checks_failing"};
    return kNames[static_cast<int>(s)];
}

PanStatus ParsePanStatus(const std::string& name) {
    for (int i = 0; i <= static_cast<int>(PanStatus::ChecksFailing); ++i) {
        if (name == PanStatusName(static_cast<PanStatus>(i))) {
            return static_cast<PanStatus>(i);
        }
    }
    return PanStatus::Unknown;
}

struct PanelItem {
    std::string kind;      // "pr", "issue", "path"; anything else is drawn like a path
    std::string repo;      // "owner/name" — only the name is shown
    std::string url;       // the thing a click opens
    std::string title;
    long        number = 0;
    PanStatus   status  = PanStatus::Unknown;

    // "repo#1375", or the last segment of a path: the short handle a human uses
    // for the thing, which is what a row leads with.
    std::string Label() const {
        if (number > 0) {
            std::string name = repo;
            const size_t slash = name.rfind('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            return (name.empty() ? "" : name) + "#" + std::to_string(number);
        }
        std::string tail = url;
        while (!tail.empty() && (tail.back() == '\\' || tail.back() == '/')) tail.pop_back();
        const size_t sep = tail.find_last_of("\\/");
        if (sep != std::string::npos) tail = tail.substr(sep + 1);
        return tail.empty() ? title : tail;
    }
};

// ── Octicons ────────────────────────────────────────────────────────────────
// A coloured dot said "this needs attention" but not *what the thing is*, and a
// row that leads with `repo#1375` is otherwise indistinguishable from a
// directory. So each row is marked with GitHub's own icon for its type — the
// same glyph the row's page shows — tinted with GitHub's own status colour. Two
// pieces of information in the space one dot took.
//
// The icons are the 16×16 Octicons (github.com/primer/octicons, MIT), verbatim
// path data, rasterized here. No font and no image: the path is parsed, its
// curves and arcs are flattened to polygons, and a nonzero-winding scanline fill
// with four subsample rows per pixel and analytic horizontal coverage turns it
// into an alpha mask. That is what keeps the hole in `issue-opened` a hole at
// every DPI, where a bitmap would smear at 1.5× and a font would need shipping.

struct PanPt { float x, y; };
using PanPolys = std::vector<std::vector<PanPt>>;

// SVG path number/flag reader. Path data omits separators before a sign or a
// decimal point ("0-3", ".5.5"), which strtof happens to handle exactly right.
struct SvgReader {
    const char* p;

    void Space() {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    }
    bool Num(float* out) {
        Space();
        char*       end = nullptr;
        const float v   = std::strtof(p, &end);
        if (!end || end == p) return false;
        p    = end;
        *out = v;
        return true;
    }
    // Arc flags may be written without a separator ("1 1", "11"), and only ever
    // appear where a single 0 or 1 is the whole value.
    bool Flag(bool* out) {
        Space();
        if (*p == '0' || *p == '1') {
            *out = (*p == '1');
            ++p;
            return true;
        }
        float v = 0;
        if (!Num(&v)) return false;
        *out = v != 0.f;
        return true;
    }
};

void PanCubic(std::vector<PanPt>* sub, PanPt p0, PanPt p1, PanPt p2, PanPt p3, int steps) {
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps, u = 1.f - t;
        const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        sub->push_back(PanPt{a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                             a * p0.y + b * p1.y + c * p2.y + d * p3.y});
    }
}

// Endpoint parameterization to centre parameterization, straight out of the SVG
// spec's appendix, then sampled. Octicons draw every dot and every ring as an
// arc — including the degenerate `a.75.75 0 1 0 0 .005` trick for a full circle,
// which this handles because the large-arc flag makes the sweep ~360°.
void PanArc(std::vector<PanPt>* sub, PanPt from, float rx, float ry, float phi_deg,
            bool large, bool sweep, PanPt to) {
    if (rx == 0.f || ry == 0.f) {
        sub->push_back(to);
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const float phi = phi_deg * 3.14159265f / 180.f;
    const float cp = std::cos(phi), sp = std::sin(phi);
    const float dx = (from.x - to.x) * 0.5f, dy = (from.y - to.y) * 0.5f;
    const float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;

    const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.f) {
        const float s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    const float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    const float co  = (den > 0.f ? std::sqrt(std::max(0.f, num / den)) : 0.f) *
                     ((large != sweep) ? 1.f : -1.f);
    const float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    const float cx = cp * cxp - sp * cyp + (from.x + to.x) * 0.5f;
    const float cy = sp * cxp + cp * cyp + (from.y + to.y) * 0.5f;

    const float t1 = std::atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
    const float t2 = std::atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx);
    float       dt = t2 - t1;
    if (!sweep && dt > 0.f)  dt -= 2.f * 3.14159265f;
    if (sweep && dt < 0.f)   dt += 2.f * 3.14159265f;

    const int steps = std::max(4, static_cast<int>(std::ceil(std::fabs(dt) / 0.18f)));
    for (int i = 1; i <= steps; ++i) {
        const float t = t1 + dt * i / steps;
        sub->push_back(PanPt{cx + rx * std::cos(t) * cp - ry * std::sin(t) * sp,
                             cy + rx * std::cos(t) * sp + ry * std::sin(t) * cp});
    }
}

// The subset of the path grammar Octicons actually use: M L H V C A Z, absolute
// and relative. Anything else is a data error rather than a silent gap.
bool PanSvgPath(const char* d, float scale, PanPolys* out) {
    SvgReader r{d};
    PanPt     cur{0, 0}, start{0, 0};
    char      cmd = 0;
    r.Space();
    while (*r.p) {
        if (std::isalpha(static_cast<unsigned char>(*r.p))) {
            cmd = *r.p++;
        } else if (!cmd) {
            return false;
        }
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const char op  = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        const auto abs2 = [&](float x, float y) {
            return rel ? PanPt{cur.x + x, cur.y + y} : PanPt{x, y};
        };
        if (op == 'Z') {
            cur = start;
            r.Space();
            continue;
        }
        if (out->empty() && op != 'M') return false;

        float a = 0, b = 0, c = 0, e = 0, f = 0, g = 0;
        switch (op) {
            case 'M': {
                if (!r.Num(&a) || !r.Num(&b)) return false;
                cur = abs2(a, b);
                start = cur;
                out->push_back({cur});
                // A repeated M coordinate pair means an implicit L.
                cmd = rel ? 'l' : 'L';
                break;
            }
            case 'L':
                if (!r.Num(&a) || !r.Num(&b)) return false;
                cur = abs2(a, b);
                out->back().push_back(cur);
                break;
            case 'H':
                if (!r.Num(&a)) return false;
                cur = PanPt{rel ? cur.x + a : a, cur.y};
                out->back().push_back(cur);
                break;
            case 'V':
                if (!r.Num(&a)) return false;
                cur = PanPt{cur.x, rel ? cur.y + a : a};
                out->back().push_back(cur);
                break;
            case 'C': {
                if (!r.Num(&a) || !r.Num(&b) || !r.Num(&c) || !r.Num(&e) ||
                    !r.Num(&f) || !r.Num(&g)) {
                    return false;
                }
                const PanPt c1 = abs2(a, b), c2 = abs2(c, e), p3 = abs2(f, g);
                PanCubic(&out->back(), cur, c1, c2, p3, 12);
                cur = p3;
                break;
            }
            case 'A': {
                bool large = false, sweep = false;
                if (!r.Num(&a) || !r.Num(&b) || !r.Num(&c) || !r.Flag(&large) ||
                    !r.Flag(&sweep) || !r.Num(&f) || !r.Num(&g)) {
                    return false;
                }
                const PanPt to = abs2(f, g);
                PanArc(&out->back(), cur, a, b, c, large, sweep, to);
                cur = to;
                break;
            }
            default:
                return false;
        }
        r.Space();
    }
    for (std::vector<PanPt>& sub : *out) {
        for (PanPt& pt : sub) {
            pt.x *= scale;
            pt.y *= scale;
        }
    }
    return true;
}

void PanAddSpan(std::vector<float>* acc, int w, float x0, float x1, float weight) {
    x0 = std::max(x0, 0.f);
    x1 = std::min(x1, static_cast<float>(w));
    if (x1 <= x0) return;
    const int first = static_cast<int>(std::floor(x0));
    const int last  = std::min(w, static_cast<int>(std::ceil(x1)));
    for (int x = std::max(0, first); x < last; ++x) {
        const float l = std::max(x0, static_cast<float>(x));
        const float rr = std::min(x1, static_cast<float>(x) + 1.f);
        if (rr > l) (*acc)[x] += (rr - l) * weight;
    }
}

// Nonzero winding, which is what keeps a counter-wound inner ring a hole.
// Vertical coverage is sampled (four rows per pixel), horizontal coverage is
// exact — the cheap half of analytic antialiasing where it matters most, since
// these glyphs are mostly vertical strokes and circles.
void PanFillPolys(const PanPolys& polys, std::vector<uint8_t>* mask, int w, int h) {
    constexpr int kSub = 4;
    struct Cross { float x; int dir; };
    std::vector<float> acc(w, 0.f);
    std::vector<Cross> xs;
    for (int py = 0; py < h; ++py) {
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int s = 0; s < kSub; ++s) {
            const float y = py + (s + 0.5f) / kSub;
            xs.clear();
            for (const std::vector<PanPt>& sub : polys) {
                const size_t n = sub.size();
                if (n < 2) continue;
                for (size_t i = 0; i < n; ++i) {
                    const PanPt& a = sub[i];
                    const PanPt& b = sub[(i + 1) % n];
                    if (a.y == b.y) continue;
                    if (y < std::min(a.y, b.y) || y >= std::max(a.y, b.y)) continue;
                    const float t = (y - a.y) / (b.y - a.y);
                    xs.push_back(Cross{a.x + t * (b.x - a.x), b.y > a.y ? 1 : -1});
                }
            }
            if (xs.empty()) continue;
            std::sort(xs.begin(), xs.end(),
                      [](const Cross& l, const Cross& r) { return l.x < r.x; });
            int   wind = 0;
            float span = 0.f;
            for (const Cross& c : xs) {
                const int prev = wind;
                wind += c.dir;
                if (prev == 0 && wind != 0) span = c.x;
                else if (prev != 0 && wind == 0) PanAddSpan(&acc, w, span, c.x, 1.f / kSub);
            }
        }
        for (int x = 0; x < w; ++x) {
            (*mask)[static_cast<size_t>(py) * w + x] =
                static_cast<uint8_t>(std::lround(Clamp01(acc[x]) * 255.f));
        }
    }
}

// Rasterizes one icon into the card's colour layer. Small and few, so it is not
// worth a cache: a rebuild happens only when the content changes.
void StampOcticon(std::vector<uint32_t>* deco, int cw, int ch, int x0, int y0, int size,
                  const char* path, const Rgb& colour, int top = 0,
                  int bottom = 1 << 24) {
    if (size <= 0) return;
    PanPolys polys;
    if (!PanSvgPath(path, size / 16.f, &polys)) {
        std::fprintf(stderr, "speak: bad octicon path data\n");
        return;
    }
    std::vector<uint8_t> mask(static_cast<size_t>(size) * size, 0);
    PanFillPolys(polys, &mask, size, size);
    for (int y = 0; y < size; ++y) {
        const int cy = y0 + y;
        if (cy < top || cy >= bottom || cy < 0 || cy >= ch) continue;
        for (int x = 0; x < size; ++x) {
            const int cx = x0 + x;
            if (cx < 0 || cx >= cw) continue;
            const uint8_t a = mask[static_cast<size_t>(y) * size + x];
            if (!a) continue;
            uint32_t& dst = (*deco)[static_cast<size_t>(cy) * cw + cx];
            dst = BlendOver(Pack(colour, a / 255.f), dst);
        }
    }
}

// ── which icon, and what colour ─────────────────────────────────────────────
// GitHub's own pairing, so a row means on this card what it means on the page it
// came from. `approved` is left plain green rather than given a tick overlay:
// at 14 px a second mark inside the glyph turns into grit, and the row's job is
// "which thing, roughly how is it doing" — the exact review state is one click
// away.

constexpr Rgb kOctGreen  {0x34 / 255.f, 0x7d / 255.f, 0x39 / 255.f};   // #347d39
constexpr Rgb kOctPurple {0x82 / 255.f, 0x56 / 255.f, 0xd0 / 255.f};   // #8256d0
constexpr Rgb kOctRed    {0xc9 / 255.f, 0x3c / 255.f, 0x37 / 255.f};   // #c93c37
constexpr Rgb kOctAmber  {0xc6 / 255.f, 0x90 / 255.f, 0x26 / 255.f};   // #c69026
constexpr Rgb kOctGrey   {0x76 / 255.f, 0x83 / 255.f, 0x90 / 255.f};   // #768390

// Octicons 16px, verbatim (MIT).
constexpr const char* kOctIssueOpened =
    "M8 9.5a1.5 1.5 0 1 0 0-3 1.5 1.5 0 0 0 0 3Z"
    "M8 0a8 8 0 1 1 0 16A8 8 0 0 1 8 0ZM1.5 8a6.5 6.5 0 1 0 13 0 6.5 6.5 0 0 0-13 0Z";
constexpr const char* kOctIssueClosed =
    "M11.28 6.78a.75.75 0 0 0-1.06-1.06L7.25 8.69 5.78 7.22a.75.75 0 0 0-1.06 1.06l2 2a.75.75 0 0 0 1.06 0l3.5-3.5Z"
    "M16 8A8 8 0 1 1 0 8a8 8 0 0 1 16 0Zm-1.5 0a6.5 6.5 0 1 0-13 0 6.5 6.5 0 0 0 13 0Z";
constexpr const char* kOctPullRequest =
    "M1.5 3.25a2.25 2.25 0 1 1 3 2.122v5.256a2.251 2.251 0 1 1-1.5 0V5.372A2.25 2.25 0 0 1 1.5 3.25Z"
    "m5.677-.177L9.573.677A.25.25 0 0 1 10 .854V2.5h1A2.5 2.5 0 0 1 13.5 5v5.628a2.251 2.251 0 1 1-1.5 0V5a1 1 0 0 0-1-1h-1v1.646a.25.25 0 0 1-.427.177L7.177 3.427a.25.25 0 0 1 0-.354Z"
    "M3.75 2.5a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5Zm0 9.5a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5Zm8.25.75a.75.75 0 1 0 1.5 0 .75.75 0 0 0-1.5 0Z";
constexpr const char* kOctPullRequestDraft =
    "M3.25 1A2.25 2.25 0 0 1 4 5.372v5.256a2.251 2.251 0 1 1-1.5 0V5.372A2.251 2.251 0 0 1 3.25 1Z"
    "m9.5 14a2.25 2.25 0 1 1 0-4.5 2.25 2.25 0 0 1 0 4.5ZM2.5 3.25a.75.75 0 1 0 1.5 0 .75.75 0 0 0-1.5 0ZM3.25 12a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5Zm9.5 0a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5ZM14 7.5a1.25 1.25 0 1 1-2.5 0 1.25 1.25 0 0 1 2.5 0Zm0-4.25a1.25 1.25 0 1 1-2.5 0 1.25 1.25 0 0 1 2.5 0Z";
constexpr const char* kOctPullRequestClosed =
    "M3.25 1A2.25 2.25 0 0 1 4 5.372v5.256a2.251 2.251 0 1 1-1.5 0V5.372A2.251 2.251 0 0 1 3.25 1Z"
    "m9.5 5.5a.75.75 0 0 1 .75.75v3.378a2.251 2.251 0 1 1-1.5 0V7.25a.75.75 0 0 1 .75-.75Z"
    "m-2.03-5.273a.75.75 0 0 1 1.06 0l.97.97.97-.97a.748.748 0 0 1 1.265.332.75.75 0 0 1-.205.729l-.97.97.97.97a.751.751 0 0 1-.018 1.042.751.751 0 0 1-1.042.018l-.97-.97-.97.97a.749.749 0 0 1-1.275-.326.749.749 0 0 1 .215-.734l.97-.97-.97-.97a.75.75 0 0 1 0-1.06Z"
    "M2.5 3.25a.75.75 0 1 0 1.5 0 .75.75 0 0 0-1.5 0ZM3.25 12a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5Zm9.5 0a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5Z";
constexpr const char* kOctMerge =
    "M5.45 5.154A4.25 4.25 0 0 0 9.25 7.5h1.378a2.251 2.251 0 1 1 0 1.5H9.25A5.734 5.734 0 0 1 5 7.123v3.505a2.25 2.25 0 1 1-1.5 0V5.372a2.25 2.25 0 1 1 1.95-.218ZM4.25 13.5a.75.75 0 1 0 0-1.5.75.75 0 0 0 0 1.5Zm8.5-4.5a.75.75 0 1 0 0-1.5.75.75 0 0 0 0 1.5ZM5 3.25a.75.75 0 1 0 0 .005V3.25Z";
constexpr const char* kOctQuestion =
    "M0 8a8 8 0 1 1 16 0A8 8 0 0 1 0 8Zm8-6.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13ZM6.92 6.085h.001a.749.749 0 1 1-1.342-.67c.169-.339.436-.701.849-.977C6.845 4.16 7.369 4 8 4a2.756 2.756 0 0 1 1.637.525c.503.377.863.965.863 1.725 0 .448-.115.83-.329 1.15-.205.307-.47.513-.692.662-.109.072-.22.138-.313.195l-.006.004a6.24 6.24 0 0 0-.26.16.952.952 0 0 0-.276.245.75.75 0 0 1-1.248-.832c.184-.264.42-.489.692-.661.103-.067.207-.132.313-.195l.007-.004c.1-.061.182-.11.258-.161a.969.969 0 0 0 .277-.245C8.96 6.514 9 6.427 9 6.25a.612.612 0 0 0-.262-.525A1.27 1.27 0 0 0 8 5.5c-.369 0-.595.09-.74.187a1.01 1.01 0 0 0-.34.398ZM9 11a1 1 0 1 1-2 0 1 1 0 0 1 2 0Z";
constexpr const char* kOctFileDirectory =
    "M0 2.75C0 1.784.784 1 1.75 1H5c.55 0 1.07.26 1.4.7l.9 1.2a.25.25 0 0 0 .2.1h6.75c.966 0 1.75.784 1.75 1.75v8.5A1.75 1.75 0 0 1 14.25 15H1.75A1.75 1.75 0 0 1 0 13.25Z"
    "m1.75-.25a.25.25 0 0 0-.25.25v10.5c0 .138.112.25.25.25h12.5a.25.25 0 0 0 .25-.25v-8.5a.25.25 0 0 0-.25-.25H7.5c-.55 0-1.07-.26-1.4-.7l-.9-1.2a.25.25 0 0 0-.2-.1Z";

struct PanGlyph {
    const char* path;
    Rgb         colour;
};

bool PanelIsQuestion(const PanelItem& item) { return item.kind == "question"; }

PanGlyph PanelGlyphFor(const PanelItem& item) {
    // A question an agent is waiting on is the one row that is about *you*, so it
    // gets the amber the review states use for "your move" — and keeps it
    // whatever else the status says, until the status says it is closed.
    if (PanelIsQuestion(item)) {
        return {kOctQuestion,
                item.status == PanStatus::Closed ? kOctGrey : kOctAmber};
    }
    const bool pr = item.kind == "pr";
    // A path, or anything numberless, is a folder. Any *other* kind with a
    // number — `question` from the agent-questions flow, whatever comes next — is
    // drawn like an issue rather than refused: a producer running ahead of this
    // binary should degrade to a plausible row, not to a folder or to nothing.
    if (!pr && (item.kind == "path" || item.number <= 0)) {
        return {kOctFileDirectory, kOctGrey};
    }
    const bool issue = !pr;

    switch (item.status) {
        case PanStatus::Merged: return {kOctMerge, kOctPurple};
        case PanStatus::Draft:  return {kOctPullRequestDraft, kOctGrey};
        case PanStatus::Closed:
            // GitHub's split: a closed issue is "completed" (purple), a closed
            // pull request is abandoned (red).
            return issue ? PanGlyph{kOctIssueClosed, kOctPurple}
                         : PanGlyph{kOctPullRequestClosed, kOctRed};
        default:
            break;
    }
    const char* path = issue ? kOctIssueOpened : kOctPullRequest;
    switch (item.status) {
        case PanStatus::Open:
        case PanStatus::Approved:         return {path, kOctGreen};
        case PanStatus::ChangesRequested: return {path, kOctAmber};
        case PanStatus::ChecksFailing:    return {path, kOctRed};
        default:                          return {path, kOctGrey};
    }
}

// ── geometry ────────────────────────────────────────────────────────────────
// Everything is expressed at 96 dpi and multiplied by the target window's scale,
// so the card is the same physical size on a 4K laptop panel and on a 1080p
// monitor. The panel thread is per-monitor aware (like pointing), which is what
// makes GetClientRect and the overlay agree in the first place.

constexpr int kPanWidth     = 360;  // the card, at 96 dpi
constexpr int kPanMargin    = 12;   // inset from the target's client corner
constexpr int kPanShadow    = 20;   // room around the card for its drop shadow
constexpr int kPanPad       = 12;
constexpr int kPanRadius    = 10;
constexpr int kPanRowGap    = 6;    // padding added to each row's tallest mark
constexpr int kPanIcon      = 14;   // octicon box, at 96 dpi
constexpr int kPanIconGap   = 9;    // icon to label
constexpr int kPanLabelGap  = 8;    // label to title
constexpr int kPanTextPx    = 13;
constexpr int kPanToggle    = 11;   // the − / + mark, corner to corner
constexpr int kPanToggleGap = 12;
constexpr int kPanHeadGap   = 7;    // padding added to the header row
constexpr int kPanRuleGap   = 5;    // header rule to the first row
constexpr int kPanStroke    = 2;
constexpr int kPanMaxRows   = 6;    // past this, the rest collapse into "+N more"
constexpr int kPanTabPx     = 12;   // the tab strip's type
constexpr int kPanTabPadX   = 8;    // inside a tab, either side of its label
constexpr int kPanTabGap    = 4;    // between tabs
constexpr int kPanTabPadY   = 6;
constexpr int kPanTabRule   = 2;    // the active tab's accent, GitHub's weight
constexpr int kPanScrollBar = 3;    // the scroll indicator, at 96 dpi
constexpr int kPanScrollPx  = 56;   // how far one wheel notch moves the list
constexpr float kPanMaxFrac = 0.70f;  // ceiling on a full list: this much of the
                                      // target's client height
constexpr int kPanTick      = 40;   // ms between follow/hover passes
constexpr int kPanResolveMs = 500;  // ms between title→window resolver passes
// A registration is dropped this long after its last POST. Long enough that a
// session left alone overnight still has its panel in the morning, short enough
// that a machine left running for a week is not carrying last week's windows.
constexpr double kPanTtlSeconds = 48.0 * 3600.0;

constexpr float kPanFillAlpha = 0.97f;
constexpr float kPanShadowA   = 0.40f;   // a dark card needs a darker shadow to read

// GitHub's own dark surface, so the card sits in the same world as the icons and
// the pages the rows lead to. The caption toast's *material* is still shared —
// one rounded-rect distance field, its hairline, its shadow, the same fonts — but
// not its palette: the toast is a light notification that appears for a sentence,
// this is a dark panel that lives on a dark terminal for hours.
constexpr Rgb kPanBg    {0x21 / 255.f, 0x28 / 255.f, 0x30 / 255.f};   // #212830
constexpr Rgb kPanLine  {0x3d / 255.f, 0x44 / 255.f, 0x4d / 255.f};   // #3d444d
constexpr Rgb kPanTextA {0xd1 / 255.f, 0xd7 / 255.f, 0xe0 / 255.f};   // #d1d7e0
constexpr Rgb kPanTextB {0x91 / 255.f, 0x98 / 255.f, 0xa1 / 255.f};   // #9198a1
constexpr Rgb kPanHover {0x2a / 255.f, 0x31 / 255.f, 0x3c / 255.f};   // #2a313c
constexpr Rgb kPanAccent{0xf7 / 255.f, 0x81 / 255.f, 0x66 / 255.f};   // #f78166
// The speaking halo. Not the ring's ember: on the card that read as a warning
// rather than as a voice, and the panel already spends red, amber and green on
// what the rows mean. White says "this one is talking" and nothing else. A hair
// off pure white, which blooms harder than it looks against a pale terminal.
constexpr Rgb kPanGlow  {0xf0 / 255.f, 0xf3 / 255.f, 0xf6 / 255.f};   // #f0f3f6

// The scale the card currently being built is drawn at. Panels are only ever
// built on the panel thread (or on the main thread by --panel-preview), so a
// plain global is enough and keeps the geometry helpers free of a context
// parameter — exactly how CapScale reads g_orb_size.
float g_pan_scale = 1.f;

int PanScale(int v) { return std::max(1, static_cast<int>(std::lround(v * g_pan_scale))); }

// Fixed, rather than following whatever --caption-variant a passing utterance
// happened to set: the panel is on screen for hours, and a colour on the card
// would claim the meaning that belongs to the icons.
Rgb PanFill()   { return kPanBg; }
Rgb PanInk()    { return kPanTextA; }
Rgb PanDimInk() { return kPanTextB; }
Rgb PanBorder() { return kPanLine; }

// ── the tab strip ───────────────────────────────────────────────────────────
// A table rather than a switch, because the interesting tab is the one that does
// not exist yet: `Pending (N)` for questions an agent is waiting on Fernando to
// answer is one row added here and nothing else. A tab with nothing in it is not
// shown, and if that leaves only `All` there is no strip at all — the panel is
// six rows in the corner of a terminal, and furniture has to earn its line.

bool PanTabAll(const PanelItem&)  { return true; }
bool PanTabIssue(const PanelItem& i) { return i.kind == "issue"; }
bool PanTabPr(const PanelItem& i)    { return i.kind == "pr"; }
bool PanTabQuestion(const PanelItem& i) { return i.kind == "question"; }

struct PanTab {
    const char* id;
    const char* label;
    bool (*match)(const PanelItem&);
    bool counted;   // "Issues (3)"; `All` is a mode, not a quantity
};

constexpr PanTab kPanTabs[] = {
    {"all",    "All",    PanTabAll,   false},
    {"issues", "Issues", PanTabIssue, true},
    {"prs",    "PRs",    PanTabPr,    true},
    {"pending", "Pending", PanTabQuestion, true},
};
constexpr int kPanTabCount = static_cast<int>(sizeof(kPanTabs) / sizeof(kPanTabs[0]));

int PanTabIndex(const std::string& id) {
    for (int i = 0; i < kPanTabCount; ++i) {
        if (id == kPanTabs[i].id) return i;
    }
    return 0;
}

size_t PanTabTally(const PanTab& tab, const std::vector<PanelItem>& items) {
    size_t n = 0;
    for (const PanelItem& it : items) {
        if (tab.match(it)) ++n;
    }
    return n;
}

std::string PanCountText(size_t n) {
    return std::to_string(n) + (n == 1 ? " item" : " items");
}

// The pill shows the worst item on the list — its icon and its colour — which
// is what makes a collapsed panel still worth glancing at. PanStatus is declared
// in that order, and an open question outranks all of it: a red pill means a
// machine is unhappy about something, an amber one means a machine is waiting
// for you, and the second is the one that will not resolve itself.
const PanelItem* PanWorstItem(const std::vector<PanelItem>& items) {
    const PanelItem* worst = nullptr;
    for (const PanelItem& it : items) {
        if (PanelIsQuestion(it) && it.status != PanStatus::Closed) return &it;
        if (!worst || static_cast<int>(it.status) > static_cast<int>(worst->status)) {
            worst = &it;
        }
    }
    return worst;
}

size_t PanPendingCount(const std::vector<PanelItem>& items) {
    size_t n = 0;
    for (const PanelItem& it : items) {
        if (PanelIsQuestion(it) && it.status != PanStatus::Closed) ++n;
    }
    return n;
}

// Rasterized once per content change; the hover highlight and the colours are
// applied per compose, so moving the pointer down the list does not re-measure
// any text. Same split as the caption card.
struct PanelCard {
    int                   w = 0, h = 0;      // including the shadow margin
    int                   panel_w = 0, panel_h = 0;
    int                   margin = 0;
    float                 radius = 0.f;
    std::vector<float>    dist;              // signed distance to the card edge
    // Composed once by BakePanelLayers: everything below the interactive bits,
    // and everything above them. A frame is a copy of the first, two small
    // overlays, and the second.
    std::vector<uint32_t> base, marks;
    // The speaking halo's shape, baked at the same time. Only its *brightness*
    // follows the voice, so the falloff is geometry like everything else here.
    std::vector<uint8_t>  halo;
    std::vector<uint8_t>  ink, dim;          // text coverage, full and muted
    std::vector<uint32_t> deco;              // premultiplied: the icons carry colour
    RECT                  header{};          // the collapse toggle's click target
    int                   rule_y = -1;       // hairline under the header, -1 for none
    struct Row {
        RECT hit;
        int  item;   // index into items, or one of the two controls below
    };
    std::vector<Row> rows;
    struct Tab {
        RECT hit;
        int  index;   // into kPanTabs
    };
    std::vector<Tab> tabs;
    int              scroll_max = 0;   // >0 when the list is taller than its viewport
};

// Every row is clickable, so the two that are controls rather than items get
// their own indices rather than being "not a row".
constexpr int kPanRowMore = -1;   // "+N more"  -> show everything
constexpr int kPanRowLess = -2;   // "show less" -> back to the first six

// The rows live in a viewport that can be shorter than they are, so everything
// drawn into it is clipped vertically. Cheaper and simpler than compositing a
// separate rows layer: the header sits outside the same band and must survive.
void PanStampMask(std::vector<uint8_t>* layer, int layer_w, int layer_h,
                  const TextMask& m, int x0, int y0, int top, int bottom) {
    for (int y = 0; y < m.h; ++y) {
        const int cy = y0 + y;
        if (cy < top || cy >= bottom || cy < 0 || cy >= layer_h) continue;
        for (int x = 0; x < m.w; ++x) {
            const int cx = x0 + x;
            if (cx < 0 || cx >= layer_w) continue;
            (*layer)[static_cast<size_t>(cy) * layer_w + cx] =
                m.a[static_cast<size_t>(y) * m.w + x];
        }
    }
}

void PanFillRect(std::vector<uint32_t>* deco, int w, int h, RECT r, const Rgb& colour,
                 float alpha) {
    for (int y = std::max<int>(0, r.top); y < std::min<int>(h, r.bottom); ++y) {
        for (int x = std::max<int>(0, r.left); x < std::min<int>(w, r.right); ++x) {
            uint32_t& dst = (*deco)[static_cast<size_t>(y) * w + x];
            dst = BlendOver(Pack(colour, alpha), dst);
        }
    }
}

// One line, cut with an ellipsis rather than wrapped: a row is a handle, not a
// paragraph. DT_SINGLELINE turns off the rasterizer's word breaking, and the
// width clamp it already applies is what gives DrawText the box to ellipsize in.
TextMask PanLine(const std::string& text, bool bold, int max_w) {
    return RenderText(Wide(text), PanScale(kPanTextPx), bold, max_w, 1,
                      DT_SINGLELINE | DT_END_ELLIPSIS);
}

// `max_h` caps the card (0 = no ceiling) and `scroll` is clamped in place, so a
// list longer than its viewport can be wheeled through. Both only ever matter
// once the "+N more" row has been clicked.
void BakePanelLayers(PanelCard* out);   // defined after this, used at the end

// `cap_title`/`cap` take the header line over while the panel is speaking: what
// the voice is saying about this terminal is more urgent than what it is working
// on, and it is the same line either way rather than a row that appears and
// shoves the list down.
void BuildPanelCard(PanelCard* out, const std::string& summary,
                    const std::vector<PanelItem>& items, bool collapsed,
                    const std::string& tab, bool expanded_all, float scale, int max_h,
                    int* scroll, const std::string& cap_title = std::string(),
                    const std::string& cap = std::string()) {
    *out = PanelCard{};
    g_pan_scale = scale;

    const int pad    = PanScale(kPanPad);
    const int margin = PanScale(kPanShadow);
    const int toggle = PanScale(kPanToggle);
    const int tgap   = PanScale(kPanToggleGap);
    const int icon   = PanScale(kPanIcon);
    const int igap   = PanScale(kPanIconGap);

    // ── the collapsed pill ──
    // A one-line lozenge: the worst item's icon and how many things are behind it. It
    // fits its text rather than keeping the expanded width, so a collapsed panel
    // gives the terminal underneath almost all of its corner back.
    if (collapsed) {
        // "2 pending" rather than "10 items" when any of them is a question: the
        // number worth reducing the panel to is the one that needs an answer.
        const size_t pending = PanPendingCount(items);
        const std::string count_text =
            pending ? std::to_string(pending) + " pending" : PanCountText(items.size());
        const TextMask count = PanLine(count_text, false,
                                       PanScale(kPanWidth) - 2 * pad);
        const int panel_w = pad + icon + igap + count.w + tgap + toggle + pad;
        const int panel_h = std::max({count.h, toggle, icon}) + 2 * PanScale(7);
        out->margin  = margin;
        out->panel_w = panel_w;
        out->panel_h = panel_h;
        out->radius  = panel_h * 0.5f;   // a real pill, not a small card
        out->w = panel_w + 2 * margin;
        out->h = panel_h + 2 * margin;
        const size_t n = static_cast<size_t>(out->w) * out->h;
        out->dist.resize(n);
        out->ink.assign(n, 0);
        out->dim.assign(n, 0);
        out->deco.assign(n, 0);
        out->header = RECT{margin, margin, margin + panel_w, margin + panel_h};
        StampMask(&out->dim, out->w, out->h, count,
                  margin + pad + icon + igap, margin + (panel_h - count.h) / 2);
        if (const PanelItem* worst = PanWorstItem(items)) {
            const PanGlyph g = PanelGlyphFor(*worst);
            StampOcticon(&out->deco, out->w, out->h, margin + pad,
                         margin + (panel_h - icon) / 2, icon, g.path, g.colour);
        }
        // A `+`, because from here the gesture is to unfold it.
        const float tw = std::max(1.f, PanScale(kPanStroke) * 0.9f) * 0.5f;
        const float tc = toggle * 0.5f;
        StampSdf(&out->ink, out->w, out->h, margin + panel_w - pad - toggle,
                 margin + (panel_h - toggle) / 2, toggle, toggle,
                 [=](float px, float py) {
            return std::min(SegDist(px, py, 0.5f, tc, toggle - 0.5f, tc),
                            SegDist(px, py, tc, 0.5f, tc, toggle - 0.5f)) - tw;
        });
    } else {
        // ── the expanded card ──
        const int panel_w = PanScale(kPanWidth);
        const int inner   = panel_w - 2 * pad;

        // The header doubles as the summary line: one row is enough for "what is
        // this terminal doing", and giving the toggle its own row would spend a
        // line of the terminal on furniture.
        const bool  spoken   = !cap_title.empty() || !cap.empty();
        const int   head_w   = inner - toggle - tgap;
        const std::string head_text =
            spoken ? cap_title
                   : (summary.empty() ? PanCountText(items.size()) : summary);
        const TextMask head = PanLine(head_text, spoken, head_w);
        // Speaking: a bold title and the caption beside it, laid out like a row.
        TextMask head_rest;
        if (spoken && !cap.empty()) {
            const int rest = head_w - head.w - (head.w ? PanScale(kPanLabelGap) : 0);
            if (rest >= PanScale(56)) head_rest = PanLine(cap, false, rest);
        }
        const int head_h =
            std::max({head.h, head_rest.h, toggle}) + PanScale(kPanHeadGap);

        // Which tabs exist for this list, and which of them is showing. An empty
        // tab is not offered, and a selection whose tab has emptied falls back to
        // `All` without being forgotten — the items may well come back.
        struct TabBuild {
            int      index;
            TextMask label;
            int      w = 0;
            size_t   count = 0;
        };
        std::vector<TabBuild> tabs;
        int                   active = 0;
        for (int i = 0; i < kPanTabCount; ++i) {
            const size_t n = PanTabTally(kPanTabs[i], items);
            if (i > 0 && n == 0) continue;
            TabBuild t;
            t.index = i;
            t.count = n;
            std::string label = kPanTabs[i].label;
            if (kPanTabs[i].counted) label += " (" + std::to_string(n) + ")";
            t.label = RenderText(Wide(label), PanScale(kPanTabPx), true, inner, 1,
                                 DT_SINGLELINE | DT_END_ELLIPSIS);
            t.w     = t.label.w + 2 * PanScale(kPanTabPadX);
            if (tab == kPanTabs[i].id) active = static_cast<int>(tabs.size());
            tabs.push_back(std::move(t));
        }
        if (tabs.size() < 2) tabs.clear();   // only `All`: no strip worth the line
        int tab_h = 0;
        for (const TabBuild& t : tabs) {
            tab_h = std::max(tab_h, t.label.h + 2 * PanScale(kPanTabPadY));
        }
        const PanTab& filter = kPanTabs[tabs.empty() ? 0 : tabs[active].index];

        // Questions first, the rest in the order the producer ranked them. Only
        // `All` mixes kinds, so this only ever reorders there — and there it
        // should: a question is the one row that is waiting on *you*.
        std::vector<int> view;
        for (size_t i = 0; i < items.size(); ++i) {
            if (filter.match(items[i]) && PanelIsQuestion(items[i])) {
                view.push_back(static_cast<int>(i));
            }
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (filter.match(items[i]) && !PanelIsQuestion(items[i])) {
                view.push_back(static_cast<int>(i));
            }
        }

        // Switching tabs must not move the strip out from under the cursor, so
        // the card keeps the height of the *tallest* tab's list and a shorter one
        // simply leaves card underneath. No text has to be measured to know that:
        // every row is one line, so a list's height is arithmetic once the line
        // height is known.
        const int line_h = std::max(PanLine("Ag", true, inner).h,
                                    PanLine("Ag", false, inner).h);
        const int item_row_h = std::max(line_h, icon) + PanScale(kPanRowGap);
        const int ctrl_row_h = line_h + PanScale(kPanRowGap);
        const auto list_h = [&](size_t n) {
            const size_t vis =
                expanded_all ? n : std::min<size_t>(n, kPanMaxRows);
            const bool ctrl = n > vis || (expanded_all && n > kPanMaxRows);
            return static_cast<int>(vis) * item_row_h + (ctrl ? ctrl_row_h : 0);
        };
        int tallest = list_h(tabs.empty() ? items.size() : 0);
        for (const TabBuild& t : tabs) tallest = std::max(tallest, list_h(t.count));

        struct RowBuild {
            TextMask label, title;
            int      h = 0, item = 0;
        };
        std::vector<RowBuild> built;
        const int text_w = inner - icon - igap;
        const size_t shown =
            expanded_all ? view.size() : std::min<size_t>(view.size(), kPanMaxRows);
        for (size_t k = 0; k < shown; ++k) {
            const PanelItem& item = items[view[k]];
            RowBuild r;
            r.item  = view[k];
            // A handle is bold; a *sentence* is not. A question's label is the
            // question, so it keeps the primary ink and drops the weight.
            r.label = PanLine(item.Label(), !PanelIsQuestion(item), text_w);
            // Whatever the label leaves. Below a usable remainder the title is
            // dropped entirely rather than shown as three characters and a dot.
            const int rest = text_w - r.label.w - PanScale(kPanLabelGap);
            std::string title = item.title;
            if (title == item.Label()) title.clear();
            if (!title.empty() && rest >= PanScale(56)) r.title = PanLine(title, false, rest);
            r.h = std::max({r.label.h, r.title.h, icon}) + PanScale(kPanRowGap);
            built.push_back(std::move(r));
        }
        // The last row is a control either way: what is hidden, or the way back.
        if (view.size() > shown || (expanded_all && view.size() > kPanMaxRows)) {
            RowBuild r;
            const bool more = view.size() > shown;
            r.item  = more ? kPanRowMore : kPanRowLess;
            r.label = PanLine(more ? "+" + std::to_string(view.size() - shown) + " more"
                                   : std::string("show less"),
                              false, inner);
            r.h     = r.label.h + PanScale(kPanRowGap);
            built.push_back(std::move(r));
        }

        int rows_h = 0;
        for (const RowBuild& r : built) rows_h += r.h;

        // A full list must not swallow the terminal it is sitting in, so it is
        // capped and the overflow is wheeled through instead.
        const int chrome  = pad + head_h + tab_h + PanScale(kPanRuleGap) + pad;
        int       view_h  = std::max(rows_h, tallest);
        if (max_h > 0 && chrome + view_h > max_h) {
            view_h = std::max(PanScale(48), max_h - chrome);
        }
        out->scroll_max = std::max(0, rows_h - view_h);
        int at = scroll ? std::max(0, std::min(*scroll, out->scroll_max)) : 0;
        if (scroll) *scroll = at;
        const int panel_h = chrome + view_h;

        out->margin  = margin;
        out->panel_w = panel_w;
        out->panel_h = panel_h;
        out->radius  = static_cast<float>(PanScale(kPanRadius));
        out->w = panel_w + 2 * margin;
        out->h = panel_h + 2 * margin;
        const size_t n = static_cast<size_t>(out->w) * out->h;
        out->dist.resize(n);
        out->ink.assign(n, 0);
        out->dim.assign(n, 0);
        out->deco.assign(n, 0);

        int y = margin + pad;
        // The whole top band, padding included: the header is meant to be an easy
        // thing to hit, since it is the one control on the card.
        out->header = RECT{margin, margin, margin + panel_w, y + head_h};
        StampMask(spoken ? &out->ink : &out->dim, out->w, out->h, head, margin + pad,
                  y + (head_h - head.h) / 2);
        if (head_rest.w) {
            StampMask(&out->dim, out->w, out->h, head_rest,
                      margin + pad + head.w + PanScale(kPanLabelGap),
                      y + (head_h - head_rest.h) / 2);
        }
        const int tx = margin + panel_w - pad - toggle;
        const int ty = y + (head_h - toggle) / 2;
        const float tw = std::max(1.f, PanScale(kPanStroke) * 0.9f) * 0.5f;
        const float tc = toggle * 0.5f;
        StampSdf(&out->ink, out->w, out->h, tx, ty, toggle, toggle,
                 [=](float px, float py) {
            return SegDist(px, py, 0.5f, tc, toggle - 0.5f, tc) - tw;
        });
        y += head_h;

        // The strip sits on the hairline, GitHub's underlined nav: the active
        // tab's accent replaces that line under itself, which is what makes the
        // pair read as one control rather than as a label above a border.
        if (!tabs.empty()) {
            int tx_at = margin + pad - PanScale(kPanTabPadX);
            for (size_t i = 0; i < tabs.size(); ++i) {
                const TabBuild& t = tabs[i];
                const RECT hit{tx_at, y, tx_at + t.w, y + tab_h};
                out->tabs.push_back(PanelCard::Tab{hit, t.index});
                const bool on = (static_cast<int>(i) == active);
                StampMask(on ? &out->ink : &out->dim, out->w, out->h, t.label,
                          tx_at + PanScale(kPanTabPadX), y + (tab_h - t.label.h) / 2);
                if (on) {
                    PanFillRect(&out->deco, out->w, out->h,
                                RECT{tx_at, y + tab_h - PanScale(kPanTabRule),
                                     tx_at + t.w, y + tab_h},
                                kPanAccent, 1.f);
                }
                tx_at += t.w + PanScale(kPanTabGap);
            }
            y += tab_h;
        }
        out->rule_y = y + (tabs.empty() ? PanScale(2) : -1);
        y += PanScale(kPanRuleGap);

        const int view_top = y, view_bottom = y + view_h;
        y -= at;   // the whole list slides under the viewport

        for (const RowBuild& r : built) {
            // Rows claim the full width bar a hair each side, so the highlight
            // reads as a band across the card rather than as a button in it.
            const int slop = PanScale(4);
            const int top = std::max(y, view_top), bot = std::min(y + r.h, view_bottom);
            if (bot > top) {
                out->rows.push_back(PanelCard::Row{
                    RECT{margin + slop, top, margin + panel_w - slop, bot}, r.item});
            }
            if (r.item < 0) {
                PanStampMask(&out->dim, out->w, out->h, r.label, margin + pad,
                             y + (r.h - r.label.h) / 2, view_top, view_bottom);
            } else {
                const PanGlyph g = PanelGlyphFor(items[r.item]);
                StampOcticon(&out->deco, out->w, out->h, margin + pad,
                             y + (r.h - icon) / 2, icon, g.path, g.colour,
                             view_top, view_bottom);
                const int lx = margin + pad + icon + igap;
                PanStampMask(&out->ink, out->w, out->h, r.label, lx,
                             y + (r.h - r.label.h) / 2, view_top, view_bottom);
                if (r.title.w) {
                    PanStampMask(&out->dim, out->w, out->h, r.title,
                                 lx + r.label.w + PanScale(kPanLabelGap),
                                 y + (r.h - r.title.h) / 2, view_top, view_bottom);
                }
            }
            y += r.h;
        }

        // No scrollbar chrome, just enough of a mark to say there is more and
        // roughly where you are in it.
        if (out->scroll_max > 0) {
            const int bw = PanScale(kPanScrollBar);
            const int bx = margin + panel_w - PanScale(5) - bw;
            const int bh = std::max(PanScale(18), view_h * view_h / std::max(1, rows_h));
            const int by = view_top + (view_h - bh) * at / out->scroll_max;
            PanFillRect(&out->deco, out->w, out->h,
                        RECT{bx, by, bx + bw, by + bh}, kPanLine, 0.95f);
        }
    }

    // The silhouette, its hairline and its shadow, all out of one rounded-rect
    // distance field — the caption card's trick, and the reason the two objects
    // have the same edge.
    const float hw = out->panel_w * 0.5f, hh = out->panel_h * 0.5f;
    const float ccx = out->w * 0.5f, ccy = out->h * 0.5f;
    const float radius = std::min(out->radius, std::min(hw, hh));
    for (int y = 0; y < out->h; ++y) {
        for (int x = 0; x < out->w; ++x) {
            const float dx = x + 0.5f - ccx, dy = y + 0.5f - ccy;
            const float qx = std::max(std::fabs(dx) - (hw - radius), 0.f);
            const float qy = std::max(std::fabs(dy) - (hh - radius), 0.f);
            out->dist[static_cast<size_t>(y) * out->w + x] =
                std::sqrt(qx * qx + qy * qy) - radius;
        }
    }

    BakePanelLayers(out);
}

// Almost none of a card changes between frames: the shadow, the fill, the
// hairline, the header rule, the icons and every glyph of text are all fixed
// until the content is. So they are composed *once*, at build time, into two
// premultiplied layers — everything under the interactive bits, and everything
// over them — and a frame is a copy plus the two things that do move.
//
// This is the difference between 5.5 ms and half a millisecond per frame, which
// matters because a glowing card redraws twenty-five times a second while the
// voice is going, on the same thread that answers every panel POST.
void BakePanelLayers(PanelCard* out) {
    const Rgb fill   = PanFill();
    const Rgb border = PanBorder();
    const Rgb ink    = PanInk();
    const Rgb dim    = PanDimInk();
    constexpr Rgb kShadowInk{0.01f, 0.02f, 0.04f};
    // Tighter than the caption toast's: that one appears for a sentence over
    // whatever the desktop is, where a wide shadow lifts it off. This one sits on
    // a terminal all day, and on a light theme a broad halo read as dirt around
    // the card — most obviously around the collapsed pill, which is small enough
    // that the shadow was most of it.
    const float sigma = std::max(1.f, PanScale(kPanShadow) * 0.31f);
    const int   drop  = PanScale(4);

    const size_t n = static_cast<size_t>(out->w) * out->h;
    out->base.assign(n, 0);
    out->marks.assign(n, 0);
    for (int y = 0; y < out->h; ++y) {
        for (int x = 0; x < out->w; ++x) {
            const size_t ci = static_cast<size_t>(y) * out->w + x;
            const float  d  = out->dist[ci];
            const float  inside = 1.f - SmoothStep(-0.7f, 0.7f, d);

            uint32_t p = 0;
            // The same distance field sampled a few rows up is the shape of the
            // shadow — and only outside the card, since the fill would cover it.
            if (y >= drop && d > -1.f) {
                const float sd =
                    std::max(out->dist[static_cast<size_t>(y - drop) * out->w + x], 0.f);
                const float a = kPanShadowA * std::exp(-(sd / sigma) * (sd / sigma));
                if (a > 0.004f) p = Pack(kShadowInk, Clamp01(a));
            }
            if (inside > 0.004f) {
                p = BlendOver(Pack(fill, inside * kPanFillAlpha), p);
                // A band a pixel or two wide just *inside* the edge. Note the
                // direction: `dist` is negative inside, so the ramp has to rise
                // towards the boundary. Getting that backwards paints the whole
                // card in the border colour, which is invisible when the border
                // is 13% off the fill (as the caption toast's is) and very much
                // not when it is #3d444d on #212830.
                const float edge = inside * SmoothStep(-1.8f, -0.3f, d);
                if (edge > 0.004f) p = BlendOver(Pack(border, edge * kPanFillAlpha), p);
                if (out->rule_y >= 0 && y == out->rule_y &&
                    x > out->margin && x < out->margin + out->panel_w) {
                    p = BlendOver(Pack(kPanLine, 1.f), p);
                }
            }
            out->base[ci] = p;

            uint32_t m = 0;
            if (const uint32_t b = out->deco[ci]) m = ScaleAlpha(b, inside);
            if (const uint8_t a = out->ink[ci])   m = BlendOver(Pack(ink, a / 255.f), m);
            if (const uint8_t a = out->dim[ci])   m = BlendOver(Pack(dim, a / 255.f), m);
            out->marks[ci] = m;
        }
    }

    // The halo: a Gaussian band on the card's own outline, reaching a little way
    // inside so the hairline is lit too and the edge reads as glowing rather
    // than as a card with something behind it.
    out->halo.assign(n, 0);
    const float gsig  = std::max(1.f, static_cast<float>(PanScale(7)));
    const float reach = 3.f * gsig;
    for (size_t ci = 0; ci < n; ++ci) {
        const float d = out->dist[ci];
        if (d < -2.5f || d > reach) continue;
        const float t = std::max(d, 0.f) / gsig;
        const float a = std::exp(-t * t) * 0.62f;
        if (a > 0.004f) out->halo[ci] = static_cast<uint8_t>(a * 255.f + 0.5f);
    }
}

// A frame: the baked card, the hovered band under the text, the speaking halo
// around the outline, the baked marks on top. `hover` is the row rectangle to
// light up (the header and the tabs count as rows), or null.
void ComposePanel(uint32_t* frame, const PanelCard& card, const RECT* hover, float fade,
                  float glow, float voice) {
    const size_t n = static_cast<size_t>(card.w) * card.h;
    if (card.base.size() != n) return;
    std::memcpy(frame, card.base.data(), n * 4);

    if (hover) {
        const float hr = static_cast<float>(PanScale(6));   // the band's corners
        const float bw = (hover->right - hover->left) * 0.5f;
        const float bh = (hover->bottom - hover->top) * 0.5f;
        const float r  = std::min(hr, std::min(bw, bh));
        const int   y0 = std::max(0, static_cast<int>(hover->top) - 2);
        const int   y1 = std::min(card.h, static_cast<int>(hover->bottom) + 2);
        const int   x0 = std::max(0, static_cast<int>(hover->left) - 2);
        const int   x1 = std::min(card.w, static_cast<int>(hover->right) + 2);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const size_t ci = static_cast<size_t>(y) * card.w + x;
                const float bx = std::fabs(x + 0.5f - (hover->left + bw));
                const float by = std::fabs(y + 0.5f - (hover->top + bh));
                const float qx = std::max(bx - (bw - r), 0.f);
                const float qy = std::max(by - (bh - r), 0.f);
                const float hd = std::sqrt(qx * qx + qy * qy) - r;
                const float inside = 1.f - SmoothStep(-0.7f, 0.7f, card.dist[ci]);
                const float ha = (1.f - SmoothStep(-0.7f, 0.7f, hd)) * inside;
                if (ha > 0.004f) frame[ci] = BlendOver(Pack(kPanHover, ha), frame[ci]);
            }
        }
    }

    // Brightness follows the voice; the shape is baked. A table for the colour
    // as well, because this runs over every pixel of the ring twenty-five times
    // a second for as long as the utterance lasts, and Pack's float work is most
    // of what that used to cost.
    const float gi = glow * (0.30f + 0.55f * Clamp01(voice));
    if (gi > 0.004f && card.halo.size() == n) {
        static uint32_t glow_ink[256];
        static bool     ready = false;
        if (!ready) {
            for (int i = 0; i < 256; ++i) glow_ink[i] = Pack(kPanGlow, i / 255.f);
            ready = true;
        }
        for (size_t ci = 0; ci < n; ++ci) {
            const uint8_t hm = card.halo[ci];
            if (!hm) continue;
            const int q = static_cast<int>(gi * hm);
            if (q > 3) frame[ci] = BlendOver(glow_ink[q > 255 ? 255 : q], frame[ci]);
        }
    }

    for (size_t ci = 0; ci < n; ++ci) {
        if (const uint32_t m = card.marks[ci]) frame[ci] = BlendOver(m, frame[ci]);
    }
    if (fade < 0.999f) {
        for (size_t ci = 0; ci < n; ++ci) frame[ci] = ScaleAlpha(frame[ci], fade);
    }
}

// ── a live panel ────────────────────────────────────────────────────────────

// What is under the pointer. Three kinds of thing are clickable now — the
// header, a tab, a row — so this is a pair rather than an index with sentinels.
struct PanHit {
    enum class Kind { None, Header, Tab, Row } kind = Kind::None;
    int index = -1;

    bool operator==(const PanHit& o) const { return kind == o.kind && index == o.index; }
    bool operator!=(const PanHit& o) const { return !(*this == o); }
};

// A *registration*, not a window: a session says what it is working on and which
// window title to look for, and that outlives any particular window. Windows
// Terminal windows have tabs and the window title is the active tab's, so the
// window a session belongs to comes and goes as Fernando switches tabs — the
// card follows, the registration does not.
struct Panel {
    std::string            session;
    std::string            title;    // what to look for, re-matched every ~500 ms
    HWND                   target = nullptr;   // the window it matches *now*, or none
    HWND                   hwnd   = nullptr;   // our card, created on the first bind
    std::string            summary;
    std::vector<PanelItem> items;
    time_t    updated_at = 0;        // last POST; registrations expire 48 h after it
    bool      collapsed = false;
    // These three survive a POST and a collapse: what the card is showing is the
    // reader's business, not the producer's.
    std::string tab = "all";         // which tab is selected, by id
    bool      expanded_all = false;  // the "+N more" row has been clicked
    int       scroll   = 0;          // px the full list is wheeled down by
    int       max_h    = 0;          // ceiling from the target's client height
    bool      dirty     = true;      // content changed: the card needs rebuilding
    PanHit    hover{};
    // The speaking glow: `glow` eases in and out so the card arrives and leaves
    // with the voice rather than snapping, `voice` is the live envelope, and the
    // captions replace the summary line while it lasts.
    float       glow = 0.f, voice = 0.f;
    bool        speaking = false;
    bool        glow_drawn = false;   // the last frame pushed had a halo on it
    std::string cap_title, cap;
    float     scale     = 1.f;
    PanelCard card;

    // The layered window's backing store, resized whenever the card is.
    HDC     dc      = nullptr;
    HBITMAP dib     = nullptr;
    HGDIOBJ old_bmp = nullptr;
    void*   bits    = nullptr;
    int     dib_w = 0, dib_h = 0;
    POINT   pos{-32000, -32000};
    bool    shown = false;

    void ReleaseDib() {
        if (dc && old_bmp) SelectObject(dc, old_bmp);
        if (dib) DeleteObject(dib);
        if (dc)  DeleteDC(dc);
        dc = nullptr; dib = nullptr; old_bmp = nullptr; bits = nullptr;
        dib_w = dib_h = 0;
    }
    ~Panel() { ReleaseDib(); }
};

Panel* PanelOf(HWND hwnd) {
    return reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// The card itself, without the shadow margin: everything else about this window
// is transparent and must stay click-through.
bool PanelInside(const Panel* p, POINT screen) {
    if (!p->card.w) return false;
    const long x = screen.x - p->pos.x, y = screen.y - p->pos.y;
    return x >= p->card.margin && x < p->card.margin + p->card.panel_w &&
           y >= p->card.margin && y < p->card.margin + p->card.panel_h;
}

PanHit PanelHitAt(const Panel* p, POINT screen) {
    if (!PanelInside(p, screen)) return {};
    POINT q{screen.x - p->pos.x, screen.y - p->pos.y};
    if (PtInRect(&p->card.header, q)) return {PanHit::Kind::Header, 0};
    for (size_t i = 0; i < p->card.tabs.size(); ++i) {
        if (PtInRect(&p->card.tabs[i].hit, q)) {
            return {PanHit::Kind::Tab, static_cast<int>(i)};
        }
    }
    for (size_t i = 0; i < p->card.rows.size(); ++i) {
        if (PtInRect(&p->card.rows[i].hit, q)) {
            return {PanHit::Kind::Row, static_cast<int>(i)};
        }
    }
    return {};
}

// A row's target is handed to ShellExecute, so it is worth being narrow about
// what may be one: an http(s) URL, or an absolute Windows path. The producer is
// on loopback, but "open whatever string arrived over a socket" is not a thing
// to build even so.
bool PanelOpenable(const std::string& u) {
    const std::string lower = LowerAscii(u);
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) return true;
    if (u.size() >= 3 && std::isalpha(static_cast<unsigned char>(u[0])) && u[1] == ':' &&
        (u[2] == '\\' || u[2] == '/')) {
        return true;
    }
    return u.size() >= 2 && u[0] == '\\' && u[1] == '\\';
}

// A question has nowhere to go — the terminal asking it is the one you are
// already looking at — so clicking it puts the text on the clipboard instead,
// which is what you want when the answer is "paste this into the other window".
void PanelCopy(const std::string& text) {
    const std::wstring wide = Wide(text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return;
    if (void* dst = GlobalLock(mem)) {
        std::memcpy(dst, wide.c_str(), bytes);
        GlobalUnlock(mem);
        // No owner window: the panel must not take focus to do this, and the
        // clipboard does not require it to.
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, mem)) mem = nullptr;   // now theirs
            CloseClipboard();
        }
    }
    if (mem) GlobalFree(mem);
}

// URLs go to the browser, directories to Explorer — one call does both, which is
// the whole reason a path row can sit in the same list as a pull request.
void PanelOpen(const PanelItem& item) {
    if (PanelIsQuestion(item)) {
        PanelCopy(item.title);
        return;
    }
    if (!PanelOpenable(item.url)) {
        std::fprintf(stderr, "speak: panel row '%s' has nothing openable\n",
                     item.Label().c_str());
        return;
    }
    ShellExecuteW(nullptr, L"open", Wide(item.url).c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void PanelCompose(Panel* p);   // defined with the render pass below

void PanelScrollBy(Panel* p, int delta) {
    if (p->card.scroll_max <= 0 || delta == 0) return;
    g_pan_scale = p->scale;
    const int step = PanScale(kPanScrollPx) * (delta > 0 ? -1 : 1);
    const int want = std::max(0, std::min(p->scroll + step, p->card.scroll_max));
    if (want == p->scroll) return;
    p->scroll = want;
    p->dirty  = true;
}

void PanelToggle(Panel* p) {
    p->collapsed = !p->collapsed;
    p->dirty     = true;
    p->hover     = PanHit{};
}

LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Panel* p = PanelOf(hwnd);
    switch (msg) {
        case WM_NCHITTEST: {
            // The shadow margin is empty pixels; it must not swallow a click meant
            // for the terminal behind it. Same trick as the orb's square.
            if (!p) break;
            const POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            return PanelInside(p, s) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_LBUTTONDOWN: {
            if (!p) break;
            POINT cur{};
            GetCursorPos(&cur);
            const PanHit hit = PanelHitAt(p, cur);
            if (hit.kind == PanHit::Kind::Header) {
                PanelToggle(p);
            } else if (hit.kind == PanHit::Kind::Tab) {
                p->tab    = kPanTabs[p->card.tabs[hit.index].index].id;
                p->scroll = 0;
                p->dirty  = true;
                p->hover  = PanHit{};
            } else if (hit.kind == PanHit::Kind::Row) {
                const int idx = p->card.rows[hit.index].item;
                if (idx >= 0 && idx < static_cast<int>(p->items.size())) {
                    PanelOpen(p->items[idx]);
                } else if (idx == kPanRowMore || idx == kPanRowLess) {
                    p->expanded_all = (idx == kPanRowMore);
                    p->scroll       = 0;
                    p->dirty        = true;
                    p->hover        = PanHit{};
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            // Windows routes the wheel to the window under the pointer for
            // inactive windows ("scroll inactive windows when I hover over
            // them", on by default since Windows 10), which is the whole reason
            // this arrives at a window that never takes focus. When that setting
            // is off, the hook below posts the same message here.
            if (p) PanelScrollBy(p, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_RBUTTONDOWN:
            // Anywhere on the card: the collapse toggle is the only gesture, so it
            // should not require finding the header.
            if (p && PanelInside(p, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
                PanelToggle(p);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Reblits the existing card with the current hover state — no text is measured
// here, so running down the list is cheap.
void PanelCompose(Panel* p) {
    if (!p->bits) return;
    g_pan_scale = p->scale;
    const RECT* hover = nullptr;
    const int   i     = p->hover.index;
    switch (p->hover.kind) {
        case PanHit::Kind::Header:
            hover = &p->card.header;
            break;
        case PanHit::Kind::Tab:
            if (i >= 0 && i < static_cast<int>(p->card.tabs.size())) {
                hover = &p->card.tabs[i].hit;
            }
            break;
        case PanHit::Kind::Row:
            if (i >= 0 && i < static_cast<int>(p->card.rows.size())) {
                hover = &p->card.rows[i].hit;
            }
            break;
        default:
            break;
    }
    ComposePanel(static_cast<uint32_t*>(p->bits), p->card, hover, 1.f, p->glow,
                 p->voice);
}

void PanelRender(Panel* p) {
    BuildPanelCard(&p->card, p->summary, p->items, p->collapsed, p->tab,
                   p->expanded_all, p->scale, p->max_h, &p->scroll, p->cap_title,
                   p->cap);
    if (p->card.w != p->dib_w || p->card.h != p->dib_h) {
        p->ReleaseDib();
        HDC screen = GetDC(nullptr);
        p->dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = p->card.w;
        bi.bmiHeader.biHeight      = -p->card.h;   // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        p->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p->bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!p->dib) { p->ReleaseDib(); return; }
        p->old_bmp = SelectObject(p->dc, p->dib);
        p->dib_w = p->card.w;
        p->dib_h = p->card.h;
    }
    PanelCompose(p);
    p->dirty = false;
}

void PanelPush(Panel* p) {
    if (!p->dc || !p->card.w) return;
    POINT pos = p->pos;
    SIZE  size{p->card.w, p->card.h};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(p->hwnd, nullptr, &pos, &size, p->dc, &src, 0, &blend, ULW_ALPHA);
}

// The scale the target is being drawn at. Per *window*, not per process: dragging
// a terminal between a scaled laptop panel and an external monitor changes it,
// and the card is rebuilt when it does.
float PanelScaleFor(HWND target) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn get_dpi = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    UINT dpi = (get_dpi && target) ? get_dpi(target) : 0;
    if (!dpi) {
        HDC screen = GetDC(nullptr);
        dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
        if (screen) ReleaseDC(nullptr, screen);
    }
    return (dpi ? dpi : 96) / 96.f;
}

// ── which window does this panel belong to? ─────────────────────────────────
// Same problem `--title` solves for pointing, and the same answer: Windows
// Terminal serves every session from one process, so the title Claude Code sets
// is the only discriminator there is. Three differences here.
//
// The title carries a spinner glyph that changes while the session works
// (`◐ …`, `✳ …`, `⠐ …`), so a leading glyph is stripped from *both* sides before
// comparing — the registered title was captured at one instant and the window is
// read at another, and the two will disagree about the glyph.
//
// An exact match wins over a containing one, so a session whose title is a
// prefix of another's still binds to its own window.
//
// And the answer changes over time. A Windows Terminal window shows the *active
// tab's* title, so a session in a background tab does not match any window at
// all until its tab comes forward. That is not a failure — it is the normal
// state of a terminal with tabs — so this is re-run on a timer rather than once,
// per registration, and a no-match hides the card instead of dropping anything.

std::string PanTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

// Drops a leading non-ASCII glyph *and the space behind it*. The space is the
// test: a title that starts with a non-ASCII word ("Ótima ideia") has no space
// there, and is left alone rather than losing its first letter.
std::string PanStripGlyph(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && static_cast<unsigned char>(s[i]) >= 0x80) ++i;
    if (i == 0 || i >= s.size()) return s;
    if (s[i] != ' ' && s[i] != '\t') return s;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Comparable form: trimmed, lowercased, leading glyph gone.
std::string PanKey(const std::string& title) {
    return PanStripGlyph(LowerAscii(PanTrim(title)));
}

// 2 = a unique exact match, 1 = a unique containing one, 0 = none or several (in
// which case `hits` holds the candidates). The quality is what breaks a tie when
// two registrations land on the same window.
int PanPickWindow(const std::vector<WindowTarget>& pool, const std::string& title,
                  HWND* out, std::vector<WindowTarget>* hits) {
    const std::string want = PanKey(title);
    if (want.empty()) return 0;
    int quality = 2;
    for (const WindowTarget& t : pool) {
        if (PanKey(t.title) == want) hits->push_back(t);
    }
    if (hits->empty()) {
        quality = 1;
        for (const WindowTarget& t : pool) {
            if (PanKey(t.title).find(want) != std::string::npos) hits->push_back(t);
        }
    }
    if (hits->size() != 1) return 0;
    *out = (*hits)[0].hwnd;
    return quality;
}

// One pass over the desktop, reused for every registration in a resolver tick:
// enumerating windows per session would be the same work several times over.
struct PanelPools {
    std::vector<WindowTarget> terminals;
    std::vector<WindowTarget> all;

    static PanelPools Snapshot() {
        PanelPools pools;
        pools.all = EnumTargets();
        for (const WindowTarget& t : pools.all) {
            if (t.process == "windowsterminal.exe") pools.terminals.push_back(t);
        }
        return pools;
    }
};

// Terminals first, everything else second. The producer is a terminal hook, so a
// Windows Terminal window is what a panel is *for*; falling back to any window
// afterwards is what makes --panel-demo usable against, say, a browser while
// developing.
int PanelResolve(const PanelPools& pools, const std::string& title, HWND* out,
                 std::string* err, std::string* candidates) {
    if (PanTrim(title).empty()) {
        if (err) *err = "name the window the panel belongs to: title";
        return 0;
    }
    const std::vector<WindowTarget>* order[2] = {&pools.terminals, &pools.all};
    for (const std::vector<WindowTarget>* pool : order) {
        if (pool->empty()) continue;
        std::vector<WindowTarget> hits;
        const int quality = PanPickWindow(*pool, title, out, &hits);
        if (quality) return quality;
        if (hits.size() > 1) {
            if (err) {
                *err = "'" + title + "' matches " + std::to_string(hits.size()) +
                       " windows";
            }
            if (candidates) *candidates = TargetsJson(hits);
            return 0;
        }
    }
    if (err) *err = "no window title matches '" + title + "' right now";
    if (candidates) {
        *candidates = TargetsJson(pools.terminals.empty() ? pools.all : pools.terminals);
    }
    return 0;
}

// For the endpoint, which answers one request and takes its own snapshot.
int ResolvePanelTarget(const std::string& title, HWND* out, std::string* err,
                       std::string* candidates) {
    return PanelResolve(PanelPools::Snapshot(), title, out, err, candidates);
}

// ── the receiving end of the beacon ─────────────────────────────────────────
// Keyed by target window, not by session or by title: the card *shown* on that
// hwnd is the context for that window, whichever registration happens to own it,
// which is the same rule the resolver uses to decide what is shown there at all.

struct SpeakingAt {
    float       level = 0.f;
    std::string cap_title, cap;
    bool        active = false;
    DWORD       heard  = 0;      // GetTickCount of the last datagram
};

std::mutex                              g_speaking_mtx;
std::unordered_map<HWND, SpeakingAt>    g_speaking;

// Its own thread, and a blocking recvfrom: an utterance's worth of datagrams
// must not wait behind a panel POST, nor hold one up.
void RunBeaconServer(int port) {
    if (!WinsockInit()) return;
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never off this machine
    addr.sin_port        = htons(static_cast<unsigned short>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "speak: speaking beacon could not take port %d\n", port);
        closesocket(fd);
        return;
    }
    for (;;) {
        SpeakBeacon msg;
        const int n = recv(fd, reinterpret_cast<char*>(&msg), sizeof(msg), 0);
        if (n == SOCKET_ERROR) break;
        if (n != static_cast<int>(sizeof(msg)) || msg.magic != kBeaconMagic ||
            msg.version != kBeaconVersion || !msg.hwnd) {
            continue;
        }
        msg.caption_title[sizeof(msg.caption_title) - 1] = 0;
        msg.caption[sizeof(msg.caption) - 1] = 0;

        SpeakingAt at;
        at.level     = Clamp01(msg.level);
        at.cap_title = msg.caption_title;
        at.cap       = msg.caption;
        at.active    = msg.active != 0;
        at.heard     = GetTickCount();
        std::lock_guard<std::mutex> lock(g_speaking_mtx);
        g_speaking[reinterpret_cast<HWND>(static_cast<uintptr_t>(msg.hwnd))] = at;
    }
    closesocket(fd);
}

// ── the panel thread ────────────────────────────────────────────────────────
// One thread owns every panel window: they are created there, drawn there, bound
// to a target there and their clicks are handled there, so nothing about a panel
// needs a lock. The endpoint registers a session and leaves a command behind; it
// resolves the title too, but only to be able to say what it resolved to *now*.

struct PanelCmd {
    bool                   remove = false;
    std::string            session;
    std::string            title;
    std::string            summary;
    std::vector<PanelItem> items;
};

std::mutex            g_pan_queue_mtx;
std::vector<PanelCmd> g_pan_queue;
std::atomic<bool>     g_pan_reassert{false};
std::atomic<bool>     g_pan_trace{false};   // --panel-demo: report what it is doing

// What GET /panels answers with, rebuilt on each resolver tick. Publishing a
// snapshot rather than locking the live registrations keeps the panel thread the
// only thing that ever touches them.
std::mutex  g_pan_snapshot_mtx;
std::string g_pan_snapshot = "[]";

void PanelEnqueue(PanelCmd cmd) {
    std::lock_guard<std::mutex> lock(g_pan_queue_mtx);
    g_pan_queue.push_back(std::move(cmd));
}

// The wheel, and why this is fussier than it looks. A layered
// WS_EX_NOACTIVATE window never has focus, and historically WM_MOUSEWHEEL went
// to the focused window rather than the one under the pointer. Windows 10 added
// "scroll inactive windows when I hover over them" and turned it on by default,
// which delivers the notch to the window under the pointer — so the wndproc
// above is the whole mechanism for almost everyone.
//
// When that setting is *off* there is no substitute for a low-level mouse hook,
// but a WH_MOUSE_LL hook is a global one: every mouse event on the machine
// round-trips through the installing thread's message queue, and a thread that
// is busy drawing makes the whole system's pointer feel late. (It did.) So the
// hook is installed only while the pointer is actually over a scrollable card,
// removed the moment it is not, and its callback does nothing but post — no
// panel state, no locks, no drawing.

#ifndef SPI_GETMOUSEWHEELROUTING
#define SPI_GETMOUSEWHEELROUTING 0x201C
#endif
#ifndef MOUSEWHEEL_ROUTING_FOCUS
#define MOUSEWHEEL_ROUTING_FOCUS 0
#endif

bool PanelWheelReachesUs() {
    UINT routing = MOUSEWHEEL_ROUTING_FOCUS + 1;   // assume the modern default
    if (!SystemParametersInfoW(SPI_GETMOUSEWHEELROUTING, 0, &routing, 0)) return true;
    return routing != MOUSEWHEEL_ROUTING_FOCUS;
}

std::atomic<HWND> g_pan_wheel_to{nullptr};

LRESULT CALLBACK PanelMouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL) {
        if (HWND to = g_pan_wheel_to.load()) {
            const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            PostMessageW(to, WM_MOUSEWHEEL,
                         MAKEWPARAM(0, GET_WHEEL_DELTA_WPARAM(m->mouseData)),
                         MAKELPARAM(m->pt.x, m->pt.y));
            return 1;   // and the terminal underneath does not scroll too
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Z-order only. Following the target's *geometry* is done by polling below: a
// LOCATIONCHANGE hook fires for every child of the terminal as it lays out and
// says nothing about minimize, cloaking or death, all of which the same poll has
// to check anyway. Foreground changes are different — they are rare, and they
// are exactly when a topmost window can end up behind something.
void CALLBACK PanelWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    g_pan_reassert.store(true);
}

void PanelDestroy(std::unique_ptr<Panel>* slot) {
    Panel* p = slot->get();
    if (p->hwnd) {
        SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(p->hwnd);
        p->hwnd = nullptr;
    }
    slot->reset();
}

// Registering is keyed by session and never touches the binding: which window a
// session is showing in is the resolver's business, and a POST that arrives while
// the session's tab is in the background must still be remembered.
//
// The collapse state is *not* taken from the payload either. New items on a
// collapsed panel bump the pill and nothing else: being interrupted by a card
// unfolding itself is the thing collapsing it was meant to stop.
void PanelApply(std::vector<std::unique_ptr<Panel>>* panels, PanelCmd&& cmd) {
    for (size_t i = panels->size(); i-- > 0;) {
        Panel* p = (*panels)[i].get();
        if (p->session != cmd.session) continue;
        if (cmd.remove) {
            PanelDestroy(&(*panels)[i]);
            panels->erase(panels->begin() + i);
            return;
        }
        p->title      = std::move(cmd.title);
        p->summary    = std::move(cmd.summary);
        p->items      = std::move(cmd.items);
        p->updated_at = std::time(nullptr);
        p->dirty      = true;
        return;
    }
    if (cmd.remove) return;

    auto up = std::make_unique<Panel>();
    up->session    = std::move(cmd.session);
    up->title      = std::move(cmd.title);
    up->summary    = std::move(cmd.summary);
    up->items      = std::move(cmd.items);
    up->updated_at = std::time(nullptr);
    panels->push_back(std::move(up));
}

// The card window, created on the first bind rather than on registration: a
// session whose tab never comes forward should cost nothing on screen.
bool PanelEnsureWindow(Panel* p) {
    if (p->hwnd) return true;
    p->hwnd = CreateWindowExW(
        // No WS_EX_TRANSPARENT: unlike the pointer, this one is clicked on.
        // WS_EX_NOACTIVATE keeps that click from stealing focus from the terminal
        // the panel is sitting in.
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"ClaudeSpeakPanel", L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!p->hwnd) {
        std::fprintf(stderr, "speak: could not create a panel window\n");
        return false;
    }
    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    return true;
}

void PanelHide(Panel* p, const char* why) {
    if (!p->shown) return;
    ShowWindow(p->hwnd, SW_HIDE);
    p->shown = false;
    if (g_pan_trace.load()) std::fprintf(stderr, "speak: panel hidden (%s)\n", why);
}

std::string PanelIso8601(time_t t) {
    char buf[32]{};
    tm   utc{};
    if (gmtime_s(&utc, &t) == 0) std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

// Re-match every registration against the desktop as it is now, then decide who
// gets which window. Two registrations can name the same window over time — that
// is what a tab switch looks like — so at most one card is shown per hwnd and the
// better match takes it.
void PanelResolveAll(std::vector<std::unique_ptr<Panel>>* panels) {
    const PanelPools pools = PanelPools::Snapshot();
    const time_t     now   = std::time(nullptr);

    // Expire first: a session that stopped posting two days ago is not a session.
    for (size_t i = panels->size(); i-- > 0;) {
        const Panel& p = *(*panels)[i];
        if (p.updated_at && std::difftime(now, p.updated_at) > kPanTtlSeconds) {
            if (g_pan_trace.load()) {
                std::fprintf(stderr, "speak: registration '%s' expired\n",
                             p.session.c_str());
            }
            PanelDestroy(&(*panels)[i]);
            panels->erase(panels->begin() + i);
        }
    }

    struct Claim {
        HWND hwnd    = nullptr;
        int  quality = 0;
    };
    std::vector<Claim> claims(panels->size());
    for (size_t i = 0; i < panels->size(); ++i) {
        Claim& c = claims[i];
        c.quality = PanelResolve(pools, (*panels)[i]->title, &c.hwnd, nullptr, nullptr);
        if (!c.quality) c.hwnd = nullptr;
    }

    std::unordered_map<HWND, size_t> best;
    for (size_t i = 0; i < claims.size(); ++i) {
        if (!claims[i].hwnd) continue;
        const auto it = best.find(claims[i].hwnd);
        if (it == best.end()) {
            best.emplace(claims[i].hwnd, i);
            continue;
        }
        const size_t k = it->second;
        const bool   i_wins =
            claims[i].quality > claims[k].quality ||
            (claims[i].quality == claims[k].quality &&
             (*panels)[i]->updated_at > (*panels)[k]->updated_at);
        if (i_wins) {
            claims[k].hwnd = nullptr;
            it->second     = i;
        } else {
            claims[i].hwnd = nullptr;
        }
    }

    std::string json = "[";
    for (size_t i = 0; i < panels->size(); ++i) {
        Panel* p = (*panels)[i].get();
        if (p->target != claims[i].hwnd) {
            p->target = claims[i].hwnd;
            p->pos    = POINT{-32000, -32000};   // force a push at the new place
            if (!p->target) {
                PanelHide(p, "no window matches its title now");
            } else if (g_pan_trace.load()) {
                std::fprintf(stderr, "speak: '%s' bound to hwnd %llu\n",
                             p->session.c_str(),
                             static_cast<unsigned long long>(
                                 reinterpret_cast<uintptr_t>(p->target)));
            }
        }
        if (i) json += ",";
        json += "{\"session\":\"" + JsonEscape(p->session) + "\"" +
                ",\"title\":\"" + JsonEscape(p->title) + "\"" +
                ",\"hwnd\":" +
                (p->target ? std::to_string(reinterpret_cast<uintptr_t>(p->target))
                           : "null") +
                ",\"resolved\":" + (p->target ? "true" : "false") +
                ",\"items\":" + std::to_string(p->items.size()) +
                ",\"collapsed\":" + (p->collapsed ? "true" : "false") +
                ",\"tab\":\"" + JsonEscape(p->tab) + "\"" +
                ",\"expanded_all\":" + (p->expanded_all ? "true" : "false") +
                ",\"speaking\":" + (p->speaking ? "true" : "false") +
                ",\"updated_at\":\"" + PanelIso8601(p->updated_at) + "\"}";
    }
    json += "]";
    {
        std::lock_guard<std::mutex> lock(g_pan_snapshot_mtx);
        g_pan_snapshot = std::move(json);
    }
}

// Runs until the process ends, or for `seconds` when --panel-demo drives it.
void PanelThread(float seconds) {
    MakeThreadDpiAware();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PanelWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = L"ClaudeSpeakPanel";
    RegisterClassExW(&wc);   // harmless if it is already there

    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, PanelWinEvent, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    std::vector<std::unique_ptr<Panel>> panels;
    // Only ever installed while the pointer is over a scrollable card, and only
    // when Windows will not route the wheel to us by itself.
    const bool wheel_arrives = PanelWheelReachesUs();
    HHOOK      mouse = nullptr;
    const DWORD started = GetTickCount();
    int         until_resolve = 0;   // ticks left before the next resolver pass

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        bool registered = false;
        {
            std::vector<PanelCmd> batch;
            {
                std::lock_guard<std::mutex> lock(g_pan_queue_mtx);
                batch.swap(g_pan_queue);
            }
            registered = !batch.empty();
            for (PanelCmd& cmd : batch) PanelApply(&panels, std::move(cmd));
        }

        // Twice a second, and immediately after a POST so a card appears as soon
        // as it is registered rather than up to half a second later. Skipped
        // entirely with nothing registered, so an idle daemon does not enumerate
        // the desktop for a living.
        if (registered || (!panels.empty() && --until_resolve <= 0)) {
            PanelResolveAll(&panels);
            until_resolve = std::max(1, kPanResolveMs / kPanTick);
        }

        const bool reassert = g_pan_reassert.exchange(false);
        POINT cursor{};
        GetCursorPos(&cursor);

        std::unordered_map<HWND, SpeakingAt> speaking;
        {
            std::lock_guard<std::mutex> lock(g_speaking_mtx);
            const DWORD now = GetTickCount();
            for (auto it = g_speaking.begin(); it != g_speaking.end();) {
                // Two seconds of silence and the entry goes: the fade below is
                // over long before that, and a client killed mid-sentence must
                // not leave a card lit.
                if (now - it->second.heard > 2000) it = g_speaking.erase(it);
                else ++it;
            }
            speaking = g_speaking;
        }

        for (size_t i = panels.size(); i-- > 0;) {
            Panel* p = panels[i].get();

            // Unbound: the session's tab is in the background, or its window is
            // gone. The registration stays either way — the card is what comes
            // and goes.
            if (!p->target || !IsWindow(p->target)) {
                PanelHide(p, "target gone or unbound");
                continue;
            }

            RECT client{};
            bool visible = IsWindowVisible(p->target) && !IsIconic(p->target) &&
                           !WindowIsCloaked(p->target) &&
                           GetClientRect(p->target, &client) &&
                           client.right - client.left > 40 && client.bottom - client.top > 40;
            if (!visible) {
                // Minimized, cloaked to another virtual desktop, or hidden: the
                // panel goes with it and comes back on restore.
                PanelHide(p, "target not showing");
                continue;
            }
            if (!PanelEnsureWindow(p)) continue;

            const float scale = PanelScaleFor(p->target);
            if (std::fabs(scale - p->scale) > 0.001f) {
                p->scale = scale;
                p->dirty = true;
            }
            // A full list is capped at a fraction of the window it sits in, so it
            // can never bury the terminal it is meant to annotate.
            const int max_h =
                static_cast<int>((client.bottom - client.top) * kPanMaxFrac);
            if (max_h != p->max_h) {
                p->max_h = max_h;
                p->dirty = true;
            }
            // A datagram inside the last quarter second means the voice is
            // still going; anything older is the tail.
            bool  live  = false;
            float voice = 0.f;
            const auto found = speaking.find(p->target);
            if (found != speaking.end()) {
                live  = found->second.active &&
                        GetTickCount() - found->second.heard < 250;
                voice = found->second.level;
                if (live && (p->cap_title != found->second.cap_title ||
                             p->cap != found->second.cap)) {
                    p->cap_title = found->second.cap_title;
                    p->cap       = found->second.cap;
                    p->dirty     = true;
                }
            }
            // A few frames to rise, and a *linear* 1.5 s to fall — an
            // exponential ease never reaches zero, which is how a card was left
            // faintly lit for the better part of ten seconds after the voice had
            // stopped.
            if (live) p->glow += (1.f - p->glow) * 0.25f;
            else      p->glow = std::max(0.f, p->glow - kPanTick / 1500.f);
            p->voice += (voice * (live ? 1.f : 0.f) - p->voice) * 0.35f;
            if (p->speaking != live) {
                p->speaking = live;
                // Coming back: the summary returns to the header line.
                if (!live) {
                    p->cap_title.clear();
                    p->cap.clear();
                    p->dirty = true;
                }
            }

            bool redrawn = false;
            if (p->dirty || !p->bits) {
                PanelRender(p);
                redrawn = true;
            }
            if (!p->card.w) continue;

            // Inside the client area, so the card never sits on the tab bar or
            // hangs off a maximized window onto the taskbar.
            POINT bottom_right{client.right, client.bottom};
            POINT top_left{client.left, client.top};
            ClientToScreen(p->target, &bottom_right);
            ClientToScreen(p->target, &top_left);
            g_pan_scale = p->scale;
            const int inset = PanScale(kPanMargin);
            POINT pos{bottom_right.x - inset - p->card.margin - p->card.panel_w,
                      bottom_right.y - inset - p->card.margin - p->card.panel_h};
            pos.x = std::max(pos.x, top_left.x - p->card.margin);
            pos.y = std::max(pos.y, top_left.y - p->card.margin);
            const bool moved = pos.x != p->pos.x || pos.y != p->pos.y;
            if (moved && g_pan_trace.load()) {
                std::fprintf(stderr, "speak: panel follows target to %ld,%ld\n",
                             pos.x, pos.y);
            }
            p->pos = pos;

            // Hover from the cursor rather than from WM_MOUSEMOVE: it needs no
            // WM_MOUSELEAVE tracking, and WindowFromPoint is also the occlusion
            // test — a row does not light up through whatever is covering it.
            PanHit hover{};
            if (WindowFromPoint(cursor) == p->hwnd) hover = PanelHitAt(p, cursor);
            if (hover != p->hover) {
                p->hover = hover;
                PanelCompose(p);
                redrawn = true;
            }
            // The glow moves every frame, which is a recompose and never a
            // rebuild: no text is re-measured to make a card breathe. The
            // `glow_drawn` half matters as much — without one last frame after it
            // reaches zero, the card keeps whatever halo was on the last one
            // pushed, forever.
            const bool lit = p->glow > 0.f;
            if ((lit || p->glow_drawn) && !redrawn) {
                PanelCompose(p);
                redrawn = true;
            }
            p->glow_drawn = lit;

            const bool first = !p->shown;
            if (moved || redrawn || first) PanelPush(p);
            if (first) {
                ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
                p->shown = true;
                if (g_pan_trace.load()) {
                    std::fprintf(stderr, "speak: panel shown at %ld,%ld (%dx%d)\n",
                                 pos.x, pos.y, p->card.panel_w, p->card.panel_h);
                }
            }
            if (reassert || first || moved) {
                SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }

        // The hover pass above already knows whether the pointer is on a card
        // that can scroll; that is exactly the window in which a global hook is
        // worth having, and no wider.
        HWND wheel_to = nullptr;
        if (!wheel_arrives) {
            for (std::unique_ptr<Panel>& up : panels) {
                if (up->shown && up->card.scroll_max > 0 &&
                    up->hover.kind != PanHit::Kind::None) {
                    wheel_to = up->hwnd;
                    break;
                }
            }
        }
        g_pan_wheel_to.store(wheel_to);
        if (wheel_to && !mouse) {
            mouse = SetWindowsHookExW(WH_MOUSE_LL, PanelMouseHook,
                                      GetModuleHandleW(nullptr), 0);
        } else if (!wheel_to && mouse) {
            UnhookWindowsHookEx(mouse);
            mouse = nullptr;
        }

        if (seconds > 0.f &&
            GetTickCount() - started >= static_cast<DWORD>(seconds * 1000.f)) {
            break;
        }
        Sleep(kPanTick);
    }

    if (mouse) UnhookWindowsHookEx(mouse);
    g_pan_wheel_to.store(nullptr);
    for (size_t i = panels.size(); i-- > 0;) PanelDestroy(&panels[i]);
    panels.clear();
    if (hook) UnhookWinEvent(hook);
}

std::once_flag g_pan_thread_once;

// Started by the first panel that needs it, so a daemon nobody posts a panel to
// never creates the thread, the class or the hook.
void EnsurePanelThread() {
    std::call_once(g_pan_thread_once, [] {
        std::thread(PanelThread, 0.f).detach();
    });
}

// ── the endpoint ────────────────────────────────────────────────────────────

std::string PanelUrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::string PanelQueryParam(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return {};
    std::string query = path.substr(q + 1);
    size_t      pos   = 0;
    while (pos <= query.size()) {
        const size_t amp  = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos
                                                       ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return PanelUrlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string PanelPathOnly(const std::string& path) {
    const size_t q = path.find('?');
    return q == std::string::npos ? path : path.substr(0, q);
}

// The producer already knows the URL in every case it matters, but a hook that
// only scraped `owner/repo#N` out of a transcript should not have to build one.
std::string PanelGuessUrl(const PanelItem& item) {
    if (item.repo.find('/') == std::string::npos || item.number <= 0) return {};
    const std::string kind = item.kind == "pr" ? "pull" : "issues";
    return "https://github.com/" + item.repo + "/" + kind + "/" +
           std::to_string(item.number);
}

bool ParsePanelItems(const JsonVal& arr, std::vector<PanelItem>* out, std::string* err) {
    if (arr.t != JsonVal::T::Arr) {
        *err = "items must be an array";
        return false;
    }
    for (const JsonVal& v : arr.arr) {
        if (v.t != JsonVal::T::Obj) {
            *err = "every item must be an object";
            return false;
        }
        PanelItem item;
        item.kind   = v.GetStr("kind");
        item.repo   = v.GetStr("repo");
        item.url    = v.GetStr("url");
        item.title  = PanTrim(v.GetStr("title"));
        item.number = v.GetNum("number", 0);
        item.status = ParsePanStatus(v.GetStr("status"));
        if (item.url.empty()) item.url = PanelGuessUrl(item);
        if (item.Label().empty() && item.title.empty()) continue;   // nothing to draw
        out->push_back(std::move(item));
        if (out->size() >= 200) break;   // a list this long is a bug at the producer
    }
    return true;
}

void HandlePanelPost(SOCKET fd, const std::string& body,
                     void (*respond)(SOCKET, int, const std::string&)) {
    JsonVal root;
    if (!JsonParse(body, &root) || root.t != JsonVal::T::Obj) {
        respond(fd, 400, "{\"ok\":false,\"error\":\"body must be a JSON object\"}");
        return;
    }
    const std::string session = PanTrim(root.GetStr("session"));
    if (session.empty()) {
        respond(fd, 400, "{\"ok\":false,\"error\":\"name the session: session\"}");
        return;
    }

    std::vector<PanelItem> items;
    if (const JsonVal* arr = root.Find("items")) {
        std::string err;
        if (!ParsePanelItems(*arr, &items, &err)) {
            respond(fd, 400, "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}");
            return;
        }
    }
    // Nothing to show is how a session says goodbye — no need to make a hook
    // remember to DELETE when its last pull request merges.
    if (items.empty()) {
        PanelCmd cmd;
        cmd.remove  = true;
        cmd.session = session;
        PanelEnqueue(std::move(cmd));
        EnsurePanelThread();
        respond(fd, 200, "{\"ok\":true,\"removed\":true}");
        return;
    }

    const std::string title = PanTrim(root.GetStr("title"));
    if (title.empty()) {
        respond(fd, 400,
                "{\"ok\":false,\"error\":\"name the window the panel belongs to: title\"}");
        return;
    }

    // Registering always succeeds. Whether the title matches a window *right now*
    // is a separate question, and one whose answer changes: a Windows Terminal
    // window shows its active tab's title, so a session sitting in a background
    // tab matches nothing until its tab comes forward. Failing the POST for that
    // would throw the payload away over a transient — so this reports it and the
    // resolver keeps trying.
    HWND        target = nullptr;
    std::string err, candidates;
    ResolvePanelTarget(title, &target, &err, &candidates);

    PanelCmd cmd;
    cmd.session = session;
    cmd.title   = title;
    cmd.summary = PanTrim(root.GetStr("summary"));
    cmd.items   = std::move(items);
    PanelEnqueue(std::move(cmd));
    EnsurePanelThread();

    std::string out = "{\"ok\":true,\"hwnd\":";
    if (target) {
        out += std::to_string(reinterpret_cast<uintptr_t>(target)) + ",\"resolved\":true}";
    } else {
        out += "null,\"resolved\":false,\"error\":\"" + JsonEscape(err) + "\"";
        if (!candidates.empty()) out += ",\"candidates\":" + candidates;
        out += "}";
    }
    respond(fd, 200, out);
}

void HandlePanelList(SOCKET fd, void (*respond)(SOCKET, int, const std::string&)) {
    std::lock_guard<std::mutex> lock(g_pan_snapshot_mtx);
    respond(fd, 200, g_pan_snapshot);
}

void HandlePanelDelete(SOCKET fd, const std::string& path, const std::string& body,
                       void (*respond)(SOCKET, int, const std::string&)) {
    std::string session = PanelQueryParam(path, "session");
    if (session.empty() && !body.empty()) {
        JsonVal root;
        if (JsonParse(body, &root)) session = root.GetStr("session");
    }
    session = PanTrim(session);
    if (session.empty()) {
        respond(fd, 400, "{\"ok\":false,\"error\":\"which panel? DELETE /panel?session=<id>\"}");
        return;
    }
    PanelCmd cmd;
    cmd.remove  = true;
    cmd.session = session;
    PanelEnqueue(std::move(cmd));
    EnsurePanelThread();
    respond(fd, 200, "{\"ok\":true,\"removed\":true}");
}

// ── previewing and demoing it ───────────────────────────────────────────────
// The overlays are invisible to GDI screen capture, so the only way to review
// how one looks is to render it. Sample data on purpose covers every badge, a
// path row, a title long enough to be cut, and one item too many so the "+N
// more" row appears.

const char* kPanSampleSummary = "i47 - wiring the attention panel into the daemon";
constexpr size_t kPanSampleRows = 10;   // the sample list, so a preview can point
                                        // at its last row

std::vector<PanelItem> SamplePanelItems() {
    const auto make = [](const char* kind, const char* repo, long number,
                         const char* title, PanStatus status, const char* url = "") {
        PanelItem it;
        it.kind   = kind;
        it.repo   = repo;
        it.number = number;
        it.title  = title;
        it.status = status;
        it.url    = *url ? std::string(url) : PanelGuessUrl(it);
        return it;
    };
    return {
        // Questions sort to the top of `All` on their own, but the producer sends
        // them in whatever order it found them — so here they are not first.
        make("question", "", 0,
             "Should the quantity step apply to archived prices too?",
             PanStatus::Open),
        make("pr", "optidatacloud/laravel-opticloud", 1375,
             "feat: calendar event reminder as a bottom-right toast",
             PanStatus::ChecksFailing),
        make("pr", "optidatacloud/optiwork-api-gateway", 1758,
             "fix: quantity step on the price table", PanStatus::Approved),
        make("issue", "optidatacloud/optiwork-ai", 377,
             "MCP P14: tool surface parity with the chat tools",
             PanStatus::ChangesRequested),
        make("issue", "optidatacloud/laravel-opticloud", 1372,
             "Feature highlights popover regressions", PanStatus::Open),
        make("pr", "optidatacloud/o-cli", 101, "feat: fmt laravel v2",
             PanStatus::Merged),
        make("path", "", 0, "1372-toast", PanStatus::Unknown,
             "C:\\Dev\\field\\work\\laravel-opticloud\\1372-toast"),
        // Past the sixth: these two are what the "+N more" row stands for.
        make("pr", "dovyski/claude-speak", 12, "feat: the attention panel",
             PanStatus::Draft),
        make("issue", "optidatacloud/optiwork-infra", 27,
             "bastion runbook", PanStatus::Closed),
        make("question", "", 0,
             "Merge the partners PR before or after the gateway one?",
             PanStatus::Open),
    };
}

void PanelPreview(const std::string& prefix) {
    // Per-monitor aware first, so the preview is drawn at the scale the screen
    // actually uses rather than at a virtualized 96 dpi — the point is to review
    // what will be on the glass.
    MakeThreadDpiAware();
    const float scale = PanelScaleFor(nullptr);
    const std::vector<PanelItem> items = SamplePanelItems();
    struct Shot {
        const char* suffix;
        const char* tab;
        bool        collapsed;
        bool        all;
        int         hover;    // row to light up, -3 for none
        float       glow;     // the speaking halo, 0..1
        float       voice;    // the amplitude driving it
        const char* cap_title;
        const char* cap;
    };
    const Shot shots[] = {
        {"-expanded.png",  "all", false, false, -3, 0.f, 0.f, "", ""},
        {"-hover.png",     "all", false, false, 1,  0.f, 0.f, "", ""},
        {"-collapsed.png", "all", true,  false, -3, 0.f, 0.f, "", ""},
        // Everything, with the way back on the last row — and that row hovered,
        // since being clickable is the whole point of it.
        {"-all.png",       "all", false, true,  static_cast<int>(kPanSampleRows),
         0.f, 0.f, "", ""},
        {"-tabs.png",      "prs", false, false, -3, 0.f, 0.f, "", ""},
        {"-pending.png",   "pending", false, false, -3, 0.f, 0.f, "", ""},
        // Mid-utterance: lit, and the header carrying what is being said.
        {"-speaking.png",  "all", false, false, -3, 1.f, 0.85f,
         "laravel-opticloud #1375", "checks are green, ready to merge"},
    };
    for (const Shot& shot : shots) {
        PanelCard card;
        int       scroll = 0;
        BuildPanelCard(&card, kPanSampleSummary, items, shot.collapsed, shot.tab,
                       shot.all, scale, 0, &scroll, shot.cap_title, shot.cap);
        std::vector<uint32_t> px(static_cast<size_t>(card.w) * card.h);
        const RECT* hover = nullptr;
        if (shot.hover >= 0 && shot.hover < static_cast<int>(card.rows.size())) {
            hover = &card.rows[shot.hover].hit;
        }
        ComposePanel(px.data(), card, hover, 1.f, shot.glow, shot.voice);
        // Timed with the geometry warm: the first number is what a content change
        // costs, the second what a hover or a glow frame costs.
        LARGE_INTEGER f{}, a{}, b{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        BuildPanelCard(&card, kPanSampleSummary, items, shot.collapsed, shot.tab,
                       shot.all, scale, 0, &scroll, shot.cap_title, shot.cap);
        QueryPerformanceCounter(&b);
        ComposePanel(px.data(), card, hover, 1.f, shot.glow, shot.voice);
        QueryPerformanceCounter(&c);
        const std::string path = prefix + shot.suffix;
        WritePng(path, px, card.w, card.h);
        std::printf("%s (%dx%d)  build %.2f ms, compose %.2f ms\n",
                    path.c_str(), card.w, card.h,
                    1000.0 * (b.QuadPart - a.QuadPart) / f.QuadPart,
                    1000.0 * (c.QuadPart - b.QuadPart) / f.QuadPart);
    }
}

// A throwaway panel against a real window, with no daemon and no producer: the
// only way to check that it follows a move, hides on minimize and opens a row.
int PanelDemo(const std::string& title, float seconds) {
    MakeThreadDpiAware();
    if (PanTrim(title).empty()) {
        std::fprintf(stderr, "speak: --panel-demo needs --title\n");
        return 2;
    }
    // Resolved here only so the run says what it started on; the demo goes
    // through the same resolver as a real panel, so a title that matches nothing
    // yet is not an error — retitle the window and the card turns up.
    HWND        target = nullptr;
    std::string err, candidates;
    if (ResolvePanelTarget(title, &target, &err, &candidates)) {
        wchar_t got[512]{};
        GetWindowTextW(target, got, 512);
        std::printf("panel demo on hwnd %llu (\"%s\") for %.0f s\n",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
                    Utf8(got).c_str(), seconds);
    } else {
        std::printf("panel demo waiting for a window matching \"%s\" (%s), %.0f s\n",
                    title.c_str(), err.c_str(), seconds);
    }

    PanelCmd cmd;
    cmd.session = "panel-demo";
    cmd.title   = title;
    cmd.summary = kPanSampleSummary;
    cmd.items   = SamplePanelItems();
    PanelEnqueue(std::move(cmd));
    g_pan_trace.store(true);
    PanelThread(seconds);   // inline: this call *is* the demo
    return 0;
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
    // The attention panel. Unlike /point this returns as soon as the panel is
    // queued: it is a thing that stays on screen, not a gesture to wait out.
    if (req.method == "GET" && PanelPathOnly(req.path) == "/panels") {
        HandlePanelList(fd, PointHttpRespond);
        return;
    }
    if (PanelPathOnly(req.path) == "/panel") {
        if (req.method == "POST") {
            HandlePanelPost(fd, req.body, PointHttpRespond);
        } else if (req.method == "DELETE") {
            HandlePanelDelete(fd, req.path, req.body, PointHttpRespond);
        } else {
            PointHttpRespond(fd, 404, "{\"ok\":false,\"error\":\"try POST /panel or "
                                      "DELETE /panel?session=<id>\"}");
        }
        return;
    }
    if (req.method != "POST" || req.path != "/point") {
        PointHttpRespond(fd, 404, "{\"ok\":false,\"error\":\"try GET /targets, "
                                  "GET /panels, POST /point or POST /panel\"}");
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
    if (JsonGetNumber(req.body, "pulses", &v))   pr.pulses = static_cast<int>(v);
    if (JsonGetNumber(req.body, "size", &v))     pr.size   = static_cast<int>(v);
    if (JsonGetNumber(req.body, "duration", &v)) pr.duration = static_cast<float>(v);

    std::string colour = pocket_tts::json_get_string(req.body, "color");
    if (colour.empty()) colour = pocket_tts::json_get_string(req.body, "colour");
    if (!colour.empty()) {
        if (!ParseColour(colour, &pr.colour)) {
            PointHttpRespond(fd, 400, "{\"ok\":false,\"error\":\"colour '" +
                                          JsonEscape(colour) +
                                          "' is not a name, #rrggbb or r,g,b\"}");
            return;
        }
        pr.have_colour = true;
    }

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
    std::string panel_preview;  // render the attention panel to <prefix>-*.png
    bool   panel_demo  = false;   // a throwaway panel on --title, for review
    float  panel_secs  = 20.f;    // how long --panel-demo lasts
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
            // And the speaking beacon on the one after that: UDP, so a 60 Hz
            // stream of level updates cannot queue behind a /panel POST.
            std::thread(RunBeaconServer, point_port + 1).detach();
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
        "  speak --list-targets          the windows --title can match, as JSON\n"
        "  speak --panel-demo --title T  park a sample attention panel in it\n\n"
        "  Click the orb to pause while speaking, click again to resume.\n"
        "  Click the caption's × to dismiss it.\n\n"
        "  --voice <name|path>   voice sample (default: jarvis.wav)\n"
        "  --save <file.wav>     also save the audio\n"
        "  --no-orb              skip the on-screen indicator\n"
        "  --caption <text>      one short line of context shown as a toast to the\n"
        "                        left of the orb (the repo, the issue, the task)\n"
        "  --caption-title <t>   the caption's title line, above that text\n"
        "  --caption-variant <v> the toast's colour: primary, secondary, success,\n"
        "                        danger, warning, info, light or dark (default light)\n"
        "  --caption-icon <i>    override the variant's icon: none, check, info,\n"
        "                        warn, ban or dot\n"
        "  --caption-opacity <n> how solid the toast is, 0..100 (default 100)\n"
        "  --subtitle <text>     bare text in the same strip as the toast — no card,\n"
        "                        no icon, no title. Stacks under a --caption if both\n"
        "                        are given\n"
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
        "  --duration <s>        how long the whole gesture lasts, in seconds\n"
        "                        (default 3.5 for 3 rings; the pacing scales)\n"
        "  --color <c>           colour to build the rings from: a name (red,\n"
        "                        amber, cyan, ...), #rrggbb, or r,g,b\n"
        "  --size <px>           pointer square size (default 320)\n"
        "                        (--point-* also works for these four)\n"
        "  --point-preview <pfx> render a strip of pointer frames and exit\n"
        "  --panel-preview <pfx> render the attention panel to <pfx>-expanded.png,\n"
        "                        -hover.png and -collapsed.png, and exit\n"
        "  --panel-demo          park a sample panel in the bottom-right of the\n"
        "                        --title window, with no daemon and no producer\n"
        "  --panel-seconds <s>   how long --panel-demo lasts (default 20)\n"
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
        else if (a == "--pulses" || a == "--point-pulses")
            opt.point_req.pulses = std::max(1, std::atoi(next(a.c_str()).c_str()));
        else if (a == "--duration" || a == "--point-duration")
            opt.point_req.duration = std::strtof(next(a.c_str()).c_str(), nullptr);
        else if (a == "--color" || a == "--colour" ||
                 a == "--point-color" || a == "--point-colour") {
            const std::string v = next(a.c_str());
            if (!ParseColour(v, &opt.point_req.colour)) {
                std::fprintf(stderr, "speak: '%s' is not a colour name, #rrggbb or r,g,b\n",
                             v.c_str());
                return 2;
            }
            opt.point_req.have_colour = true;
        }
        else if (a == "--size" || a == "--point-size")
            g_point_size = std::max(80, std::atoi(next(a.c_str()).c_str()));
        else if (a == "--point-preview") opt.point_preview = next("--point-preview");
        else if (a == "--panel-preview") opt.panel_preview = next("--panel-preview");
        else if (a == "--panel-demo")    opt.panel_demo = true;
        else if (a == "--panel-seconds")
            opt.panel_secs = std::max(1.f,
                std::strtof(next("--panel-seconds").c_str(), nullptr));
        else if (a == "--point-port")  opt.point_port = std::atoi(next("--point-port").c_str());
        else if (a == "--no-point-server") opt.point_server = false;
        else if (a == "--caption")       g_cap_text  = next("--caption");
        else if (a == "--caption-title") g_cap_title = next("--caption-title");
        else if (a == "--subtitle")      g_sub_text  = next("--subtitle");
        else if (a == "--caption-variant") {
            const std::string name = next("--caption-variant");
            const CapVariant* v = FindCapVariant(name);
            if (!v) {
                std::fprintf(stderr, "speak: unknown caption variant '%s' (primary|"
                             "secondary|success|danger|warning|info|light|dark)\n",
                             name.c_str());
                return 2;
            }
            g_cap_variant = v;
        }
        else if (a == "--caption-opacity") {
            const std::string v = next("--caption-opacity");
            const int pct = std::atoi(v.c_str());
            if (pct < 0 || pct > 100) {
                std::fprintf(stderr, "speak: --caption-opacity wants 0..100, got '%s'\n",
                             v.c_str());
                return 2;
            }
            g_cap_opacity = pct / 100.f;
        }
        else if (a == "--caption-icon") {
            const std::string name = next("--caption-icon");
            CapIcon icon{};
            if (!ParseCapIcon(name, &icon)) {
                std::fprintf(stderr, "speak: unknown caption icon '%s' (none|check|"
                             "info|warn|ban|dot)\n", name.c_str());
                return 2;
            }
            g_cap_icon = static_cast<int>(icon);
        }
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

    // Rasterized once here, before anything can render a frame: --orb-size is
    // settled by now and the text never changes after this point.
    BuildCaptionCard();
    BuildSubtitle();

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
        const int  pulses = ApplyPointStyle(opt.point_req);
        const float total = PointerDuration(pulses);
        for (int i = 0; i < kFrames; ++i) {
            std::vector<uint32_t> px(static_cast<size_t>(S) * S);
            ComposePointer(px.data(), S, total * (i + 0.5f) / kFrames, pulses);
            WriteBmp(opt.point_preview + std::to_string(i) + ".bmp", std::move(px), S, S);
        }
        std::printf("wrote %d frames to %s0..%d.bmp\n", kFrames,
                    opt.point_preview.c_str(), kFrames - 1);
        return 0;
    }
    if (!opt.panel_preview.empty()) {
        PanelPreview(opt.panel_preview);
        return 0;
    }
    if (opt.panel_demo) return PanelDemo(opt.point_req.title, opt.panel_secs);
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

    // Aimed at a window? Then a panel parked in that window's corner should
    // glow with this utterance. The handle has to be known before the first
    // sample, so it is resolved here rather than at the pointing call afterwards
    // — and only the handle is needed, so no DPI-aware thread is involved.
    if (opt.point || opt.point_req.hwnd || !opt.point_req.title.empty()) {
        g_beacon_port = (opt.point_port ? opt.point_port : opt.port + 1) + 1;
        g_beacon_hwnd.store(
            reinterpret_cast<uintptr_t>(ResolveTargetHwnd(opt.point_req)));
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
