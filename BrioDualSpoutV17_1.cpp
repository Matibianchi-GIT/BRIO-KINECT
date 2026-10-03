#define NOMINMAX
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.MediaProperties.h>

#include "SpoutLibrary.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Capture;
using namespace Windows::Media::Capture::Frames;
using namespace Windows::Media::MediaProperties;

using Clock = std::chrono::steady_clock;

struct __declspec(uuid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D"))
IMemoryBufferByteAccess : ::IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(
        BYTE** value,
        UINT32* capacity) = 0;
};

struct FrameStore
{
    std::mutex mutex;
    std::vector<unsigned char> bgra;
    int width = 0;
    int height = 0;
    std::atomic<unsigned long long> version{0};
    std::atomic<unsigned long long> captureFrames{0};
    std::atomic<unsigned long long> convertedFrames{0};
    std::atomic<unsigned long long> errors{0};
};

static HWND g_hwnd = nullptr;

static MediaCapture g_capture{nullptr};
static MediaFrameSource g_rgbSource{nullptr};
static MediaFrameSource g_irSource{nullptr};
static MediaFrameReader g_rgbReader{nullptr};
static MediaFrameReader g_irReader{nullptr};
static event_token g_rgbToken{};
static event_token g_irToken{};

static FrameStore g_rgb;
static FrameStore g_ir;

static HMODULE g_spoutDll = nullptr;
static SPOUTHANDLE g_glOwner = nullptr;
static SPOUTHANDLE g_spoutRgb = nullptr;
static SPOUTHANDLE g_spoutIr = nullptr;

static unsigned long long g_lastRgbSentVersion = 0;
static unsigned long long g_lastIrSentVersion = 0;
static std::atomic<unsigned long long> g_rgbSpoutFrames{0};
static std::atomic<unsigned long long> g_irSpoutFrames{0};

static double g_rgbCaptureFps = 0.0;
static double g_irCaptureFps = 0.0;
static double g_rgbSpoutFps = 0.0;
static double g_irSpoutFps = 0.0;

static std::wstring g_status = L"Inicializando...";
static bool g_faceAuthVerified = false;
static bool g_rgbStarted = false;
static bool g_irStarted = false;

static const wchar_t* kWindowClass = L"BRIO_DUAL_SPOUT_V17_1";
static const wchar_t* kWindowTitle = L"Logitech BRIO Dual RGB + IR / Spout V17.1";

static bool IsPhysicalBrio(MediaFrameSourceInfo const& info)
{
    std::wstring id = info.Id().c_str();

    return
        id.find(L"USB#VID_046D&PID_085E&MI_00") != std::wstring::npos &&
        id.find(L"SWD#VCAMDEVAPI") == std::wstring::npos;
}

static double FrameRate(MediaFrameFormat const& f)
{
    auto r = f.FrameRate();
    auto d = r.Denominator();

    if (!d)
        return 0.0;

    return (double)r.Numerator() / (double)d;
}

static MediaFrameSourceGroup FindPhysicalBrioGroup()
{
    auto groups = MediaFrameSourceGroup::FindAllAsync().get();

    for (auto const& group : groups)
    {
        bool hasRgb = false;
        bool hasIr = false;

        for (auto const& info : group.SourceInfos())
        {
            if (!IsPhysicalBrio(info))
                continue;

            if (info.SourceKind() == MediaFrameSourceKind::Color)
                hasRgb = true;

            if (info.SourceKind() == MediaFrameSourceKind::Infrared)
                hasIr = true;
        }

        if (hasRgb && hasIr)
            return group;
    }

    return nullptr;
}

static MediaFrameFormat FindExactFormat(
    MediaFrameSource const& source,
    uint32_t width,
    uint32_t height,
    double fps,
    wchar_t const* subtype)
{
    for (auto const& f : source.SupportedFormats())
    {
        auto vf = f.VideoFormat();

        if (vf.Width() != width || vf.Height() != height)
            continue;

        if (std::abs(FrameRate(f) - fps) > 0.25)
            continue;

        if (subtype && _wcsicmp(f.Subtype().c_str(), subtype) != 0)
            continue;

        return f;
    }

    return nullptr;
}

