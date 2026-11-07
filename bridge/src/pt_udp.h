// Sends frames to PaperTracker using its wireless face-tracker UDP protocol.
//
// Datagram = 16-byte header + payload chunk (all integers big-endian):
//   0  'P' 'T'        magic
//   2  0x01           protocol version
//   3  type           1 = video, 2 = status
//   4  channel        0 = face, 1 = left eye, 2 = right eye
//   5  0              reserved
//   6  u16 sequence   frame sequence number
//   8  u16 total      number of datagrams in this frame (1..256)
//  10  u16 index      datagram index within the frame
//  12  u16 length     payload bytes in this datagram
//  14  0 0            reserved
// The reassembled payload of a video frame is a complete JPEG image.
// Discovery handshake: device sends "PTRACKER_PROBE_V1", PaperTracker answers "PTRACKER_ACK_V1".
#pragma once
#include <winsock2.h>

#include <cstdint>
#include <string>
#include <vector>

class PaperTrackerSender {
public:
    ~PaperTrackerSender();
    bool open(const std::string& host, uint16_t port, std::string& err);
    void close();
    // Sends the discovery probe and waits briefly for the acknowledgement.
    bool probe(DWORD timeoutMs);
    bool sendFrame(const std::vector<uint8_t>& jpeg);

private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in target_{};
    uint16_t seq_ = 0;
    std::vector<uint8_t> packet_;
};
