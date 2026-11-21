// PenLink host (Windows 10 1809+/11)
// DXGI Desktop Duplication -> H.264 (NVENC or libx264) -> TCP (USB via usbmuxd or Wi-Fi)
// iPad -> Apple Pencil события -> Windows Pointer Injection (PT_PEN: нажим, наклон); хоткеи -> SendInput
// Сообщения: [u32 длина LE][тип + данные]. Очередей кадров нет: кодирование синхронно в потоке захвата.
//
// Запуск:  penlink-host.exe                 (USB-кабель, по умолчанию)
//          penlink-host.exe --ip 192.168.0.5 (Wi-Fi: IP iPad)   доп.: --fps 60 --kbps 40000
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11_4.h>
#include <timeapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <climits>
#include <algorithm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
using Microsoft::WRL::ComPtr;

static const uint16_t IPAD_PORT = 47801;   // порт, который слушает приложение на iPad

#pragma pack(push, 1)
struct VideoHdr { uint8_t type; uint8_t flags; uint16_t fragIdx; uint16_t fragCount; uint32_t frameId; uint32_t hostUs; };
struct PenPkt   { uint8_t type; uint8_t phase; uint8_t buttons; float x, y, pressure, altitude, azimuth; };
struct KeyPkt   { uint8_t type; uint8_t action; uint8_t mods; uint16_t vk; };
#pragma pack(pop)
enum { T_VIDEO = 1, T_PEN = 2, T_IDR = 3, T_KEY = 4, T_PING = 5, T_PONG = 6 };

