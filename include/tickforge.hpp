#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace tickforge {
using Frame = std::array<std::uint8_t, 64>;
enum class Kind : std::uint8_t { snapshot = 1, update = 2, heartbeat = 3 };
struct Message {
    Kind kind{};
    std::uint64_t generation{}, sequence{}, bid{}, ask{}, bid_size{}, ask_size{};
};
enum class ParseError { none, length, magic, reserved, kind, crc, book };
struct Parsed { Message message{}; ParseError error{ParseError::none}; explicit operator bool() const { return error == ParseError::none; } };

inline std::uint64_t read64(const std::uint8_t* data) noexcept {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result = (result << 8) | data[i];
    return result;
}
inline std::uint32_t read32(const std::uint8_t* data) noexcept {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result = (result << 8) | data[i];
    return result;
}
inline void write64(std::uint8_t* data, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) { data[i] = static_cast<std::uint8_t>(value); value >>= 8; }
}
inline void write32(std::uint8_t* data, std::uint32_t value) noexcept {
    for (int i = 3; i >= 0; --i) { data[i] = static_cast<std::uint8_t>(value); value >>= 8; }
}
inline std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (auto byte : data) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
inline Frame encode(const Message& message) noexcept {
    Frame frame{};
    frame[0] = 'T'; frame[1] = 'K'; frame[2] = 'F'; frame[3] = '1';
    frame[4] = static_cast<std::uint8_t>(message.kind);
    write64(frame.data() + 8, message.generation); write64(frame.data() + 16, message.sequence);
    write64(frame.data() + 24, message.bid); write64(frame.data() + 32, message.ask);
    write64(frame.data() + 40, message.bid_size); write64(frame.data() + 48, message.ask_size);
    write32(frame.data() + 56, crc32(std::span(frame).first(56)));
    return frame;
}
inline Parsed decode(std::span<const std::uint8_t> frame) noexcept {
    if (frame.size() != 64) return {{}, ParseError::length};
    if (frame[0] != 'T' || frame[1] != 'K' || frame[2] != 'F' || frame[3] != '1') return {{}, ParseError::magic};
    if (frame[5] || frame[6] || frame[7] || read32(frame.data() + 60)) return {{}, ParseError::reserved};
    if (frame[4] < 1 || frame[4] > 3) return {{}, ParseError::kind};
    if (read32(frame.data() + 56) != crc32(frame.first(56))) return {{}, ParseError::crc};
    Message message{static_cast<Kind>(frame[4]), read64(frame.data() + 8), read64(frame.data() + 16),
                    read64(frame.data() + 24), read64(frame.data() + 32),
                    read64(frame.data() + 40), read64(frame.data() + 48)};
    constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!message.generation) return {{}, ParseError::book};
    if (message.kind == Kind::heartbeat) {
        if (message.bid || message.ask || message.bid_size || message.ask_size) return {{}, ParseError::book};
    } else if (!message.bid || message.ask <= message.bid || message.ask > max ||
               !message.bid_size || !message.ask_size || message.bid_size > max || message.ask_size > max) {
        return {{}, ParseError::book};
    }
    return {message, ParseError::none};
}

