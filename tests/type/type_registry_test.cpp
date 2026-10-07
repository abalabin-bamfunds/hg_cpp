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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <span>
#include <thread>
#include <vector>

static_assert(noexcept(bg::int_type()));
static_assert(noexcept(bg::TypeRegistry::register_static(bg::int_type())));

namespace {

class RegistryTestType final : public bg::ScalarType {
  public:
    explicit RegistryTestType(std::atomic_size_t *destructions = nullptr) : destructions_{destructions} {}
    ~RegistryTestType() override {
        if (destructions_ != nullptr) {
            ++*destructions_;
        }
    }

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }

  private:
    std::atomic_size_t *destructions_;
};

class NestedRegistryTestType final : public bg::ScalarType {
  public:
    explicit NestedRegistryTestType(const bg::Type *dependency = nullptr) : dependency_{dependency} {}

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    const bg::Type *dependency() const noexcept { return dependency_; }

  private:
    const bg::Type *dependency_;
};

std::atomic_size_t cached_candidate_destructions{};

const RegistryTestType &concurrent_registration_type() {
    static const RegistryTestType type;
    return type;
}

const bg::GuaranteeVariable &registered_static_variable() {
    static const bg::GuaranteeVariable variable{bg::util::symbol{"static guarantee"}};
    [[maybe_unused]] static const auto &registration = bg::VariableRegistry::register_static(variable);
    return variable;
}

} // namespace

TEST_CASE("static type registration publishes before returning", "[type_system][feature:type-system]") {
    constexpr auto thread_count = std::size_t{16};
    auto start = std::atomic<bool>{};
    auto all_registered = std::atomic<bool>{true};
    auto threads = std::vector<std::thread>{};
    threads.reserve(thread_count);

    const auto &type = concurrent_registration_type();
    for (auto index = std::size_t{}; index < thread_count; ++index) {
        threads.emplace_back([&] {
            start.wait(false, std::memory_order_acquire);
            const auto &registered = bg::TypeRegistry::register_static(type);
            if (&registered != &type || !bg::TypeRegistry::instance().contains(type)) {
                all_registered.store(false, std::memory_order_release);
            }
        });
    }

    start.store(true, std::memory_order_release);
    start.notify_all();
    for (auto &thread : threads) {
        thread.join();
    }

    CHECK(all_registered.load(std::memory_order_acquire));
}

TEST_CASE("registries own dynamic declarations and roll them back", "[type_system][feature:type-system]") {
    auto destructions = std::atomic_size_t{};
    auto type = std::make_unique<RegistryTestType>(&destructions);
    const auto *type_address = type.get();
    const auto &registered_type = bg::TypeRegistry::instance().register_owned(std::move(type));

    CHECK(&registered_type == type_address);
    CHECK(bg::TypeRegistry::instance().contains(registered_type));
    bg::TypeRegistry::instance().rollback(registered_type);
    CHECK(destructions.load(std::memory_order_relaxed) == 1);

    auto variable = std::make_unique<bg::GuaranteeVariable>(bg::util::symbol{"registered"});
    const auto *variable_address = variable.get();
    const auto &registered_variable = bg::VariableRegistry::instance().register_owned(std::move(variable));

    CHECK(&registered_variable == variable_address);
    CHECK(bg::VariableRegistry::instance().contains(registered_variable));
    bg::VariableRegistry::instance().rollback(registered_variable);
}

TEST_CASE("type registry caches factory descriptors by family and key", "[type_system][feature:type-system]") {
    auto factory_calls = std::size_t{};
    const auto factory = [&] {
        ++factory_calls;
        return std::make_unique<RegistryTestType>();
    };
    auto &registry = bg::TypeRegistry::instance();

    const auto &first = registry.get_or_create<RegistryTestType, std::hash<int>>(7101, factory);
    const auto &same_key = registry.get_or_create<RegistryTestType, std::hash<int>>(7101, factory);
    const auto &other_key = registry.get_or_create<RegistryTestType, std::hash<int>>(7102, factory);

    CHECK(&first == &same_key);
    CHECK(&first != &other_key);
    CHECK(factory_calls == 2);
    CHECK(registry.contains(first));
    CHECK(registry.contains(other_key));
}

TEST_CASE("type registry retries a failed factory without retaining its key", "[type_system][feature:type-system]") {
    auto factory_calls = std::size_t{};
    auto &registry = bg::TypeRegistry::instance();
    const auto failing_factory = [&]() -> std::unique_ptr<RegistryTestType> {
        ++factory_calls;
        throw std::runtime_error{"factory failed"};
    };

    CHECK_THROWS_AS((registry.get_or_create<RegistryTestType, std::hash<int>>(7103, failing_factory)),
                    std::runtime_error);

    const auto &created = registry.get_or_create<RegistryTestType, std::hash<int>>(7103, [&] {
        ++factory_calls;
        return std::make_unique<RegistryTestType>();
    });
    const auto &reused = registry.get_or_create<RegistryTestType, std::hash<int>>(7103, failing_factory);

    CHECK(&created == &reused);
    CHECK(factory_calls == 2);
    CHECK(registry.contains(created));
}

