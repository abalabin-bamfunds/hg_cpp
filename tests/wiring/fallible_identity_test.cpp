#include <hg/graph/details/graph_wiring_structure.hpp>
#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/node.hpp>
#include <hg/node/node_template_identity.hpp>
#include <hg/scalar/value.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace test_identity {

inline bool throw_on_hash{};
inline bool throw_on_equality{};
inline bool throw_on_cacheability{};

struct fallible_value {
    explicit fallible_value(hg::i64 value) : value{value} {}

    hg::i64 value;

    std::size_t hash() const {
        if (throw_on_hash) {
            throw std::runtime_error{"hash failed"};
        }
        return std::hash<hg::i64>{}(value);
    }

    bool cacheable() const {
        if (throw_on_cacheability) {
            throw std::runtime_error{"cacheability failed"};
        }
        return true;
    }

    friend bool operator==(const fallible_value &left, const fallible_value &right) {
        if (throw_on_equality) {
            throw std::runtime_error{"equality failed"};
        }
        return left.value == right.value;
    }
};

} // namespace test_identity

namespace std {

template <> struct hash<test_identity::fallible_value> {
    std::size_t operator()(const test_identity::fallible_value &value) const {
        if (test_identity::throw_on_hash) {
            throw std::runtime_error{"hash failed"};
        }
        return std::hash<hg::i64>{}(value.value);
    }
};

} // namespace std

namespace hg {

template <> struct scalar_type_traits<test_identity::fallible_value> {
    static const ScalarType &type() noexcept {
        class Type final : public ScalarType {
          public:
            bool is_resolved() const noexcept override { return true; }
            const hg::Type &resolve(const TypeMap &) const override { return *this; }
            bool supports_equality() const noexcept override { return true; }
            bool supports_hash() const noexcept override { return true; }
        };

        static const auto type = util::static_storage<Type>{};
        [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
        return type.get();
    }
};

} // namespace hg

namespace {

struct ScalarSource {
    hg::Scalar<test_identity::fallible_value> value;
    hg::Out<hg::TS<hg::i64>> output;

    void evaluate() const noexcept {}
};

const auto scalar_source = hg::node<ScalarSource>();

using ScalarGraph = std::pair<std::unique_ptr<hg::GraphWiring>, hg::WiringPoint>;

ScalarGraph make_scalar_graph() {
    auto wiring = std::make_unique<hg::GraphWiring>();
    const auto context = hg::WiringContext{*wiring};
    const auto value = hg::ScalarValue<test_identity::fallible_value>{test_identity::fallible_value{1}};
    const auto output = scalar_source(value);
    (void)wiring->complete_pending_wiring(output);
    return {std::move(wiring), output};
}

hg::details::GraphWiringStructure structure(const ScalarGraph &graph) {
    return hg::details::graph_wiring_structure(*graph.first, std::span<const hg::WiringPoint>{}, graph.second);
}

} // namespace

TEST_CASE("node template identity propagates operation exceptions", "[feature:wiring][wiring][identity][scalar]") {
    using Value = test_identity::fallible_value;

    const auto left = hg::NodeTemplateIdentity::from_value(Value{1});
    const auto right = hg::NodeTemplateIdentity::from_value(Value{1});

    test_identity::throw_on_hash = true;
    CHECK_THROWS_AS(left.hash(), std::runtime_error);
    test_identity::throw_on_hash = false;

    test_identity::throw_on_equality = true;
    CHECK_THROWS_AS(left == right, std::runtime_error);
    test_identity::throw_on_equality = false;

    test_identity::throw_on_cacheability = true;
    CHECK_THROWS_AS(left.cacheable(), std::runtime_error);
    test_identity::throw_on_cacheability = false;
}

TEST_CASE("graph structure propagates scalar hash and equality exceptions", "[feature:wiring][wiring][identity][scalar]") {
    auto graph = make_scalar_graph();
    test_identity::throw_on_hash = true;
    CHECK_THROWS_AS(structure(graph), std::runtime_error);
    test_identity::throw_on_hash = false;

    const auto left_graph = make_scalar_graph();
    const auto right_graph = make_scalar_graph();
    const auto left = structure(left_graph);
    const auto right = structure(right_graph);

    REQUIRE(left.hash() == right.hash());
    test_identity::throw_on_equality = true;
    CHECK_THROWS_AS(left == right, std::runtime_error);
    test_identity::throw_on_equality = false;
}
