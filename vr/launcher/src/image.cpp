#include "image.h"
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#include <utility>
#elif defined(HSL_PNG)
#include <png.h>
#endif

namespace img {

#if defined(_WIN32)
// (Windows: its own PNG codec, WIC, so the Windows launcher needs no libpng. Paths are UTF-8, turned into UTF-16 here)
static std::wstring W(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
template <class T> static void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}
struct Com {   // (COM for this thread while one call runs; a thread that has it already keeps its own)
    bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ~Com() { if (ok) CoUninitialize(); }
};
static IWICImagingFactory *factory() {
    IWICImagingFactory *f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool available() { return true; }

bool load_png(const std::string &path, Image &out) {
    Com com;
    IWICImagingFactory *f = factory();
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = f && SUCCEEDED(f->CreateDecoderFromFilename(W(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h)) && w && h;
    if (ok) {
        out.width = (int)w, out.height = (int)h;
        out.rgba.resize((size_t)w * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.rgba.size(), out.rgba.data()));
    }
    if (!ok) out = Image();
    release(conv), release(frame), release(dec), release(f);
    return ok;
}

bool save_png(const std::string &path, const uint8_t *rgba, int width, int height, bool alpha) {
    Com com;
    std::vector<uint8_t> px(rgba, rgba + (size_t)width * height * 4);
    if (!alpha)
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // (opaque: the window has no see-through parts)
    IWICImagingFactory *f = factory();
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    // (the encoder answers the pixel format it takes: RGBA, or BGRA where it has no RGBA (older Windows, Wine))
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    bool ok = f && SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(W(path).c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize((UINT)width, (UINT)height)) &&
              SUCCEEDED(frame->SetPixelFormat(&fmt));
    if (ok && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    else if (ok && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA))
        ok = false;
    ok = ok && SUCCEEDED(frame->WritePixels((UINT)height, (UINT)width * 4, (UINT)px.size(), px.data())) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    release(frame), release(enc), release(st), release(f);
    return ok;
}
#elif defined(HSL_PNG)
bool available() { return true; }

bool load_png(const std::string &path, Image &out) {
    png_image im{};
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path.c_str())) return false;
    im.format = PNG_FORMAT_RGBA;
    out.width = (int)im.width, out.height = (int)im.height;
    out.rgba.resize(PNG_IMAGE_SIZE(im));
    if (!png_image_finish_read(&im, nullptr, out.rgba.data(), 0, nullptr)) {
        png_image_free(&im);
        out = Image();
        return false;
    }
    return true;
}

bool save_png(const std::string &path, const uint8_t *rgba, int width, int height, bool alpha) {
    png_image im{};
    im.version = PNG_IMAGE_VERSION;
    im.width = (png_uint_32)width, im.height = (png_uint_32)height;
    if (alpha) {
        im.format = PNG_FORMAT_RGBA;
        return png_image_write_to_file(&im, path.c_str(), 0, rgba, 0, nullptr) != 0;
    }
    im.format = PNG_FORMAT_RGB;
    std::vector<uint8_t> rgb((size_t)width * height * 3);   // (opaque: the window has no see-through parts)
    for (size_t i = 0, n = (size_t)width * height; i < n; i++)
        rgb[i * 3] = rgba[i * 4], rgb[i * 3 + 1] = rgba[i * 4 + 1], rgb[i * 3 + 2] = rgba[i * 4 + 2];
    return png_image_write_to_file(&im, path.c_str(), 0, rgb.data(), 0, nullptr) != 0;
}
#else
bool available() { return false; }
bool load_png(const std::string &, Image &) { return false; }
bool save_png(const std::string &, const uint8_t *, int, int, bool) { return false; }
#endif

}  // namespace img
