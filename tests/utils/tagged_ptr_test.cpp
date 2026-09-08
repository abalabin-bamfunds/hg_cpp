#include <bg/util/tagged_ptr.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

namespace {

enum class PointerState : std::uintptr_t {
    first,
    second,
    third,
};

struct First {
    int value;
};

struct Second {
    int value;
};

struct RecursiveFirst;
struct RecursiveSecond;

using RecursivePointer = bg::util::erased_tagged_variant_ptr<alignof(void *), RecursiveFirst, RecursiveSecond>;

struct RecursiveFirst {
    RecursivePointer next;
};

struct RecursiveSecond {
    RecursivePointer next;
};

} // namespace

TEST_CASE("a tagged pointer stores a pointer and tag in one word", "[utils][coverage-exempt]") {
    auto first = First{12};
    auto pointer = bg::util::tagged_ptr<First, 2, PointerState>{&first, PointerState::second};

    STATIC_CHECK(sizeof(pointer) == sizeof(void *));
    CHECK(pointer.get() == &first);
    CHECK(pointer->value == 12);
    CHECK(pointer.tag() == PointerState::second);

    pointer.reset_tag(PointerState::third);
    CHECK(pointer.get() == &first);
    CHECK(pointer.tag() == PointerState::third);
}

TEST_CASE("a null tagged pointer has no tag", "[utils][coverage-exempt]") {
    auto pointer = bg::util::tagged_ptr<First, 2, PointerState>{nullptr};

    pointer.reset(nullptr, PointerState::third);

    CHECK_FALSE(pointer);
    CHECK(pointer.raw() == 0);
    CHECK(pointer == decltype(pointer){nullptr});
}

TEST_CASE("a tagged variant pointer identifies and visits its active type", "[utils][coverage-exempt]") {
    auto first = First{12};
    auto second = Second{34};
    auto pointer = bg::util::tagged_variant_ptr<First, Second>{&first};

    STATIC_CHECK(sizeof(pointer) == sizeof(void *));
    CHECK(pointer.holds<First>());
    CHECK(pointer.get<First>() == &first);
    CHECK(pointer.get<Second>() == nullptr);
    CHECK(pointer.visit([](const auto *value) { return value->value; }) == 12);

    pointer = &second;
    CHECK(pointer.holds<Second>());
    CHECK(pointer.visit([](const auto *value) { return value->value; }) == 34);

    pointer = nullptr;
    CHECK_FALSE(pointer);
    CHECK(pointer.index() == decltype(pointer)::npos);
}

TEST_CASE("a tagged pointer preserves the pointed-to const type", "[utils][coverage-exempt]") {
    const auto value = First{12};
    const auto pointer = bg::util::tagged_ptr<const First>{&value};

    STATIC_CHECK(std::is_same_v<decltype(pointer.get()), const First *>);
    CHECK(pointer.get() == &value);
}

TEST_CASE("an erased tagged variant supports recursive alternatives", "[utils][coverage-exempt]") {
    auto first = RecursiveFirst{};
    auto second = RecursiveSecond{};

    first.next = &second;
    second.next = &first;

    STATIC_CHECK(sizeof(RecursivePointer) == sizeof(void *));
    CHECK(first.next.get<RecursiveSecond>() == &second);
    CHECK(second.next.get<RecursiveFirst>() == &first);
}