static uint32_t nowUs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint32_t)((c.QuadPart * 1000000ULL) / f.QuadPart);
}
static bool recvAll(SOCKET s, void* p, int n) {
    char* c = (char*)p;
    while (n > 0) { int r = recv(s, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
    return true;
}
static bool sendAll(SOCKET s, const void* p, int n) {
    const char* c = (const char*)p;
    while (n > 0) { int r = send(s, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
    return true;
}

// ---------------- Подключение: Wi-Fi (прямой TCP) или USB (usbmuxd из Apple Mobile Device Service) ----------------
static SOCKET tcpConnect(const char* ip, uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}
static bool muxSend(SOCKET s, const std::string& body, uint32_t tag) {
    uint32_t h[4] = { 16u + (uint32_t)body.size(), 1, 8, tag };   // версия 1 (plist), тип 8
    return sendAll(s, h, 16) && sendAll(s, body.data(), (int)body.size());
}
static bool muxRecv(SOCKET s, std::string& out) {
    uint32_t h[4]; if (!recvAll(s, h, 16)) return false;
    int n = (int)h[0] - 16; if (n < 0 || n > (1 << 20)) return false;
    out.resize(n); return n == 0 || recvAll(s, &out[0], n);
}
static std::string plistMsg(const std::string& type, const std::string& extra = "") {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict>"
           "<key>MessageType</key><string>" + type + "</string>"
           "<key>ClientVersionString</key><string>penlink</string><key>ProgName</key><string>penlink</string>" + extra + "</dict></plist>";
}
static int firstInt(const std::string& s, size_t from) {
    size_t a = s.find("<integer>", from); if (a == std::string::npos) return -1;
    return atoi(s.c_str() + a + 9);
}
static SOCKET usbConnect(uint16_t port) {
    SOCKET s = tcpConnect("127.0.0.1", 27015);                   // служба Apple Mobile Device (ставится с iTunes)
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    std::string r; int id = -1;
    if (muxSend(s, plistMsg("ListDevices"), 1) && muxRecv(s, r)) {
        const std::string k = "<key>DeviceID</key>";
        for (size_t p = r.find(k); p != std::string::npos && id < 0; p = r.find(k, p + 1)) {
            size_t e = r.find("</dict>", p);
            std::string chunk = r.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (chunk.find("<string>USB</string>") != std::string::npos) id = firstInt(r, p);
        }
    }
    closesocket(s);
    if (id < 0) return INVALID_SOCKET;                            // iPad по USB не найден
    s = tcpConnect("127.0.0.1", 27015);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    uint16_t swapped = htons(port);
    std::string extra = "<key>DeviceID</key><integer>" + std::to_string(id) + "</integer><key>PortNumber</key><integer>" + std::to_string(swapped) + "</integer>";
    r.clear();
    if (muxSend(s, plistMsg("Connect", extra), 2) && muxRecv(s, r)) {
        size_t n = r.find("<key>Number</key>");
        if (n != std::string::npos && firstInt(r, n) == 0) return s;   // дальше сокет — прямой канал в приложение на iPad
    }
    closesocket(s);
    return INVALID_SOCKET;                                         // приложение на iPad не запущено
}

// ---------------- Ввод ----------------
struct Injector {
    HSYNTHETICPOINTERDEVICE dev = nullptr;
    RECT mon{};
    bool inRange = false;

    bool init(RECT r) {
        mon = r;
        if (!dev) dev = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE);
        return dev != nullptr;
    }
    void pen(const PenPkt& p) {
        if (!dev) return;
        POINTER_TYPE_INFO info{};
        info.type = PT_PEN;
        POINTER_PEN_INFO& pi = info.penInfo;
        POINTER_INFO& pp = pi.pointerInfo;
        pp.pointerType = PT_PEN;
        pp.pointerId = 0;
        float x = std::clamp(p.x, 0.f, 1.f), y = std::clamp(p.y, 0.f, 1.f);
        pp.ptPixelLocation.x = mon.left + (LONG)lroundf(x * (mon.right - mon.left - 1));
        pp.ptPixelLocation.y = mon.top + (LONG)lroundf(y * (mon.bottom - mon.top - 1));
        pi.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
        bool contact = (p.phase == 0 || p.phase == 1);
        pi.pressure = contact ? (UINT32)std::clamp(p.pressure * 1024.f, 1.f, 1024.f) : 0;
        // Apple: altitude 0 = плашмя, pi/2 = перпендикулярно; azimuth — направление в плоскости экрана.
        float ca = cosf(p.altitude), sa = std::max(sinf(p.altitude), 0.01f);
        float tx = cosf(p.azimuth) * ca, ty = sinf(p.azimuth) * ca;
        pi.tiltX = (INT32)std::clamp(atan2f(tx, sa) * 57.29578f, -90.f, 90.f);
        pi.tiltY = (INT32)std::clamp(atan2f(ty, sa) * 57.29578f, -90.f, 90.f);
        if (p.buttons & 1) pi.penFlags |= PEN_FLAG_BARREL;
        if (p.buttons & 2) pi.penFlags |= PEN_FLAG_ERASER;

        POINTER_FLAGS fl = POINTER_FLAG_NONE;
        switch (p.phase) {
        case 0: fl = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON | POINTER_FLAG_DOWN; break;
        case 1: fl = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON | POINTER_FLAG_UPDATE; break;
        case 2: fl = POINTER_FLAG_INRANGE | POINTER_FLAG_UP; break;
        case 3: fl = POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE; break;
        case 4: fl = POINTER_FLAG_UPDATE; break;
        }
        if (!inRange && (fl & POINTER_FLAG_INRANGE)) fl |= POINTER_FLAG_NEW;
        inRange = (fl & POINTER_FLAG_INRANGE) != 0;
        pp.pointerFlags = fl;
        InjectSyntheticPointerInput(dev, &info, 1);
    }
    static void key(uint8_t action, uint8_t mods, uint16_t vk) {  // action: 0 tap, 1 down, 2 up
        std::vector<INPUT> v;
        auto add = [&](WORD k, bool up) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; v.push_back(i); };
        const WORD m[4] = { VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN };
        if (action != 2) { for (int i = 0; i < 4; i++) if (mods & (1 << i)) add(m[i], false); add(vk, false); }
        if (action != 1) { add(vk, true); for (int i = 3; i >= 0; i--) if (mods & (1 << i)) add(m[i], true); }
        SendInput((UINT)v.size(), v.data(), sizeof(INPUT));
    }
};

// ---------------- Захват + кодирование ----------------
static void dxError(const char* step, HRESULT hr) {
    fprintf(stderr, "%s: HRESULT 0x%08lX\n", step, static_cast<unsigned long>(hr));
}

static void avError(const char* step, int err) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, message, sizeof message);
    fprintf(stderr, "%s: %s (%d)\n", step, message, err);
}

