#include <bg/library.hpp>
#include <bg/logging.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/scalar/format.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/util/scope_exit.hpp>

#include "../value/scalar/test_scalar_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <quill/LogMacros.h>
#include <quill/sinks/Sink.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace {

struct CapturedMessage {
    quill::LogLevel level;
    std::string text;
    std::string statement;
};

class TestSink final : public quill::Sink {
  public:
    explicit TestSink(bool gate_on_message = false) : gate_on_message_{gate_on_message} {}

    void wait_for_gate() {
        auto lock = std::unique_lock{mutex_};
        gate_condition_.wait(lock, [&] { return gate_entered_; });
    }

    void release_gate() {
        const auto lock = std::scoped_lock{mutex_};
        gate_released_ = true;
        gate_condition_.notify_all();
    }

    std::vector<CapturedMessage> messages() const {
        const auto lock = std::scoped_lock{mutex_};
        return messages_;
    }

    std::thread::id backend_thread() const {
        const auto lock = std::scoped_lock{mutex_};
        return backend_thread_;
    }

  protected:
    void write_log(quill::MacroMetadata const *, uint64_t, std::string_view, std::string_view,
                   std::string const &, std::string_view, quill::LogLevel level, std::string_view,
                   std::string_view, std::vector<std::pair<std::string, std::string>> const *,
                   std::string_view message, std::string_view statement) override {
        auto lock = std::unique_lock{mutex_};
        backend_thread_ = std::this_thread::get_id();
        messages_.push_back({level, std::string{message}, std::string{statement}});
        if (!gate_on_message_ || message != "gate") {
            return;
        }

        gate_entered_ = true;
        gate_condition_.notify_all();
        gate_condition_.wait(lock, [&] { return gate_released_; });
    }

    void flush_sink() override {}

  private:
    mutable std::mutex mutex_;
    std::condition_variable gate_condition_;
    std::vector<CapturedMessage> messages_;
    std::thread::id backend_thread_;
    bool gate_on_message_{};
    bool gate_entered_{};
    bool gate_released_{};
};

struct ExtensionFormatObservation {
    std::mutex mutex;
    std::vector<std::pair<int, std::thread::id>> values;
};

struct BackendErrorObservation {
    std::mutex mutex;
    std::vector<std::string> messages;
};

ExtensionFormatObservation &extension_format_observation() {
    static auto observation = ExtensionFormatObservation{};
    return observation;
}

BackendErrorObservation &backend_error_observation() {
    static auto observation = BackendErrorObservation{};
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

    if (number == 99) {
        throw std::runtime_error{"extension format failed"};
    }

    context.advance_to(fmt::format_to(context.out(), "provider({})", number));
}

} // namespace

TEST_CASE("log_ defers scalar and nested tick formatting to the Quill backend",
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

    auto &backend_errors = backend_error_observation();
    auto backend_options = quill::BackendOptions{};
    backend_options.error_notifier = [&backend_errors](const std::string &message) {
        const auto lock = std::scoped_lock{backend_errors.mutex};
        backend_errors.messages.push_back(message);
    };
    auto sink = std::make_shared<TestSink>(true);
    auto secondary_sink = std::make_shared<TestSink>();
    auto pattern = quill::PatternFormatterOptions{"%(message)"};
    pattern.pattern_suffix = quill::PatternFormatterOptions::NO_SUFFIX;
    auto *logger = bg::logging::initialize({sink, secondary_sink}, pattern, backend_options);
    REQUIRE(logger->get_sinks().size() == 2);
    CHECK(logger->get_pattern_formatter_options().format_pattern == "%(message)");
    CHECK(bg::logging::logger() == logger);
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
    logger->set_log_level(quill::LogLevel::Info);

    QUILL_LOG_INFO(logger, "gate");
    sink->wait_for_gate();
    auto release_gate = util::scope_exit{[sink]() noexcept { sink->release_gate(); }};

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

    sink->release_gate();
    release_gate.release();
    bg::logging::flush();

    const auto messages = sink->messages();
    const auto secondary_messages = secondary_sink->messages();
    const auto contains_message = [&](std::string_view expected) {
        return std::ranges::any_of(messages, [&](const auto &message) { return message.text == expected; });
    };
    const auto message_count = [&](std::string_view expected) {
        return std::ranges::count_if(messages, [&](const auto &message) { return message.text == expected; });
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
    CHECK(std::ranges::any_of(secondary_messages, [](const auto &message) {
        return message.text == "trigger=true nested=SKIP" && message.statement == message.text;
    }));
    CHECK(std::ranges::none_of(messages, [](const auto &message) {
        return message.text.starts_with("disabled=") || message.text.starts_with("debug=");
    }));

    const auto backend_thread = sink->backend_thread();
    auto &observation = extension_format_observation();
    {
        const auto lock = std::scoped_lock{observation.mutex};
        REQUIRE(observation.values.size() == 3);
        CHECK(observation.values[0] == std::make_pair(42, backend_thread));
        CHECK(observation.values[1] == std::make_pair(42, backend_thread));
        CHECK(observation.values[2] == std::make_pair(99, backend_thread));
    }

    CHECK(discarded_extension_values().load(std::memory_order_relaxed) == 2);

    const auto warning = std::ranges::find_if(messages, [](const auto &message) {
        return message.text == "warning=7";
    });
    REQUIRE(warning != messages.end());
    CHECK(warning->level == quill::LogLevel::Warning);

    const auto error_lock = std::scoped_lock{backend_errors.mutex};
    CHECK(std::ranges::any_of(backend_errors.messages, [](const auto &message) {
        return message.find("extension format failed") != std::string::npos;
    }));
}
