// ============================================================================
//  mkassets — сборщик ресурсов для Sam Altman.
//  Берёт исходный портрет, делает:
//    * face.png  — квадратный портрет 512x512 для летающего спрайта,
//    * icon.ico  — многоразмерную иконку программы (16…256 px).
//
//  Использование: mkassets <исходник.jpg> <face.png> <icon.ico>
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
#include <shlwapi.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace Gdiplus;

static std::wstring Widen(const char* text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
    return out;
}

static int GetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;

    std::vector<BYTE> buffer(size);
    ImageCodecInfo* codecs = (ImageCodecInfo*)buffer.data();
    GetImageEncoders(num, size, codecs);

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return 0;
        }
    }
    return -1;
}

static bool EncodePng(Bitmap& bitmap, std::vector<BYTE>& out)
{
    CLSID png{};
    if (GetEncoderClsid(L"image/png", &png) < 0)
        return false;

    IStream* stream = SHCreateMemStream(nullptr, 0);
    if (!stream)
        return false;

    bool ok = false;
    if (bitmap.Save(stream, &png, nullptr) == Ok) {
        STATSTG stat{};
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
            ULONG size = (ULONG)stat.cbSize.QuadPart;
            out.resize(size);
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            ULONG read = 0;
            if (SUCCEEDED(stream->Read(out.data(), size, &read)) && read == size)
                ok = true;
        }
    }
    stream->Release();
    return ok;
}

static void Append(std::vector<BYTE>& v, const void* data, size_t n)
{
    const BYTE* p = (const BYTE*)data;
    v.insert(v.end(), p, p + n);
}

static int Generate(const char* srcPath, const char* facePath, const char* iconPath)
{
    std::wstring sourcePath = Widen(srcPath);
    std::wstring faceOut    = Widen(facePath);
    std::wstring iconOut    = Widen(iconPath);

    Bitmap source(sourcePath.c_str());
    if (source.GetLastStatus() != Ok) {
        printf("cannot load %s\n", srcPath);
        return 1;
    }

    printf("source: %dx%d\n", (int)source.GetWidth(), (int)source.GetHeight());

    // --- face.png (512x512) -------------------------------------------------
    {
        Bitmap face(512, 512, PixelFormat32bppARGB);
        {
            Graphics g(&face);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(&source, Rect(0, 0, 512, 512));
        }

        CLSID png{};
        GetEncoderClsid(L"image/png", &png);
        if (face.Save(faceOut.c_str(), &png, nullptr) != Ok) {
            printf("cannot save %s\n", facePath);
            return 1;
        }
        printf("face.png: ok\n");
    }

    // --- icon.ico -----------------------------------------------------------
    const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };
    const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));

    struct Entry {
        int size;
        std::vector<BYTE> png;
    };
    std::vector<Entry> entries;
    entries.reserve(count);

    for (int i = 0; i < count; ++i) {
        int s = sizes[i];
        Bitmap icon(s, s, PixelFormat32bppARGB);
        {
            Graphics g(&icon);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(&source, Rect(0, 0, s, s));
        }

        Entry entry{ s, {} };
        if (!EncodePng(icon, entry.png)) {
            printf("png encode failed for %d\n", s);
            return 1;
        }
        entries.push_back(std::move(entry));
    }

    std::vector<BYTE> ico;
    WORD header[3] = { 0, 1, (WORD)count };
    Append(ico, header, sizeof(header));

    DWORD offset = 6 + 16 * (DWORD)count;
    for (const Entry& e : entries) {
        BYTE wb = (e.size >= 256) ? 0 : (BYTE)e.size;
        BYTE entry[16] = {};
        entry[0] = wb;
        entry[1] = wb;
        entry[2] = 0;
        entry[3] = 0;
        *(WORD*)(entry + 4) = 1;
        *(WORD*)(entry + 6) = 32;
        *(DWORD*)(entry + 8) = (DWORD)e.png.size();
        *(DWORD*)(entry + 12) = offset;
        Append(ico, entry, sizeof(entry));
        offset += (DWORD)e.png.size();
    }
    for (const Entry& e : entries)
        Append(ico, e.png.data(), e.png.size());

    FILE* f = _wfopen(iconOut.c_str(), L"wb");
    if (!f) {
        printf("cannot write %s\n", iconPath);
        return 1;
    }
    fwrite(ico.data(), 1, ico.size(), f);
    fclose(f);

    printf("icon.ico: %d sizes, %zu bytes\n", count, ico.size());
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        printf("usage: mkassets <source.jpg> <face.png> <icon.ico>\n");
        return 2;
    }

    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        printf("GDI+ startup failed\n");
        return 1;
    }

    int rc = Generate(argv[1], argv[2], argv[3]);

    GdiplusShutdown(token);
    return rc;
}
