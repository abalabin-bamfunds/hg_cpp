#include <bg/details/evaluation_scratch.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>

TEST_CASE("evaluation scratch retains record addresses while it grows", "[evaluation][scratch]") {
    auto memory = counting_memory_resource{};

    {
        auto scratch = bg::details::EvaluationScratch{memory};
        CHECK(memory.allocations() == 0);

        auto &frame = scratch.push_frame();
        auto *first = std::construct_at(static_cast<int *>(scratch.allocate(frame, sizeof(int), alignof(int))), 42);
        (void)scratch.allocate(frame, 128 * 1024, alignof(std::max_align_t));

        CHECK(*first == 42);
        CHECK(memory.allocations() > 0);

        scratch.pop_frame(frame);
    }

    CHECK(memory.allocations() == 0);
}

TEST_CASE("evaluation scratch restores nested frames", "[evaluation][scratch]") {
    auto memory = counting_memory_resource{};
    auto scratch = bg::details::EvaluationScratch{memory};
    auto &outer = scratch.push_frame();
    (void)scratch.allocate(outer, 32, alignof(std::max_align_t));

    auto &inner = scratch.push_frame();
    const auto *inner_address = &inner;
    (void)scratch.allocate(inner, 64, alignof(std::max_align_t));
    scratch.pop_frame(inner);

    auto &replacement = scratch.push_frame();
    CHECK(&replacement == inner_address);

    scratch.pop_frame(replacement);
    scratch.pop_frame(outer);
}
