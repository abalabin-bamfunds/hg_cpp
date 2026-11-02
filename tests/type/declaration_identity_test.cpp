#include "test_shared_declarations_provider.hpp"

#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/graph/wiring_error.hpp>
#include <hg/node.hpp>
#include <hg/type/variable_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>

using namespace hg;
namespace shared = test_shared_declarations;

static_assert(noexcept(cpp_scalar_type<Opaque<shared::OpaquePayload>>()));
static_assert(noexcept(TypeRegistry::register_static(int_type())));

TEST_CASE("declaration descriptors are shared across dynamic libraries", "[feature:type-system]") {
    const auto provider = shared::provider_types();
    const auto local = shared::declaration_types();
    REQUIRE(provider.size() == local.size());
    for (auto index = std::size_t{}; index < local.size(); ++index) {
        CAPTURE(index);
        CHECK(local[index] == provider[index]);
        CHECK(TypeRegistry::instance().contains(*local[index]));
    }

    const auto local_variables = shared::declaration_variables();
    const auto provider_variables = shared::provider_variables();
    auto mappings = TypeMap{};
    mappings.set(*local_variables[0], &int_type());
    mappings.set(*local_variables[1], &TS<i64>::type());
    mappings.set(*local_variables[2], TimeSeriesGuarantee::final);
    mappings.set(*local_variables[3], Size{2});
    for (auto index = std::size_t{}; index < local_variables.size(); ++index) {
        CAPTURE(index);
        CHECK(local_variables[index] == provider_variables[index]);
        REQUIRE(mappings.find(*provider_variables[index]) != nullptr);
        CHECK(*mappings.find(*provider_variables[index]) == *mappings.find(*local_variables[index]));
    }

    const auto local_reverse = shared::reverse_variables();
    const auto provider_reverse = shared::provider_reverse_variables();
    mappings.set(*local_reverse[0], TimeSeriesGuarantee::ordinary);
    mappings.set(*local_reverse[1], Size{3});
    for (auto index = std::size_t{}; index < local_reverse.size(); ++index) {
        CHECK(local_reverse[index] == provider_reverse[index]);
        REQUIRE(mappings.find(*provider_reverse[index]) != nullptr);
    }
}

TEST_CASE("static declaration lookup runs only the first module factory", "[feature:type-system]") {
    const auto &provider_first = shared::provider_lazy_type(0);
    auto local_factory_calls = 0;
    const auto &local_second = TypeRegistry::instance().get_or_create_static<
        shared::LazyDeclaration<0>, shared::LazyType>([&]() -> const shared::LazyType & {
            ++local_factory_calls;
            static const auto storage = util::static_storage<shared::LazyType>{};
            return storage.get();
        });
    CHECK(&provider_first == &local_second);
    CHECK(shared::provider_factory_calls(0) == 1);
    CHECK(local_factory_calls == 0);

    const auto &local_first = TypeRegistry::instance().get_or_create_static<
        shared::LazyDeclaration<1>, shared::LazyType>([&]() -> const shared::LazyType & {
            ++local_factory_calls;
            static const auto storage = util::static_storage<shared::LazyType>{};
            return storage.get();
        });
    const auto &provider_second = shared::provider_lazy_type(1);
    CHECK(&local_first == &provider_second);
    CHECK(&local_first != &provider_first);
    CHECK(shared::provider_factory_calls(1) == 0);
    CHECK(local_factory_calls == 1);
}

TEST_CASE("equal display names preserve distinct declaration identities", "[feature:type-system]") {
    using INTEGER = ScalarVariable<"same_name", i64>;
    using FLOAT = ScalarVariable<"same_name", double>;
    using SERIES = TimeSeriesVariable<"same_name">;
    CHECK(INTEGER::type().name() == FLOAT::type().name());
    CHECK(&INTEGER::type() != &FLOAT::type());
    CHECK(static_cast<const Type *>(&INTEGER::type()) != &SERIES::type());

    constexpr auto ordinary = guarantee_variable<"same_name", TimeSeriesGuarantee::ordinary>;
    constexpr auto final = guarantee_variable<"same_name", TimeSeriesGuarantee::final>;
    const auto &ordinary_variable = details::guarantee_variable_instance<ordinary>();
    const auto &final_variable = details::guarantee_variable_instance<final>();
    CHECK(ordinary_variable.name() == final_variable.name());
    CHECK(&ordinary_variable != &final_variable);

    const auto &first_dynamic = VariableRegistry::instance().register_owned(
        std::make_unique<GuaranteeVariable>(util::symbol{"same_name"}));
    const auto &second_dynamic = VariableRegistry::instance().register_owned(
        std::make_unique<GuaranteeVariable>(util::symbol{"same_name"}));
    CHECK(&first_dynamic != &second_dynamic);
    CHECK(&first_dynamic != &ordinary_variable);
}

