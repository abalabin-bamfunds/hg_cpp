#include <bg/library.hpp>
#include <bg/logging.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/scalar/format.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/util/scope_exit.hpp>

#include "../value/scalar/test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace bg;
using namespace bg::literals;

struct LogFormattingGate {
    void wait_until_entered() {
        auto lock = std::unique_lock{mutex};
        condition.wait(lock, [&] { return entered; });
    }

    void release() {
        const auto lock = std::scoped_lock{mutex};
        released = true;
        condition.notify_all();
    }

    std::thread::id backend_thread() {
        const auto lock = std::scoped_lock{mutex};
        return formatting_thread;
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool released{};
    std::thread::id formatting_thread;
};

struct LogGateProbe {
    LogFormattingGate *gate;
};

template <> struct fmt::formatter<LogGateProbe> {
    constexpr auto parse(fmt::format_parse_context &context) { return context.begin(); }

    auto format(const LogGateProbe &value, fmt::format_context &context) const {
        auto lock = std::unique_lock{value.gate->mutex};
        value.gate->formatting_thread = std::this_thread::get_id();
        value.gate->entered = true;
        value.gate->condition.notify_all();
        value.gate->condition.wait(lock, [&] { return value.gate->released; });
        return fmt::format_to(context.out(), "gate");
    }
};

namespace {

std::filesystem::path log_path() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("bg_log_operator_" + std::to_string(nonce) + ".log");
}

std::vector<std::string> read_lines(const std::filesystem::path &path) {
    auto file = std::ifstream{path};
    REQUIRE(file.is_open());
    auto lines = std::vector<std::string>{};
    for (auto line = std::string{}; std::getline(file, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

auto remove_file_on_exit(const std::filesystem::path &path) {
    return util::scope_exit{[path]() noexcept {
        auto error = std::error_code{};
        std::filesystem::remove(path, error);
    }};
}

struct ExtensionFormatObservation {
    std::mutex mutex;
    std::vector<std::pair<int, std::thread::id>> values;
};

ExtensionFormatObservation &extension_format_observation() {
    static auto observation = ExtensionFormatObservation{};
    return observation;
}

std::atomic_size_t &discarded_extension_values() {
    static auto discarded = std::atomic_size_t{};
    return discarded;
}

void discard_extension_scalar(util::buffer_reader &reader) {
    reader.skip<ScalarValue<>>();
    discarded_extension_values().fetch_add(1, std::memory_order_relaxed);
}

void format_extension_scalar(Scalar<> value, fmt::format_context &context) {
    const auto number = scalar_cast<test_scalar_provider::value>(value).value().get();
    {
        auto &observation = extension_format_observation();
        const auto lock = std::scoped_lock{observation.mutex};
        observation.values.emplace_back(number, std::this_thread::get_id());
    }

    context.advance_to(fmt::format_to(context.out(), "provider({})", number));
}

} // namespace

TEST_CASE("log_ defers scalar and nested tick formatting to the logging thread",
          "[feature:operator][series:value][scalar:i64][transition:removal]") {
    const auto embedded_nul = "text"_n = "a\0b";
    CHECK(embedded_nul.value == std::string{"a\0b", 3});

    auto &dispatch = ScalarDispatch<ScalarFormatOperations>::instance();
    REQUIRE(dispatch.add(test_scalar_provider::preserving_runtime_type(),
                         ScalarFormatOperations{&format_extension_scalar}));
    REQUIRE(dispatch.add(test_scalar_provider::semantic_type_from_provider(),
                         ScalarFormatOperations{&format_extension_scalar}));
    auto buffer_operations = ScalarBufferOperations::for_discard(&discard_extension_scalar);
    REQUIRE(ScalarDispatch<ScalarBufferOperations>::instance().add(test_scalar_provider::semantic_type_from_provider(),
                                                                   std::move(buffer_operations)));

    CHECK_THROWS_AS(testing::test_wiring([] {
                         (void)operators::log_("bad={}", 1, "__level__"_n = "verbose");
                     }),
                     WiringError);
    CHECK_THROWS_AS(testing::test_wiring([] {
                         (void)operators::log_("bad={}", 1, "__level__"_n = 1);
                     }),
                     WiringError);
    CHECK_THROWS_AS(testing::test_wiring([] {
                         const auto level = operators::const_("info");
                         (void)operators::log_("value={}", 1, "__level__"_n = level);
                     }),
                     WiringError);

    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    bg::logging::initialize({.file = path.string(), .pattern = "{message}"});
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);

    auto gate = LogFormattingGate{};
    bg::log::info("{}", LogGateProbe{&gate});
    gate.wait_until_entered();
    auto release_gate = util::scope_exit{[&gate]() noexcept { gate.release(); }};

    const auto graph = [](wp<TSD<i64, TS<i64>>> book, wp<TS<i64>> value, wp<TS<string>> text) {
        operators::log_("value={:04d}", value);
        operators::log_(book);
        operators::log_("book={}", book);
        operators::log_("disabled={}", book, "__level__"_n = "off");
        operators::log_("text={}", text);
        operators::log_(text);
        operators::log_("debug={}", value, "__level__"_n = "debug");
        operators::log_("warning={}", value, "__level__"_n = "warning");
        return operators::const_(i64{0});
    };
    (void)run_graph(graph, {{{1, 10}}, {{1, literal::REMOVE}}}, {7, 8}, {"alpha", "beta"});

    {
        (void)cpp_scalar_runtime_type<test_scalar_provider::value>();
        const auto extension_graph = [](wp<TSD<i64, TS<test_scalar_provider::value>>> values) {
            operators::log_("extension={}", test_scalar_provider::value{42});
            operators::log_(values);
            return operators::const_(i64{0});
        };
        (void)run_graph(extension_graph, {{{1, test_scalar_provider::value{42}}},
                                         {{1, test_scalar_provider::value{99}}}});
    }

    const auto validity_graph = [](wp<TS<bool>> trigger, wp<TS<i64>> value) {
        operators::log_("trigger={} value={}", trigger, value);
        return operators::const_(i64{0});
    };
    (void)run_graph(validity_graph, {true, true, true}, {1, literal::INVALID, literal::SKIP});

    const auto nested_graph = [](wp<TS<bool>> trigger, wp<TSD<i64, TSD<i64, TS<i64>>>> value) {
        operators::log_("trigger={} nested={}", trigger, value);
        return operators::const_(i64{0});
    };
    (void)run_graph(nested_graph, {true, true, true},
                    {{{1, {{2, 10}}}}, {{1, {{2, literal::INVALID}}}}, literal::SKIP});

    gate.release();
    release_gate.release();
    bg::logging::flush();

    const auto messages = read_lines(path);
    const auto contains_message = [&](std::string_view expected) {
        return std::ranges::find(messages, std::string{expected}) != messages.end();
    };
    const auto message_count = [&](std::string_view expected) {
        return std::ranges::count(messages, std::string{expected});
    };
    CHECK(contains_message("gate"));
    CHECK(contains_message("value=0007"));
    CHECK(contains_message("{1: 10}"));
    CHECK(contains_message("book={1: 10}"));
    CHECK(contains_message("text=alpha"));
    CHECK(contains_message("\"alpha\""));
    CHECK(contains_message("extension=provider(42)"));
    CHECK(contains_message("{1: provider(42)}"));
    CHECK(contains_message("value=0008"));
    CHECK(contains_message("{1: REMOVE}"));
    CHECK(contains_message("book={1: REMOVE}"));
    CHECK(contains_message("text=beta"));
    CHECK(contains_message("\"beta\""));
    CHECK(message_count("trigger=true value=1") == 1);
    CHECK(message_count("trigger=true value=INVALID") == 1);
    CHECK(message_count("trigger=true value=SKIP") == 1);
    CHECK(message_count("trigger=true nested={1: {2: 10}}") == 1);
    CHECK(message_count("trigger=true nested={1: {2: INVALID}}") == 1);
    CHECK(message_count("trigger=true nested=SKIP") == 1);
    CHECK(std::ranges::none_of(messages, [](const auto &message) {
        return message.starts_with("disabled=") || message.starts_with("debug=");
    }));

    const auto backend_thread = gate.backend_thread();
    auto &observation = extension_format_observation();
    {
        const auto lock = std::scoped_lock{observation.mutex};
        REQUIRE(observation.values.size() == 3);
        CHECK(observation.values[0] == std::make_pair(42, backend_thread));
        CHECK(observation.values[1] == std::make_pair(42, backend_thread));
        CHECK(observation.values[2] == std::make_pair(99, backend_thread));
    }

    CHECK(discarded_extension_values().load(std::memory_order_relaxed) == 2);

    CHECK(contains_message("warning=7"));
}
