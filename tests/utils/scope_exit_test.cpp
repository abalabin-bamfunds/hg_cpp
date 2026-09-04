#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <utility>

TEST_CASE("a scope exit function runs when its scope ends",
          "[utils][scope_exit][coverage-exempt]") {
    auto calls = 0;
    {
        const auto cleanup = bg::util::scope_exit{[&]() noexcept { ++calls; }};
    }

    CHECK(calls == 1);
}

TEST_CASE("a released scope exit function does not run", "[utils][scope_exit][coverage-exempt]") {
    auto calls = 0;
    {
        auto cleanup = bg::util::scope_exit{[&]() noexcept { ++calls; }};
        cleanup.release();
    }

    CHECK(calls == 0);
}

TEST_CASE("a scope exit function runs during exception cleanup",
          "[utils][scope_exit][coverage-exempt]") {
    auto calls = 0;

    CHECK_THROWS_AS(
        [&] {
            const auto cleanup = bg::util::scope_exit{[&]() noexcept { ++calls; }};
            throw std::runtime_error{"failure"};
        }(),
        std::runtime_error);
    CHECK(calls == 1);
}

TEST_CASE("moving a scope exit function transfers cleanup",
          "[utils][scope_exit][coverage-exempt]") {
    auto calls = 0;
    {
        auto first = bg::util::scope_exit{[&]() noexcept { ++calls; }};
        const auto second = std::move(first);
    }

    CHECK(calls == 1);
}
