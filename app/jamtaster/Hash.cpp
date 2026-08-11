#include "Hash.hpp"
#include "FileSystem.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace jamtaster::native {
namespace {

constexpr std::array<std::uint32_t, 64> k{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

std::uint32_t rotate(std::uint32_t value, int amount)
{
    return (value >> amount) | (value << (32 - amount));
}

std::string digest(std::vector<std::uint8_t> bytes)
{
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8ULL;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffU));
    std::array<std::uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t block = 0; block < bytes.size(); block += 64) {
        std::array<std::uint32_t, 64> words{};
        for (int i = 0; i < 16; ++i) {
            const auto offset = block + static_cast<std::size_t>(i) * 4;
            words[i] = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto [a,b,c,d,e,f,g,hh] = h;
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotate(e,6) ^ rotate(e,11) ^ rotate(e,25);
            const auto choice = (e & f) ^ (~e & g);
            const auto t1 = hh + s1 + choice + k[i] + words[i];
            const auto s0 = rotate(a,2) ^ rotate(a,13) ^ rotate(a,22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : h) output << std::setw(8) << value;
    return output.str();
}

} // namespace

std::string sha256(std::string_view value)
{
    return digest(std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::string sha256File(const std::filesystem::path& path)
{
    std::ifstream input(filesystemIoPath(path), std::ios::binary);
    if (!input) throw std::runtime_error("could not hash file: " + path.string());
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    return digest(std::move(bytes));
}

} // namespace jamtaster::native
