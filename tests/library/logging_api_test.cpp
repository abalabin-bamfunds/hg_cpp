#include <bg/logging.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

struct DeferredFormatProbe {
    DeferredFormatProbe(int value, std::mutex *mutex, std::thread::id *formatted_on, int *copies = nullptr)
        : value{value}, mutex{mutex}, formatted_on{formatted_on}, copies{copies} {}

    DeferredFormatProbe(const DeferredFormatProbe &other)
        : value{other.value}, mutex{other.mutex}, formatted_on{other.formatted_on}, copies{other.copies} {
        if (copies != nullptr) {
            ++*copies;
        }
    }

    int value;
    std::mutex *mutex;
    std::thread::id *formatted_on;
    int *copies;
};

struct MoveOnlyFormatProbe {
    std::unique_ptr<int> value;
};

template <> struct fmt::formatter<DeferredFormatProbe> {
    constexpr auto parse(fmt::format_parse_context &context) { return context.begin(); }

    auto format(const DeferredFormatProbe &value, fmt::format_context &context) const {
        {
            const auto lock = std::scoped_lock{*value.mutex};
            *value.formatted_on = std::this_thread::get_id();
        }
        return fmt::format_to(context.out(), "probe({})", value.value);
    }
};

template <> struct fmt::formatter<MoveOnlyFormatProbe> {
    constexpr auto parse(fmt::format_parse_context &context) { return context.begin(); }

    auto format(const MoveOnlyFormatProbe &value, fmt::format_context &context) const {
        return fmt::format_to(context.out(), "move-only({})", *value.value);
    }
};

namespace {

std::filesystem::path log_path() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("bg_logging_api_" + std::to_string(nonce) + ".log");
}

std::string read_file(const std::filesystem::path &path) {
    auto file = std::ifstream{path};
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

auto remove_file_on_exit(const std::filesystem::path &path) {
    return bg::util::scope_exit{[path]() noexcept {
        auto error = std::error_code{};
        std::filesystem::remove(path, error);
    }};
}

} // namespace

TEST_CASE("BG logging snapshots strings and defers value formatting", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    bg::logging::initialize({.file = path.string(), .pattern = "{message}"});

    auto source = std::string{"before"};
    auto view = std::string_view{source};
    char characters[]{'a', '\0', 'b'};
    auto format_mutex = std::mutex{};
    auto formatted_on = std::thread::id{};
    auto probe = DeferredFormatProbe{17, &format_mutex, &formatted_on};
    const auto caller_thread = std::this_thread::get_id();
    const auto tuple = std::tuple{3, 4};
    auto move_only = MoveOnlyFormatProbe{std::make_unique<int>(23)};

    auto lock = std::unique_lock{format_mutex};
    bg::log::info("probe={} view={} array={} tuple={} value={}", probe, view, characters, tuple,
                  std::move(move_only));
    source[0] = 'x';
    characters[0] = 'x';
    lock.unlock();
    bg::log::info("single={}", tuple);

    bg::logging::flush();
    const auto output = read_file(path);
    CHECK(output.find("view=before") != std::string::npos);
    CHECK(output.find("array=a\\x00b") != std::string::npos);
    CHECK(output.find("tuple=(3, 4)") != std::string::npos);
    CHECK(output.find("single=(3, 4)") != std::string::npos);
    CHECK(output.find("probe=probe(17)") != std::string::npos);
    CHECK(output.find("value=move-only(23)") != std::string::npos);
    {
        const auto lock = std::scoped_lock{format_mutex};
        CHECK(formatted_on != std::thread::id{});
        CHECK(formatted_on != caller_thread);
    }
}

TEST_CASE("disabled BG logging calls do not copy arguments", "[library][logging]") {
    bg::logging::set_level(bg::logging::Level::off);

    auto format_mutex = std::mutex{};
    auto formatted_on = std::thread::id{};
    auto copies = 0;
    auto probe = DeferredFormatProbe{17, &format_mutex, &formatted_on, &copies};
    bg::log::info("disabled {}", probe);

    CHECK(copies == 0);
    CHECK(formatted_on == std::thread::id{});
}