static std::array<uint8_t, sizeof(KSPROPERTY)> MakeFaceAuthId(ULONG propertyFlags)
{
    KSPROPERTY p{};
    p.Set = KSPROPERTYSETID_ExtendedCameraControl;
    p.Id = KSPROPERTY_CAMERACONTROL_EXTENDED_FACEAUTH_MODE;
    p.Flags = propertyFlags;

    std::array<uint8_t, sizeof(KSPROPERTY)> bytes{};
    std::memcpy(bytes.data(), &p, sizeof(p));
    return bytes;
}

static bool FaceAuthGet(
    MediaFrameSource const& ir,
    std::vector<uint8_t>& out)
{
    auto id = MakeFaceAuthId(KSPROPERTY_TYPE_GET);

    auto maxSize =
        box_value((uint32_t)4096)
        .as<IReference<uint32_t>>();

    auto result =
        ir.Controller().GetPropertyByExtendedIdAsync(
            array_view<uint8_t const>(id),
            maxSize).get();

    if ((int)result.Status() != 0 || !result.Value())
        return false;

    auto property = result.Value().as<IPropertyValue>();

    winrt::com_array<uint8_t> arr;
    property.GetUInt8Array(arr);

    out.assign(arr.begin(), arr.end());
    return !out.empty();
}

static int FaceAuthSetExact(
    MediaFrameSource const& ir,
    std::vector<uint8_t> const& payload)
{
    auto id = MakeFaceAuthId(KSPROPERTY_TYPE_SET);

    auto status =
        ir.Controller().SetPropertyByExtendedIdAsync(
            array_view<uint8_t const>(id),
            array_view<uint8_t const>(
                payload.data(),
                payload.data() + payload.size())).get();

    return (int)status;
}

static bool ValidateFaceAuthPayload(
    std::vector<uint8_t> const& payload,
    std::wstring& description)
{
    if (payload.size() < sizeof(KSCAMERA_EXTENDEDPROP_HEADER))
    {
        description = L"FaceAuth payload demasiado corto";
        return false;
    }

    KSCAMERA_EXTENDEDPROP_HEADER h{};
    std::memcpy(&h, payload.data(), sizeof(h));

    std::wstringstream ss;
    ss
        << L"FaceAuth pin=" << h.PinId
        << L" size=" << h.Size
        << L" flags=0x" << std::hex << std::uppercase
        << h.Flags;

    if (h.Flags & KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION)
        ss << L" ALT_FRAME_ILLUMINATION";

    if (h.Flags & KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_BACKGROUND_SUBTRACTION)
        ss << L" BACKGROUND_SUBTRACTION";

    if (h.Flags & KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_DISABLED)
        ss << L" DISABLED";

    description = ss.str();

    return
        h.Version == 1 &&
        h.PinId == 1 &&
        h.Size == payload.size() &&
        (h.Flags & KSCAMERA_EXTENDEDPROP_FACEAUTH_MODE_ALTERNATIVE_FRAME_ILLUMINATION) != 0;
}