static std::string utf8(const wchar_t* value) {
    char buffer[512]{};
    WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer, sizeof buffer, nullptr, nullptr);
    return buffer;
}

static HRESULT createDevice(IDXGIAdapter1* adapter, ID3D11Device** dev, ID3D11DeviceContext** ctx) {
    return D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             nullptr, 0, D3D11_SDK_VERSION, dev, nullptr, ctx);
}

struct Encoder {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scale = nullptr;
    ComPtr<ID3D11DeviceContext> upload;
    bool nvenc = false;

    ~Encoder() { reset(); }
    void reset() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        sws_freeContext(scale);
        scale = nullptr;
        upload.Reset();
        nvenc = false;
    }

    bool init(const char* name, int w, int h, int fps, int kbps,
              ID3D11Device* dev = nullptr, ID3D11DeviceContext* ctx = nullptr) {
        reset();
        const AVCodec* encoder = avcodec_find_encoder_by_name(name);
        if (!encoder) {
            fprintf(stderr, "FFmpeg encoder %s not found. Use a build with NVENC and libx264 (e.g. BtbN win64-gpl-shared).\n", name);
            return false;
        }
        codec = avcodec_alloc_context3(encoder);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!codec || !frame || !packet) {
            fprintf(stderr, "%s: could not allocate FFmpeg buffers.\n", name);
            return false;
        }
        nvenc = dev != nullptr;
        codec->width = nvenc ? w : ((w + 1) & ~1);
        codec->height = nvenc ? h : ((h + 1) & ~1);
        codec->time_base = {1, fps};
        codec->framerate = {fps, 1};
        codec->pix_fmt = nvenc ? AV_PIX_FMT_D3D11 : AV_PIX_FMT_YUV420P;
        codec->bit_rate = (int64_t)kbps * 1000;
        codec->rc_max_rate = codec->bit_rate;
        codec->rc_buffer_size = (int)(codec->bit_rate / fps * 2);
        codec->gop_size = INT_MAX;
        codec->max_b_frames = 0;
        codec->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (nvenc) {
            ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE);
            AVBufferRef* hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!hw) {
                avError("av_hwdevice_ctx_alloc (NVENC)", AVERROR(ENOMEM));
                return false;
            }
            auto* dc = reinterpret_cast<AVD3D11VADeviceContext*>(
                reinterpret_cast<AVHWDeviceContext*>(hw->data)->hwctx);
            dc->device = dev;
            dev->AddRef();
            int err = av_hwdevice_ctx_init(hw);
            if (err >= 0) codec->hw_frames_ctx = av_hwframe_ctx_alloc(hw);
            av_buffer_unref(&hw);
            if (err < 0 || !codec->hw_frames_ctx) {
                avError("D3D11 device/frames allocation (NVENC)", err < 0 ? err : AVERROR(ENOMEM));
                return false;
            }
            auto* fc = reinterpret_cast<AVHWFramesContext*>(codec->hw_frames_ctx->data);
            fc->format = AV_PIX_FMT_D3D11;
            fc->sw_format = AV_PIX_FMT_BGRA;
            fc->width = w;
            fc->height = h;
            err = av_hwframe_ctx_init(codec->hw_frames_ctx);
            if (err < 0) {
                avError("av_hwframe_ctx_init (NVENC)", err);
                return false;
            }
            upload = ctx;
        } else {
            scale = sws_getContext(w, h, AV_PIX_FMT_BGRA, codec->width, codec->height,
                                   codec->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!scale) {
                fprintf(stderr, "libx264: could not initialize BGRA to YUV420P conversion.\n");
                return false;
            }
            frame->format = codec->pix_fmt;
            frame->width = codec->width;
            frame->height = codec->height;
            int err = av_frame_get_buffer(frame, 0);
            if (err < 0) {
                avError("av_frame_get_buffer (libx264)", err);
                return false;
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "forced-idr", "1", 0);
        if (nvenc) {
            av_dict_set(&options, "preset", "p1", 0);
            av_dict_set(&options, "tune", "ull", 0);
            av_dict_set(&options, "rc", "cbr", 0);
            av_dict_set(&options, "zerolatency", "1", 0);
            av_dict_set(&options, "delay", "0", 0);
            av_dict_set(&options, "profile", "high", 0);
        } else {
            av_dict_set(&options, "preset", "ultrafast", 0);
            av_dict_set(&options, "tune", "zerolatency", 0);
            av_dict_set(&options, "x264-params", "annexb=1:repeat-headers=1", 0);
        }
        int err = avcodec_open2(codec, encoder, &options);
        av_dict_free(&options);
        if (err < 0) {
            avError(name, err);
            return false;
        }
        return true;
    }
};

