#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

using namespace bg;

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

class InvalidListRuntimeType final : public ScalarRuntimeType {
  public:
    explicit InvalidListRuntimeType(const ScalarType &type) noexcept
        : ScalarRuntimeType{type, sizeof(i64), alignof(i64), false, false, true} {}

    void construct_initial_value(void *) const noexcept override {}
    void copy_construct(void *, const void *, allocator &) const override {}
    std::size_t hash(const void *) const noexcept override { return 0; }
    void destroy(void *, allocator &) const noexcept override {}

  private:
    void do_construct_from_input(void *, const details::ScalarData &, allocator &) const override {}
    void do_assign_from_input(void *, const details::ScalarData &) const override {}
    bool equals_same_type(const void *, const details::ScalarData &) const noexcept override { return false; }
};

} // namespace

TEST_CASE("C++ scalar storage copies into graph-owned memory", "[scalar][feature:scalar-storage]") {
    std::pmr::unsynchronized_pool_resource source_memory;
    std::pmr::unsynchronized_pool_resource graph_memory;
    const std::pmr::string source{"a scalar value long enough to require allocated storage", &source_memory};
    const ScalarRuntimeType &runtime_type = cpp_scalar_runtime_type<string>();
    auto allocator = bg::allocator{graph_memory};

    void *storage = allocator.allocate_bytes(runtime_type.size(), runtime_type.alignment());
    const auto source_view = std::string_view{source};
    runtime_type.construct_from_input(
        storage, details::ScalarData{runtime_type, std::addressof(source_view), ScalarDataMode::source_view},
        allocator);

    const auto copy = Scalar<string>::from(runtime_type, storage);
    CHECK(copy.value() == source);

    runtime_type.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
}

TEST_CASE("C++ scalar storage reports its destruction requirements", "[scalar][feature:scalar-storage]") {
    const ScalarRuntimeType &float_runtime_type = cpp_scalar_runtime_type<float>();
    const ScalarRuntimeType &string_runtime_type = cpp_scalar_runtime_type<string>();

    CHECK_FALSE(float_runtime_type.uses_variable_memory());
    CHECK_FALSE(float_runtime_type.requires_finalization());
    CHECK_FALSE(float_runtime_type.requires_destruction());
    CHECK(float_runtime_type.supports_byte_copy_initialization());
    CHECK(string_runtime_type.uses_variable_memory());
    CHECK(string_runtime_type.requires_finalization());
    CHECK(string_runtime_type.requires_destruction());
    CHECK(string_runtime_type.supports_byte_copy_initialization());
}

TEST_CASE("scalar storage distinguishes variable memory from finalization",
          "[scalar][storage][feature:scalar-storage]") {
    const auto &opaque = cpp_scalar_runtime_type<Opaque<finalizable_value>>();
    const auto &strings = cpp_scalar_runtime_type<List<string>>();
    const auto &opaque_values = cpp_scalar_runtime_type<List<Opaque<finalizable_value>>>();

    CHECK_FALSE(opaque.uses_variable_memory());
    CHECK(opaque.requires_finalization());
    CHECK(strings.uses_variable_memory());
    CHECK(strings.requires_finalization());
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

TEST_CASE("an opaque scalar runtime constructs a default value through its read type",
          "[feature:scalar-storage][scalar:opaque][storage:fixed]") {
    const auto &runtime_type = cpp_scalar_runtime_type<Opaque<finalizable_value>>();
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    alignas(finalizable_value) std::byte storage[sizeof(finalizable_value)];
    auto finalization_count = 0;

    runtime_type.construct_runtime_value(storage, allocator);
    auto &value = *reinterpret_cast<finalizable_value *>(storage);
    CHECK(value.count == nullptr);
    value.count = &finalization_count;
    runtime_type.destroy(storage, allocator);
    CHECK(finalization_count == 1);
}

TEST_CASE("C++ scalar storage constructs an initial runtime value", "[scalar][feature:scalar-storage]") {
    const ScalarRuntimeType &runtime_type = cpp_scalar_runtime_type<double>();
    alignas(double) std::byte storage[sizeof(double)];

    runtime_type.construct_initial_value(storage);

    CHECK(*reinterpret_cast<const double *>(storage) == 0.0);
}

TEST_CASE("evaluation time storage supports byte-copy initialization",
          "[scalar][feature:scalar-storage][scalar:evaluation-time]") {
    const auto &runtime_type = cpp_scalar_runtime_type<EvaluationTime>();
    alignas(EvaluationTime) std::byte storage[sizeof(EvaluationTime)];

    REQUIRE(runtime_type.supports_byte_copy_initialization());
    runtime_type.construct_initial_value(storage);

    CHECK(*reinterpret_cast<const EvaluationTime *>(storage) == MIN_DT);
}

TEST_CASE("resolved collection storage prefers a registered concrete runtime_type",
          "[scalar][storage][collection][feature:scalar-storage]") {
    const auto &concrete = cpp_scalar_runtime_type<Dict<i64, string>>();

    const auto &resolved = resolve_scalar_runtime_type(concrete.type(), {});

    CHECK(&resolved == &concrete);
}

TEST_CASE("scalar runtime type boundaries reject an incompatible collection interface",
          "[scalar][storage][collection][feature:scalar-storage][declaration:runtime]") {
    const auto &type = cpp_scalar_type<List<i64>>();
    const auto invalid = InvalidListRuntimeType{type};
    const auto available = std::array<const ScalarRuntimeType *, 1>{&invalid};

    CHECK_THROWS_AS(register_scalar_runtime_type(invalid), std::invalid_argument);
    CHECK_THROWS_AS(resolve_scalar_runtime_type(type, available), std::invalid_argument);
}
