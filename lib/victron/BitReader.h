#pragma once
#include <cstddef>
#include <cstdint>

namespace victron {

// Reads fields from a byte buffer LSB-first within each byte, matching the
// bit packing Victron uses in its decrypted advertisement records (and the
// reference `victron-ble` BitReader).
class BitReader {
public:
    BitReader(const uint8_t* data, size_t len) : data_(data), len_(len), bitPos_(0) {}

    uint32_t readUnsigned(int nbits) {
        uint32_t value = 0;
        for (int i = 0; i < nbits; ++i) {
            size_t byteIdx = bitPos_ >> 3;
            int bitIdx = static_cast<int>(bitPos_ & 7);
            uint32_t bit = (byteIdx < len_) ? ((data_[byteIdx] >> bitIdx) & 1u) : 0u;
            value |= (bit << i);
            ++bitPos_;
        }
        return value;
    }

    int32_t readSigned(int nbits) {
        uint32_t u = readUnsigned(nbits);
        if (u & (1u << (nbits - 1))) {
            return static_cast<int32_t>(u) - static_cast<int32_t>(1u << nbits);
        }
        return static_cast<int32_t>(u);
    }

    size_t bitsRead() const { return bitPos_; }

private:
    const uint8_t* data_;
    size_t len_;
    size_t bitPos_;
};

}  // namespace victron
