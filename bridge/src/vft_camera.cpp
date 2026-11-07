#include "vft_camera.h"

#include <mfapi.h>
#include <mferror.h>
#include <algorithm>
#include <chrono>
#include <cwctype>

#include "log.h"

using Microsoft::WRL::ComPtr;

class ReaderCallback : public IMFSourceReaderCallback {
public:
    explicit ReaderCallback(VftCamera* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override {
        VftCamera* o = owner_.load();
        if (o) o->onSample(hr, flags, sample);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    void detach() { owner_.store(nullptr); }

private:
    std::atomic<VftCamera*> owner_;
    volatile ULONG refs_ = 1;
};

namespace {
std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring attrString(IMFActivate* a, const GUID& key) {
    WCHAR* v = nullptr;
    UINT32 n = 0;
    std::wstring out;
    if (SUCCEEDED(a->GetAllocatedString(key, &v, &n))) {
        out.assign(v, n);
        CoTaskMemFree(v);
    }
    return out;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}
}  // namespace

VftCamera::VftCamera() = default;
VftCamera::~VftCamera() { close(); }

std::vector<VideoDevice> VftCamera::enumerate() {
    std::vector<VideoDevice> out;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devs, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        VideoDevice d;
        d.name = attrString(devs[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        d.symlink = attrString(devs[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        d.isTracker = lower(d.symlink).find(L"vid_0bb4&pid_0321") != std::wstring::npos;
        out.push_back(d);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return out;
}

bool VftCamera::open(const VideoDevice& dev, uint8_t irLevel, std::string& err) {
    close();
    stopping_ = false;
    streamError_ = S_OK;
    frameCount_ = 0;
    badSizeCount_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = RawFrame{};
        readPending_ = false;
    }

    HRESULT hr;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dev.symlink.c_str());
    hr = MFCreateDeviceSourceActivate(attrs.Get(), &activate_);
    if (SUCCEEDED(hr)) hr = activate_->ActivateObject(IID_PPV_ARGS(&source_));
    if (FAILED(hr)) {
        char b[160];
        snprintf(b, sizeof(b), "could not open camera (0x%08lX)%s", static_cast<unsigned long>(hr),
                 hr == static_cast<HRESULT>(0xC00D3704) || hr == E_ACCESSDENIED
                     ? " - it may be in use by another app (SRanipal?) or blocked by camera privacy settings"
                     : "");
        err = b;
        close();
        return false;
    }

    if (!xu_.open(source_.Get(), err)) {
        close();
        return false;
    }
    logInfo("Extension unit found: node=%lu payload=%lu bytes", xu_.node(), xu_.payloadLength());

    if (!xu_.activate(irLevel, err)) {
        err = "activation failed: " + err;
        close();
        return false;
    }
    logInfo("Tracker activated (IR level %u)", irLevel);

    callback_.Attach(new ReaderCallback(this));
    ComPtr<IMFAttributes> rattrs;
    MFCreateAttributes(&rattrs, 3);
    rattrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
    rattrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    rattrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), rattrs.Get(), &reader_);
    if (FAILED(hr)) {
        char b[96];
        snprintf(b, sizeof(b), "MFCreateSourceReaderFromMediaSource failed (0x%08lX)", static_cast<unsigned long>(hr));
        err = b;
        close();
        return false;
    }

    // Pick the native YUY2 400x400 mode (the device only advertises one).
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    bool selected = false;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(reader_->GetNativeMediaType(stream, i, &mt))) break;
        GUID sub{};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);
        logVerbose("native type %lu: %s %ux%u @ %u/%u", i, sub == MFVideoFormat_YUY2 ? "YUY2" : "other", w, h, num, den);
        if (!selected && sub == MFVideoFormat_YUY2 && w == 400 && h == 400) {
            hr = reader_->SetCurrentMediaType(stream, nullptr, mt.Get());
            selected = SUCCEEDED(hr);
        }
    }
    if (!selected) {
        err = "camera does not offer YUY2 400x400 (is this really a VIVE Facial Tracker?)";
        close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        readPending_ = true;
    }
    hr = reader_->ReadSample(stream, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            readPending_ = false;
        }
        char b[64];
        snprintf(b, sizeof(b), "ReadSample failed (0x%08lX)", static_cast<unsigned long>(hr));
        err = b;
        close();
        return false;
    }
    logInfo("Capture started on \"%s\"", narrow(dev.name).c_str());
    return true;
}

void VftCamera::onSample(HRESULT hr, DWORD flags, IMFSample* sample) {
    if (SUCCEEDED(hr) && sample) {
        ComPtr<IMFMediaBuffer> buf;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                if (len == kRawBytes) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_.data.assign(p, p + len);
                    latest_.sequence = ++frameCount_;
                } else if (badSizeCount_++ == 0) {
                    logWarn("unexpected frame size %lu bytes (expected %zu)", len, kRawBytes);
                }
                buf->Unlock();
            }
        }
    }
    if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        streamError_ = FAILED(hr) ? hr : E_FAIL;
    }

    bool again = !stopping_ && SUCCEEDED(streamError_.load());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (again && reader_) {
            again = SUCCEEDED(reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                                  nullptr, nullptr, nullptr));
            if (!again && SUCCEEDED(streamError_.load())) streamError_ = E_FAIL;
        }
        readPending_ = again;
    }
    cv_.notify_all();
}

bool VftCamera::waitFrame(RawFrame& out, uint64_t lastSeq, DWORD timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return latest_.sequence > lastSeq || FAILED(streamError_.load());
    });
    if (!ok || latest_.sequence <= lastSeq) return false;
    out.data = latest_.data;
    out.sequence = latest_.sequence;
    return true;
}

void VftCamera::close() {
    stopping_ = true;
    {
        // Let an in-flight ReadSample finish before tearing the graph down.
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(1000), [&] { return !readPending_; });
    }
    if (source_) xu_.deactivate();
    xu_.close();
    if (callback_) callback_->detach();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reader_.Reset();
    }
    callback_.Reset();
    source_.Reset();
    if (activate_) {
        // ShutdownObject (not IMFMediaSource::Shutdown) so the next open does not fail to start streaming.
        activate_->ShutdownObject();
        activate_.Reset();
    }
}
