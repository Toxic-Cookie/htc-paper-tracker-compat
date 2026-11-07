#pragma once
#include <cstdint>
#include <vector>

// Encodes an 8-bit grayscale image as baseline JPEG using the Windows Imaging Component.
class JpegEncoder {
public:
    bool init();
    bool encodeGray(const uint8_t* pixels, int width, int height, int stride, float quality,
                    std::vector<uint8_t>& out);

private:
    struct IWICImagingFactory* factory_ = nullptr;
};
