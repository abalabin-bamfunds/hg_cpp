#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

namespace {

struct finalizable_value {
    finalizable_value() = default;
    finalizable_value(const finalizable_value &) = default;
    ~finalizable_value() noexcept {
        if (count != nullptr) {
            ++*count;
        }
    }

    int *count{};
};

} // namespace

TEST_CASE("C++ scalar storage copies into graph-owned memory", "[scalar][feature:scalar-storage]") {
    std::pmr::unsynchronized_pool_resource source_memory;
    std::pmr::unsynchronized_pool_resource graph_memory;
    const std::pmr::string source{"a scalar value long enough to require allocated storage", &source_memory};
    const bg::ScalarRuntimeType &runtime_type = bg::cpp_scalar_runtime_type<bg::string>();
    auto allocator = bg::allocator{graph_memory};

    void *storage = allocator.allocate_bytes(runtime_type.size(), runtime_type.alignment());
    runtime_type.copy_construct(storage, &source, allocator);

    const auto &copy = *static_cast<const std::pmr::string *>(storage);
    CHECK(copy == source);
    CHECK(copy.get_allocator().resource() == &graph_memory);

    runtime_type.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
}

TEST_CASE("C++ scalar storage reports its destruction requirements", "[scalar][feature:scalar-storage]") {
    const bg::ScalarRuntimeType &float_runtime_type = bg::cpp_scalar_runtime_type<float>();
    const bg::ScalarRuntimeType &string_runtime_type = bg::cpp_scalar_runtime_type<bg::string>();

    CHECK_FALSE(float_runtime_type.uses_variable_memory());
    CHECK_FALSE(float_runtime_type.requires_finalization());
    CHECK_FALSE(float_runtime_type.requires_destruction());
    CHECK(float_runtime_type.supports_byte_copy_initialization());
    CHECK(string_runtime_type.uses_variable_memory());
    CHECK_FALSE(string_runtime_type.requires_finalization());
    CHECK(string_runtime_type.requires_destruction());
    CHECK_FALSE(string_runtime_type.supports_byte_copy_initialization());
}

TEST_CASE("scalar storage distinguishes variable memory from finalization",
          "[scalar][storage][feature:scalar-storage]") {
    const auto &opaque = bg::cpp_scalar_runtime_type<bg::Opaque<finalizable_value>>();
    const auto &strings = bg::cpp_scalar_runtime_type<bg::List<bg::string>>();
    const auto &opaque_values = bg::cpp_scalar_runtime_type<bg::List<bg::Opaque<finalizable_value>>>();

    CHECK_FALSE(opaque.uses_variable_memory());
    CHECK(opaque.requires_finalization());
    CHECK(strings.uses_variable_memory());
    CHECK_FALSE(strings.requires_finalization());
    CHECK(opaque_values.requires_finalization());

    auto finalization_count = 0;
    auto source = finalizable_value{};
    source.count = &finalization_count;
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    alignas(finalizable_value) std::byte storage[sizeof(finalizable_value)];
    opaque.construct_from_value(storage, &source, allocator);
    opaque.destroy(storage, allocator);
    CHECK(finalization_count == 1);
}

TEST_CASE("C++ scalar storage constructs an initial runtime value", "[scalar][feature:scalar-storage]") {
    const bg::ScalarRuntimeType &runtime_type = bg::cpp_scalar_runtime_type<double>();
    alignas(double) std::byte storage[sizeof(double)];

    runtime_type.construct_initial_value(storage);

    CHECK(*reinterpret_cast<const double *>(storage) == 0.0);
}

TEST_CASE("evaluation time storage supports byte-copy initialization",
          "[scalar][feature:scalar-storage][scalar:evaluation-time]") {
    const auto &runtime_type = bg::cpp_scalar_runtime_type<bg::EvaluationTime>();
    alignas(bg::EvaluationTime) std::byte storage[sizeof(bg::EvaluationTime)];

    REQUIRE(runtime_type.supports_byte_copy_initialization());
    runtime_type.construct_initial_value(storage);

    CHECK(*reinterpret_cast<const bg::EvaluationTime *>(storage) == bg::MIN_DT);
}

TEST_CASE("resolved collection storage prefers a registered concrete runtime_type",
          "[scalar][storage][collection][feature:scalar-storage]") {
    const auto &concrete = bg::cpp_scalar_runtime_type<bg::Dict<std::int64_t, bg::string>>();

    const auto &resolved = bg::resolve_scalar_runtime_type(concrete.type(), {});

    CHECK(&resolved == &concrete);
}
