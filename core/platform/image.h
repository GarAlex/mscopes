//
// platform/image.h — write a framebuffer as a PNG, dependency-free.
//
// Uncompressed PNG (zlib "stored" blocks) so the tools and self-tests build
// on any platform with no image library. Files are large but exact, which
// is what regression comparisons want. Row 0 of a Framebuffer is the
// bottom; PNG rows go top-down, so the writer flips.
//
#pragma once
#include "Framebuffer.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace viz { namespace platform {

namespace detail {
inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
inline void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32(td.data(), td.size()));
}
} // namespace detail

// Encode fb (RGB, 8-bit, clamped) as PNG bytes.
inline std::vector<uint8_t> encodePng(const Framebuffer& fb)
{
    using namespace detail;
    const int W = fb.w, H = fb.h;
    // Raw scanlines: filter byte 0 + RGB triplets, top row first.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)H * (1 + 3 * W));
    for (int y = H - 1; y >= 0; --y) {
        raw.push_back(0);
        const float* p = fb.at(0, y);
        for (int x = 0; x < W; ++x, p += 4)
            for (int c = 0; c < 3; ++c) {
                float v = p[c] < 0.f ? 0.f : (p[c] > 1.f ? 1.f : p[c]);
                raw.push_back((uint8_t)(v * 255.f + 0.5f));
            }
    }
    // zlib stream with stored (uncompressed) deflate blocks of <= 65535 bytes.
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (raw.empty()) break;
    }
    uint32_t a = 1, b = 0;                     // adler32
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, (uint32_t)W); be32(ihdr, (uint32_t)H);
    ihdr.push_back(8); ihdr.push_back(2);      // 8-bit RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

inline bool writePng(const Framebuffer& fb, const std::string& path)
{
    if (fb.w <= 0 || fb.h <= 0) return false;
    std::vector<uint8_t> bytes = encodePng(fb);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

}} // namespace viz::platform