struct Streamer {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> staging;
    RECT mon{};
    int W = 0, H = 0;
    SOCKET sock = INVALID_SOCKET;
    std::mutex sm;
    std::vector<uint8_t> tx;
    std::atomic<bool> running{false}, idrReq{true}, linkOk{true};
    std::thread th;

    bool initCapture() {
        dupl.Reset();
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            dxError("CreateDXGIFactory1 (capture)", hr);
            return false;
        }

        struct Display {
            ComPtr<IDXGIAdapter1> adapter;
            ComPtr<IDXGIOutput> output;
            DXGI_OUTPUT_DESC desc;
        };
        std::vector<Display> displays;
        HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            hr = factory->EnumAdapters1(i, &a);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) {
                dxError("EnumAdapters1 (capture)", hr);
                break;
            }
            DXGI_ADAPTER_DESC1 ad{};
            if (FAILED(a->GetDesc1(&ad))) continue;
            fprintf(stderr, "DXGI adapter %u: %s (vendor 0x%04X, device 0x%04X)%s\n",
                    i, utf8(ad.Description).c_str(), ad.VendorId, ad.DeviceId,
                    ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? " [software]" : "");
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> o;
                hr = a->EnumOutputs(j, &o);
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) {
                    dxError("EnumOutputs", hr);
                    break;
                }
                DXGI_OUTPUT_DESC d{};
                hr = o->GetDesc(&d);
                if (FAILED(hr)) {
                    dxError("IDXGIOutput::GetDesc", hr);
                    continue;
                }
                fprintf(stderr, "  Output %s: %s%s\n", utf8(d.DeviceName).c_str(),
                        d.AttachedToDesktop ? "attached" : "detached",
                        d.Monitor == primary ? " (primary)" : "");
                if (d.AttachedToDesktop) displays.push_back({a, o, d});
            }
        }
        std::stable_partition(displays.begin(), displays.end(),
                              [primary](const Display& d) { return d.desc.Monitor == primary; });
        for (const auto& display : displays) {
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            ComPtr<IDXGIOutput1> selected;
            hr = createDevice(display.adapter.Get(), &device, &context);
            if (SUCCEEDED(hr)) hr = display.output.As(&selected);
            if (SUCCEEDED(hr)) hr = selected->DuplicateOutput(device.Get(), &dupl);
            if (FAILED(hr)) {
                fprintf(stderr, "Capture output %s: ", utf8(display.desc.DeviceName).c_str());
                dxError("D3D11/DXGI Desktop Duplication initialization", hr);
                continue;
            }
            dev = device;
            ctx = context;
            adapter = display.adapter;
            output = selected;
            staging.Reset();
            mon = display.desc.DesktopCoordinates;
            W = mon.right - mon.left;
            H = mon.bottom - mon.top;
            DXGI_ADAPTER_DESC1 ad{};
            adapter->GetDesc1(&ad);
            fprintf(stderr, "Capture: %s on %s, %dx%d at (%ld,%ld)\n",
                    utf8(display.desc.DeviceName).c_str(), utf8(ad.Description).c_str(), W, H, mon.left, mon.top);
            return true;
        }
        fprintf(stderr, "No DXGI desktop output could be captured. Check that a display is active and the host runs in the interactive Windows session.\n");
        return false;
    }

    bool initEncoder(Encoder& encoder, int fps, int kbps) {
        DXGI_ADAPTER_DESC1 capture{};
        adapter->GetDesc1(&capture);
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) dxError("CreateDXGIFactory1 (NVENC)", hr);
        if (factory) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> a;
                hr = factory->EnumAdapters1(i, &a);
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) {
                    dxError("EnumAdapters1 (NVENC)", hr);
                    break;
                }
                DXGI_ADAPTER_DESC1 ad{};
                if (FAILED(a->GetDesc1(&ad)) || ad.VendorId != 0x10DE) continue;
                const bool same = ad.AdapterLuid.HighPart == capture.AdapterLuid.HighPart &&
                                  ad.AdapterLuid.LowPart == capture.AdapterLuid.LowPart;
                ComPtr<ID3D11Device> device = dev;
                ComPtr<ID3D11DeviceContext> context = ctx;
                fprintf(stderr, "Trying NVENC on %s (DXGI adapter %u)\n", utf8(ad.Description).c_str(), i);
                if (!same) {
                    hr = createDevice(a.Get(), device.ReleaseAndGetAddressOf(), context.ReleaseAndGetAddressOf());
                    if (FAILED(hr)) {
                        dxError("D3D11CreateDevice (NVENC)", hr);
                        continue;
                    }
                }
                if (encoder.init("h264_nvenc", W, H, fps, kbps, device.Get(), context.Get())) {
                    fprintf(stderr, "Encoder: NVENC on %s (%s)\n", utf8(ad.Description).c_str(),
                            same ? "GPU copy" : "capture GPU -> system memory -> NVIDIA GPU");
                    return true;
                }
            }
        }
        fprintf(stderr, "NVENC unavailable. Falling back to FFmpeg libx264.\n");
        return encoder.init("libx264", W, H, fps, kbps);
    }

    int copyFrame(Encoder& encoder, ID3D11Texture2D* texture) {
        if (encoder.nvenc) {
            av_frame_unref(encoder.frame);
            int err = av_hwframe_get_buffer(encoder.codec->hw_frames_ctx, encoder.frame, 0);
            if (err < 0) return err;
        }

        auto* target = reinterpret_cast<ID3D11Texture2D*>(encoder.frame->data[0]);
        UINT subresource = static_cast<UINT>(reinterpret_cast<intptr_t>(encoder.frame->data[1]));
        if (encoder.nvenc && encoder.upload.Get() == ctx.Get()) {
            ctx->CopySubresourceRegion(target, subresource, 0, 0, 0, texture, 0, nullptr);
            ctx->Flush();
            return 0;
        }

        if (!staging) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &staging);
            if (FAILED(hr)) {
                dxError("CreateTexture2D (capture staging)", hr);
                return AVERROR(EIO);
            }
        }

        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            dxError("Map (capture readback)", hr);
            return AVERROR(EIO);
        }
        int err = 0;
        if (encoder.nvenc) {
            encoder.upload->UpdateSubresource(target, subresource, nullptr, mapped.pData, mapped.RowPitch, 0);
            encoder.upload->Flush();
        } else {
            err = av_frame_make_writable(encoder.frame);
            if (err >= 0) {
                const uint8_t* source[] = {static_cast<const uint8_t*>(mapped.pData)};
                int stride[] = {static_cast<int>(mapped.RowPitch)};
                err = sws_scale(encoder.scale, source, stride, 0, H, encoder.frame->data, encoder.frame->linesize);
                if (err != encoder.codec->height) err = err < 0 ? err : AVERROR(EIO);
            }
        }
        ctx->Unmap(staging.Get(), 0);
        return err < 0 ? err : 0;
    }

    // одно сообщение = [u32 длина][данные]; всё одним send, чтобы не плодить пакеты
    bool sendMsg(const uint8_t* p, int n) {
        std::lock_guard<std::mutex> l(sm);
        tx.resize(4 + n); memcpy(tx.data(), &n, 4); memcpy(tx.data() + 4, p, n);
        return sendAll(sock, tx.data(), (int)tx.size());
    }
    bool sendFrame(const uint8_t* d, int n, bool key, uint32_t id, uint32_t ts) {
        std::lock_guard<std::mutex> l(sm);
        int body = (int)sizeof(VideoHdr) + n;
        tx.resize(4 + body);
        VideoHdr h{ T_VIDEO, (uint8_t)(key ? 1 : 0), 0, 1, id, ts };
        memcpy(tx.data(), &body, 4); memcpy(tx.data() + 4, &h, sizeof h); memcpy(tx.data() + 4 + sizeof h, d, n);
        return sendAll(sock, tx.data(), (int)tx.size());
    }

    void loop(Encoder& encoder, int fps, int kbps) {
        int64_t pts = 0;
        uint32_t fid = 0;
        int nf = 0;
        double sumCap = 0;
        uint32_t t0 = nowUs();
        fprintf(stderr, "Stream: %s, %dx%d @%d fps, %d kbps\n",
                encoder.codec->codec->name, encoder.codec->width, encoder.codec->height, fps, kbps);

        while (running && linkOk) {
            ComPtr<IDXGIResource> res;
            DXGI_OUTDUPL_FRAME_INFO fi{};
            HRESULT hr = dupl->AcquireNextFrame(50, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) {
                dxError("AcquireNextFrame", hr);
                dupl.Reset();
                staging.Reset();
                // Lock/unlock and desktop switches can temporarily invalidate duplication.
                DXGI_OUTPUT_DESC current{};
                for (int attempt = 0; attempt < 10 && running; ++attempt) {
                    Sleep(100);
                    hr = output->GetDesc(&current);
                    if (FAILED(hr)) break;
                    if (!current.AttachedToDesktop || !EqualRect(&current.DesktopCoordinates, &mon)) {
                        hr = DXGI_ERROR_ACCESS_LOST;
                        break;
                    }
                    hr = output->DuplicateOutput(dev.Get(), &dupl);
                    if (SUCCEEDED(hr)) break;
                }
                if (!dupl) {
                    dxError("Could not restore capture; reconnecting to select the display again", hr);
                    break;
                }
                idrReq = true;
                continue;
            }

            if (fi.LastPresentTime.QuadPart == 0 && !idrReq) {
                dupl->ReleaseFrame();
                continue;
            }
            uint32_t tCap = nowUs();
            ComPtr<ID3D11Texture2D> tex;
            hr = res.As(&tex);
            if (FAILED(hr)) dxError("Desktop frame texture", hr);
            int err = SUCCEEDED(hr) ? copyFrame(encoder, tex.Get()) : AVERROR(EIO);
            dupl->ReleaseFrame();

            if (err >= 0) {
                encoder.frame->pts = pts++;
                encoder.frame->pict_type = idrReq.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
                err = avcodec_send_frame(encoder.codec, encoder.frame);
            }
            if (err >= 0) {
                while ((err = avcodec_receive_packet(encoder.codec, encoder.packet)) == 0) {
                    AVPacket* pkt = encoder.packet;
                    bool sent = sendFrame(pkt->data, pkt->size, pkt->flags & AV_PKT_FLAG_KEY, fid++, tCap);
                    av_packet_unref(pkt);
                    if (!sent) {
                        linkOk = false;
                        break;
                    }
                }
                if (err == AVERROR(EAGAIN)) err = 0;
            }
            if (err < 0) {
                avError(encoder.codec->codec->name, err);
                if (!encoder.nvenc || !running) break;
                fprintf(stderr, "NVENC frame failed. Switching to libx264.\n");
                if (!encoder.init("libx264", W, H, fps, kbps)) break;
                idrReq = true;
                continue;
            }

            uint32_t tEnd = nowUs();
            sumCap += (tEnd - tCap) / 1000.0;
            ++nf;
            if (tEnd - t0 > 1000000) {
                fprintf(stderr, "fps=%d  захват→отправка=%.2f мс\n", nf, sumCap / nf);
                nf = 0; sumCap = 0; t0 = tEnd;
            }
        }
        if (running) shutdown(sock, SD_BOTH);
    }

    void start(Encoder& encoder, int fps, int kbps) {
        stop(); running = true; idrReq = true; linkOk = true;
        th = std::thread([this, &encoder, fps, kbps] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            loop(encoder, fps, kbps);
        });
    }
    void stop() { running = false; if (th.joinable()) th.join(); }
};

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);

    int fps = 60, kbps = 40000; std::string ip;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--fps") fps = atoi(argv[i + 1]);
        else if (a == "--kbps") kbps = atoi(argv[i + 1]);
        else if (a == "--ip") ip = argv[i + 1];
    }
    fps = std::clamp(fps, 30, 240); kbps = std::clamp(kbps, 2000, 200000);

    Streamer S;
    if (!S.initCapture()) { fprintf(stderr, "Не удалось начать захват экрана\n"); return 1; }
    Injector inj;
    if (!inj.init(S.mon)) { fprintf(stderr, "Pointer injection недоступен (нужна Windows 10 1809+)\n"); return 1; }
    printf("PenLink host. Экран %dx%d. Режим: %s. Откройте приложение PenLink на iPad.\n", S.W, S.H, ip.empty() ? "USB-кабель" : "Wi-Fi");

    std::vector<uint8_t> buf(1 << 16);
    for (;;) {
        if (!S.dupl) {
            if (!S.initCapture()) { Sleep(1000); continue; }
            inj.mon = S.mon;
        }
        Encoder encoder;
        if (!S.initEncoder(encoder, fps, kbps)) {
            fprintf(stderr, "No usable H.264 encoder. Check the FFmpeg DLLs and the NVIDIA driver.\n");
            return 1;
        }
        SOCKET s;
        do {
            s = ip.empty() ? usbConnect(IPAD_PORT) : tcpConnect(ip.c_str(), IPAD_PORT);
            if (s == INVALID_SOCKET) Sleep(1000);
        } while (s == INVALID_SOCKET);
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
        S.sock = s;
        printf("iPad подключён.\n");
        S.start(encoder, fps, kbps);
        for (;;) {
            uint32_t n;
            if (!recvAll(s, &n, 4) || n == 0 || n > buf.size() || !recvAll(s, buf.data(), (int)n)) break;
            switch (buf[0]) {
            case T_PEN: if (n >= sizeof(PenPkt)) { PenPkt p; memcpy(&p, buf.data(), sizeof p); inj.pen(p); } break;
            case T_KEY: if (n >= sizeof(KeyPkt)) { KeyPkt k; memcpy(&k, buf.data(), sizeof k); Injector::key(k.action, k.mods, k.vk); } break;
            case T_IDR: S.idrReq = true; break;
            case T_PING: buf[0] = T_PONG; S.sendMsg(buf.data(), (int)n); break;
            }
        }
        shutdown(s, SD_BOTH);          // будим поток кодирования, если он завис в send()
        S.stop(); closesocket(s); S.sock = INVALID_SOCKET;
        printf("Соединение потеряно, жду iPad...\n");
    }
}
