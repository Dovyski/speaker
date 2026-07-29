// ────────────────────────────────────────────────────────────────────────────
// speak.exe — self-contained text-to-speech with an on-screen pulsing orb.
//
// Links PocketTTS.cpp's C API directly (no HTTP, no Python), renders audio
// through WASAPI and shows a click-through layered window that pulses with the
// amplitude of whatever is playing right now.
//
//   speak.exe "Hello world."
//   speak.exe --voice alba.wav --save out.wav "Hello world."
// ────────────────────────────────────────────────────────────────────────────

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── PocketTTS.cpp C API (compiled into this binary) ─────────────────────────
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

constexpr int    kSampleRate = 24000;
constexpr size_t kEnvBlock   = 256;   // frames per amplitude-envelope entry

// Shared state between the audio writer and the orb renderer.
std::atomic<uint64_t> g_play_pos{0};      // frames handed to the speakers
std::atomic<bool>     g_audio_done{false};
std::mutex            g_env_mtx;
std::vector<float>    g_env;             // peak amplitude per kEnvBlock frames

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

// ── WASAPI playback ─────────────────────────────────────────────────────────

// Renders mono 24 kHz float audio pulled from `stream` to the default device,
// recording an amplitude envelope and the live playback position as it goes.
// Returns false on a fatal audio error.
bool PlayStream(void* stream, std::vector<float>* recorded) {
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

    uint64_t written = 0;
    float*   chunk = nullptr;
    int      chunk_len = 0, chunk_pos = 0;
    bool     source_done = false;

    while (true) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        g_play_pos.store(written > padding ? written - padding : 0);

        if (!chunk && !source_done) {
            if (ptt_stream_read(stream, &chunk, &chunk_len) == 1 && chunk_len > 0) {
                chunk_pos = 0;
                // Envelope: peak per block, for the orb.
                std::lock_guard<std::mutex> lock(g_env_mtx);
                for (int i = 0; i < chunk_len; i += static_cast<int>(kEnvBlock)) {
                    float peak = 0.f;
                    const int end = std::min<int>(i + kEnvBlock, chunk_len);
                    for (int j = i; j < end; ++j) peak = std::max(peak, std::fabs(chunk[j]));
                    g_env.push_back(peak);
                }
                if (recorded) recorded->insert(recorded->end(), chunk, chunk + chunk_len);
            } else {
                source_done = true;
                if (chunk) { ptt_free_audio(chunk); chunk = nullptr; }
            }
        }

        if (chunk) {
            const UINT32 space = buffer_frames - padding;
            if (space > 0) {
                const UINT32 n = std::min<UINT32>(space, chunk_len - chunk_pos);
                BYTE* dst = nullptr;
                if (SUCCEEDED(render->GetBuffer(n, &dst))) {
                    std::memcpy(dst, chunk + chunk_pos, n * sizeof(float));
                    render->ReleaseBuffer(n, 0);
                    written += n;
                    chunk_pos += static_cast<int>(n);
                }
                if (chunk_pos >= chunk_len) {
                    ptt_free_audio(chunk);
                    chunk = nullptr;
                }
                continue;   // keep the buffer topped up before sleeping
            }
        } else if (source_done) {
            if (padding == 0) { ok = true; break; }   // fully drained
        }
        Sleep(4);
    }

    if (chunk) ptt_free_audio(chunk);
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

std::string ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::string s = Utf8(path);
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

void Usage() {
    std::fprintf(stderr,
        "speak — text to speech with an on-screen orb\n\n"
        "  speak [options] \"text to speak\"\n\n"
        "  --voice <name|path>   voice sample (default: alba.wav)\n"
        "  --save <file.wav>     also save the audio\n"
        "  --no-orb              skip the on-screen indicator\n"
        "  --dump-orb <f.bmp>    render one orb frame to a BMP and exit\n"
        "  --temperature <f>     sampling temperature (default 0.7)\n"
        "  --threads <n>         thread budget (0 = half the cores)\n"
        "  --models-dir <dir>    ONNX models (default: <exe dir>/models)\n"
        "  --voices-dir <dir>    voice samples (default: <exe dir>/voices)\n");
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return 1;

    const std::string exe_dir = ExeDir();
    std::string text, voice = "alba.wav", save_path;
    std::string models_dir = exe_dir + "\\models";
    std::string voices_dir = exe_dir + "\\voices";
    bool  show_orb   = true;
    float temperature = 0.7f;
    int   threads     = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = Utf8(wargv[i]);
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "speak: %s needs a value\n", name);
                std::exit(2);
            }
            return Utf8(wargv[++i]);
        };
        if (a == "--voice")            voice = next("--voice");
        else if (a == "--save")        save_path = next("--save");
        else if (a == "--models-dir")  models_dir = next("--models-dir");
        else if (a == "--voices-dir")  voices_dir = next("--voices-dir");
        else if (a == "--temperature") temperature = std::strtof(next("--temperature").c_str(), nullptr);
        else if (a == "--threads")     threads = std::atoi(next("--threads").c_str());
        else if (a == "--no-orb")      show_orb = false;
        else if (a == "--dump-orb") {
            DumpOrbFrame(next("--dump-orb"), 0.65f);
            return 0;
        }
        else if (a == "-h" || a == "--help") { Usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "speak: unknown option %s\n", a.c_str());
            return 2;
        } else {
            if (!text.empty()) text += " ";
            text += a;
        }
    }
    LocalFree(wargv);

    if (text.empty()) { Usage(); return 2; }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::fprintf(stderr, "speak: COM init failed\n");
        return 1;
    }

    const std::string tokenizer = models_dir + "\\tokenizer.model";
    void* tts = ptt_create(models_dir.c_str(), voices_dir.c_str(),
                           tokenizer.c_str(), "int8", temperature, 1, threads);
    if (!tts) {
        std::fprintf(stderr, "speak: could not load models from %s\n", models_dir.c_str());
        CoUninitialize();
        return 1;
    }

    std::thread orb;
    if (show_orb) orb = std::thread(OrbThread);

    void* stream = ptt_stream_start(tts, text.c_str(), voice.c_str());
    bool ok = false;
    if (!stream) {
        std::fprintf(stderr, "speak: could not start synthesis\n");
    } else {
        std::vector<float> recorded;
        ok = PlayStream(stream, save_path.empty() ? nullptr : &recorded);
        ptt_stream_end(stream);
        if (ok && !save_path.empty()) WriteWav(save_path, recorded);
    }

    g_audio_done.store(true);
    if (orb.joinable()) orb.join();

    ptt_destroy(tts);
    CoUninitialize();
    return ok ? 0 : 1;
}
