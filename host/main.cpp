// PenLink host (Windows 10 1809+/11, NVIDIA GPU)
// DXGI Desktop Duplication -> GPU copy -> h264_nvenc (FFmpeg, D3D11 hw frames) -> TCP (по USB через usbmuxd или по Wi-Fi)
// iPad -> Apple Pencil события -> Windows Pointer Injection (PT_PEN: нажим, наклон); хоткеи -> SendInput
// Сообщения: [u32 длина LE][тип + данные]. Очередей кадров нет: кодирование синхронно в потоке захвата.
//
// Запуск:  penlink-host.exe                 (USB-кабель, по умолчанию)
//          penlink-host.exe --ip 192.168.0.5 (Wi-Fi: IP iPad)   доп.: --fps 60 --kbps 40000
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11_4.h>
#include <timeapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
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
#include <climits>
#include <algorithm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
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
struct Streamer {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIAdapter1> adapter; ComPtr<IDXGIOutputDuplication> dupl;
    RECT mon{}; int W = 0, H = 0; bool nvidia = false;
    SOCKET sock = INVALID_SOCKET;
    std::mutex sm; std::vector<uint8_t> tx;
    std::atomic<bool> running{false}, idrReq{true}, linkOk{true};
    std::thread th;

    bool initCapture() {
        dupl.Reset();
        if (!dev) {
            ComPtr<IDXGIFactory1> f; CreateDXGIFactory1(IID_PPV_ARGS(&f));
            for (UINT i = 0;; i++) {
                ComPtr<IDXGIAdapter1> a;
                if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                ComPtr<IDXGIOutput> o;
                if (FAILED(a->EnumOutputs(0, &o))) continue;  // берём адаптер, к которому подключён монитор
                if (SUCCEEDED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { adapter = a; break; }
            }
            if (!dev) return false;
            DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
            nvidia = (ad.VendorId == 0x10DE);
            wprintf(L"GPU, к которому подключён экран: %ls\n", ad.Description);
            ComPtr<ID3D11Multithread> mt; if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        }
        ComPtr<IDXGIOutput> o; adapter->EnumOutputs(0, &o);
        DXGI_OUTPUT_DESC d; o->GetDesc(&d);
        mon = d.DesktopCoordinates; W = mon.right - mon.left; H = mon.bottom - mon.top;
        ComPtr<IDXGIOutput1> o1; o.As(&o1);
        return SUCCEEDED(o1->DuplicateOutput(dev.Get(), &dupl));
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

    void loop(int fps, int kbps) {
        if (!nvidia) {
            fprintf(stderr, "Экран выводится не через NVIDIA (гибридная графика). NVENC здесь недоступен.\n"
                            "Переключите ноутбук в режим дискретной графики (MUX / Ultimate / GPU-only) и перезагрузитесь.\n");
            return;
        }
        AVBufferRef* hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        auto* dc = (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hw->data)->hwctx;
        dc->device = dev.Get(); dev->AddRef();
        if (av_hwdevice_ctx_init(hw) < 0) { fprintf(stderr, "hwdevice init failed\n"); return; }
        AVBufferRef* fr = av_hwframe_ctx_alloc(hw);
        auto* fc = (AVHWFramesContext*)fr->data;
        fc->format = AV_PIX_FMT_D3D11; fc->sw_format = AV_PIX_FMT_BGRA; fc->width = W; fc->height = H; fc->initial_pool_size = 3;
        if (av_hwframe_ctx_init(fr) < 0) { fprintf(stderr, "hwframes init failed\n"); return; }

        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) { fprintf(stderr, "h264_nvenc not found (нужна сборка FFmpeg с NVENC)\n"); return; }
        AVCodecContext* cc = avcodec_alloc_context3(codec);
        cc->width = W; cc->height = H; cc->time_base = { 1, fps }; cc->framerate = { fps, 1 };
        cc->pix_fmt = AV_PIX_FMT_D3D11; cc->hw_frames_ctx = av_buffer_ref(fr);
        cc->bit_rate = (int64_t)kbps * 1000; cc->rc_max_rate = cc->bit_rate;
        cc->rc_buffer_size = (int)(cc->bit_rate / fps * 2);
        cc->gop_size = INT_MAX; cc->max_b_frames = 0; cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
        av_opt_set(cc->priv_data, "preset", "p1", 0);
        av_opt_set(cc->priv_data, "tune", "ull", 0);
        av_opt_set(cc->priv_data, "rc", "cbr", 0);
        av_opt_set(cc->priv_data, "zerolatency", "1", 0);
        av_opt_set(cc->priv_data, "delay", "0", 0);
        av_opt_set(cc->priv_data, "forced-idr", "1", 0);
        av_opt_set(cc->priv_data, "profile", "high", 0);
        if (avcodec_open2(cc, codec, nullptr) < 0) { fprintf(stderr, "avcodec_open2 failed\n"); return; }

        AVFrame* frame = av_frame_alloc(); AVPacket* pkt = av_packet_alloc();
        int64_t pts = 0; uint32_t fid = 0;
        int nf = 0; double sumCap = 0; uint32_t t0 = nowUs();
        fprintf(stderr, "Стрим %dx%d @%d fps, %d kbps\n", W, H, fps, kbps);

        while (running && linkOk) {
            ComPtr<IDXGIResource> res; DXGI_OUTDUPL_FRAME_INFO fi;
            HRESULT hr = dupl ? dupl->AcquireNextFrame(50, &fi, &res) : DXGI_ERROR_ACCESS_LOST;
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) { Sleep(100); initCapture(); continue; }
            if (fi.LastPresentTime.QuadPart == 0 && !idrReq) { dupl->ReleaseFrame(); continue; }  // обновился только курсор
            uint32_t tCap = nowUs();
            ComPtr<ID3D11Texture2D> tex; res.As(&tex);
            av_hwframe_get_buffer(fr, frame, 0);
            ctx->CopySubresourceRegion((ID3D11Texture2D*)frame->data[0], (UINT)(intptr_t)frame->data[1], 0, 0, 0, tex.Get(), 0, nullptr);
            ctx->Flush();
            dupl->ReleaseFrame();
            frame->pts = pts++;
            frame->pict_type = idrReq.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            if (avcodec_send_frame(cc, frame) >= 0) {
                while (avcodec_receive_packet(cc, pkt) == 0) {
                    if (!sendFrame(pkt->data, pkt->size, pkt->flags & AV_PKT_FLAG_KEY, fid++, tCap)) linkOk = false;
                    av_packet_unref(pkt);
                }
            }
            av_frame_unref(frame);
            uint32_t tEnd = nowUs(); sumCap += (tEnd - tCap) / 1000.0; nf++;
            if (tEnd - t0 > 1000000) {
                fprintf(stderr, "fps=%d  захват→отправка=%.2f мс\n", nf, sumCap / nf);
                nf = 0; sumCap = 0; t0 = tEnd;
            }
        }
        avcodec_free_context(&cc); av_frame_free(&frame); av_packet_free(&pkt);
        av_buffer_unref(&fr); av_buffer_unref(&hw);
    }

    void start(int fps, int kbps) {
        stop(); running = true; idrReq = true; linkOk = true;
        th = std::thread([=] { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); loop(fps, kbps); });
    }
    void stop() { running = false; if (th.joinable()) th.join(); }
};

int main(int argc, char** argv) {
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
        SOCKET s = ip.empty() ? usbConnect(IPAD_PORT) : tcpConnect(ip.c_str(), IPAD_PORT);
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }       // ждём iPad / приложение
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
        S.sock = s;
        printf("iPad подключён.\n");
        S.start(fps, kbps);
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