TEST_CASE("static declaration factories cannot reuse registered descriptors", "[feature:type-system]") {
    struct First {};
    struct Second {};
    auto &types = TypeRegistry::instance();
    const auto &first = types.get_or_create_static<First, shared::LazyType>([]() -> const shared::LazyType & {
        static const auto storage = util::static_storage<shared::LazyType>{};
        return storage.get();
    });
    CHECK_THROWS_AS((types.get_or_create_static<Second, shared::LazyType>(
                        [&]() -> const shared::LazyType & { return first; })), std::invalid_argument);
    const auto &second = types.get_or_create_static<Second, shared::LazyType>([]() -> const shared::LazyType & {
        static const auto storage = util::static_storage<shared::LazyType>{};
        return storage.get();
    });
    CHECK(&first != &second);

    auto &variables = VariableRegistry::instance();
    const auto &first_variable = variables.get_or_create_static<First, GuaranteeVariable>(
        []() -> const GuaranteeVariable & {
            static const auto storage = util::static_storage<GuaranteeVariable>{util::symbol{"reused"}};
            return storage.get();
        });
    CHECK_THROWS_AS((variables.get_or_create_static<Second, GuaranteeVariable>(
                        [&]() -> const GuaranteeVariable & { return first_variable; })), std::invalid_argument);
    const auto &second_variable = variables.get_or_create_static<Second, GuaranteeVariable>(
        []() -> const GuaranteeVariable & {
            static const auto storage = util::static_storage<GuaranteeVariable>{util::symbol{"reused"}};
            return storage.get();
        });
    CHECK(&first_variable != &second_variable);

    const auto &size_family = variables.get_or_create_static<First, SizeVariable>([]() -> const SizeVariable & {
        static const auto storage = util::static_storage<SizeVariable>{util::symbol{"reused"}};
        return storage.get();
    });
    CHECK(static_cast<const Variable *>(&first_variable) != &size_family);
}

TEST_CASE("imported operators consume explicit type arguments", "[feature:type-system][feature:operator]") {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto nothing = operators::nothing[type<TS<i64>>]();
    CHECK(&nothing.type() == &TS<i64, TimeSeriesGuarantee::final>::type());

    const auto source = node([](Out<TS<List<i64>>>) {});
    const auto converted = operators::convert[type<TSL<TS<i64>, 2>>](source());
    CHECK(&converted.type() == &TSL<TS<i64>, 2>::type());
}

namespace {

template <template <typename, auto...> typename Field> struct Pair {
    Field<TS<i64>> left;
};

} // namespace

TEST_CASE("imported selection operators reject unused explicit types", "[feature:type-system][feature:operator]") {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto list = node([](Out<TSL<TS<i64>, 2>>) {});
    const auto bundle = node([](Out<TSB<Pair>>) {});
    const auto check_unused = [](auto &&call) {
        try {
            call();
            FAIL("the unused explicit type argument was accepted");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic().has_value());
            CHECK(std::ranges::any_of(error.diagnostic()->candidates, [](const auto &candidate) {
                return candidate.message == "overload does not accept the explicit type argument";
            }));
        }
    };
    check_unused([&] { (void)operators::getitem[type<i64>](list(), i64{0}); });
    check_unused([&] { (void)operators::getitem[type<string>](list(), i64{0}); });
    check_unused([&] { (void)operators::getattr[type<i64>](bundle(), util::symbol{"left"}); });
    check_unused([&] { (void)operators::getattr[type<string>](bundle(), util::symbol{"left"}); });
}
