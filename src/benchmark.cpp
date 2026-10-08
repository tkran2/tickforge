#include "tickforge.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

using Clock = std::chrono::steady_clock;
struct Item { tickforge::Frame frame{}; std::int64_t enqueue_ns{}; };
std::int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
struct Result { double seconds{}, p50{}, p99{}, maximum{}; std::uint64_t digest{}, full_waits{}, empty_waits{}, publications{}; };

template<class Queue> Result run(const std::vector<tickforge::Frame>& frames, std::size_t capacity) {
    Queue queue(capacity);
    std::atomic<unsigned> ready{}; std::atomic<bool> start{};
    std::vector<std::int64_t> samples; samples.reserve((frames.size() + 127) / 128);
    Result result;
    std::int64_t finished{};
    std::thread producer([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t i = 0; i < frames.size(); ++i) {
            // Clock only sampled messages. Latency includes any queue backpressure.
            Item item{frames[i], i % 128 == 0 ? now_ns() : 0};
            while (!queue.push(item)) { ++result.full_waits; std::this_thread::yield(); }
        }
    });
    std::thread consumer([&] {
        tickforge::FeedState state;
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        Item item;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            while (!queue.pop(item)) { ++result.empty_waits; std::this_thread::yield(); }
            state.ingest(item.frame);
            if (item.enqueue_ns) samples.push_back(now_ns() - item.enqueue_ns);
        }
        finished = now_ns(); result.digest = state.digest(); result.publications = state.publications;
        if (!state.trusted() || state.gaps || state.malformed) std::terminate();
    });
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    const auto began = now_ns(); start.store(true, std::memory_order_release);
    producer.join(); consumer.join();
    result.seconds = static_cast<double>(finished - began) / 1e9;
    std::sort(samples.begin(), samples.end());
    result.p50 = static_cast<double>(samples[(samples.size() - 1) / 2]);
    result.p99 = static_cast<double>(samples[(samples.size() - 1) * 99 / 100]);
    result.maximum = static_cast<double>(samples.back());
    return result;
}
void print(const char* name, unsigned trial, std::size_t count, std::size_t capacity, const Result& result) {
    std::cout << name << ',' << trial << ',' << count << ',' << capacity << ',' << result.seconds
        << ',' << count / result.seconds << ',' << result.p50 << ',' << result.p99 << ',' << result.maximum
        << ',' << result.full_waits << ',' << result.empty_waits << ',' << result.publications << ',' << result.digest << '\n';
}
int main(int argc, char** argv) {
    try {
        const std::size_t count = argc > 1 ? std::stoull(argv[1]) : 100000;
        const unsigned trials = argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 5;
        const std::size_t capacity = argc > 3 ? std::stoull(argv[3]) : 256;
        if (!count || count > 10000000 || !trials || trials > 100) throw std::invalid_argument("invalid workload size/trials");
        tickforge::checked_capacity(capacity);
        const auto frames = tickforge::workload(count);
        // Warm up both implementations; exclude these passes from recorded trials.
        run<tickforge::SpscRing<Item>>(frames, capacity); run<tickforge::MutexRing<Item>>(frames, capacity);
        std::cout << "implementation,trial,messages,capacity,seconds,messages_per_second,p50_ns,p99_ns,max_ns,full_waits,empty_waits,publications,digest\n";
        for (unsigned trial = 0; trial < trials; ++trial) {
            Result spsc, mutex;
            if (trial % 2 == 0) { spsc = run<tickforge::SpscRing<Item>>(frames, capacity); mutex = run<tickforge::MutexRing<Item>>(frames, capacity); }
            else { mutex = run<tickforge::MutexRing<Item>>(frames, capacity); spsc = run<tickforge::SpscRing<Item>>(frames, capacity); }
            if (spsc.digest != mutex.digest || spsc.publications != count || mutex.publications != count)
                throw std::runtime_error("differential workload mismatch");
            print("spsc", trial, count, capacity, spsc); print("mutex", trial, count, capacity, mutex);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
