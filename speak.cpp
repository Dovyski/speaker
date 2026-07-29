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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── PocketTTS.cpp C API (defined in the include above) ──────────────────────
extern "C" {
void*  ptt_create(const char* models_dir, const char* voices_dir,
                  const char* tokenizer_path, const char* precision,
                  float temperature, int lsd_steps, int num_threads);
void   ptt_destroy(void* handle);
void   ptt_free_audio(float* samples);
void*  ptt_stream_start(void* handle, const char* text, const char* voice);
int    ptt_stream_read(void* stream_ctx, float** out_samples, int* out_len);
void   ptt_stream_end(void* stream_ctx);
}

namespace {

constexpr int    kSampleRate  = 24000;
constexpr size_t kEnvBlock    = 256;   // frames per amplitude-envelope entry
constexpr int    kDefaultPort = 8123;

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

constexpr int kOrbSize = 180;

struct Rgb { float r, g, b; };
constexpr Rgb kCoreIn  {1.00f, 0.86f, 0.78f};   // hot centre
constexpr Rgb kCoreOut {0.85f, 0.42f, 0.26f};   // core rim
constexpr Rgb kGlow    {0.88f, 0.48f, 0.31f};   // outer halo

float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

float SmoothStep(float edge0, float edge1, float x) {
    float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.f - 2.f * t);
}

LRESULT CALLBACK OrbWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Composes one frame of the orb into a premultiplied-alpha BGRA buffer.
// `level` is 0..1 loudness, `fade` is 0..1 opacity.
void ComposeOrb(uint32_t* pixels, float level, float fade) {
    const float cx = kOrbSize * 0.5f, cy = kOrbSize * 0.5f;
    const float r  = 20.f + 12.f * level;              // solid core radius
    const float R  = r + 34.f + 22.f * level;          // halo radius

    for (int y = 0; y < kOrbSize; ++y) {
        for (int x = 0; x < kOrbSize; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);

            const float core = 1.f - SmoothStep(r - 1.5f, r + 0.75f, d);
            float halo = 0.f;
            if (d > r && d < R) {
                const float u = 1.f - (d - r) / (R - r);
                halo = 0.55f * u * u * u;
            }
            float a = Clamp01(core + (1.f - core) * halo);
            if (a <= 0.f) { pixels[y * kOrbSize + x] = 0; continue; }

            // core: hot centre → rim, then rim → halo colour outside the core
            const float t = std::min(1.f, d / std::max(r, 1.f));
            Rgb c{kCoreIn.r + (kCoreOut.r - kCoreIn.r) * t,
                  kCoreIn.g + (kCoreOut.g - kCoreIn.g) * t,
                  kCoreIn.b + (kCoreOut.b - kCoreIn.b) * t};
            if (core < 1.f) {
                const float m = 1.f - core;
                c = {c.r + (kGlow.r - c.r) * m, c.g + (kGlow.g - c.g) * m,
                     c.b + (kGlow.b - c.b) * m};
            }

            a *= fade;
            const auto ch = [a](float v) {
                return static_cast<uint32_t>(Clamp01(v) * a * 255.f + 0.5f);
            };
            pixels[y * kOrbSize + x] = (static_cast<uint32_t>(a * 255.f + 0.5f) << 24) |
                                       (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
        }
    }
}

// Pushes the composed frame to the layered window, parked just inside the
// bottom-right corner of the work area (above the taskbar).
void PushOrb(HWND hwnd, HDC mem_dc) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT pos{work.right - kOrbSize - 24, work.bottom - kOrbSize - 24};
    SIZE  size{kOrbSize, kOrbSize};
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
        wc.lpszClassName, L"", WS_POPUP, 0, 0, kOrbSize, kOrbSize, nullptr,
        nullptr, wc.hInstance, nullptr);
    if (!hwnd) return;

    HDC screen = GetDC(nullptr);
    HDC mem_dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = kOrbSize;
    bi.bmiHeader.biHeight      = -kOrbSize;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem_dc, dib);
    auto* pixels = static_cast<uint32_t*>(bits);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    float level = 0.f, fade = 0.f;
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
        // Keep it alive between words with a slow breath.
        const float breath = 0.10f + 0.06f * std::sin(frame * 0.09f);
        target = std::max(Clamp01(target * 1.6f), breath);
        level += (target - level) * 0.35f;

        if (g_audio_done.load()) closing = true;
        fade += ((closing ? 0.f : 1.f) - fade) * (closing ? 0.12f : 0.22f);
        if (closing && fade < 0.01f) break;

        ComposeOrb(pixels, level, fade);
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

