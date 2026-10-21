#include <bg/details/periodic_callback_runner.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <time.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Result {
    double nanoseconds_per_cycle;
    std::uint64_t checksum;
};

template <typename Poll>
Result run_cycles(std::size_t cycles, Poll &&poll) {
    auto checksum = std::uint64_t{0x91e10da5c79e7b1d};
    const auto started = Clock::now();
    for (auto cycle = std::size_t{}; cycle < cycles; ++cycle) {
        poll();
        checksum = std::rotl(checksum, 7) + static_cast<std::uint64_t>(cycle) + 0x9e3779b97f4a7c15;
    }
    const auto elapsed = Clock::now() - started;
    return {std::chrono::duration<double, std::nano>{elapsed}.count() / cycles, checksum};
}

Result run_adaptive_cycles(std::size_t cycles) {
    constexpr auto target_poll_ns = std::int64_t{500'000};
    constexpr auto maximum_stride = std::size_t{1'000'000};
    auto stride = std::size_t{64};
    auto remaining = stride;
    auto cycles_since_poll = std::size_t{};
    auto window_started = Clock::now();
    auto checksum = std::uint64_t{0x91e10da5c79e7b1d};

    const auto started = Clock::now();
    for (auto cycle = std::size_t{}; cycle < cycles; ++cycle) {
        if (--remaining == 0) {
            const auto now = Clock::now();
            const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - window_started).count();
            if (elapsed_ns > 0) {
                stride = std::clamp<std::size_t>(target_poll_ns * cycles_since_poll / elapsed_ns, 1, maximum_stride);
            } else {
                stride = maximum_stride;
            }
            remaining = stride;
            cycles_since_poll = 0;
            window_started = now;
        }
        ++cycles_since_poll;
        checksum = std::rotl(checksum, 7) + static_cast<std::uint64_t>(cycle) + 0x9e3779b97f4a7c15;
    }
    const auto elapsed = Clock::now() - started;
    return {std::chrono::duration<double, std::nano>{elapsed}.count() / cycles, checksum};
}

template <typename Run>
void report(std::string_view name, std::size_t samples, Run &&run) {
    auto values = std::vector<double>{};
    auto checksum = std::uint64_t{};
    values.reserve(samples);
    for (auto sample = std::size_t{}; sample < samples; ++sample) {
        const auto result = run();
        values.push_back(result.nanoseconds_per_cycle);
        checksum = result.checksum;
        std::cout << "sample case=" << name << " index=" << sample
                  << " ns_per_cycle=" << result.nanoseconds_per_cycle << " checksum=" << result.checksum << '\n';
    }
    std::ranges::sort(values);
    std::cout << "case=" << name << " samples=" << samples << " median_ns_per_cycle="
              << values[values.size() / 2] << " min_ns_per_cycle=" << values.front()
              << " max_ns_per_cycle=" << values.back() << " checksum=" << checksum << '\n';
}

} // namespace

int main(int argc, char **argv) {
    auto cycles = std::size_t{1'000'000};
    auto samples = std::size_t{7};
    if (argc == 2 && std::string_view{argv[1]} == "--quick") {
        cycles = 100'000;
        samples = 3;
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--quick]\n";
        return 2;
    }

    const auto deadline = Clock::now() + std::chrono::hours{24};
    const auto no_poll = [] {};
    report("no_poll", samples, [&] { return run_cycles(cycles, no_poll); });
    report("steady_clock_each_cycle", samples, [&] {
        return run_cycles(cycles, [&] { (void)(Clock::now() < deadline); });
    });

    auto callback = bg::PeriodicCallback{.callback = [] {}, .interval = std::chrono::hours{24}};
    auto callback_runner = bg::details::PeriodicCallbackRunner{callback};
    callback_runner.run_now();
    report("periodic_runner_each_cycle", samples, [&] {
        return run_cycles(cycles, [&] { callback_runner.run_if_due(); });
    });

    for (const auto stride : {std::size_t{8}, std::size_t{32}, std::size_t{64}}) {
        auto remaining = stride;
        const auto name = "steady_clock_stride_" + std::to_string(stride);
        report(name, samples, [&] {
            remaining = stride;
            return run_cycles(cycles, [&] {
                if (--remaining == 0) {
                    (void)(Clock::now() < deadline);
                    remaining = stride;
                }
            });
        });
    }

    report("adaptive_500us_poll", samples, [&] { return run_adaptive_cycles(cycles); });

#if defined(__linux__)
    report("clock_monotonic_coarse_each_cycle", samples, [&] {
        auto timestamp = timespec{};
        return run_cycles(cycles, [&] { (void)::clock_gettime(CLOCK_MONOTONIC_COARSE, &timestamp); });
    });
#endif
}
