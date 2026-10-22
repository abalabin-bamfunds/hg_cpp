#include <bg/schema/reflection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <thread>
#include <utility>

using namespace bg;

namespace {

enum class field_property { optional };

template <typename T, auto... Properties> struct reflected_field {
    using value_type = T;

    template <template <typename, auto...> typename Field>
    using rebind = Field<T, Properties...>;
};

template <template <typename, auto...> typename Field> inline constexpr bool is_reflected_field = false;
template <> inline constexpr bool is_reflected_field<reflected_field> = true;

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field,
          bool Reflection = is_reflected_field<Field>>
struct extends;

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field>
struct extends<Base, Field, false> : Base<Field> {};

template <template <template <typename, auto...> typename> typename Base, template <typename, auto...> typename Field>
struct extends<Base, Field, true> : details::schema_base_marker<Base> {};

template <template <typename, auto...> typename Field> struct base_schema {
    Field<int, field_property::optional> first;
    Field<double> replaced;
};

template <template <typename, auto...> typename Field> struct derived_schema : extends<base_schema, Field> {
    Field<float> replaced;
    Field<bool> last;
};

using reflection = details::schema_reflection<derived_schema, reflected_field>;

template <typename T, auto... Properties> struct copyable_probe_field {
    copyable_probe_field() = default;
    copyable_probe_field(details::FieldProbe &probe) { probe.record(this); }
};

static_assert(std::is_convertible_v<details::FieldProbe &, copyable_probe_field<int>>);
static_assert(std::is_copy_constructible_v<copyable_probe_field<int>>);
static_assert(std::is_move_constructible_v<copyable_probe_field<int>>);

} // namespace

TEST_CASE("schema reflection discovers base, replacement, and new fields", "[reflection][coverage-exempt]") {
    STATIC_CHECK(reflection::has_base);
    STATIC_CHECK(reflection::direct_field_count == 2);
    STATIC_CHECK(reflection::field_count == 3);
    STATIC_CHECK(reflection::direct_field_name<0>() == "replaced");
    STATIC_CHECK(reflection::direct_field_name<1>() == "last");
    STATIC_CHECK(reflection::field_index("first") == 0);
    STATIC_CHECK(reflection::field_index("replaced") == 1);
    STATIC_CHECK(reflection::field_index("last") == 2);
    STATIC_CHECK(reflection::field_index("missing") == details::missing_schema_field);
    STATIC_CHECK(reflection::direct_field_logical_index(0) == 1);
    STATIC_CHECK(reflection::direct_field_logical_index(1) == 2);
}

TEST_CASE("schema reflection resolves inherited and replaced field types", "[reflection][coverage-exempt]") {
    STATIC_CHECK(std::same_as<typename reflection::field_type<"first">,
                              reflected_field<int, field_property::optional>>);
    STATIC_CHECK(std::same_as<typename reflection::field_type<"replaced">::value_type, float>);
    STATIC_CHECK(std::same_as<typename reflection::field_type<"last">::value_type, bool>);
}

namespace {

template <typename T, auto... Properties> struct constructed_field {
    explicit constructed_field(std::size_t field_index) : index{field_index} {}
    constructed_field(details::FieldProbe &probe) { probe.record(this); }
    constructed_field(const constructed_field &) = delete;
    constructed_field(constructed_field &&) = delete;

    std::size_t index;
};

struct factory {
    std::array<std::size_t, 8> indices{};
    std::size_t count{};

    template <typename ReflectedField>
    typename ReflectedField::template rebind<constructed_field> make(std::size_t index) {
        indices[count++] = index;
        return typename ReflectedField::template rebind<constructed_field>{index};
    }
};

} // namespace

TEST_CASE("schema construction uses logical field indices throughout the hierarchy", "[reflection][coverage-exempt]") {
    auto values = factory{};

    const auto result = reflection::make<constructed_field>(values);
    const auto &base = static_cast<const base_schema<constructed_field> &>(result);

    CHECK(base.first.index == 0);
    STATIC_CHECK(std::same_as<std::remove_cvref_t<decltype(base.first)>,
                              constructed_field<int, field_property::optional>>);
    CHECK(base.replaced.index == 1);
    CHECK(result.replaced.index == 1);
    CHECK(result.last.index == 2);
    CHECK(values.count == 4);
    CHECK(values.indices[0] == 0);
    CHECK(values.indices[1] == 1);
    CHECK(values.indices[2] == 1);
    CHECK(values.indices[3] == 2);
}

