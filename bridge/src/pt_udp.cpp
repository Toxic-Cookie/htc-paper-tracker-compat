#include "pt_udp.h"

#include <ws2tcpip.h>
#include <cstring>

namespace {
constexpr size_t kHeaderSize = 16;
constexpr size_t kMaxChunk = 1400;  // stay below a typical MTU, like the ESP32 firmware
constexpr size_t kMaxChunks = 256;
const char kProbe[] = "PTRACKER_PROBE_V1";
const char kAck[] = "PTRACKER_ACK_V1";

void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xff);
}
}  // namespace

PaperTrackerSender::~PaperTrackerSender() { close(); }

bool PaperTrackerSender::open(const std::string& host, uint16_t port, std::string& err) {
    close();
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        err = "socket() failed: " + std::to_string(WSAGetLastError());
        return false;
    }
    target_ = {};
    target_.sin_family = AF_INET;
    target_.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &target_.sin_addr) != 1) {
        err = "invalid PaperTracker address: " + host;
        close();
        return false;
    }
    int sndbuf = 1 << 20;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
    // connect() fixes the destination and lets us receive the ACK; also filters stray datagrams.
    if (connect(sock_, reinterpret_cast<sockaddr*>(&target_), sizeof(target_)) != 0) {
        err = "connect() failed: " + std::to_string(WSAGetLastError());
        close();
        return false;
    }
    return true;
}

void PaperTrackerSender::close() {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
    sock_ = INVALID_SOCKET;
}

bool PaperTrackerSender::probe(DWORD timeoutMs) {
    if (sock_ == INVALID_SOCKET) return false;
    send(sock_, kProbe, static_cast<int>(sizeof(kProbe) - 1), 0);
    DWORD to = timeoutMs;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    char buf[256];
    for (int i = 0; i < 8; ++i) {
        int n = recv(sock_, buf, sizeof(buf), 0);
        if (n <= 0) return false;  // timeout, or ICMP port unreachable (PaperTracker not running)
        if (n == static_cast<int>(sizeof(kAck) - 1) && memcmp(buf, kAck, n) == 0) return true;
    }
    return false;
}

bool PaperTrackerSender::sendFrame(const std::vector<uint8_t>& jpeg) {
    if (sock_ == INVALID_SOCKET || jpeg.empty()) return false;
    const size_t total = (jpeg.size() + kMaxChunk - 1) / kMaxChunk;
    if (total > kMaxChunks) return false;
    packet_.resize(kHeaderSize + kMaxChunk);
    const uint16_t seq = seq_++;
    bool ok = true;
    for (size_t i = 0; i < total; ++i) {
        const size_t off = i * kMaxChunk;
        const size_t len = (jpeg.size() - off < kMaxChunk) ? jpeg.size() - off : kMaxChunk;
        uint8_t* h = packet_.data();
        h[0] = 'P';
        h[1] = 'T';
        h[2] = 1;  // version
        h[3] = 1;  // video
        h[4] = 0;  // face channel
        h[5] = 0;
        putU16(h + 6, seq);
        putU16(h + 8, static_cast<uint16_t>(total));
        putU16(h + 10, static_cast<uint16_t>(i));
        putU16(h + 12, static_cast<uint16_t>(len));
        h[14] = h[15] = 0;
        memcpy(h + kHeaderSize, jpeg.data() + off, len);
        // Errors (e.g. WSAECONNRESET while PaperTracker is closed) are expected; keep streaming.
        if (send(sock_, reinterpret_cast<const char*>(h), static_cast<int>(kHeaderSize + len), 0) < 0) ok = false;
    }
    return ok;
}
