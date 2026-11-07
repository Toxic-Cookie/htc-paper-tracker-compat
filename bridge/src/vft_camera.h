// Media Foundation capture of the VIVE Facial Tracker with SRanipal-free activation.
#pragma once
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "vft_xu.h"

struct VideoDevice {
    std::wstring name;
    std::wstring symlink;
    bool isTracker = false;
};

struct RawFrame {
    std::vector<uint8_t> data;  // 320000 bytes: 800x400 8-bit grayscale (left|right view)
    uint64_t sequence = 0;
};

class ReaderCallback;

class VftCamera {
public:
    static constexpr int kRawWidth = 800;  // bytes per row of the "YUY2 400x400" frame
    static constexpr int kRawHeight = 400;
    static constexpr size_t kRawBytes = size_t(kRawWidth) * kRawHeight;

    VftCamera();
    ~VftCamera();

    static std::vector<VideoDevice> enumerate();

    bool open(const VideoDevice& dev, uint8_t irLevel, std::string& err);
    void close();
    bool isOpen() const { return reader_ != nullptr; }

    // Waits for a frame newer than `lastSeq`. Returns false on timeout or stream error.
    bool waitFrame(RawFrame& out, uint64_t lastSeq, DWORD timeoutMs);
    HRESULT streamError() const { return streamError_.load(); }
    uint64_t framesReceived() const { return frameCount_.load(); }
    VftExtensionUnit& xu() { return xu_; }

private:
    friend class ReaderCallback;
    void onSample(HRESULT hr, DWORD flags, IMFSample* sample);

    Microsoft::WRL::ComPtr<IMFActivate> activate_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<ReaderCallback> callback_;
    VftExtensionUnit xu_;

    std::mutex mutex_;
    std::condition_variable cv_;
    RawFrame latest_;
    bool readPending_ = false;
    std::atomic<bool> stopping_{false};
    std::atomic<HRESULT> streamError_{S_OK};
    std::atomic<uint64_t> frameCount_{0};
    std::atomic<uint64_t> badSizeCount_{0};
};