// In-process synthesis through the ptt_* streaming API.
class LocalSource : public PcmSource {
public:
    explicit LocalSource(void* stream) : stream_(stream) {}
    bool Next(std::vector<float>* out) override {
        float* samples = nullptr;
        int    len     = 0;
        if (ptt_stream_read(stream_, &samples, &len) != 1 || len <= 0) return false;
        out->assign(samples, samples + len);
        ptt_free_audio(samples);
        return true;
    }
private:
    void* stream_;
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
                source_done = true;
                chunk.clear();
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
void DumpOrbFrame(const std::string& path, float level) {
    std::vector<uint32_t> px(kOrbSize * kOrbSize);
    ComposeOrb(px.data(), level, 1.0f);

    const uint32_t data_bytes = static_cast<uint32_t>(px.size() * 4);
    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize    = fh.bfOffBits + data_bytes;
    BITMAPINFOHEADER ih{};
    ih.biSize     = sizeof(ih);
    ih.biWidth    = kOrbSize;
    ih.biHeight   = -kOrbSize;  // top-down
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
    bool   show_orb    = true;
    bool   timing      = false;
    bool   auto_serve  = true;   // warm a daemon in the background on a cold call
    bool   use_daemon  = true;   // try the daemon first
    float  temperature = 0.7f;
    int    threads     = 0;
    int    port        = kDefaultPort;
};

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

    pocket_tts::Config cfg;
    cfg.models_dir     = opt.models_dir;
    cfg.voices_dir     = opt.voices_dir;
    cfg.tokenizer_path = opt.models_dir + "\\tokenizer.model";
    cfg.precision      = "int8";
    cfg.temperature    = opt.temperature;
    cfg.num_threads    = opt.threads;

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
                      " --voices-dir \"" + opt.voices_dir + "\"";
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
        "  --dump-orb <f.bmp>    render one orb frame to a BMP and exit\n"
        "  --local               never use the daemon; synthesize in-process\n"
        "  --no-auto-serve       do not start a daemon in the background\n"
        "  --timing              report time to first audio\n"
        "  --port <n>            daemon port (default 8123)\n"
        "  --temperature <f>     sampling temperature (default 0.7)\n"
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
        else if (a == "--threads")     opt.threads = std::atoi(next("--threads").c_str());
        else if (a == "--port")        opt.port = std::atoi(next("--port").c_str());
        else if (a == "--no-orb")      opt.show_orb = false;
        else if (a == "--timing")      opt.timing = true;
        else if (a == "--local")       opt.use_daemon = false;
        else if (a == "--no-auto-serve") opt.auto_serve = false;
        else if (a == "--serve")       serve = true;
        else if (a == "--stop")        stop = true;
        else if (a == "--status")      status = true;
        else if (a == "--dump-orb") {
            DumpOrbFrame(next("--dump-orb"), 0.65f);
            return 0;
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
    void* tts    = nullptr;
    void* stream = nullptr;
    if (!source) {
        if (opt.use_daemon && opt.auto_serve && !DaemonRegistered(opt.port)) {
            SpawnDaemon(opt);
        }
        const std::string tokenizer = opt.models_dir + "\\tokenizer.model";
        tts = ptt_create(opt.models_dir.c_str(), opt.voices_dir.c_str(),
                         tokenizer.c_str(), "int8", opt.temperature, 1, opt.threads);
        if (!tts) {
            std::fprintf(stderr, "speak: could not load models from %s\n",
                         opt.models_dir.c_str());
            CoUninitialize();
            return 1;
        }
        stream = ptt_stream_start(tts, opt.text.c_str(), opt.voice.c_str());
        if (!stream) {
            std::fprintf(stderr, "speak: could not start synthesis\n");
            ptt_destroy(tts);
            CoUninitialize();
            return 1;
        }
        source = std::make_unique<LocalSource>(stream);
    }

    std::thread orb;
    if (opt.show_orb) orb = std::thread(OrbThread);

    std::vector<float> recorded;
    double first_audio_ms = 0;
    const bool ok = PlayStream(source.get(), opt.save_path.empty() ? nullptr : &recorded,
                               &first_audio_ms);
    source.reset();          // closes the socket / joins the generator thread
    if (stream) ptt_stream_end(stream);
    if (ok && !opt.save_path.empty()) WriteWav(opt.save_path, recorded);

    g_audio_done.store(true);
    if (orb.joinable()) orb.join();

    if (tts) ptt_destroy(tts);
    CoUninitialize();

    if (opt.timing) {
        std::fprintf(stderr, "speak: first audio in %.0f ms (%s)\n", first_audio_ms,
                     stream ? "local" : "daemon");
    }
    if (!ok && !daemon_err.empty() && opt.use_daemon) {
        std::fprintf(stderr, "speak: daemon path failed (%s)\n", daemon_err.c_str());
    }
    return ok ? 0 : 1;
}
