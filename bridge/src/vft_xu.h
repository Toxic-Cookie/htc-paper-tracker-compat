// UVC Extension Unit control for the HTC VIVE Facial Tracker (0BB4:0321).
//
// Protocol (unit 4, selector 2, GUID 2CCB0BDA-6331-4FDB-850E-79054DBD5671) as documented by
// LordOfDragons/vivefacialtracker, Project Babble (BabbleApp/Baballonia), Unvived and SRanipatch.
#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

// IKsControl (ksproxy.h), declared locally to avoid pulling DirectShow/KS headers into every user.
// The pointer parameters are PKSPROPERTY / PKSMETHOD / PKSEVENT in the SDK; the ABI is identical.
struct __declspec(uuid("28F54685-06FD-11D2-B27A-00A0C9223196")) IKsControlX : IUnknown {
    virtual HRESULT __stdcall KsProperty(void* property, ULONG propertyLen, void* data, ULONG dataLen,
                                         ULONG* returned) = 0;
    virtual HRESULT __stdcall KsMethod(void* method, ULONG methodLen, void* data, ULONG dataLen,
                                       ULONG* returned) = 0;
    virtual HRESULT __stdcall KsEvent(void* event, ULONG eventLen, void* data, ULONG dataLen,
                                      ULONG* returned) = 0;
};

class VftExtensionUnit {
public:
    // `source` is any COM object that exposes IKsControl for the device (MF media source).
    bool open(IUnknown* source, std::string& err);
    void close();

    // Full SRanipal-free activation: handshake, sensor/IR registers, stream on.
    bool activate(uint8_t irLevel, std::string& err);
    // Stream off + IR off. Leaves the tracker MCU in a clean state for the next open.
    void deactivate();
    // Re-send the stream-on command (some units need a nudge once video is running).
    bool streamOn(std::string& err);

    ULONG node() const { return node_; }
    ULONG payloadLength() const { return len_; }

private:
    HRESULT prop(ULONG flags, void* data, ULONG dataLen, ULONG* returned);
    bool setCur(const std::vector<uint8_t>& cmd, std::string& err);
    bool setCurWait(const std::vector<uint8_t>& cmd, std::string& err);
    bool magic(std::string& err);
    bool writeSensorReg(uint8_t addr, uint8_t value, std::string& err);
    bool setStream(bool enable, std::string& err);
    bool writeRegisterBlock(uint8_t irLevel, std::string& err);
    std::vector<uint8_t> blank() const { return std::vector<uint8_t>(len_, 0); }

    Microsoft::WRL::ComPtr<IKsControlX> ctl_;
    ULONG node_ = 0;
    ULONG len_ = 0;
};
