// ============================================================================
//  Sam Altman  ·  пожиратель RAM и VRAM
// ----------------------------------------------------------------------------
//  Что делает:
//    * занимает почти всю свободную оперативную память (оставляет ~1.5 ГБ),
//    * занимает почти всю свободную видеопамять (оставляет ~1 ГБ),
//    * запускает по рабочему столу портрет Sam Altman, который отскакивает
//      от краёв экрана, как DVD-логотип на старых телевизорах.
//
//  Выход:  Ctrl+Alt+Q   (глобальный горячий ключ)
//          Ctrl+C      (в консоли)
//  Аварийно освободить память / пауза: Ctrl+Alt+F
//  Скрыть/показать портрет:            Ctrl+Alt+H
//
//  Сборка: build.ps1 (MinGW-w64 g++). См. README.md.
// ============================================================================

#define _WIN32_WINNT 0x0A00
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <psapi.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")
#endif

// ---------------------------------------------------------------------------
//  Ресурсы (см. resource.rc)
// ---------------------------------------------------------------------------
#define IDI_APP   101
#define IDR_FACE  200

#define HOTKEY_QUIT 1
#define HOTKEY_FREE 2
#define HOTKEY_HIDE 3

// ---------------------------------------------------------------------------
//  Мелкие утилиты
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_fillPaused{false};

static inline uint64_t ToMB(uint64_t bytes) { return bytes >> 20; }
static inline uint64_t ToGB(uint64_t bytes) { return bytes >> 30; }

