#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

TEST_CASE("C++ scalar storage copies into graph-owned memory", "[scalar]") {
    std::pmr::unsynchronized_pool_resource source_memory;
    std::pmr::unsynchronized_pool_resource graph_memory;
    const std::pmr::string source{"a scalar value long enough to require allocated storage", &source_memory};
    const bg::ScalarStorageRepresentation &representation = bg::cpp_scalar_storage_representation<bg::string>();

    void *storage = graph_memory.allocate(representation.size(), representation.alignment());
    representation.copy_construct(storage, &source, graph_memory);

    const auto &copy = *static_cast<const std::pmr::string *>(storage);
    CHECK(copy == source);
    CHECK(copy.get_allocator().resource() == &graph_memory);

    representation.destroy(storage, graph_memory);
    graph_memory.deallocate(storage, representation.size(), representation.alignment());
}

TEST_CASE("C++ scalar storage reports its destruction requirements", "[scalar]") {
    const bg::ScalarStorageRepresentation &float_representation = bg::cpp_scalar_storage_representation<float>();
    const bg::ScalarStorageRepresentation &string_representation = bg::cpp_scalar_storage_representation<bg::string>();

    CHECK_FALSE(float_representation.uses_variable_memory());
    CHECK_FALSE(float_representation.requires_destruction());
    CHECK(float_representation.supports_byte_copy_initialization());
    CHECK(string_representation.uses_variable_memory());
    CHECK(string_representation.requires_destruction());
    CHECK_FALSE(string_representation.supports_byte_copy_initialization());
}

TEST_CASE("C++ scalar storage constructs an initial runtime value", "[scalar]") {
    const bg::ScalarStorageRepresentation &representation = bg::cpp_scalar_storage_representation<double>();
    alignas(double) std::byte storage[sizeof(double)];

    representation.construct_initial_value(storage);

    CHECK(*reinterpret_cast<const double *>(storage) == 0.0);
}

TEST_CASE("evaluation time storage supports byte-copy initialization", "[scalar]") {
    const auto &representation = bg::cpp_scalar_storage_representation<bg::EvaluationTime>();
    alignas(bg::EvaluationTime) std::byte storage[sizeof(bg::EvaluationTime)];

    REQUIRE(representation.supports_byte_copy_initialization());
    representation.construct_initial_value(storage);

    CHECK(*reinterpret_cast<const bg::EvaluationTime *>(storage) == bg::MIN_DT);
}

TEST_CASE("resolved collection storage prefers a registered concrete representation", "[scalar][storage][collection]") {
    const auto &concrete = bg::cpp_scalar_storage_representation<bg::Dict<std::int64_t, bg::string>>();

    const auto &resolved = bg::resolve_scalar_storage_representation(concrete.type(), {});

    CHECK(&resolved == &concrete);
}
