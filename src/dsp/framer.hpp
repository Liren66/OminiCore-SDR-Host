#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace och::dsp {

// Streaming detector for the FPGA's complete 16 KiB byte-ramp preamble.
// It discards bytes before the first full preamble and removes every later
// preamble, including adjacent ones. IQ groups always begin immediately after
// the most recent preamble, independently of USB transfer boundaries.
//
// A ramp prefix at the end of a feed is held until it is disproved or complete.
// Thus a <=16383 byte prefix may remain delayed; reset discards it. A full ramp
// occurring in actual IQ is indistinguishable from a real preamble in this wire
// protocol. Bytes already delivered before an overflow cannot be retracted;
// epoch/start indicate the new boundary, not an exact lost-sample count.
class Framer {
public:
    static constexpr std::size_t header_bytes = 16 * 1024;
    using Sink = void (*)(void* context, const std::uint8_t* data,
                         std::size_t size, std::uint64_t epoch, bool start);

    explicit Framer(unsigned channels, unsigned wire_sample_bytes = 3);
    void reset() noexcept;
    // Each view is valid only for the duration of the callback. Callbacks must
    // not reenter this Framer. size is nonzero and divisible by wire bytes*channels.
    // start is true on the first nonempty view after a new header. No heap
    // allocation occurs in feed, and null data is allowed when size is zero.
    void feed(const std::uint8_t* data, std::size_t size, Sink sink, void* context);
    std::uint64_t epoch() const noexcept { return epoch_; }
    bool synchronized() const noexcept { return epoch_ != 0; }
    std::size_t pending_bytes() const noexcept { return candidate_ + tail_size_; }

private:
    void emit(const std::uint8_t* data, std::size_t size, Sink sink, void* context);
    void found_header() noexcept;
    const unsigned group_bytes_;
    std::size_t candidate_ = 0;
    std::size_t tail_size_ = 0;
    std::array<std::uint8_t, 6> tail_{};
    std::uint64_t epoch_ = 0;
    bool start_pending_ = false;
};

} // namespace och::dsp