TEST_CASE("schema field access resolves visible inherited and shadowed fields", "[reflection][coverage-exempt]") {
    STATIC_CHECK(reflection::field_name<0>() == "first");
    STATIC_CHECK(reflection::field_name<1>() == "replaced");
    STATIC_CHECK(reflection::field_name<2>() == "last");
    STATIC_CHECK(std::same_as<typename reflection::field<0>::value_type, int>);
    STATIC_CHECK(std::same_as<typename reflection::field<1>::value_type, float>);

    auto values = factory{};
    auto result = reflection::make<constructed_field>(values);
    auto [first, replaced, last] = reflection::fields(result);
    CHECK(std::addressof(first) == std::addressof(static_cast<base_schema<constructed_field> &>(result).first));
    CHECK(std::addressof(replaced) == std::addressof(result.replaced));
    CHECK(std::addressof(last) == std::addressof(result.last));

    const auto &constant = result;
    auto [const_first, const_replaced, const_last] = reflection::fields(constant);
    STATIC_CHECK(std::is_const_v<std::remove_reference_t<decltype(const_first)>>);
    CHECK(std::addressof(const_first) == std::addressof(static_cast<const base_schema<constructed_field> &>(result).first));
    CHECK(std::addressof(const_replaced) == std::addressof(result.replaced));
    CHECK(std::addressof(const_last) == std::addressof(result.last));
}

TEST_CASE("schema field offsets are captured from a local real schema", "[reflection][coverage-exempt]") {
    const auto &offsets = reflection::field_offsets<constructed_field>();
    auto values = factory{};
    const auto result = reflection::make<constructed_field>(values);
    const auto *object = reinterpret_cast<const std::byte *>(std::addressof(result));
    auto [first, replaced, last] = reflection::fields(result);

    CHECK(reinterpret_cast<const std::byte *>(std::addressof(first)) - object ==
          static_cast<std::ptrdiff_t>(offsets[0]));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(replaced)) - object ==
          static_cast<std::ptrdiff_t>(offsets[1]));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(last)) - object ==
          static_cast<std::ptrdiff_t>(offsets[2]));
}

namespace {

template <template <typename, auto...> typename Field> struct empty_schema {};

template <template <typename, auto...> typename Field> struct empty_base_schema : extends<empty_schema, Field> {};

template <template <typename, auto...> typename Field>
struct empty_base_derived_schema : extends<empty_base_schema, Field> {
    Field<int> present;
};

using empty_reflection = details::schema_reflection<empty_schema, reflected_field>;
using empty_base_reflection = details::schema_reflection<empty_base_derived_schema, reflected_field>;

} // namespace

TEST_CASE("schema construction handles empty schemas and empty base subobjects", "[reflection][coverage-exempt]") {
    STATIC_CHECK(sizeof(details::FieldProbe) == sizeof(void *));
    STATIC_CHECK(empty_reflection::field_count == 0);
    STATIC_CHECK(std::tuple_size_v<decltype(empty_reflection::fields(empty_schema<constructed_field>{}))> == 0);

    auto values = factory{};
    const auto empty = empty_reflection::make<constructed_field>(values);
    const auto with_empty_base = empty_base_reflection::make<constructed_field>(values);
    CHECK(sizeof(empty) == 1);
    CHECK(with_empty_base.present.index == 0);
}

namespace {

template <template <typename, auto...> typename Field> struct intermediate_schema : extends<derived_schema, Field> {};

template <template <typename, auto...> typename Field> struct deep_schema : extends<intermediate_schema, Field> {
    Field<char> deepest;
};

using deep_reflection = details::schema_reflection<deep_schema, reflected_field>;

} // namespace

TEST_CASE("schema construction flattens inherited fields through empty intermediate schemas",
          "[reflection][coverage-exempt]") {
    STATIC_CHECK(deep_reflection::field_count == 4);
    STATIC_CHECK(deep_reflection::field_name<0>() == "first");
    STATIC_CHECK(deep_reflection::field_name<1>() == "replaced");
    STATIC_CHECK(deep_reflection::field_name<2>() == "last");
    STATIC_CHECK(deep_reflection::field_name<3>() == "deepest");

    auto values = factory{};
    const auto result = deep_reflection::make<constructed_field>(values);
    const auto &base = static_cast<const base_schema<constructed_field> &>(result);
    CHECK(base.first.index == 0);
    CHECK(base.replaced.index == 1);
    CHECK(result.deepest.index == 3);
}

namespace {

template <typename T, auto... Properties> struct aligned_field {
    aligned_field(details::FieldProbe &probe) { probe.record(this); }

    alignas(64) std::byte payload[sizeof(T) + 7];
};

static_assert(std::is_convertible_v<details::FieldProbe &, aligned_field<int>>);
static_assert(std::is_copy_constructible_v<aligned_field<int>>);
static_assert(std::is_move_constructible_v<aligned_field<int>>);

template <template <typename, auto...> typename Field> struct aligned_base_schema {
    Field<std::byte[3]> head;
    Field<std::byte[65]> body;
};

template <template <typename, auto...> typename Field>
struct aligned_derived_schema : extends<aligned_base_schema, Field> {
    Field<std::byte[4]> tail;
};

struct aligned_factory {
    details::FieldProbe probe;

