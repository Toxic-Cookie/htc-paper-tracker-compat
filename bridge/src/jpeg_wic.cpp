#include "jpeg_wic.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

bool JpegEncoder::init() {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory_)));
}

bool JpegEncoder::encodeGray(const uint8_t* pixels, int width, int height, int stride, float quality,
                             std::vector<uint8_t>& out) {
    if (!factory_) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;

    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = quality;
    props->Write(1, &opt, &v);

    WICPixelFormatGUID fmt = GUID_WICPixelFormat8bppGray;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat8bppGray) return false;
    if (FAILED(frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(pixels)))) return false;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;

    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return false;
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    const size_t size = static_cast<size_t>(st.cbSize.QuadPart);
    const void* p = GlobalLock(mem);
    if (!p) return false;
    out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);
    GlobalUnlock(mem);
    return true;
}
