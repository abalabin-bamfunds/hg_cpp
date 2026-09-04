#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <vector>

TEST_CASE("resolved scalar list storage supports mutation",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    const auto &item = bg::cpp_scalar_runtime_type<bg::string>();
    const auto &type = bg::list_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item};
    const auto &list = static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(list.size(), list.alignment());
    list.construct_initial_value(storage);

    const auto first = bg::ScalarValue{"first"};
    const auto second = bg::ScalarValue{"second"};
    list.append(storage, first.input_view(), memory);
    list.append(storage, second.input_view(), memory);
    list.set(storage, 0, second.input_view());
    list.erase(storage, 1, memory);

    REQUIRE(list.item_count(storage) == 1);
    CHECK(*static_cast<const std::pmr::string *>(list.item(storage, 0)) == "second");

    list.clear(storage, memory);
    CHECK(list.item_count(storage) == 0);

    list.destroy(storage, memory);
    memory.deallocate(storage, list.size(), list.alignment());
}

namespace {

struct RuntimeListItem {
    std::int64_t first;
    std::int64_t second;
};

} // namespace

TEST_CASE("a scalar list copies between composed and C++ storage",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    using Item = bg::Opaque<RuntimeListItem>;
    using List = bg::List<Item>;

    const auto &item = bg::cpp_scalar_runtime_type<Item>();
    const auto &type = bg::list_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item};
    const auto &composed =
        static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    const auto source =
        bg::ScalarValue<List>{std::vector<RuntimeListItem>{{.first = 1, .second = 2}, {.first = 3, .second = 4}}};
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(composed.size(), composed.alignment());
    composed.construct_from_input(storage, source.input_view(), memory);

    const auto composed_view = bg::Scalar<List>::from(composed, storage);
    const auto copy = bg::ScalarValue<List>{composed_view, memory};

    REQUIRE(copy.view().size() == 2);
    CHECK(copy.view()[0].value().first == 1);
    CHECK(copy.view()[1].value().second == 4);

    composed.destroy(storage, memory);
    memory.deallocate(storage, composed.size(), composed.alignment());
}
