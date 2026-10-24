#include "framer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define OCH_SSE2_FRAMER 1
#else
#define OCH_SSE2_FRAMER 0
#endif

namespace och::dsp {
namespace {
constexpr auto make_header() {
    std::array<std::uint8_t, Framer::header_bytes> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(i);
    return result;
}
alignas(64) constexpr auto header = make_header();

const std::uint8_t* find_start(const std::uint8_t* p, const std::uint8_t* end) noexcept {
#if OCH_SSE2_FRAMER
    const auto zero = _mm_setzero_si128();
    const auto one = _mm_set1_epi8(1);
    while (end - p >= 17) {
        const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 1));
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(
            _mm_cmpeq_epi8(a, zero), _mm_cmpeq_epi8(b, one))));
        if (mask) {
            unsigned bit = 0;
            while (!(mask & 1u)) { mask >>= 1; ++bit; }
            return p + bit;
        }
        p += 16;
    }
#endif
    for (; end - p >= 2; ++p)
        if (p[0] == 0 && p[1] == 1) return p;
    return p != end && *p == 0 ? p : nullptr;
}

// First mismatching offset. Word comparisons accelerate long candidate runs;
// random IQ usually fails at byte one. memcpy avoids alignment/aliasing UB.
std::size_t common_prefix(const std::uint8_t* a, const std::uint8_t* b,
                          std::size_t size) noexcept {
    std::size_t i = 0;
    while (size - i >= 8) {
        std::uint64_t x, y;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        if (x != y) break;
        i += 8;
    }
    while (i < size && a[i] == b[i]) ++i;
    return i;
}
} // namespace

Framer::Framer(unsigned channels, unsigned wire_sample_bytes) : group_bytes_(channels * wire_sample_bytes) {
    if (channels != 1 && channels != 2)
        throw std::invalid_argument("RX framing requires one or two channels");
    if (wire_sample_bytes != 3 && (wire_sample_bytes != 2 || channels != 2))
        throw std::invalid_argument("8-bit RX framing requires two channels");
}

void Framer::reset() noexcept {
    candidate_ = 0;
    tail_size_ = 0;
    epoch_ = 0;
    start_pending_ = false;
}

void Framer::found_header() noexcept {
    candidate_ = 0;
    // A restart can occur after a partial IQ group. Never join that old tail to
    // the new epoch. Epoch zero is reserved for "not yet synchronized".
    tail_size_ = 0;
    if (++epoch_ == 0) ++epoch_;
    start_pending_ = true;
}

void Framer::emit(const std::uint8_t* data, std::size_t size,
                  Sink sink, void* context) {
    if (!synchronized() || !size) return;
    if (tail_size_) {
        const auto count = std::min(size, group_bytes_ - tail_size_);
        std::memcpy(tail_.data() + tail_size_, data, count);
        tail_size_ += count;
        data += count;
        size -= count;
        if (tail_size_ == group_bytes_) {
            sink(context, tail_.data(), group_bytes_, epoch_, start_pending_);
            start_pending_ = false;
            tail_size_ = 0;
        }
    }
    const auto whole = size - size % group_bytes_;
    if (whole) {
        sink(context, data, whole, epoch_, start_pending_);
        start_pending_ = false;
        data += whole;
        size -= whole;
    }
    if (size) {
        std::memcpy(tail_.data(), data, size);
        tail_size_ = size;
    }
}

void Framer::feed(const std::uint8_t* data, std::size_t size,
                  Sink sink, void* context) {
    if (!size) return;
    if (!data || !sink) throw std::invalid_argument("RX framer requires data and sink");
    const auto* p = data;
    const auto* end = data + size;

    if (candidate_) {
        const auto limit = std::min(size, header_bytes - candidate_);
        const auto matched = common_prefix(p, header.data() + candidate_, limit);
        candidate_ += matched;
        p += matched;
        if (candidate_ == header_bytes) {
            found_header();
        } else if (matched == limit) {
            return; // Candidate continues across another USB transfer.
        } else {
            // Every possible nested zero in this prefix is 256 bytes apart and
            // fails at the same mismatch. Emit the entire disproven prefix.
            emit(header.data(), candidate_, sink, context);
            candidate_ = 0;
        }
    }

    auto* ordinary = p;
    while (p != end) {
        const auto* zero = find_start(p, end);
        if (!zero) break;
        const auto limit = std::min(static_cast<std::size_t>(end - zero), header_bytes);
        const auto matched = common_prefix(zero, header.data(), limit);
        if (matched == header_bytes) {
            emit(ordinary, static_cast<std::size_t>(zero - ordinary), sink, context);
            found_header();
            p = zero + header_bytes;
            ordinary = p;
        } else if (matched == limit) {
            emit(ordinary, static_cast<std::size_t>(zero - ordinary), sink, context);
            candidate_ = matched;
            return;
        } else {
            // Keep false candidates within one contiguous ordinary view; the
            // number of callbacks does not grow with the number of zero bytes.
            p = zero + matched;
        }
    }
    emit(ordinary, static_cast<std::size_t>(end - ordinary), sink, context);
}

} // namespace och::dsp
