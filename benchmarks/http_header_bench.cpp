#include "../extensions/web/src/http_header.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

NOINLINE std::string old_encode(std::string_view value) {
    auto result = std::string{};
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x80) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back(static_cast<char>(0xc0 | (character >> 6)));
            result.push_back(static_cast<char>(0x80 | (character & 0x3f)));
        }
    }
    return result;
}

NOINLINE std::string new_encode(std::string_view value) {
    return bg::web::details::latin1_header_value_as_utf8(value);
}

NOINLINE void old_convert(std::string &value) {
    auto converted = std::string{};
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character < 0x80) {
            converted.push_back(static_cast<char>(character));
        } else {
            converted.push_back(static_cast<char>(0xc0 | (character >> 6)));
            converted.push_back(static_cast<char>(0x80 | (character & 0x3f)));
        }
    }
    value.swap(converted);
}

NOINLINE void new_convert(std::string &value) {
    bg::web::details::convert_latin1_header_value_as_utf8(value);
}

volatile std::size_t consumed = 0;

template <typename Encode>
double run(const std::vector<std::string> &values, std::size_t iterations, Encode encode) {
    auto checksum = std::size_t{};
    const auto start = std::chrono::steady_clock::now();
    for (auto index = std::size_t{}; index < iterations; ++index) {
        const auto &value = values[index % values.size()];
        const auto result = encode(value);
        checksum += result.size();
        if (!result.empty()) checksum += static_cast<unsigned char>(result[index % result.size()]);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    consumed = consumed + checksum;
    return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(iterations);
}

template <typename Convert>
double run_in_place(std::vector<std::string> &values, std::size_t iterations, Convert convert) {
    auto checksum = std::size_t{};
    const auto start = std::chrono::steady_clock::now();
    for (auto index = std::size_t{}; index < iterations; ++index) {
        auto &value = values[index % values.size()];
        convert(value);
        checksum += value.size();
        if (!value.empty()) checksum += static_cast<unsigned char>(value[index % value.size()]);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    consumed = consumed + checksum;
    return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(iterations);
}

std::vector<std::string> make_values(std::size_t size, std::size_t non_ascii_stride) {
    auto values = std::vector<std::string>{};
    for (auto variant = 0; variant < 64; ++variant) {
        auto value = std::string{};
        value.reserve(size);
        for (auto index = std::size_t{}; index < size; ++index) {
            if (non_ascii_stride != 0 && index % non_ascii_stride == non_ascii_stride - 1) {
                value.push_back(static_cast<char>(0xa0 + (variant % 32)));
            } else {
                value.push_back(static_cast<char>('a' + ((index + static_cast<std::size_t>(variant)) % 26)));
            }
        }
        values.push_back(std::move(value));
    }
    return values;
}

int main() {
    struct Case {
        std::string_view name;
        std::vector<std::string> values;
        std::size_t iterations;
    };
    const auto cases = std::array{
        Case{"short ASCII", make_values(24, 0), 500'000},
        Case{"long ASCII", make_values(512, 0), 100'000},
        Case{"short mixed Latin-1", make_values(24, 3), 500'000},
        Case{"dense Latin-1", make_values(512, 1), 100'000},
    };

    for (const auto &test_case : cases) {
        for (const auto &value : test_case.values) {
            if (old_encode(value) != new_encode(value)) {
                std::cerr << "byte mismatch in " << test_case.name << '\n';
                return 1;
            }
        }
    }

    for (const auto &test_case : cases) {
        auto old_times = std::array<double, 7>{};
        auto new_times = std::array<double, 7>{};
        for (auto sample = std::size_t{}; sample < old_times.size(); ++sample) {
            old_times[sample] = run(test_case.values, test_case.iterations, old_encode);
            new_times[sample] = run(test_case.values, test_case.iterations, new_encode);
        }
        std::ranges::sort(old_times);
        std::ranges::sort(new_times);
        std::cout << test_case.name << ": old=" << old_times[3] << " ns/op, new=" << new_times[3]
                  << " ns/op, speedup=" << old_times[3] / new_times[3] << "x\n";
    }

    for (const auto size : {std::size_t{24}, std::size_t{512}}) {
        auto old_values = make_values(size, 0);
        auto new_values = old_values;
        const auto iterations = size == 24 ? std::size_t{500'000} : std::size_t{100'000};
        auto old_times = std::array<double, 7>{};
        auto new_times = std::array<double, 7>{};
        for (auto sample = std::size_t{}; sample < old_times.size(); ++sample) {
            old_times[sample] = run_in_place(old_values, iterations, old_convert);
            new_times[sample] = run_in_place(new_values, iterations, new_convert);
        }
        std::ranges::sort(old_times);
        std::ranges::sort(new_times);
        std::cout << "client ASCII " << size << " bytes: old=" << old_times[3] << " ns/op, new=" << new_times[3]
                  << " ns/op, speedup=" << old_times[3] / new_times[3] << "x\n";
    }
    std::cout << "checksum=" << consumed << '\n';
}