enum class Action { published, heartbeat, duplicate, old_generation, fenced };
class FeedState {
    std::uint64_t generation_{}, sequence_{}, digest_{14695981039346656037ULL};
    bool have_sequence_{}, trusted_{};
    Message quote_{};
    void mix(std::uint64_t value) noexcept {
        for (unsigned i = 0; i < 8; ++i) { digest_ ^= value & 255U; digest_ *= 1099511628211ULL; value >>= 8; }
    }
public:
    std::uint64_t publications{}, gaps{}, duplicates{}, old_generations{}, malformed{};
    void invalidate() noexcept { trusted_ = false; have_sequence_ = true; }
    bool trusted() const noexcept { return trusted_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t digest() const noexcept { return digest_; }
    Message quote() const {
        if (!trusted_) throw std::logic_error("quote unavailable behind trust gate");
        return quote_;
    }
    long double microprice() const {
        const auto q = quote();
        return (static_cast<long double>(q.ask) * q.bid_size + static_cast<long double>(q.bid) * q.ask_size)
            / (static_cast<long double>(q.bid_size) + q.ask_size);
    }
private:
    Action accept(const Message& message) noexcept {
        if (message.generation < generation_) { ++old_generations; return Action::old_generation; }
        if (message.generation > generation_) {
            generation_ = message.generation; have_sequence_ = false; trusted_ = false;
        }
        if (!have_sequence_) {
            have_sequence_ = true; sequence_ = message.sequence;
            if (message.kind != Kind::snapshot || message.sequence != 0) { ++gaps; return Action::fenced; }
            trusted_ = true;
        } else {
            if (message.sequence <= sequence_) { ++duplicates; return Action::duplicate; }
            if (!trusted_) return Action::fenced;
            if (sequence_ == std::numeric_limits<std::uint64_t>::max() || message.sequence != sequence_ + 1
                || message.kind == Kind::snapshot) { ++gaps; invalidate(); return Action::fenced; }
            sequence_ = message.sequence;
        }
        if (message.kind == Kind::heartbeat) return Action::heartbeat;
        quote_ = message;
        for (auto value : {message.generation, message.sequence, message.bid, message.ask, message.bid_size, message.ask_size}) mix(value);
        ++publications;
        return Action::published;
    }
public:
    Action ingest(std::span<const std::uint8_t> frame) noexcept {
        const auto parsed = decode(frame);
        if (!parsed) { ++malformed; invalidate(); return Action::fenced; }
        return accept(parsed.message);
    }
};

inline std::size_t checked_capacity(std::size_t capacity) {
    if (capacity < 2 || (capacity & (capacity - 1)) || capacity > (1ULL << 30))
        throw std::invalid_argument("capacity must be a power of two between 2 and 2^30");
    return capacity;
}

// Exactly one producer and one consumer. Slot ownership changes only across release/acquire.
template<class T> class SpscRing {
    static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_copy_assignable_v<T>);
    struct alignas(64) Cursor { std::atomic<std::uint64_t> value; explicit Cursor(std::uint64_t v) : value(v) {} };
    std::vector<T> slots_;
    const std::uint64_t mask_;
    Cursor head_, tail_;
public:
    explicit SpscRing(std::size_t capacity, std::uint64_t initial = 0)
        : slots_(checked_capacity(capacity)), mask_(capacity - 1), head_(initial), tail_(initial) {}
    bool push(const T& value) noexcept {
        const auto head = head_.value.load(std::memory_order_relaxed);
        if (head - tail_.value.load(std::memory_order_acquire) == slots_.size()) return false;
        slots_[head & mask_] = value;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        if (tail == head_.value.load(std::memory_order_acquire)) return false;
        value = slots_[tail & mask_];
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }
};

// Matched baseline: identical preallocated ring and backpressure policy, protected by a mutex.
template<class T> class MutexRing {
    std::vector<T> slots_;
    std::uint64_t head_{}, tail_{}, mask_;
    std::mutex mutex_;
public:
    explicit MutexRing(std::size_t capacity, std::uint64_t initial = 0)
        : slots_(checked_capacity(capacity)), head_(initial), tail_(initial), mask_(capacity - 1) {}
    bool push(const T& value) {
        std::lock_guard lock(mutex_);
        if (head_ - tail_ == slots_.size()) return false;
        slots_[head_++ & mask_] = value; return true;
    }
    bool pop(T& value) {
        std::lock_guard lock(mutex_);
        if (head_ == tail_) return false;
        value = slots_[tail_++ & mask_]; return true;
    }
};

inline std::vector<Frame> workload(std::size_t count) {
    std::vector<Frame> frames; frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        frames.push_back(encode({i == 0 ? Kind::snapshot : Kind::update, 1, i,
            100000 + i % 100, 100002 + i % 100, 1 + i % 79, 1 + i % 61}));
    return frames;
}
} // namespace tickforge