static bool CopyBitmapToBGRA(
    SoftwareBitmap const& input,
    std::vector<unsigned char>& out,
    int& width,
    int& height)
{
    if (!input)
        return false;

    SoftwareBitmap bmp = input;
    auto format = bmp.BitmapPixelFormat();

    if (format != BitmapPixelFormat::Gray8 &&
        format != BitmapPixelFormat::Bgra8)
    {
        bmp = SoftwareBitmap::Convert(
            input,
            BitmapPixelFormat::Bgra8,
            BitmapAlphaMode::Ignore);

        format = bmp.BitmapPixelFormat();
    }

    width = bmp.PixelWidth();
    height = bmp.PixelHeight();

    if (width <= 0 || height <= 0)
        return false;

    auto buffer = bmp.LockBuffer(BitmapBufferAccessMode::Read);
    auto desc = buffer.GetPlaneDescription(0);
    auto reference = buffer.CreateReference();
    auto access = reference.as<IMemoryBufferByteAccess>();

    BYTE* base = nullptr;
    UINT32 capacity = 0;

    HRESULT hr = access->GetBuffer(&base, &capacity);

    if (FAILED(hr) || !base)
        return false;

    const size_t dstStride = (size_t)width * 4;
    out.resize(dstStride * (size_t)height);

    if (format == BitmapPixelFormat::Gray8)
    {
        for (int y = 0; y < height; ++y)
        {
            const BYTE* src =
                base + desc.StartIndex +
                (ptrdiff_t)y * (ptrdiff_t)desc.Stride;

            BYTE* dst =
                out.data() +
                (size_t)y * dstStride;

            for (int x = 0; x < width; ++x)
            {
                BYTE v = src[x];
                dst[x * 4 + 0] = v;
                dst[x * 4 + 1] = v;
                dst[x * 4 + 2] = v;
                dst[x * 4 + 3] = 255;
            }
        }

        return true;
    }

    if (format != BitmapPixelFormat::Bgra8)
        return false;

    for (int y = 0; y < height; ++y)
    {
        const BYTE* src =
            base + desc.StartIndex +
            (ptrdiff_t)y * (ptrdiff_t)desc.Stride;

        BYTE* dst =
            out.data() +
            (size_t)y * dstStride;

        memcpy(dst, src, dstStride);
    }

    return true;
}

static void AttachReaderCallback(
    MediaFrameReader const& reader,
    FrameStore& store,
    event_token& token)
{
    token = reader.FrameArrived(
        [&store](
            MediaFrameReader const& sender,
            MediaFrameArrivedEventArgs const&)
        {
            try
            {
                auto frame = sender.TryAcquireLatestFrame();

                if (!frame)
                    return;

                ++store.captureFrames;

                auto vmf = frame.VideoMediaFrame();

                if (!vmf)
                    return;

                auto bmp = vmf.SoftwareBitmap();

                if (!bmp)
                    return;

                std::vector<unsigned char> pixels;
                int w = 0;
                int h = 0;

                if (!CopyBitmapToBGRA(bmp, pixels, w, h))
                    return;

                {
                    std::lock_guard<std::mutex> lock(store.mutex);
                    store.bgra = std::move(pixels);
                    store.width = w;
                    store.height = h;
                    ++store.version;
                }

                ++store.convertedFrames;
            }
            catch (...)
            {
                ++store.errors;
            }
        });
}

