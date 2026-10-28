#include <bg/logging.hpp>

#include <catch2/catch_test_macros.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <stdexcept>
#include <utility>

// Each test case changes Quill's process-wide state. CTest discovery runs each case in a separate process.
TEST_CASE("BG logging lazily initializes and rejects later configuration", "[library][logging]") {
    auto *logger = bg::logging::logger();

    REQUIRE(logger != nullptr);
    CHECK(logger->get_logger_name() == "bg");
    CHECK(logger->get_sinks().size() == 1);
    CHECK(logger->get_pattern_formatter_options().format_pattern ==
          "%(time) LOG_%(log_level:<9) %(logger:<12) %(message)%(mdc)");
    CHECK(logger->get_pattern_formatter_options().timestamp_pattern == "%H:%M:%S.%Qus");
    CHECK(quill::Backend::is_running());
    CHECK(bg::logging::logger() == logger);
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
}

TEST_CASE("BG logging explicit initialization uses stdout and BG defaults", "[library][logging]") {
    auto *logger = bg::logging::initialize();

    REQUIRE(logger != nullptr);
    CHECK(logger->get_logger_name() == "bg");
    CHECK(logger->get_sinks().size() == 1);
    CHECK(logger->get_pattern_formatter_options().format_pattern ==
          "%(time) LOG_%(log_level:<9) %(logger:<12) %(message)%(mdc)");
    CHECK(logger->get_pattern_formatter_options().timestamp_pattern == "%H:%M:%S.%Qus");
    CHECK(quill::Backend::is_running());
}

TEST_CASE("BG logging adopts an application logger", "[library][logging]") {
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("application", quill::ConsoleSinkConfig{});
    auto application_pattern = quill::PatternFormatterOptions{"application %(message)", "%H:%M:%S"};
    auto *application_logger = quill::Frontend::create_logger("bg", std::move(sink), application_pattern);

    REQUIRE_FALSE(quill::Backend::is_running());
    CHECK(bg::logging::logger() == application_logger);
    CHECK(application_logger->get_pattern_formatter_options().format_pattern == "application %(message)");
    CHECK(application_logger->get_pattern_formatter_options().timestamp_pattern == "%H:%M:%S");
    CHECK(quill::Backend::is_running());
}

TEST_CASE("BG logging rejects initialization after Quill backend startup", "[library][logging]") {
    quill::Backend::start();

    REQUIRE(quill::Backend::is_running());
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
}
