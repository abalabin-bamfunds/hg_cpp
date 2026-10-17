#include <bg/details/engine_wakeup.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BinarySemaphoreWakeup {
    std::binary_semaphore semaphore{0};

    void notify() noexcept { semaphore.release(); }
    void wait() noexcept { semaphore.acquire(); }
};

template <typename Wakeup>
bool run_case(std::string_view name, Wakeup &wakeup, std::size_t warmup_count, std::size_t sample_count) {
    std::binary_semaphore acknowledged{1};
    std::uint64_t sent_sequence{};
    auto sent_at = Clock::time_point{};
    auto received_count = std::size_t{};
    auto sequence_valid = true;
    auto latencies = std::vector<std::int64_t>(sample_count);

    auto consumer = std::thread{[&] {
        for (auto expected_sequence = std::uint64_t{};
             expected_sequence < warmup_count + sample_count; ++expected_sequence) {
            wakeup.wait();
            const auto received_at = Clock::now();
            if (sent_sequence != expected_sequence) {
                sequence_valid = false;
            }
            if (expected_sequence >= warmup_count) {
                latencies[expected_sequence - warmup_count] =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(received_at - sent_at).count();
            }
            ++received_count;
            acknowledged.release();
        }
    }};

    const auto send = [&](std::uint64_t sequence) {
        acknowledged.acquire();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        sent_sequence = sequence;
        sent_at = Clock::now();
        wakeup.notify();
    };

    for (auto sequence = std::uint64_t{}; sequence < warmup_count; ++sequence) {
        send(sequence);
    }

    for (auto sequence = std::uint64_t{warmup_count}; sequence < warmup_count + sample_count; ++sequence) {
        send(sequence);
    }
    acknowledged.acquire();
    consumer.join();

    const auto expected_count = warmup_count + sample_count;
    if (!sequence_valid || received_count != expected_count) {
        std::cerr << "validation failed for " << name << ": expected " << expected_count << " messages, received "
                  << received_count << ", sequence_valid=" << std::boolalpha << sequence_valid << '\n';
        return false;
    }

    auto sorted_latencies = latencies;
    std::ranges::sort(sorted_latencies);
    const auto percentile = [&](std::size_t percent) {
        const auto rank = (percent * sorted_latencies.size() + 99) / 100;
        return sorted_latencies[rank - 1];
    };

    std::cout << "case=" << name << " warmup=" << warmup_count << " samples=" << sample_count << '\n';
    for (auto sample = std::size_t{}; sample < latencies.size(); ++sample) {
        std::cout << "raw case=" << name << " sample=" << sample << " latency_ns=" << latencies[sample] << '\n';
    }
    std::cout << "summary case=" << name << " p50_ns=" << percentile(50) << " p95_ns=" << percentile(95)
              << " p99_ns=" << percentile(99) << " max_ns=" << sorted_latencies.back() << '\n';
    return true;
}

} // namespace

int main(int argc, char **argv) {
    auto warmup_count = std::size_t{50};
    auto sample_count = std::size_t{1'000};
    if (argc == 2 && std::string_view{argv[1]} == "--quick") {
        warmup_count = 5;
        sample_count = 25;
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--quick]\n";
        return 2;
    }

    auto semaphore_wakeup = BinarySemaphoreWakeup{};
    auto engine_wakeup = bg::details::EngineWakeup{};
    const auto semaphore_valid = run_case("binary_semaphore", semaphore_wakeup, warmup_count, sample_count);
    const auto engine_valid = run_case("engine_wakeup", engine_wakeup, warmup_count, sample_count);
    return semaphore_valid && engine_valid ? 0 : 1;
}