TEST_CASE("type registry factories can request nested descriptors", "[type_system][feature:type-system]") {
    auto &registry = bg::TypeRegistry::instance();
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

TEST_CASE("type registry concurrent misses publish one owned descriptor", "[type_system][feature:type-system]") {
    constexpr auto thread_count = std::size_t{8};
    auto factory_gate = std::barrier{static_cast<std::ptrdiff_t>(thread_count)};
    auto factory_calls = std::atomic_size_t{};
    auto results = std::array<const RegistryTestType *, thread_count>{};
    auto threads = std::vector<std::thread>{};
    threads.reserve(thread_count);
    cached_candidate_destructions.store(0, std::memory_order_relaxed);
    auto &registry = bg::TypeRegistry::instance();

    for (auto index = std::size_t{}; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            results[index] = &registry.get_or_create<RegistryTestType, std::hash<int>>(7106, [&] {
                factory_calls.fetch_add(1, std::memory_order_relaxed);
                factory_gate.arrive_and_wait();
                return std::make_unique<RegistryTestType>(&cached_candidate_destructions);
            });
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }

    CHECK(factory_calls.load(std::memory_order_relaxed) == thread_count);
    CHECK(std::ranges::all_of(results, [&](const auto *result) { return result == results.front(); }));
    CHECK(cached_candidate_destructions.load(std::memory_order_relaxed) == thread_count - 1);
    CHECK(registry.contains(*results.front()));
}

TEST_CASE("type variable declarations participate in the type registry", "[type_system][feature:type-system]") {
    using Declaration = bg::TypeVariable<"registered", std::int64_t>;
    const auto &type = Declaration::type();

    CHECK(bg::TypeRegistry::instance().contains(type));
    CHECK_FALSE(bg::VariableRegistry::instance().contains(type));
}

TEST_CASE("static variable registration exposes process-static metadata", "[type_system][feature:type-system]") {
    CHECK(bg::VariableRegistry::instance().contains(registered_static_variable()));
}

TEST_CASE("registry membership covers primitive, generic, composed, graph, and schema types",
          "[type_system][feature:type-system]") {
    struct OpaqueValue {};
    using GENERIC_TIME_SERIES = bg::TS<bg::SCALAR>;

    CHECK(bg::TypeRegistry::instance().contains(bg::int_type()));
    CHECK(bg::TypeRegistry::instance().contains(bg::cpp_scalar_type<bg::Opaque<OpaqueValue>>()));
    CHECK(bg::TypeRegistry::instance().contains(GENERIC_TIME_SERIES::type()));

    const auto &list = bg::list_scalar_type(bg::int_type());
    const auto &time_series = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    CHECK(bg::TypeRegistry::instance().contains(list));
    CHECK(bg::TypeRegistry::instance().contains(time_series));

    const auto parameters = std::array<const bg::Type *, 1>{&bg::int_type()};
    const auto &graph = bg::graph_argument_type(parameters, nullptr);
    CHECK(bg::TypeRegistry::instance().contains(graph));
    CHECK(bg::TypeRegistry::instance().contains(bg::GraphArgument::type()));

    const auto &schema = bg::bundle_schema(bg::util::symbol{"RegistrySchema"}, nullptr, nullptr,
                                           std::span<const bg::BundleField>{});
    CHECK(bg::TypeRegistry::instance().contains(schema));
}

TEST_CASE("same-name metadata variables retain separate identity", "[type_system][feature:type-system]") {
    auto left_owner = std::make_unique<bg::GuaranteeVariable>(bg::util::symbol{"same"});
    auto right_owner = std::make_unique<bg::GuaranteeVariable>(bg::util::symbol{"same"});
    const auto &left = bg::VariableRegistry::instance().register_owned(std::move(left_owner));
    const auto &right = bg::VariableRegistry::instance().register_owned(std::move(right_owner));
    auto type_map = bg::TypeMap{};
    type_map.set(left, bg::TimeSeriesGuarantee::final);
    type_map.set(right, bg::TimeSeriesGuarantee::ordinary);

    CHECK(left.resolve(type_map) == bg::TimeSeriesGuarantee::final);
    CHECK(right.resolve(type_map) == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("same-name type variables retain separate identity", "[type_system][feature:type-system]") {
    auto left_owner = std::make_unique<bg::TypeVariableType<bg::ScalarType>>(
        bg::util::symbol{"same"}, std::vector<const bg::ScalarType *>{&bg::int64_type(), &bg::float_type()});
    auto right_owner = std::make_unique<bg::TypeVariableType<bg::ScalarType>>(
        bg::util::symbol{"same"}, std::vector<const bg::ScalarType *>{&bg::int64_type(), &bg::float_type()});
    const auto &left = bg::TypeRegistry::instance().register_owned(std::move(left_owner));
    const auto &right = bg::TypeRegistry::instance().register_owned(std::move(right_owner));
    auto type_map = bg::TypeMap{};
    type_map.set(left, &bg::int64_type());
    type_map.set(right, &bg::float_type());

    CHECK(&left != &right);
    CHECK(&left.resolve(type_map) == &bg::int64_type());
    CHECK(&right.resolve(type_map) == &bg::float_type());
}
