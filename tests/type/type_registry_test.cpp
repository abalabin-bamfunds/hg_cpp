#include <bg/graph/graph_argument.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <span>
#include <thread>
#include <vector>

using namespace bg;

static_assert(noexcept(int_type()));
static_assert(noexcept(TypeRegistry::register_static(int_type())));

namespace {

class RegistryTestType final : public ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }
};

const RegistryTestType &concurrent_registration_type() {
    static const auto type = util::static_storage<RegistryTestType>{};
    return type.get();
}

} // namespace

TEST_CASE("completed static type registrations support concurrent reads", "[type_system][feature:type-system]") {
    constexpr auto thread_count = std::size_t{16};
    auto all_registered = std::atomic<bool>{true};
    auto threads = std::vector<std::thread>{};
    threads.reserve(thread_count);

    const auto &type = concurrent_registration_type();
    const auto &registered = TypeRegistry::register_static(type);
    CHECK(&registered == &type);
    for (auto index = std::size_t{}; index < thread_count; ++index) {
        threads.emplace_back([&] {
            if (!TypeRegistry::instance().contains(type) || !type.is_resolved()) {
                all_registered.store(false, std::memory_order_release);
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    CHECK(all_registered.load(std::memory_order_acquire));
}

TEST_CASE("registries retain dynamic declarations for process lifetime", "[type_system][feature:type-system]") {
    auto type = std::make_unique<RegistryTestType>();
    const auto *type_address = type.get();
    const auto &registered_type = TypeRegistry::instance().register_owned(std::move(type));

    CHECK(&registered_type == type_address);
    CHECK(TypeRegistry::instance().contains(registered_type));

    auto variable = std::make_unique<GuaranteeVariable>(util::symbol{"registered"});
    const auto *variable_address = variable.get();
    const auto &registered_variable = VariableRegistry::instance().register_owned(std::move(variable));

    CHECK(&registered_variable == variable_address);
    CHECK(VariableRegistry::instance().contains(registered_variable));
}

TEST_CASE("type registry caches factory descriptors by family and key", "[type_system][feature:type-system]") {
    auto factory_calls = std::size_t{};
    const auto factory = [&] {
        ++factory_calls;
        return std::make_unique<RegistryTestType>();
    };
    auto &registry = TypeRegistry::instance();

    const auto &first = registry.get_or_create<RegistryTestType, std::hash<int>>(7101, factory);
    const auto &same_key = registry.get_or_create<RegistryTestType, std::hash<int>>(7101, factory);
    const auto &other_key = registry.get_or_create<RegistryTestType, std::hash<int>>(7102, factory);

    CHECK(&first == &same_key);
    CHECK(&first != &other_key);
    CHECK(factory_calls == 2);
    CHECK(registry.contains(first));
    CHECK(registry.contains(other_key));
}

TEST_CASE("type registry propagates a factory failure", "[type_system][feature:type-system]") {
    auto &registry = TypeRegistry::instance();
    const auto failing_factory = []() -> std::unique_ptr<RegistryTestType> {
        throw std::runtime_error{"factory failed"};
    };

    CHECK_THROWS_AS((registry.get_or_create<RegistryTestType, std::hash<int>>(7103, failing_factory)),
                    std::runtime_error);
}

namespace {

class NestedRegistryTestType final : public ScalarType {
  public:
    explicit NestedRegistryTestType(const bg::Type *dependency = nullptr) : dependency_{dependency} {}

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }
    const bg::Type *dependency() const noexcept { return dependency_; }

  private:
    const bg::Type *dependency_;
};

} // namespace

TEST_CASE("type registry factories can request nested descriptors", "[type_system][feature:type-system]") {
    auto &registry = TypeRegistry::instance();
    const auto &outer = registry.get_or_create<NestedRegistryTestType, std::hash<int>>(7104, [&] {
        const auto &inner = registry.get_or_create<NestedRegistryTestType, std::hash<int>>(7105, [] {
            return std::make_unique<NestedRegistryTestType>();
        });
        const auto &other_family = registry.get_or_create<RegistryTestType, std::hash<int>>(7104, [] {
            return std::make_unique<RegistryTestType>();
        });
        CHECK(registry.contains(other_family));
        return std::make_unique<NestedRegistryTestType>(&inner);
    });
    const auto &inner = registry.get_or_create<NestedRegistryTestType, std::hash<int>>(7105, [] {
        return std::make_unique<NestedRegistryTestType>();
    });

    CHECK(outer.dependency() == &inner);
    CHECK(registry.contains(outer));
    CHECK(registry.contains(inner));
}

TEST_CASE("type registry returns a completed descriptor to concurrent readers", "[type_system][feature:type-system]") {
    constexpr auto thread_count = std::size_t{8};
    auto results = std::array<const RegistryTestType *, thread_count>{};
    auto threads = std::vector<std::thread>{};
    threads.reserve(thread_count);
    auto &registry = TypeRegistry::instance();
    const auto &registered = registry.get_or_create<RegistryTestType, std::hash<int>>(7106, [] {
        return std::make_unique<RegistryTestType>();
    });

    for (auto index = std::size_t{}; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            results[index] = &registry.get_or_create<RegistryTestType, std::hash<int>>(7106, [] {
                return std::make_unique<RegistryTestType>();
            });
            (void)results[index]->is_resolved();
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    CHECK(std::ranges::all_of(results, [&](const auto *result) { return result == results.front(); }));
    CHECK(results.front() == &registered);
}

TEST_CASE("type variable declarations participate in the type registry", "[type_system][feature:type-system]") {
    using Declaration = TypeVariable<"registered", i64>;
    const auto &type = Declaration::type();

    CHECK(TypeRegistry::instance().contains(type));
    CHECK_FALSE(VariableRegistry::instance().contains(type));
}

namespace {

const GuaranteeVariable &registered_static_variable() {
    static const auto variable = util::static_storage<GuaranteeVariable>{util::symbol{"static guarantee"}};
    [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(variable.get());
    return variable.get();
}

} // namespace

TEST_CASE("static variable registration exposes process-static metadata", "[type_system][feature:type-system]") {
    CHECK(VariableRegistry::instance().contains(registered_static_variable()));
}

TEST_CASE("registry membership covers primitive, generic, composed, graph, and schema types",
          "[type_system][feature:type-system]") {
    struct OpaqueValue {};
    using GENERIC_TIME_SERIES = TS<SCALAR>;

    CHECK(TypeRegistry::instance().contains(int_type()));
    CHECK(TypeRegistry::instance().contains(cpp_scalar_type<Opaque<OpaqueValue>>()));
    CHECK(TypeRegistry::instance().contains(GENERIC_TIME_SERIES::type()));

    const auto &list = list_scalar_type(int_type());
    const auto &time_series = time_series_value_type(float_type(), TimeSeriesGuarantee::ordinary);
    CHECK(TypeRegistry::instance().contains(list));
    CHECK(TypeRegistry::instance().contains(time_series));

    const auto parameters = std::array<const bg::Type *, 1>{&int_type()};
    const auto &graph = graph_argument_type(parameters, nullptr);
    CHECK(TypeRegistry::instance().contains(graph));
    CHECK(TypeRegistry::instance().contains(GraphArgument::type()));

    const auto &schema =
        bg::schema(util::symbol{"RegistrySchema"}, std::monostate{}, nullptr, std::span<const SchemaField>{});
    CHECK(TypeRegistry::instance().contains(schema));
}

TEST_CASE("same-name metadata variables retain separate identity", "[type_system][feature:type-system]") {
    auto left_owner = std::make_unique<GuaranteeVariable>(util::symbol{"same"});
    auto right_owner = std::make_unique<GuaranteeVariable>(util::symbol{"same"});
    const auto &left = VariableRegistry::instance().register_owned(std::move(left_owner));
    const auto &right = VariableRegistry::instance().register_owned(std::move(right_owner));
    auto type_map = TypeMap{};
    type_map.set(left, TimeSeriesGuarantee::final);
    type_map.set(right, TimeSeriesGuarantee::ordinary);

    CHECK(left.resolve(type_map) == TimeSeriesGuarantee::final);
    CHECK(right.resolve(type_map) == TimeSeriesGuarantee::ordinary);
}

TEST_CASE("same-name type variables retain separate identity", "[type_system][feature:type-system]") {
    auto left_owner = std::make_unique<TypeVariableType<ScalarType>>(
        util::symbol{"same"}, std::vector<const ScalarType *>{&int64_type(), &float_type()});
    auto right_owner = std::make_unique<TypeVariableType<ScalarType>>(
        util::symbol{"same"}, std::vector<const ScalarType *>{&int64_type(), &float_type()});
    const auto &left = TypeRegistry::instance().register_owned(std::move(left_owner));
    const auto &right = TypeRegistry::instance().register_owned(std::move(right_owner));
    auto type_map = TypeMap{};
    type_map.set(left, &int64_type());
    type_map.set(right, &float_type());

    CHECK(&left != &right);
    CHECK(&left.resolve(type_map) == &int64_type());
    CHECK(&right.resolve(type_map) == &float_type());
}
