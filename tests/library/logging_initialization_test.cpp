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
    CHECK(quill::Backend::is_running());
    CHECK(bg::logging::logger() == logger);
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
}

TEST_CASE("BG logging explicit initialization uses stdout and Quill defaults", "[library][logging]") {
    auto *logger = bg::logging::initialize();

    REQUIRE(logger != nullptr);
    CHECK(logger->get_logger_name() == "bg");
    CHECK(logger->get_sinks().size() == 1);
    CHECK(logger->get_pattern_formatter_options() == quill::PatternFormatterOptions{});
    CHECK(quill::Backend::is_running());
}

TEST_CASE("BG logging adopts an application logger", "[library][logging]") {
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("application", quill::ConsoleSinkConfig{});
    auto *application_logger = quill::Frontend::create_logger("bg", std::move(sink));

    REQUIRE_FALSE(quill::Backend::is_running());
    CHECK(bg::logging::logger() == application_logger);
    CHECK(quill::Backend::is_running());
}

TEST_CASE("BG logging rejects initialization after Quill backend startup", "[library][logging]") {
    quill::Backend::start();

    REQUIRE(quill::Backend::is_running());
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
}
