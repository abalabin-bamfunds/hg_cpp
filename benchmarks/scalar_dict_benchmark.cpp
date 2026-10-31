#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/value.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
using Dictionary = bg::Dict<bg::i64, bg::i64>;
constexpr auto SmallBatchSize = std::size_t{16};
constexpr auto LargeBatchSize = std::size_t{64};
constexpr auto LookupCount = std::size_t{100'000};
constexpr auto FullPassCount = std::size_t{3};
constexpr auto SampleCount = std::size_t{7};

volatile std::uint64_t benchmark_result;

void consume(std::uint64_t value) noexcept { benchmark_result = value; }

template <typename Function> double median_nanoseconds_per_call(Function &&function, std::size_t operations) {
    auto samples = std::array<double, SampleCount>{};
    std::invoke(function);

    for (auto &sample : samples) {
        const auto start = clock_type::now();
        std::invoke(function);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count();
        sample = static_cast<double>(elapsed) / static_cast<double>(operations);
    }

    std::ranges::sort(samples);
    return samples[SampleCount / 2];
}

struct Snapshot {
    std::vector<bg::ScalarDictRuntimeType::BatchEntry> *entries;
};

void append_snapshot(void *context, bg::ScalarDictRuntimeType::Entry entry) {
    auto &snapshot = *static_cast<Snapshot *>(context);
    snapshot.entries->push_back({entry.key.data, entry.value.data});
}

std::uint64_t sum_entry(const bg::ScalarDictRuntimeType::BatchEntry &entry) {
    return static_cast<std::uint64_t>(*static_cast<const std::int64_t *>(entry.key) +
                                       *static_cast<const std::int64_t *>(entry.mapped));
}

template <std::size_t Size> double benchmark_cursor(const bg::Scalar<Dictionary> &dictionary) {
    return median_nanoseconds_per_call(
        [&] {
            auto sum = std::uint64_t{};
            for (auto pass = std::size_t{}; pass < FullPassCount; ++pass) {
                auto cursor = dictionary.make_cursor();
                auto buffer = std::array<bg::ScalarDictRuntimeType::BatchEntry, Size>{};
                for (auto count = cursor->next_batch(buffer); count != 0; count = cursor->next_batch(buffer)) {
                    for (const auto &entry : std::span{buffer}.first(count)) {
                        sum += sum_entry(entry);
                    }
                }
            }
            consume(sum);
        },
        FullPassCount);
}

void benchmark(std::size_t size) {
    auto entries = std::vector<std::pair<std::int64_t, std::int64_t>>{};
    entries.reserve(size);
    for (auto key = std::int64_t{}; key < static_cast<std::int64_t>(size); ++key) {
        entries.emplace_back(key, key + 1);
    }

    const auto value = bg::ScalarValue<Dictionary>{entries};
    const auto dictionary = value.view();
    auto queries = std::vector<std::int64_t>(LookupCount);
    for (auto index = std::size_t{}; index < queries.size(); ++index) {
        queries[index] = static_cast<std::int64_t>(index % size);
    }

    const auto find_ns = median_nanoseconds_per_call(
        [&] {
            auto sum = std::uint64_t{};
            for (const auto key : queries) {
                if (const auto mapped = dictionary.find(key)) {
                    sum += static_cast<std::uint64_t>(mapped->value());
                }
            }
            consume(sum);
        },
        LookupCount);

    const auto contains_and_at_ns = median_nanoseconds_per_call(
        [&] {
            auto sum = std::uint64_t{};
            for (const auto key : queries) {
                if (dictionary.contains(key)) {
                    sum += static_cast<std::uint64_t>(dictionary.at(key).value());
                }
            }
            consume(sum);
        },
        LookupCount);

    const auto for_each_ns = median_nanoseconds_per_call(
        [&] {
            auto sum = std::uint64_t{};
            for (auto pass = std::size_t{}; pass < FullPassCount; ++pass) {
                dictionary.for_each([&](const auto key, const auto mapped) {
                    sum += static_cast<std::uint64_t>(key.value() + mapped.value());
                });
            }
            consume(sum);
        },
        FullPassCount);

    const auto snapshot_ns = median_nanoseconds_per_call(
        [&] {
            auto sum = std::uint64_t{};
            for (auto pass = std::size_t{}; pass < FullPassCount; ++pass) {
                auto snapshot = std::vector<bg::ScalarDictRuntimeType::BatchEntry>{};
                snapshot.reserve(size);
                auto state = Snapshot{&snapshot};
                dictionary.runtime_type().for_each(dictionary.data(), &state, append_snapshot);
                for (const auto &entry : snapshot) {
                    sum += sum_entry(entry);
                }
            }
            consume(sum);
        },
        FullPassCount);

    const auto cursor_16_ns = benchmark_cursor<SmallBatchSize>(dictionary);
    const auto cursor_64_ns = benchmark_cursor<LargeBatchSize>(dictionary);

    std::cout << std::setw(7) << size << "  " << std::setw(10) << std::fixed << std::setprecision(1) << find_ns
              << "  " << std::setw(13) << contains_and_at_ns << "  " << std::setw(12) << for_each_ns << "  "
              << std::setw(12) << snapshot_ns << "  " << std::setw(12) << cursor_16_ns << "  " << std::setw(12)
              << cursor_64_ns << "  " << std::setw(10) << size * sizeof(bg::ScalarDictRuntimeType::BatchEntry)
              << "  " << std::setw(12) << SmallBatchSize * sizeof(bg::ScalarDictRuntimeType::BatchEntry) << "  "
              << std::setw(12) << LargeBatchSize * sizeof(bg::ScalarDictRuntimeType::BatchEntry) << '\n';
}

} // namespace

int main() {
    std::cout << "Cursor traversal allocates one runtime-specific cursor object per active iterator.\n";
    std::cout << "entries  find ns/lookup  contains+at ns/lookup  for_each ns/pass  snapshot ns/pass  cursor16 ns/pass"
                 "  cursor64 ns/pass  snapshot bytes  cursor16 buffer bytes  cursor64 buffer bytes\n";
    benchmark(32);
    benchmark(1'000);
    benchmark(100'000);
}