static bool EnablePrivilege(LPCWSTR name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(ERROR_SUCCESS);
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

static bool SetHardWorkingSet(uint64_t bytes)
{
#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE 0x00000001
#endif
    // Просим Windows держать наши страницы в физической памяти, а не сбрасывать
    // их в файл подкачки. Если прав не хватает — молча продолжаем без этого.
    static std::atomic<int> mode{0};        // 0 = не проверяли, 1 = работает, 2 = нет
    static std::atomic<bool> told{false};
    if (mode.load() == 2)
        return false;

    SIZE_T minWs = (SIZE_T)bytes + (32ull << 20);
    SIZE_T maxWs = (SIZE_T)bytes + (256ull << 20);
    BOOL ok = SetProcessWorkingSetSizeEx(GetCurrentProcess(), minWs, maxWs,
                                         QUOTA_LIMITS_HARDWS_MIN_ENABLE);
    mode.store(ok ? 1 : 2);

    if (!told.exchange(true)) {
        printf("[RAM] Удержание страниц в физической памяти: %s\n",
               ok ? "включено" : "недоступно (страницы могут уходить в подкачку)");
    }
    return ok != FALSE;
}

static uint64_t g_ramDangerFrees = 0;

// ---------------------------------------------------------------------------
//  Параметры
// ---------------------------------------------------------------------------
struct Options
{
    uint64_t ramReserveMB  = 1536;   // сколько RAM оставить свободной
    uint64_t vramReserveMB = 1024;   // сколько VRAM оставить свободной
    uint64_t ramCapMB      = 0;      // 0 = без доп. ограничения
    uint64_t vramCapMB     = 0;
    bool noRam     = false;
    bool noVram    = false;
    bool noOverlay = false;
    bool dumpSprite = false;
    int  durationSec = 0;            // 0 = работать до выхода
    int  spriteSize  = 220;
    float speed      = 280.0f;       // пикселей в секунду
};

// ---------------------------------------------------------------------------
//  Пожиратель оперативной памяти
// ---------------------------------------------------------------------------
class RamFiller
{
public:
    explicit RamFiller(const Options& opt) : opt_(opt) {}

    ~RamFiller() { Stop(); }

    void Start()
    {
        run_.store(true);
        thread_ = std::thread([this] { Loop(); });
    }

    void Stop()
    {
        run_.store(false);
        if (thread_.joinable())
            thread_.join();
        FreeAll();
    }

    // Освободить всё немедленно (без остановки потока).
    void FreeAll()
    {
        std::lock_guard<std::mutex> lk(allocMx_);
        for (void* p : blocks_)
            VirtualFree(p, 0, MEM_RELEASE);
        blocks_.clear();
        bytes_.store(0);
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    }

    uint64_t AllocatedBytes() const { return bytes_.load(); }

private:
    void Loop();
    bool AllocChunk(uint64_t bytes);

    Options opt_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> bytes_{0};
    std::mutex allocMx_;
    std::vector<void*> blocks_;
};

void RamFiller::Loop()
{
    const uint64_t chunk = 64ull << 20;

    bool quota = EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    bool lockMem = EnablePrivilege(L"SeLockMemoryPrivilege");
    printf("[RAM] Привилегии: quota=%s, lock=%s\n",
           quota ? "да" : "нет", lockMem ? "да" : "нет");

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    const uint64_t reserve = opt_.ramReserveMB << 20;
    uint64_t cap = (ms.ullTotalPhys > reserve) ? (ms.ullTotalPhys - reserve) : 0;
    if (opt_.ramCapMB)
        cap = std::min<uint64_t>(cap, opt_.ramCapMB << 20);

    if (cap < (64ull << 20)) {
        printf("[RAM] Нечего занимать: свободной памяти и так мало.\n");
        return;
    }

    printf("[RAM] Цель: занять до %.1f ГБ (резерв %.0f МБ).\n",
           cap / 1073741824.0, (double)opt_.ramReserveMB);

    bool warnedFailure = false;

    while (run_.load()) {
        if (g_fillPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        GlobalMemoryStatusEx(&ms);
        if (ms.ullAvailPhys < (256ull << 20)) {
            // Аварийный предохранитель: система уже задыхается — отдаём память.
            std::lock_guard<std::mutex> lk(allocMx_);
            size_t n = std::min<size_t>(blocks_.size(), 4);
            for (size_t i = 0; i < n; ++i) {
                VirtualFree(blocks_.back(), 0, MEM_RELEASE);
                blocks_.pop_back();
                bytes_ -= chunk;
            }
            g_ramDangerFrees += n;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        uint64_t used = bytes_.load();
        if (used + chunk <= cap) {
            if (AllocChunk(chunk)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            if (!warnedFailure) {
                printf("[RAM] Достигнут предел выделения (%llu МБ).\n",
                       (unsigned long long)ToMB(bytes_.load()));
                warnedFailure = true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool RamFiller::AllocChunk(uint64_t bytes)
{
    void* p = VirtualAlloc(nullptr, (SIZE_T)bytes,
                           MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!p)
        return false;

    // Материализуем страницы: по одному байту на страницу.
    volatile unsigned char* q = (volatile unsigned char*)p;
    for (uint64_t off = 0; off < bytes; off += 4096)
        q[off] = 0xA5;

    {
        std::lock_guard<std::mutex> lk(allocMx_);
        blocks_.push_back(p);
        bytes_ += bytes;
        SetHardWorkingSet(bytes_.load());
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Пожиратель видеопамяти (Direct3D 11)
// ---------------------------------------------------------------------------
static const GUID kIID_IDXGIDevice =
    {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID kIID_IDXGIAdapter3 =
    {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};

class VramFiller
{
public:
    explicit VramFiller(const Options& opt) : opt_(opt) {}
    ~VramFiller() { Stop(); }

    bool Init();
    void Start()
    {
        run_.store(true);
        thread_ = std::thread([this] { Loop(); });
    }
    void Stop()
    {
        run_.store(false);
        if (thread_.joinable())
            thread_.join();
        FreeAll();
        if (context_) { context_->Release(); context_ = nullptr; }
        if (device_)  { device_->Release();  device_  = nullptr; }
        if (adapter_) { adapter_->Release(); adapter_ = nullptr; }
    }

    void FreeAll()
    {
        std::lock_guard<std::mutex> lk(mx_);
        for (ID3D11Texture2D* t : textures_)
            t->Release();
        textures_.clear();
        bytes_.store(0);
    }

    uint64_t AllocatedBytes() const { return bytes_.load(); }
    uint64_t BudgetBytes() const { return budget_.load(); }
    uint64_t UsageBytes() const { return usage_.load(); }

private:
    void Loop();

    Options opt_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIAdapter3* adapter_ = nullptr;
    std::vector<ID3D11Texture2D*> textures_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> bytes_{0}, budget_{0}, usage_{0};
    std::mutex mx_;
    std::vector<unsigned char> zeros_;
};

bool VramFiller::Init()
{
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, &fl, &context_);
    if (FAILED(hr)) {
        printf("[VRAM] Не удалось создать D3D11-устройство (hr=0x%08lX).\n",
               (unsigned long)hr);
        return false;
    }

    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(device_->QueryInterface(kIID_IDXGIDevice, (void**)&dxgiDev)) || !dxgiDev) {
        printf("[VRAM] DXGI недоступен.\n");
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    if (!adapter) {
        printf("[VRAM] Не найден видеоадаптер.\n");
        return false;
    }

    if (FAILED(adapter->QueryInterface(kIID_IDXGIAdapter3, (void**)&adapter_)) || !adapter_) {
        adapter->Release();
        printf("[VRAM] Нужен DXGI 1.4+ (Windows 10). Видеопамять не трогаем.\n");
        return false;
    }
    adapter->Release();

    DXGI_ADAPTER_DESC1 desc{};
    adapter_->GetDesc1(&desc);
    printf("[VRAM] GPU: %ls\n", desc.Description);
    printf("[VRAM] Выделенная видеопамять: %.1f ГБ, общая: %.1f ГБ\n",
           desc.DedicatedVideoMemory / 1073741824.0,
           desc.SharedSystemMemory / 1073741824.0);

    zeros_.assign(4096ull * 4096ull * 4ull, 0);   // 64 МБ нулей для «материализации»
    return true;
}

void VramFiller::Loop()
{
    const uint64_t chunk = 64ull << 20;           // текстура 4096x4096 RGBA
    const uint64_t reserve = opt_.vramReserveMB << 20;

    printf("[VRAM] Цель: оставить свободными %.0f МБ.\n", (double)opt_.vramReserveMB);

    bool reported = false;
    int  failCount = 0;

    while (run_.load()) {
        if (g_fillPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        budget_.store(info.Budget);
        usage_.store(info.CurrentUsage);

        uint64_t limit = (info.Budget > reserve) ? (info.Budget - reserve) : 0;
        if (opt_.vramCapMB)
            limit = std::min<uint64_t>(limit, opt_.vramCapMB << 20);

        uint64_t used = bytes_.load();
        bool room = (used + chunk <= limit) && (info.CurrentUsage + chunk <= info.Budget);

        if (room) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 4096;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* tex = nullptr;
            HRESULT hr = device_->CreateTexture2D(&d, nullptr, &tex);
            if (SUCCEEDED(hr) && tex) {
                // Копирование нулей заставляет драйвер реально занять память.
                context_->UpdateSubresource(tex, 0, nullptr, zeros_.data(),
                                            4096 * 4, 0);
                {
                    std::lock_guard<std::mutex> lk(mx_);
                    textures_.push_back(tex);
                    bytes_ += chunk;
                }
                failCount = 0;
                if (!reported) {
                    printf("[VRAM] Заполнение началось (бюджет %.1f ГБ).\n",
                           info.Budget / 1073741824.0);
                    reported = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            if (tex)
                tex->Release();
            if (++failCount >= 3) {
                printf("[VRAM] Драйвер отказал в выделении (hr=0x%08lX, занято %llu МБ).\n",
                       (unsigned long)hr, (unsigned long long)ToMB(bytes_.load()));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    printf("[VRAM] Итог: занято %llu МБ, бюджет %llu МБ.\n",
           (unsigned long long)ToMB(bytes_.load()),
           (unsigned long long)ToMB(budget_.load()));
}

// ---------------------------------------------------------------------------
//  Летающий портрет (слойное окно + GDI+)
// ---------------------------------------------------------------------------
class Overlay
{
public:
    ~Overlay() { Shutdown(); }

    bool Init(HINSTANCE inst, Gdiplus::Image* face, const Options& opt);
    void Update(double dt);
    void ToggleVisible();
    void Shutdown();
    bool DumpSprite(const wchar_t* path);
    uint64_t Bounces() const { return bounces_; }

private:
    void Render();
    void Present();
    void OnBounce();
    void RefreshBounds();

    HWND hwnd_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    Gdiplus::Bitmap* surface_ = nullptr;
    Gdiplus::Image* face_ = nullptr;
    HDC screenDC_ = nullptr;

    int w_ = 0, h_ = 0;
    float x_ = 0, y_ = 0, vx_ = 0, vy_ = 0;
    float angle_ = 0;
    float renderedAngle_ = 0;
    bool dirty_ = true;
    RECT bounds_{};
    bool visible_ = true;
    bool presentWarned_ = false;

    BYTE tintR_ = 255, tintG_ = 255, tintB_ = 255;
    uint64_t bounces_ = 0;

    std::mt19937 rng_{std::random_device{}()};
    float speed_ = 280.0f;
};

static const BYTE kTints[][3] = {    {255, 90, 90}, {90, 255, 120}, {90, 150, 255}, {255, 220, 90},
    {90, 255, 235}, {235, 90, 255}, {255, 150, 60}, {255, 255, 255},
};

static int EncoderClsid(const wchar_t* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;

    std::vector<BYTE> buffer(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buffer.data();
    Gdiplus::GetImageEncoders(num, size, codecs);

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return 0;
        }
    }
    return -1;
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l){
    if (msg == WM_DISPLAYCHANGE)
        return 0;
    if (msg == WM_DESTROY)
        return 0;
    return DefWindowProcW(hwnd, msg, w, l);
}

bool Overlay::Init(HINSTANCE inst, Gdiplus::Image* face, const Options& opt)
{
    face_ = face;
    w_ = h_ = opt.spriteSize;
    speed_ = opt.speed;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SamAltmanOverlay";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        printf("[Окно] Не удалось зарегистрировать класс (err=%lu).\n", GetLastError());
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        L"SamAltmanOverlay", L"Sam Altman", WS_POPUP,
        0, 0, w_, h_, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) {
        printf("[Окно] Не удалось создать окно (err=%lu).\n", GetLastError());
        return false;
    }

    HICON icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (icon) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)icon);
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w_;
    bmi.bmiHeader.biHeight = -h_;          // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    memDC_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!memDC_ || !dib_) {
        printf("[Окно] Не удалось создать DIB-секцию.\n");
        return false;
    }
    SelectObject(memDC_, dib_);

    surface_ = new Gdiplus::Bitmap(w_, h_, w_ * 4, PixelFormat32bppPARGB,
                                   (BYTE*)bits_);

    screenDC_ = GetDC(nullptr);

    RefreshBounds();
    std::uniform_real_distribution<float> ux((float)bounds_.left,
                                             (float)(bounds_.right - w_));
    std::uniform_real_distribution<float> uy((float)bounds_.top,
                                             (float)(bounds_.bottom - h_));
    x_ = ux(rng_);
    y_ = uy(rng_);

    float ang = (float)(rng_() % 360) * 3.14159265f / 180.0f;
    vx_ = std::cos(ang) * speed_;
    vy_ = std::sin(ang) * speed_;

    int ti = (int)(rng_() % 8);
    tintR_ = kTints[ti][0];
    tintG_ = kTints[ti][1];
    tintB_ = kTints[ti][2];

    printf("[Окно] Sam Altman %dx%d, старт %.0f,%.0f\n", w_, h_, x_, y_);

    Render();
    renderedAngle_ = angle_;
    dirty_ = false;
    Present();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void Overlay::RefreshBounds()
{
    bounds_.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds_.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds_.right  = bounds_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds_.bottom = bounds_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

void Overlay::OnBounce()
{
    ++bounces_;
    angle_ = ((float)(rng_() % 100) / 100.0f - 0.5f) * 10.0f;
    dirty_ = true;

    int ti = (int)(rng_() % 8);
    for (int i = 0; i < 8 && kTints[ti][0] == tintR_ &&
                    kTints[ti][1] == tintG_ && kTints[ti][2] == tintB_; ++i)
        ti = (ti + 1) % 8;
    tintR_ = kTints[ti][0];
    tintG_ = kTints[ti][1];
    tintB_ = kTints[ti][2];
}

void Overlay::Update(double dt)
{
    if (dt > 0.1)
        dt = 0.1;

    RefreshBounds();

    angle_ *= (float)std::exp(-dt * 3.0);
    if (std::fabs(angle_ - renderedAngle_) > 0.7f)
        dirty_ = true;

    x_ += vx_ * (float)dt;
    y_ += vy_ * (float)dt;

    float minX = (float)bounds_.left;
    float minY = (float)bounds_.top;
    float maxX = (float)(bounds_.right - w_);
    float maxY = (float)(bounds_.bottom - h_);
    if (maxX < minX) maxX = minX;
    if (maxY < minY) maxY = minY;

    bool bounced = false;
    if (x_ < minX)      { x_ = minX; vx_ = std::fabs(vx_);  bounced = true; }
    else if (x_ > maxX) { x_ = maxX; vx_ = -std::fabs(vx_); bounced = true; }

    if (y_ < minY)      { y_ = minY; vy_ = std::fabs(vy_);  bounced = true; }
    else if (y_ > maxY) { y_ = maxY; vy_ = -std::fabs(vy_); bounced = true; }

    if (bounced)
        OnBounce();

    if (visible_) {
        if (dirty_) {
            Render();
            renderedAngle_ = angle_;
            dirty_ = false;
        }
        Present();
    }
}

void Overlay::Render()
{
    using namespace Gdiplus;

    // 1. Портрет с круглой маской.
    {
        Graphics g(surface_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.Clear(Color(0, 0, 0, 0));

        g.TranslateTransform(w_ / 2.0f, h_ / 2.0f);
        g.RotateTransform(angle_);
        g.TranslateTransform(-w_ / 2.0f, -h_ / 2.0f);

        GraphicsPath clip;
        clip.AddEllipse(3.0f, 3.0f, (REAL)w_ - 6.0f, (REAL)h_ - 6.0f);
        g.SetClip(&clip);

        if (face_)
            g.DrawImage(face_, RectF(0, 0, (REAL)w_, (REAL)h_), 0, 0,
                        (REAL)face_->GetWidth(), (REAL)face_->GetHeight(), UnitPixel);
    }

    // 2. Тонирование «как у DVD-логотипа» — вручную по пикселям,
    //    потому что в MinGW-шном GDI+ нет CompositingModeSourceAtop.
    {
        BYTE mx = std::max(std::max(tintR_, tintG_), tintB_);
        if (mx == 0)
            mx = 1;
        const float strength = 0.55f;
        const float fr = 1.0f - strength + strength * (float)tintR_ / (float)mx;
        const float fg = 1.0f - strength + strength * (float)tintG_ / (float)mx;
        const float fb = 1.0f - strength + strength * (float)tintB_ / (float)mx;

        BYTE* p = (BYTE*)bits_;
        const int total = w_ * h_;
        for (int i = 0; i < total; ++i, p += 4) {
            if (p[3] == 0)
                continue;
            p[0] = (BYTE)std::min(255.0f, (float)p[0] * fb);
            p[1] = (BYTE)std::min(255.0f, (float)p[1] * fg);
            p[2] = (BYTE)std::min(255.0f, (float)p[2] * fr);
        }
    }

    // 3. Ободок.
    {
        Graphics g(surface_);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.TranslateTransform(w_ / 2.0f, h_ / 2.0f);
        g.RotateTransform(angle_);
        g.TranslateTransform(-w_ / 2.0f, -h_ / 2.0f);

        Pen shadow(Color(110, 0, 0, 0), 7.0f);
        g.DrawEllipse(&shadow, 3.0f, 3.0f, (REAL)w_ - 6.0f, (REAL)h_ - 6.0f);
        Pen ring(Color(235, 255, 255, 255), 3.5f);
        g.DrawEllipse(&ring, 3.0f, 3.0f, (REAL)w_ - 6.0f, (REAL)h_ - 6.0f);
    }
}

void Overlay::Present()
{
    if (!hwnd_ || !memDC_ || !screenDC_)
        return;

    POINT dst{ (LONG)std::lround(x_), (LONG)std::lround(y_) };
    SIZE sz{ w_, h_ };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    if (!UpdateLayeredWindow(hwnd_, screenDC_, &dst, &sz, memDC_, &src, 0, &bf,
                             ULW_ALPHA)) {
        if (!presentWarned_) {
            printf("[Окно] UpdateLayeredWindow: ошибка %lu.\n", GetLastError());
            presentWarned_ = true;
        }
    }
}

bool Overlay::DumpSprite(const wchar_t* path)
{
    CLSID png{};
    if (!surface_ || EncoderClsid(L"image/png", &png) < 0)
        return false;
    return surface_->Save(path, &png, nullptr) == Gdiplus::Ok;
}

void Overlay::ToggleVisible()
{
    visible_ = !visible_;
    if (!visible_)
        ShowWindow(hwnd_, SW_HIDE);
    else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        Render();
        Present();
    }
}

void Overlay::Shutdown(){
    if (screenDC_) { ReleaseDC(nullptr, screenDC_); screenDC_ = nullptr; }
    delete surface_;
    surface_ = nullptr;
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
}

// ---------------------------------------------------------------------------
//  Ресурсы
// ---------------------------------------------------------------------------
static Gdiplus::Image* LoadFaceFromResource(HMODULE inst)
{
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(IDR_FACE), MAKEINTRESOURCEW(10));
    if (!res)
        return nullptr;

    DWORD size = SizeofResource(inst, res);
    HGLOBAL handle = LoadResource(inst, res);
    void* data = handle ? LockResource(handle) : nullptr;
    if (!data || !size)
        return nullptr;

    IStream* stream = SHCreateMemStream((const BYTE*)data, size);
    if (!stream)
        return nullptr;

    Gdiplus::Image* img = Gdiplus::Image::FromStream(stream, FALSE);
    stream->Release();
    return img;
}

// ---------------------------------------------------------------------------
//  Помощь / разбор аргументов
// ---------------------------------------------------------------------------
static void PrintHelp()
{
    printf(
        "Sam Altman — занимает свободную RAM/VRAM и запускает портрет по экрану.\n"
        "\n"
        "Аргументы:\n"
        "  --ram-reserve=МБ     сколько RAM оставить свободной (по умолчанию 1536)\n"
        "  --vram-reserve=МБ    сколько VRAM оставить свободной (по умолчанию 1024)\n"
        "  --ram-cap=МБ         максимум занятой RAM (0 = без ограничения)\n"
        "  --vram-cap=МБ        максимум занятой VRAM (0 = без ограничения)\n"
        "  --no-ram             не трогать оперативную память\n"
        "  --no-vram            не трогать видеопамять\n"
        "  --no-overlay         не показывать летающего Sam\n"
        "  --size=ПИКСЕЛИ       размер портрета (по умолчанию 220)\n"
        "  --speed=ПИКС/СЕК     скорость полёта (по умолчанию 280)\n"
        "  --duration=СЕК       выйти через N секунд (0 = до Ctrl+Alt+Q)\n"
        "  --test               безопасный тест: 2 ГБ RAM, 512 МБ VRAM, 12 сек\n"
        "  --help               эта справка\n"
        "\n"
        "Выход: Ctrl+Alt+Q | Пауза/освободить: Ctrl+Alt+F | Скрыть: Ctrl+Alt+H\n");
}

static bool ParseU64(const std::string& text, uint64_t& out)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    unsigned long long value = _strtoui64(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
        return false;
    out = (uint64_t)value;
    return true;
}

static void ParseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* prefix) { return a.substr(std::strlen(prefix)); };

        if (a == "--help" || a == "-h" || a == "/?") {
            PrintHelp();
            ExitProcess(0);
        }
        else if (a.rfind("--ram-reserve=", 0) == 0)  ParseU64(value("--ram-reserve="), opt.ramReserveMB);
        else if (a.rfind("--vram-reserve=", 0) == 0) ParseU64(value("--vram-reserve="), opt.vramReserveMB);
        else if (a.rfind("--ram-cap=", 0) == 0)      ParseU64(value("--ram-cap="), opt.ramCapMB);
        else if (a.rfind("--vram-cap=", 0) == 0)     ParseU64(value("--vram-cap="), opt.vramCapMB);
        else if (a.rfind("--size=", 0) == 0) {
            uint64_t v = 0;
            if (ParseU64(value("--size="), v))
                opt.spriteSize = (int)std::clamp<uint64_t>(v, 64, 512);
        }
        else if (a.rfind("--speed=", 0) == 0) {
            uint64_t v = 0;
            if (ParseU64(value("--speed="), v))
                opt.speed = (float)std::clamp<uint64_t>(v, 40, 2000);
        }
        else if (a.rfind("--duration=", 0) == 0) {
            uint64_t v = 0;
            if (ParseU64(value("--duration="), v))
                opt.durationSec = (int)std::min<uint64_t>(v, 86400);
        }
        else if (a == "--no-ram")     opt.noRam = true;
        else if (a == "--no-vram")    opt.noVram = true;
        else if (a == "--no-overlay") opt.noOverlay = true;
        else if (a == "--dump-sprite") opt.dumpSprite = true;
        else if (a == "--test") {
            opt.ramReserveMB = 2048;
            opt.ramCapMB = 1024;
            opt.vramReserveMB = 512;
            opt.vramCapMB = 512;
            if (opt.durationSec == 0)
                opt.durationSec = 12;
        }
        else {
            printf("Неизвестный аргумент: %s (см. --help)\n", a.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
//  Консоль
// ---------------------------------------------------------------------------
static BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

static void PrintBanner(const Options& opt)
{
    printf(
        "============================================================\n"
        "  Sam Altman  ·  пожиратель RAM и VRAM\n"
        "============================================================\n"
        "  RAM:  оставить свободными %llu МБ\n"
        "  VRAM: оставить свободными %llu МБ\n"
        "  Выход: Ctrl+Alt+Q   |   Пауза + освободить: Ctrl+Alt+F\n"
        "  Скрыть/показать: Ctrl+Alt+H   |   Ctrl+C в консоли — тоже выход\n"
        "------------------------------------------------------------\n",
        (unsigned long long)opt.ramReserveMB,
        (unsigned long long)opt.vramReserveMB);
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Sam Altman — пожиратель памяти");

    Options opt;
    ParseArgs(argc, argv, opt);

    // DPI: работаем в физических пикселях, чтобы окно совпадало с экраном.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok) {
        printf("Не удалось запустить GDI+.\n");
        return 1;
    }

    HMODULE inst = GetModuleHandleW(nullptr);
    PrintBanner(opt);
    printf("[i] Экран (физ. пиксели): %d x %d, начало %d,%d\n",
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
           GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN));

    timeBeginPeriod(1);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    RamFiller ram(opt);
    VramFiller vram(opt);

    if (!opt.noRam)
        ram.Start();

    bool vramReady = false;
    if (!opt.noVram) {
        vramReady = vram.Init();
        if (vramReady)
            vram.Start();
    }

    Gdiplus::Image* face = LoadFaceFromResource(inst);
    if (!face)
        printf("[Ресурс] Портрет не найден — летать будет пустой кружок.\n");

    Overlay overlay;
    bool overlayOk = false;
    if (!opt.noOverlay) {
        overlayOk = overlay.Init(inst, face, opt);
        if (!overlayOk)
            printf("[Окно] Портрет не запущен.\n");
        else if (opt.dumpSprite)
            printf("[Окно] Дамп спрайта: %s\n",
                   overlay.DumpSprite(L"sprite_debug.png") ? "ok" : "fail");
    }

    RegisterHotKey(nullptr, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT, 'Q');
    RegisterHotKey(nullptr, HOTKEY_FREE, MOD_CONTROL | MOD_ALT, 'F');
    RegisterHotKey(nullptr, HOTKEY_HIDE, MOD_CONTROL | MOD_ALT, 'H');

    printf("[i] Поехали. Память заполняется в фоне, Sam уже летает.\n\n");

    auto start = std::chrono::steady_clock::now();
    auto last = start;
    auto lastStatus = start;
    auto nextFrame = start;
    double fpsAccum = 0.0;
    int fpsFrames = 0;
    double fps = 0.0;

    while (g_running.load()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOTKEY_QUIT) {
                    g_running.store(false);
                }
                else if (msg.wParam == HOTKEY_FREE) {
                    bool paused = !g_fillPaused.load();
                    g_fillPaused.store(paused);
                    if (paused) {
                        ram.FreeAll();
                        if (vramReady)
                            vram.FreeAll();
                        printf("\n[!] Память освобождена, заполнение на паузе. "
                               "Ctrl+Alt+F — продолжить.\n");
                    }
                    else {
                        printf("\n[!] Заполнение продолжено.\n");
                    }
                }
                else if (msg.wParam == HOTKEY_HIDE) {
                    overlay.ToggleVisible();
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        if (!opt.noOverlay)
            overlay.Update(dt);

        fpsAccum += dt;
        ++fpsFrames;
        if (fpsAccum >= 0.5) {
            fps = fpsFrames / fpsAccum;
            fpsAccum = 0.0;
            fpsFrames = 0;
        }

        if (std::chrono::duration<double>(now - lastStatus).count() >= 1.0) {
            lastStatus = now;

            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);

            printf("[%3llds] RAM: занято %.1f ГБ | свободно %.1f ГБ | "
                   "VRAM: %.1f / %.1f ГБ | %.0f FPS | отскоков: %llu\n",
                   (long long)std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                   ram.AllocatedBytes() / 1073741824.0,
                   ms.ullAvailPhys / 1073741824.0,
                   vram.AllocatedBytes() / 1073741824.0,
                   vram.BudgetBytes() / 1073741824.0,
                   fps,
                   (unsigned long long)overlay.Bounces());
            fflush(stdout);
        }

        if (opt.durationSec > 0 &&
            std::chrono::duration<double>(now - start).count() >= opt.durationSec) {
            printf("\n[i] Время вышло (--duration).\n");
            g_running.store(false);
        }

        nextFrame += std::chrono::milliseconds(16);
        auto finished = std::chrono::steady_clock::now();
        if (nextFrame > finished)
            std::this_thread::sleep_until(nextFrame);
        else
            nextFrame = finished;
    }

    printf("\n[i] Выход: освобождаю память...\n");
    UnregisterHotKey(nullptr, HOTKEY_QUIT);
    UnregisterHotKey(nullptr, HOTKEY_FREE);
    UnregisterHotKey(nullptr, HOTKEY_HIDE);

    overlay.Shutdown();
    ram.Stop();
    vram.Stop();
    delete face;

    if (g_ramDangerFrees)
        printf("[i] Аварийный предохранитель срабатывал %llu раз.\n",
               (unsigned long long)g_ramDangerFrees);

    timeEndPeriod(1);
    Gdiplus::GdiplusShutdown(gdiToken);
    printf("[i] Готово. Пока!\n");
    return 0;
}
