#include "tickforge.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

thread_local bool allocation_probe = false;
thread_local std::uint64_t allocations = 0;
void* operator new(std::size_t size) {
    if (allocation_probe) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

std::uint64_t checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #condition); } while(0)
using namespace tickforge;
Message message(std::uint64_t generation = 1, std::uint64_t sequence = 0, Kind kind = Kind::snapshot) {
    return {kind, generation, sequence, 99, 101, 9, 1};
}

void parser_tests() {
    auto frame = encode(message());
    CHECK(decode(frame)); CHECK(decode(frame).message.ask == 101);
    CHECK(frame[8] == 0 && frame[15] == 1); CHECK(frame[31] == 99);
    const std::array<std::uint8_t, 9> check_string{'1','2','3','4','5','6','7','8','9'};
    CHECK(crc32(check_string) == 0xcbf43926U); // independent standard CRC-32 test vector
    CHECK(decode(std::span(frame).first(63)).error == ParseError::length);
    std::array<std::uint8_t, 65> large{}; CHECK(decode(large).error == ParseError::length);
    auto bad = frame; bad[0] = 'X'; CHECK(decode(bad).error == ParseError::magic);
    bad = frame; bad[60] = 1; CHECK(decode(bad).error == ParseError::reserved);
    bad = frame; bad[5] = 1; CHECK(decode(bad).error == ParseError::reserved);
    bad = frame; bad[4] = 4; CHECK(decode(bad).error == ParseError::kind);
    bad = frame; bad[40] ^= 2; CHECK(decode(bad).error == ParseError::crc);
    auto crossed = message(); crossed.ask = 98; CHECK(decode(encode(crossed)).error == ParseError::book);
    crossed = message(); crossed.ask = 99; CHECK(decode(encode(crossed)).error == ParseError::book);
    crossed = message(); crossed.bid_size = 0; CHECK(decode(encode(crossed)).error == ParseError::book);
    crossed = message(); crossed.ask = std::numeric_limits<std::uint64_t>::max(); CHECK(!decode(encode(crossed)));
    auto heartbeat = Message{Kind::heartbeat, 1, 1, 0, 0, 0, 0}; CHECK(decode(encode(heartbeat)));
    heartbeat.bid = 1; CHECK(!decode(encode(heartbeat)));
    for (std::uint64_t i = 0; i < 10000; ++i) {
        auto m = message(i + 1, i, Kind::update); m.bid = 100 + i; m.ask = 101 + i;
        const auto decoded = decode(encode(m)); CHECK(decoded); CHECK(decoded.message.bid == m.bid && decoded.message.sequence == i);
    }
}
void state_tests() {
    FeedState state;
    CHECK(state.ingest(encode(message())) == Action::published);
    CHECK(state.trusted()); CHECK(state.microprice() == 100.8L);
    CHECK(state.ingest(encode(message(1, 0))) == Action::duplicate);
    CHECK(state.ingest(encode(Message{Kind::heartbeat, 1, 1, 0, 0, 0, 0})) == Action::heartbeat);
    CHECK(state.publications == 1);
    CHECK(state.ingest(encode(message(1, 3, Kind::update))) == Action::fenced); CHECK(!state.trusted());
    CHECK(state.ingest(encode(message(1, 2, Kind::update))) == Action::fenced);
    CHECK(state.ingest(encode(message(1, 0))) == Action::duplicate); CHECK(!state.trusted());
    CHECK(state.ingest(encode(message(2, 0))) == Action::published); CHECK(state.trusted());
    CHECK(state.ingest(encode(message(1, 99, Kind::update))) == Action::old_generation); CHECK(state.trusted());
    auto bad = encode(message(2, 1, Kind::update)); bad[24] ^= 1;
    CHECK(state.ingest(bad) == Action::fenced); CHECK(!state.trusted());
    bool rejected = false; try { state.quote(); } catch (const std::logic_error&) { rejected = true; } CHECK(rejected);
    CHECK(state.ingest(encode(message(2, 2, Kind::update))) == Action::fenced);
    CHECK(state.ingest(encode(message(3, 0))) == Action::published);
    CHECK(state.ingest(encode(message(4, 1, Kind::update))) == Action::fenced);
    CHECK(state.ingest(encode(message(4, 0))) == Action::duplicate); CHECK(!state.trusted());
    CHECK(state.ingest(encode(message(5, 0))) == Action::published);
    CHECK(state.ingest(encode(message(5, std::numeric_limits<std::uint64_t>::max(), Kind::update))) == Action::fenced);
    CHECK(!state.trusted());
}
template<template<class> class Queue> void queue_tests() {
    Queue<std::uint64_t> queue(8, std::numeric_limits<std::uint64_t>::max() - 3);
    std::uint64_t out{}; CHECK(!queue.pop(out));
    for (std::uint64_t i = 0; i < 8; ++i) CHECK(queue.push(i));
    CHECK(!queue.push(8));
    for (std::uint64_t i = 0; i < 8; ++i) { CHECK(queue.pop(out)); CHECK(out == i); }
    CHECK(!queue.pop(out));
    allocations = 0; allocation_probe = true;
    bool correct = true;
    for (std::uint64_t i = 0; i < 100000; ++i) correct = correct && queue.push(i) && queue.pop(out) && out == i;
    allocation_probe = false;
    CHECK(correct); CHECK(allocations == 0);
    bool rejected = false; try { Queue<int> invalid(7); } catch (const std::invalid_argument&) { rejected = true; } CHECK(rejected);
}
template<template<class> class Queue> void stress(std::size_t capacity) {
    Queue<std::uint64_t> queue(capacity);
    constexpr std::uint64_t count = 400000;
    std::atomic<bool> correct{true};
    std::thread producer([&] { for (std::uint64_t i = 0; i < count; ++i) while (!queue.push(i)) std::this_thread::yield(); });
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < count; ++i) { std::uint64_t value{}; while (!queue.pop(value)) std::this_thread::yield(); if (value != i) correct.store(false); }
    });
    producer.join(); consumer.join(); CHECK(correct.load());
}
int main() {
    try {
        parser_tests(); state_tests(); queue_tests<SpscRing>(); queue_tests<MutexRing>();
        stress<SpscRing>(8); stress<SpscRing>(256); stress<MutexRing>(8); stress<MutexRing>(256);
        const auto frames = workload(10000); FeedState state;
        for (auto frame : frames) CHECK(state.ingest(frame) == Action::published);
        CHECK(state.publications == frames.size()); CHECK(state.trusted());
        std::cout << "PASS: " << checks << " assertions; 1,600,000 concurrent transfers; allocation probes for both queues\n";
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