    template <typename ReflectedField> details::FieldProbe &make(std::size_t) { return probe; }
};

using aligned_reflection = details::schema_reflection<aligned_derived_schema, reflected_field>;

} // namespace

TEST_CASE("schema offset capture preserves over-aligned inherited field layout", "[reflection][coverage-exempt]") {
    STATIC_CHECK(alignof(aligned_field<int>) == 64);
    const auto &offsets = aligned_reflection::field_offsets<aligned_field>();
    auto factory = aligned_factory{};
    auto sample = aligned_reflection::make<aligned_field>(factory);
    auto [head, body, tail] = aligned_reflection::fields(sample);
    const auto *object = reinterpret_cast<const std::byte *>(std::addressof(sample));

    CHECK(std::addressof(head) == std::addressof(static_cast<aligned_base_schema<aligned_field> &>(sample).head));
    CHECK(std::addressof(body) == std::addressof(static_cast<aligned_base_schema<aligned_field> &>(sample).body));
    CHECK(std::addressof(tail) == std::addressof(sample.tail));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(head)) - object ==
          static_cast<std::ptrdiff_t>(offsets[0]));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(body)) - object ==
          static_cast<std::ptrdiff_t>(offsets[1]));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(tail)) - object ==
          static_cast<std::ptrdiff_t>(offsets[2]));
}

namespace {

template <typename T, auto... Properties> struct tail_padding_field {
    explicit tail_padding_field(std::size_t) {}
    tail_padding_field(details::FieldProbe &probe) { probe.record(this); }

    T value{};
    char padding_source{};
};

template <template <typename, auto...> typename Field> struct tail_padding_base_schema {
    Field<std::uint64_t> wide;
};

template <template <typename, auto...> typename Field>
struct tail_padding_derived_schema : extends<tail_padding_base_schema, Field> {
    Field<char> narrow;
};

struct tail_padding_factory {
    template <typename ReflectedField>
    typename ReflectedField::template rebind<tail_padding_field> make(std::size_t index) const {
        return typename ReflectedField::template rebind<tail_padding_field>{index};
    }
};

using tail_padding_reflection = details::schema_reflection<tail_padding_derived_schema, reflected_field>;

} // namespace

TEST_CASE("schema offset capture preserves inherited tail-padding reuse", "[reflection][coverage-exempt]") {
    STATIC_CHECK(sizeof(tail_padding_field<std::uint64_t>) >
                 sizeof(std::uint64_t) + sizeof(char));
    const auto &offsets = tail_padding_reflection::field_offsets<tail_padding_field>();
    auto values = tail_padding_factory{};
    const auto sample = tail_padding_reflection::make<tail_padding_field>(values);
    auto [wide, narrow] = tail_padding_reflection::fields(sample);
    const auto &base = static_cast<const tail_padding_base_schema<tail_padding_field> &>(sample);

    CHECK(std::addressof(wide) == std::addressof(base.wide));
    CHECK(std::addressof(narrow) == std::addressof(sample.narrow));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(wide)) -
              reinterpret_cast<const std::byte *>(std::addressof(sample)) ==
          static_cast<std::ptrdiff_t>(offsets[0]));
    CHECK(reinterpret_cast<const std::byte *>(std::addressof(narrow)) -
              reinterpret_cast<const std::byte *>(std::addressof(sample)) ==
          static_cast<std::ptrdiff_t>(offsets[1]));
}

namespace {

inline std::atomic_size_t cache_probe_constructions{};

template <typename T, auto... Properties> struct cache_field {
    cache_field(details::FieldProbe &probe) {
        cache_probe_constructions.fetch_add(1, std::memory_order_relaxed);
        probe.record(this);
    }
};

template <template <typename, auto...> typename Field> struct cache_schema {
    Field<int> first;
    Field<double> second;
};

using cache_reflection = details::schema_reflection<cache_schema, reflected_field>;

} // namespace

TEST_CASE("schema field offsets capture once during concurrent first use", "[reflection][coverage-exempt]") {
    constexpr auto ThreadCount = std::size_t{8};
    std::array<const void *, ThreadCount> addresses{};
    std::array<std::jthread, ThreadCount> threads{};
    for (auto index = std::size_t{}; index < ThreadCount; ++index) {
        threads[index] = std::jthread{[&, index] {
            addresses[index] = std::addressof(cache_reflection::field_offsets<cache_field>());
        }};
    }
    for (auto &thread : threads) {
        thread.join();
    }

    CHECK(cache_probe_constructions.load(std::memory_order_relaxed) == 2);
    CHECK(std::ranges::all_of(addresses, [&](const auto *address) { return address == addresses.front(); }));
}
