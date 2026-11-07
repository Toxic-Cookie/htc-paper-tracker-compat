#include "vft_xu.h"

#include <ks.h>
#include <vidcap.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {
// guidExtensionCode of the tracker's extension unit.
const GUID kXuGuid = {0x2ccb0bda, 0x6331, 0x4fdb, {0x85, 0x0e, 0x79, 0x05, 0x4d, 0xbd, 0x56, 0x71}};
// KSNODETYPE_DEV_SPECIFIC: how UVC extension units appear in the KS topology.
const GUID kNodeDevSpecific = {0x941C7AC0, 0xC559, 0x11D0, {0x8A, 0x2B, 0x00, 0xA0, 0xC9, 0x25, 0x5A, 0xC1}};
constexpr ULONG kSelector = 2;

constexpr uint8_t kTaskSet = 0x50;
constexpr uint8_t kRegSensor = 0xab;
constexpr uint8_t kReplyBusy = 0x55;
constexpr uint8_t kReplyDone = 0x56;
constexpr auto kReplyTimeout = std::chrono::milliseconds(500);

bool validLength(ULONG n) { return n == 384 || n == 64; }

std::string hrText(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}
}  // namespace

HRESULT VftExtensionUnit::prop(ULONG flags, void* data, ULONG dataLen, ULONG* returned) {
    KSP_NODE p{};
    p.Property.Set = kXuGuid;
    p.Property.Id = kSelector;
    p.Property.Flags = flags | KSPROPERTY_TYPE_TOPOLOGY;
    p.NodeId = node_;
    ULONG dummy = 0;
    return ctl_->KsProperty(&p, sizeof(p), data, dataLen, returned ? returned : &dummy);
}

bool VftExtensionUnit::open(IUnknown* source, std::string& err) {
    close();
    HRESULT hr = source->QueryInterface(__uuidof(IKsControlX), reinterpret_cast<void**>(ctl_.GetAddressOf()));
    if (FAILED(hr)) {
        err = "media source does not expose IKsControl (" + hrText(hr) + ")";
        return false;
    }

    // Candidate nodes: DEV_SPECIFIC nodes from the topology first, then a brute-force sweep.
    std::vector<ULONG> candidates;
    ComPtr<IKsTopologyInfo> topo;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&topo)))) {
        DWORD n = 0;
        if (SUCCEEDED(topo->get_NumNodes(&n))) {
            for (DWORD i = 0; i < n; ++i) {
                GUID t{};
                if (SUCCEEDED(topo->get_NodeType(i, &t)) && t == kNodeDevSpecific) candidates.push_back(i);
            }
        }
    }
    for (ULONG i = 0; i < 16; ++i) candidates.push_back(i);

    for (ULONG id : candidates) {
        node_ = id;
        ULONG size = 0;
        // A zero-length GET reports the control size through ERROR_MORE_DATA.
        hr = prop(KSPROPERTY_TYPE_GET, nullptr, 0, &size);
        if ((hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) || hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
             SUCCEEDED(hr)) && validLength(size)) {
            len_ = size;
            return true;
        }
    }
    ctl_.Reset();
    err = "extension unit not found (no topology node answered with a 384/64-byte control)";
    return false;
}

void VftExtensionUnit::close() {
    ctl_.Reset();
    node_ = 0;
    len_ = 0;
}

bool VftExtensionUnit::setCur(const std::vector<uint8_t>& cmd, std::string& err) {
    std::vector<uint8_t> buf = cmd;
    ULONG ret = 0;
    HRESULT hr = prop(KSPROPERTY_TYPE_SET, buf.data(), len_, &ret);
    if (FAILED(hr)) {
        err = "XU SET_CUR failed (" + hrText(hr) + ")";
        return false;
    }
    return true;
}

bool VftExtensionUnit::setCurWait(const std::vector<uint8_t>& cmd, std::string& err) {
    if (!setCur(cmd, err)) return false;
    std::vector<uint8_t> reply(len_);
    const auto deadline = std::chrono::steady_clock::now() + kReplyTimeout;
    while (true) {
        std::fill(reply.begin(), reply.end(), uint8_t{0});
        ULONG ret = 0;
        HRESULT hr = prop(KSPROPERTY_TYPE_GET, reply.data(), len_, &ret);
        if (FAILED(hr)) {
            err = "XU GET_CUR failed (" + hrText(hr) + ")";
            return false;
        }
        if (reply[0] == kReplyDone) {
            // The device echoes the command after the status byte.
            if (std::equal(cmd.begin(), cmd.begin() + 16, reply.begin() + 1)) return true;
            err = "XU reply echo mismatch";
            return false;
        }
        if (reply[0] != kReplyBusy) {
            char b[64];
            snprintf(b, sizeof(b), "XU unexpected reply 0x%02X", reply[0]);
            err = b;
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            err = "XU reply timeout";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool VftExtensionUnit::magic(std::string& err) {
    auto cmd = blank();
    cmd[0] = 0x51;
    cmd[1] = 0x52;
    if (len_ >= 256) {
        cmd[254] = 0x53;
        cmd[255] = 0x54;
    }
    return setCurWait(cmd, err);
}

bool VftExtensionUnit::writeSensorReg(uint8_t addr, uint8_t value, std::string& err) {
    auto cmd = blank();
    const uint8_t layout[17] = {kTaskSet, kRegSensor, 0x60, 0x01, 0x01, 0x00, 0x00, 0x00, addr,
                                0x90,     0x01,       0x00, 0x01, 0x00, 0x00, 0x00, value};
    std::copy(std::begin(layout), std::end(layout), cmd.begin());
    if (setCurWait(cmd, err)) return true;
    char b[48];
    snprintf(b, sizeof(b), " (sensor reg 0x%02X)", addr);
    err += b;
    return false;
}

bool VftExtensionUnit::setStream(bool enable, std::string& err) {
    auto cmd = blank();
    cmd[0] = kTaskSet;
    cmd[1] = 0x14;
    cmd[2] = 0x00;
    cmd[3] = enable ? 0x01 : 0x00;
    // Reference implementations do not wait for an acknowledgement here.
    return setCur(cmd, err);
}

bool VftExtensionUnit::writeRegisterBlock(uint8_t irLevel, std::string& err) {
    const uint8_t regs[][2] = {
        {0x00, 0x40}, {0x08, 0x01}, {0x70, 0x00},
        {0x02, irLevel}, {0x03, irLevel}, {0x04, irLevel},  // IR illumination / exposure
        {0x0e, 0x00},
        {0x05, 0xb2}, {0x06, 0xb2}, {0x07, 0xb2},  // gain
        {0x0f, 0x03},
    };
    for (const auto& r : regs) {
        if (!writeSensorReg(r[0], r[1], err)) return false;
    }
    return true;
}

bool VftExtensionUnit::activate(uint8_t irLevel, std::string& err) {
    if (!magic(err)) return false;
    if (!setStream(false, err)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!magic(err)) return false;
    if (!writeRegisterBlock(irLevel, err)) return false;
    if (!magic(err)) return false;
    if (!setStream(true, err)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return true;
}

bool VftExtensionUnit::streamOn(std::string& err) { return setStream(true, err); }

void VftExtensionUnit::deactivate() {
    if (!ctl_) return;
    std::string err;
    if (!magic(err)) logWarn("deactivate: %s", err.c_str());
    err.clear();
    if (!setStream(false, err)) logWarn("deactivate: %s", err.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    err.clear();
    // Switch the IR LEDs off so the tracker does not keep glowing after we exit.
    if (!magic(err) || !writeRegisterBlock(0x00, err)) logWarn("deactivate (IR off): %s", err.c_str());
}
