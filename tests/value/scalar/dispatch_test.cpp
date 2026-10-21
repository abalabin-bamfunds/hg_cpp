#include "test_scalar_provider.hpp"

#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

using namespace bg;

namespace {

class RecordingVisitor {
  public:
    struct Handler {
        std::function<void(RecordingVisitor &, Scalar<>)> visit;
    };

    template <typename ScalarType, typename Function> void add_type_handler(Function handler) {
        const auto added = handlers_.add(cpp_scalar_type<ScalarType>(),
                                         typed_handler<ScalarType>(std::move(handler)));
        if (!added) {
            throw std::logic_error{"duplicate test handler"};
        }
    }

    template <typename ScalarType, typename Function> void add_runtime_handler(Function handler) {
        const auto added = handlers_.add(cpp_scalar_runtime_type<ScalarType>(),
                                         typed_handler<ScalarType>(std::move(handler)));
        if (!added) {
            throw std::logic_error{"duplicate test handler"};
        }
    }

    void visit(Scalar<> value) {
        value.visit(
            [&](const Scalar<List<SCALAR>> *list) {
                ++structural_collections_;
                for (auto index = std::size_t{}; index < list->size(); ++index) {
                    const auto item = (*list)[index];
                    visit(Scalar<>::from(item));
                }
            },
            [&](const Scalar<Set<SCALAR>> *) { ++structural_collections_; },
            [&](const Scalar<Dict<SCALAR, SCALAR>> *) { ++structural_collections_; },
            [&](const Scalar<i64> *scalar) { record(scalar->value()); },
            [&](const Scalar<> *scalar) {
                ++extension_lookups_;
                const auto *handler = handlers_.find(scalar->runtime_type());
                if (handler == nullptr) {
                    throw std::logic_error{"test scalar has no handler"};
                }
                handler->visit(*this, *scalar);
            },
            ddv::noop);
    }

    void record(int value) { values_.push_back(value); }

    const std::vector<int> &values() const noexcept { return values_; }
    std::size_t structural_collections() const noexcept { return structural_collections_; }
    std::size_t extension_lookups() const noexcept { return extension_lookups_; }

  private:
    template <typename ScalarType, typename Function> static Handler typed_handler(Function handler) {
        return {[handler = std::move(handler)](RecordingVisitor &visitor, Scalar<> value) {
            std::invoke(handler, visitor, scalar_cast<ScalarType>(value));
        }};
    }

    ScalarDispatch<Handler> handlers_;
    std::vector<int> values_;
    std::size_t structural_collections_{};
    std::size_t extension_lookups_{};
};

void visit_external_type(RecordingVisitor &visitor, Scalar<test_scalar_provider::value> value) {
    visitor.record(-1);
    const auto nested = ScalarValue{static_cast<i64>(value.value().get())};
    const auto nested_view = nested.view();
    visitor.visit(Scalar<>::from(nested_view));
}

void visit_external_runtime(RecordingVisitor &visitor, Scalar<test_scalar_provider::value> value) {
    visitor.record(-2);
    visitor.record(value.value().get());
}

void reject_structural_collection(RecordingVisitor &, Scalar<List<test_scalar_provider::value>>) {
    throw std::logic_error{"a structural collection reached extension dispatch"};
}

} // namespace

TEST_CASE("scalar dispatch prefers an exact runtime type handler",
          "[feature:visitor][scalar:generic][declaration:runtime]") {
    auto visitor = RecordingVisitor{};
    visitor.add_type_handler<test_scalar_provider::value>(visit_external_type);
    visitor.add_runtime_handler<test_scalar_provider::value>(visit_external_runtime);
    const auto value = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};

    const auto view = value.view();
    visitor.visit(Scalar<>::from(view));

    CHECK(visitor.values() == std::vector<int>{-2, 42});
}

TEST_CASE("an external scalar provider exports stable type identities",
          "[feature:type-system][declaration:concrete]") {
    CHECK(&cpp_scalar_type<test_scalar_provider::value>() ==
          &test_scalar_provider::semantic_type_from_provider());
    CHECK(&cpp_scalar_runtime_type<test_scalar_provider::value>() ==
          &test_scalar_provider::runtime_type_from_provider());
}

TEST_CASE("an operation adapter delivers the concrete scalar view to an extension handler",
          "[feature:visitor][scalar:generic][declaration:runtime]") {
    auto visitor = RecordingVisitor{};
    visitor.add_type_handler<test_scalar_provider::value>(
        [](RecordingVisitor &target, Scalar<test_scalar_provider::value> value) {
            target.record(value.value().get());
        });
    const auto value = ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};

    visitor.visit(Scalar<>::from(value));

    CHECK(visitor.values() == std::vector<int>{42});
}

TEST_CASE("a recursive scalar visitor dispatches extension values from a structural list",
          "[feature:visitor][feature:scalar-collection][scalar:list][declaration:runtime]") {
    auto visitor = RecordingVisitor{};
    visitor.add_type_handler<test_scalar_provider::value>(visit_external_type);
    visitor.add_type_handler<List<test_scalar_provider::value>>(reject_structural_collection);
    const auto value = ScalarValue<List<test_scalar_provider::value>>{
        std::vector<test_scalar_provider::value>{{1}, {2}}};

    const auto view = value.view();
    visitor.visit(Scalar<>::from(view));

    CHECK(visitor.values() == std::vector<int>{-1, 1, -1, 2});
    CHECK(visitor.structural_collections() == 1);
    CHECK(visitor.extension_lookups() == 2);
}