static bool InitializeCamera()
{
    g_status = L"Buscando BRIO física...";

    auto group = FindPhysicalBrioGroup();

    if (!group)
    {
        MessageBoxW(
            g_hwnd,
            L"No se encontró la Logitech BRIO física VID_046D / PID_085E con fuentes Color + Infrared.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    MediaCaptureInitializationSettings settings;
    settings.SourceGroup(group);
    settings.SharingMode(MediaCaptureSharingMode::ExclusiveControl);
    settings.StreamingCaptureMode(StreamingCaptureMode::Video);
    settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);

    g_capture = MediaCapture();
    g_capture.InitializeAsync(settings).get();

    for (auto const& kv : g_capture.FrameSources())
    {
        auto source = kv.Value();

        if (!IsPhysicalBrio(source.Info()))
            continue;

        if (source.Info().SourceKind() == MediaFrameSourceKind::Color)
            g_rgbSource = source;

        if (source.Info().SourceKind() == MediaFrameSourceKind::Infrared)
            g_irSource = source;
    }

    if (!g_rgbSource || !g_irSource)
    {
        MessageBoxW(
            g_hwnd,
            L"La BRIO fue encontrada, pero no aparecen simultáneamente Source#0 Color y Source#1 Infrared.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    auto rgbFormat = FindExactFormat(
        g_rgbSource,
        800,
        600,
        30.0,
        L"YUY2");

    auto irFormat = FindExactFormat(
        g_irSource,
        340,
        340,
        30.0,
        L"L8");

    if (!rgbFormat || !irFormat)
    {
        MessageBoxW(
            g_hwnd,
            L"No se encontró la combinación probada: RGB 800x600 YUY2 @30 + IR 340x340 L8 @30.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    // Preserve the exact combination that V16 proved simultaneous.
    g_rgbSource.SetFormatAsync(rgbFormat).get();
    g_irSource.SetFormatAsync(irFormat).get();

    // Read the real 40-byte payload from the IR source and re-assert it exactly.
    std::vector<uint8_t> facePayload;

    if (!FaceAuthGet(g_irSource, facePayload))
    {
        MessageBoxW(
            g_hwnd,
            L"No se pudo leer FaceAuth desde Source#1 Infrared.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    std::wstring faceDescription;

    if (!ValidateFaceAuthPayload(facePayload, faceDescription))
    {
        std::wstring msg =
            L"El payload FaceAuth no coincide con la ruta validada por V15.2.\n\n" +
            faceDescription;

        MessageBoxW(
            g_hwnd,
            msg.c_str(),
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    const int faceSetStatus =
        FaceAuthSetExact(
            g_irSource,
            facePayload);

    if (faceSetStatus != 0)
    {
        MessageBoxW(
            g_hwnd,
            L"FaceAuth SET no devolvió Success.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    std::vector<uint8_t> verifyPayload;

    if (!FaceAuthGet(g_irSource, verifyPayload) ||
        verifyPayload != facePayload)
    {
        MessageBoxW(
            g_hwnd,
            L"FaceAuth SET se ejecutó, pero la verificación exacta del payload falló.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    g_faceAuthVerified = true;

    // Ask FrameReader for BGRA output while preserving the native source
    // formats selected above. This is only a conversion for our preview/Spout.
    try
    {
        g_irReader =
            g_capture.CreateFrameReaderAsync(
                g_irSource,
                MediaEncodingSubtypes::Bgra8()).get();
    }
    catch (...)
    {
        g_irReader =
            g_capture.CreateFrameReaderAsync(
                g_irSource).get();
    }

    try
    {
        g_rgbReader =
            g_capture.CreateFrameReaderAsync(
                g_rgbSource,
                MediaEncodingSubtypes::Bgra8()).get();
    }
    catch (...)
    {
        g_rgbReader =
            g_capture.CreateFrameReaderAsync(
                g_rgbSource).get();
    }

    AttachReaderCallback(
        g_irReader,
        g_ir,
        g_irToken);

    AttachReaderCallback(
        g_rgbReader,
        g_rgb,
        g_rgbToken);

    // Critical order discovered by V16:
    // IR first, then RGB after ~100 ms.
    g_status = L"Iniciando IR primero...";

    auto irStatus =
        g_irReader.StartAsync().get();

    g_irStarted =
        irStatus == MediaFrameReaderStartStatus::Success;

    if (!g_irStarted)
    {
        MessageBoxW(
            g_hwnd,
            L"No se pudo iniciar IR. Cierra Camera, OBS, TouchDesigner, Logi Tune y otras apps que usen la BRIO.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));

    g_status = L"Iniciando RGB después de IR...";

    auto rgbStatus =
        g_rgbReader.StartAsync().get();

    g_rgbStarted =
        rgbStatus == MediaFrameReaderStartStatus::Success;

    if (!g_rgbStarted)
    {
        MessageBoxW(
            g_hwnd,
            L"IR inició, pero RGB no pudo iniciarse.",
            L"BRIO V17.1",
            MB_ICONERROR);

        return false;
    }

    g_status =
        L"RUN: IR 340x340 L8 @30 -> RGB 800x600 YUY2 @30";

    return true;
}

static void ShutdownCamera()
{
    try
    {
        if (g_rgbReader)
        {
            g_rgbReader.FrameArrived(g_rgbToken);
            g_rgbReader.StopAsync().get();
            g_rgbReader.Close();
            g_rgbReader = nullptr;
        }

        if (g_irReader)
        {
            g_irReader.FrameArrived(g_irToken);
            g_irReader.StopAsync().get();
            g_irReader.Close();
            g_irReader = nullptr;
        }

        if (g_capture)
        {
            g_capture.Close();
            g_capture = nullptr;
        }
    }
    catch (...)
    {
    }
}

static bool InitOneSpout(
    SPOUTHANDLE& h,
    const char* name)
{
    using GETSPOUT = SPOUTHANDLE(WINAPI*)();

    auto fn =
        reinterpret_cast<GETSPOUT>(
            GetProcAddress(
                g_spoutDll,
                "GetSpout"));

    if (!fn)
        return false;

    h = fn();

    if (!h)
        return false;

    h->SetSenderName(name);
    h->SetAutoShare(true);

    return true;
}

static bool InitializeSpout()
{
    g_spoutDll =
        LoadLibraryW(
            L"SpoutLibrary.dll");

    if (!g_spoutDll)
        return false;

    using GETSPOUT = SPOUTHANDLE(WINAPI*)();

    auto fn =
        reinterpret_cast<GETSPOUT>(
            GetProcAddress(
                g_spoutDll,
                "GetSpout"));

    if (!fn)
        return false;

    g_glOwner = fn();

    if (!g_glOwner)
        return false;

    if (!g_glOwner->CreateOpenGL(g_hwnd))
        return false;

    if (!InitOneSpout(g_spoutRgb, "BRIO_RGB"))
        return false;

    if (!InitOneSpout(g_spoutIr, "BRIO_IR"))
        return false;

    return true;
}

static void ReleaseOneSpout(
    SPOUTHANDLE& h)
{
    if (!h)
        return;

    try
    {
        h->ReleaseSender();
        h->Release();
    }
    catch (...)
    {
    }

    h = nullptr;
}

static void ShutdownSpout()
{
    ReleaseOneSpout(g_spoutRgb);
    ReleaseOneSpout(g_spoutIr);

    if (g_glOwner)
    {
        try
        {
            g_glOwner->CloseOpenGL();
            g_glOwner->Release();
        }
        catch (...)
        {
        }

        g_glOwner = nullptr;
    }

    if (g_spoutDll)
    {
        FreeLibrary(g_spoutDll);
        g_spoutDll = nullptr;
    }
}

static void SendStoreToSpout(
    FrameStore& store,
    SPOUTHANDLE sender,
    unsigned long long& lastVersion,
    std::atomic<unsigned long long>& sentCounter)
{
    if (!sender)
        return;

    const auto version =
        store.version.load();

    if (!version || version == lastVersion)
        return;

    std::vector<unsigned char> pixels;
    int w = 0;
    int h = 0;

    {
        std::lock_guard<std::mutex> lock(store.mutex);

        if (store.bgra.empty())
            return;

        pixels = store.bgra;
        w = store.width;
        h = store.height;
    }

    if (sender->SendImage(
        pixels.data(),
        (unsigned)w,
        (unsigned)h,
        GL_BGRA,
        false))
    {
        lastVersion = version;
        ++sentCounter;
    }
}

static void SendSpout()
{
    SendStoreToSpout(
        g_rgb,
        g_spoutRgb,
        g_lastRgbSentVersion,
        g_rgbSpoutFrames);

    SendStoreToSpout(
        g_ir,
        g_spoutIr,
        g_lastIrSentVersion,
        g_irSpoutFrames);
}

static void SnapshotStore(
    FrameStore& store,
    std::vector<unsigned char>& pixels,
    int& width,
    int& height)
{
    std::lock_guard<std::mutex> lock(store.mutex);

    pixels = store.bgra;
    width = store.width;
    height = store.height;
}

static void DrawPanel(
    HDC hdc,
    RECT rc,
    FrameStore& store,
    const wchar_t* label,
    double captureFps,
    double spoutFps)
{
    FillRect(
        hdc,
        &rc,
        (HBRUSH)GetStockObject(BLACK_BRUSH));

    std::vector<unsigned char> pixels;
    int w = 0;
    int h = 0;

    SnapshotStore(
        store,
        pixels,
        w,
        h);

    RECT imageRc = rc;
    imageRc.top += 34;

    if (!pixels.empty() && w > 0 && h > 0)
    {
        const int cw =
            imageRc.right - imageRc.left;

        const int ch =
            imageRc.bottom - imageRc.top;

        const double scale =
            std::min(
                (double)cw / (double)w,
                (double)ch / (double)h);

        const int dw =
            (int)std::lround(
                w * scale);

        const int dh =
            (int)std::lround(
                h * scale);

        const int dx =
            imageRc.left +
            (cw - dw) / 2;

        const int dy =
            imageRc.top +
            (ch - dh) / 2;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(
            hdc,
            HALFTONE);

        StretchDIBits(
            hdc,
            dx,
            dy,
            dw,
            dh,
            0,
            0,
            w,
            h,
            pixels.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    wchar_t text[256]{};

    swprintf_s(
        text,
        L"%s | %dx%d | Capture %.1f fps | Spout %.1f fps",
        label,
        w,
        h,
        captureFps,
        spoutFps);

    RECT textRc = rc;
    textRc.left += 10;
    textRc.top += 8;

    SetBkMode(
        hdc,
        TRANSPARENT);

    SetTextColor(
        hdc,
        RGB(255, 255, 255));

    DrawTextW(
        hdc,
        text,
        -1,
        &textRc,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
}

static void DrawPreview(
    HDC hdc,
    RECT client)
{
    FillRect(
        hdc,
        &client,
        (HBRUSH)GetStockObject(BLACK_BRUSH));

    RECT statusRc = client;
    statusRc.bottom = 42;
    statusRc.left += 10;
    statusRc.top += 8;

    SetBkMode(
        hdc,
        TRANSPARENT);

    SetTextColor(
        hdc,
        RGB(220, 220, 220));

    std::wstring status =
        g_status +
        L" | FaceAuth=" +
        (g_faceAuthVerified ? L"OK" : L"NO");

    DrawTextW(
        hdc,
        status.c_str(),
        -1,
        &statusRc,
        DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT body = client;
    body.top = 42;

    const int mid =
        (body.left + body.right) / 2;

    RECT rgbRc =
        {
            body.left,
            body.top,
            mid,
            body.bottom
        };

    RECT irRc =
        {
            mid,
            body.top,
            body.right,
            body.bottom
        };

    DrawPanel(
        hdc,
        rgbRc,
        g_rgb,
        L"RGB / BRIO_RGB",
        g_rgbCaptureFps,
        g_rgbSpoutFps);

    DrawPanel(
        hdc,
        irRc,
        g_ir,
        L"IR / BRIO_IR",
        g_irCaptureFps,
        g_irSpoutFps);

    MoveToEx(
        hdc,
        mid,
        body.top,
        nullptr);

    LineTo(
        hdc,
        mid,
        body.bottom);
}

static void UpdateStats()
{
    static unsigned long long lastRgbCapture = 0;
    static unsigned long long lastIrCapture = 0;
    static unsigned long long lastRgbSpout = 0;
    static unsigned long long lastIrSpout = 0;
    static auto lastTime = Clock::now();

    auto now = Clock::now();

    const double seconds =
        std::chrono::duration<double>(
            now - lastTime).count();

    if (seconds < 0.9)
        return;

    const auto rgbCapture =
        g_rgb.captureFrames.load();

    const auto irCapture =
        g_ir.captureFrames.load();

    const auto rgbSpout =
        g_rgbSpoutFrames.load();

    const auto irSpout =
        g_irSpoutFrames.load();

    g_rgbCaptureFps =
        (double)(rgbCapture - lastRgbCapture) /
        seconds;

    g_irCaptureFps =
        (double)(irCapture - lastIrCapture) /
        seconds;

    g_rgbSpoutFps =
        (double)(rgbSpout - lastRgbSpout) /
        seconds;

    g_irSpoutFps =
        (double)(irSpout - lastIrSpout) /
        seconds;

    lastRgbCapture = rgbCapture;
    lastIrCapture = irCapture;
    lastRgbSpout = rgbSpout;
    lastIrSpout = irSpout;
    lastTime = now;

    wchar_t title[512]{};

    swprintf_s(
        title,
        L"%s | RGB %.1f / IR %.1f fps | Spout RGB %.1f / IR %.1f",
        kWindowTitle,
        g_rgbCaptureFps,
        g_irCaptureFps,
        g_rgbSpoutFps,
        g_irSpoutFps);

    SetWindowTextW(
        g_hwnd,
        title);
}

static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        SendSpout();
        UpdateStats();
        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc =
            BeginPaint(
                hwnd,
                &ps);

        RECT rc{};
        GetClientRect(
            hwnd,
            &rc);

        DrawPreview(
            hdc,
            rc);

        EndPaint(
            hwnd,
            &ps);

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PostMessageW(
                hwnd,
                WM_CLOSE,
                0,
                0);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam);
}

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow)
{
    try
    {
        init_apartment(
            apartment_type::single_threaded);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.hCursor =
            LoadCursor(
                nullptr,
                IDC_ARROW);
        wc.hbrBackground =
            (HBRUSH)GetStockObject(
                BLACK_BRUSH);
        wc.lpszClassName =
            kWindowClass;

        if (!RegisterClassExW(&wc))
            return 1;

        g_hwnd =
            CreateWindowExW(
                0,
                kWindowClass,
                kWindowTitle,
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1080,
                620,
                nullptr,
                nullptr,
                hInstance,
                nullptr);

        if (!g_hwnd)
            return 2;

        ShowWindow(
            g_hwnd,
            nCmdShow);

        UpdateWindow(
            g_hwnd);

        if (!InitializeSpout())
        {
            MessageBoxW(
                g_hwnd,
                L"No se pudo inicializar Spout. Verifica que SpoutLibrary.dll esté junto al EXE.",
                L"BRIO V17.1",
                MB_ICONERROR);

            DestroyWindow(g_hwnd);
            ShutdownSpout();
            return 3;
        }

        if (!InitializeCamera())
        {
            DestroyWindow(g_hwnd);
            ShutdownCamera();
            ShutdownSpout();
            return 4;
        }

        // GUI/Spout pump at ~60 Hz. Each sender only pushes when a new camera
        // frame version exists.
        SetTimer(
            g_hwnd,
            1,
            16,
            nullptr);

        MSG msg{};

        while (GetMessageW(
            &msg,
            nullptr,
            0,
            0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        ShutdownCamera();
        ShutdownSpout();

        return 0;
    }
    catch (winrt::hresult_error const& e)
    {
        wchar_t buffer[1024]{};

        swprintf_s(
            buffer,
            L"Error WinRT 0x%08X\n%s",
            (unsigned)e.code().value,
            e.message().c_str());

        MessageBoxW(
            nullptr,
            buffer,
            L"BRIO V17.1",
            MB_ICONERROR);

        ShutdownCamera();
        ShutdownSpout();

        return 100;
    }
    catch (...)
    {
        MessageBoxW(
            nullptr,
            L"Error no controlado.",
            L"BRIO V17.1",
            MB_ICONERROR);

        ShutdownCamera();
        ShutdownSpout();

        return 101;
    }
}
