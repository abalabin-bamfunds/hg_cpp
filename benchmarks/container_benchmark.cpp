#include <bg/util/hash_dictionary.hpp>
#include <bg/util/linear_hash_table.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <numeric>
#include <random>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

class tracking_memory_resource final : public std::pmr::memory_resource {
  public:
    [[nodiscard]] std::size_t peak_bytes() const noexcept { return peak_bytes_; }

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        current_bytes_ += bytes;
        peak_bytes_ = std::max(peak_bytes_, current_bytes_);

        return result;
    }

    void do_deallocate(void *address, std::size_t bytes, std::size_t alignment) override {
        current_bytes_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(address, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }

    std::size_t current_bytes_{};
    std::size_t peak_bytes_{};
};

template <typename Function> [[nodiscard]] double measure(Function &&function) {
    const auto start = clock_type::now();
    std::forward<Function>(function)();
    const auto end = clock_type::now();

    return std::chrono::duration<double>{end - start}.count();
}

struct batched_measurement {
    double seconds;
    double slowest_batch_seconds;
};

template <typename Function>
[[nodiscard]] batched_measurement measure_batched(std::size_t count, std::size_t batch_size, Function &&function) {
    const auto start = clock_type::now();
    auto batch_start = start;
    auto slowest_batch = clock_type::duration{};

    for (auto index = std::size_t{}; index < count; ++index) {
        function(index);

        if ((index + 1) % batch_size != 0 && index + 1 != count) {
            continue;
        }

        const auto now = clock_type::now();
        slowest_batch = std::max(slowest_batch, now - batch_start);
        batch_start = now;
    }

    const auto end = clock_type::now();
    return {std::chrono::duration<double>{end - start}.count(), std::chrono::duration<double>{slowest_batch}.count()};
}

void report(std::string_view container, std::string_view operation, std::size_t count, double seconds) {
    const auto rate = static_cast<double>(count) / seconds / 1'000'000.0;
    std::cout << container << '\t' << operation << '\t' << rate << " million operations/s\n";
}

void report_memory(std::string_view container, std::size_t count, std::size_t bytes) {
    const auto bytes_per_item = static_cast<double>(bytes) / static_cast<double>(count);
    std::cout << container << "\tpeak memory\t" << bytes_per_item << " bytes/item\n";
}

void report_slowest_batch(std::string_view container, std::size_t batch_size, double seconds) {
    std::cout << container << "\tslowest " << batch_size << " inserts\t" << seconds * 1'000'000.0 << " us\n";
}

void benchmark_hash_index(std::span<const std::uint64_t> keys) {
    auto allocations = tracking_memory_resource{};
    auto memory = std::pmr::unsynchronized_pool_resource{&allocations};
    auto index = bg::util::linear_hash_table{};

    auto position = std::uint32_t{};
    for (const auto key : keys) {
        index.insert(bg::util::linear_hash_table::hash_code(key), position++, memory);
    }

    auto checksum = std::uint64_t{};
    const auto lookup = measure([&] {
        for (const auto key : keys) {
            checksum += *index.find(bg::util::linear_hash_table::hash_code(key),
                                    [&](auto candidate) { return keys[candidate] == key; });
        }
    });

    report("bg::linear_hash_table", "find by hash", keys.size(), lookup);
    report_memory("bg::linear_hash_table", keys.size(), allocations.peak_bytes());
    std::cout << "bg::linear_hash_table\tcheck\t" << checksum << '\n';

    index.release(memory);
}

void benchmark_custom_dictionary(std::span<const std::uint64_t> keys, std::span<const std::uint64_t> missing_keys) {
    constexpr auto insertion_batch_size = std::size_t{256};

    auto allocations = tracking_memory_resource{};
    auto memory = std::pmr::unsynchronized_pool_resource{&allocations};
    auto values = bg::util::hash_dictionary<std::uint64_t, std::uint64_t>{};

    const auto insertion = measure_batched(keys.size(), insertion_batch_size, [&](auto index) {
        values.insert_or_assign(keys[index], keys[index], memory);
    });

    auto checksum = std::uint64_t{};
    const auto lookup = measure([&] {
        for (const auto key : keys) {
            checksum += *values.find(key);
        }
    });

    auto misses = std::size_t{};
    const auto missing_lookup = measure([&] {
        for (const auto key : missing_keys) {
            misses += values.find(key) == nullptr;
        }
    });

    auto iteration_checksum = std::uint64_t{};
    const auto iteration = measure([&] {
        for (const auto [key, value] : values) {
            iteration_checksum += key + value;
        }
    });

    const auto erasure = measure([&] {
        for (const auto key : keys) {
            values.erase(key, memory);
        }
    });

    report("bg::hash_dictionary", "insert", keys.size(), insertion.seconds);
    report_slowest_batch("bg::hash_dictionary", insertion_batch_size, insertion.slowest_batch_seconds);
    report("bg::hash_dictionary", "find", keys.size(), lookup);
    report("bg::hash_dictionary", "find missing", keys.size(), missing_lookup);
    report("bg::hash_dictionary", "iterate", keys.size(), iteration);
    report("bg::hash_dictionary", "erase", keys.size(), erasure);
    report_memory("bg::hash_dictionary", keys.size(), allocations.peak_bytes());
    std::cout << "bg::hash_dictionary\tcheck\t" << checksum << ' ' << misses << ' ' << iteration_checksum << '\n';

    values.release(memory);
}

void benchmark_standard_dictionary(std::span<const std::uint64_t> keys, std::span<const std::uint64_t> missing_keys) {
    constexpr auto insertion_batch_size = std::size_t{256};

    auto allocations = tracking_memory_resource{};
    auto memory = std::pmr::unsynchronized_pool_resource{&allocations};
    auto values = std::pmr::unordered_map<std::uint64_t, std::uint64_t>{&memory};

    const auto insertion = measure_batched(keys.size(), insertion_batch_size,
                                           [&](auto index) { values.insert_or_assign(keys[index], keys[index]); });

    auto checksum = std::uint64_t{};
    const auto lookup = measure([&] {
        for (const auto key : keys) {
            checksum += values.find(key)->second;
        }
    });

    auto misses = std::size_t{};
    const auto missing_lookup = measure([&] {
        for (const auto key : missing_keys) {
            misses += values.find(key) == values.end();
        }
    });

    auto iteration_checksum = std::uint64_t{};
    const auto iteration = measure([&] {
        for (const auto &[key, value] : values) {
            iteration_checksum += key + value;
        }
    });

    const auto erasure = measure([&] {
        for (const auto key : keys) {
            values.erase(key);
        }
    });

    report("std::unordered_map", "insert", keys.size(), insertion.seconds);
    report_slowest_batch("std::unordered_map", insertion_batch_size, insertion.slowest_batch_seconds);
    report("std::unordered_map", "find", keys.size(), lookup);
    report("std::unordered_map", "find missing", keys.size(), missing_lookup);
    report("std::unordered_map", "iterate", keys.size(), iteration);
    report("std::unordered_map", "erase", keys.size(), erasure);
    report_memory("std::unordered_map", keys.size(), allocations.peak_bytes());
    std::cout << "std::unordered_map\tcheck\t" << checksum << ' ' << misses << ' ' << iteration_checksum << '\n';
}

[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9;
    value ^= value >> 27;
    value *= 0x94d049bb133111eb;
    value ^= value >> 31;

    return value;
}

void benchmark_dictionaries(std::string_view distribution, std::span<const std::uint64_t> keys,
                            std::span<const std::uint64_t> missing_keys) {
    std::cout << distribution << " keys\n";
    benchmark_custom_dictionary(keys, missing_keys);
    benchmark_hash_index(keys);
    benchmark_standard_dictionary(keys, missing_keys);
}

} // namespace

int main(int argument_count, char **arguments) {
    auto count = std::size_t{500'000};
    if (argument_count == 2) {
        const auto text = std::string_view{arguments[1]};
        std::from_chars(text.data(), text.data() + text.size(), count);
    }

    auto keys = std::vector<std::uint64_t>(count);
    std::iota(keys.begin(), keys.end(), std::uint64_t{});
    std::ranges::shuffle(keys, std::mt19937{12345});

    auto missing_keys = keys;
    std::ranges::transform(missing_keys, missing_keys.begin(), [count](auto key) { return key + count; });
    benchmark_dictionaries("sequential", keys, missing_keys);

    std::ranges::transform(keys, keys.begin(), mix);
    std::ranges::transform(missing_keys, missing_keys.begin(), mix);
    benchmark_dictionaries("mixed", keys, missing_keys);
}
