#include "test_scalar_provider.hpp"

#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

class RecordingVisitor {
  public:
    struct Handler {
        void (*visit)(RecordingVisitor &, bg::Scalar<>);
    };

    template <typename ScalarType>
    void add_type_handler(void (*handler)(RecordingVisitor &, bg::Scalar<>)) {
        const auto added = handlers_.add(bg::cpp_scalar_type<ScalarType>(), Handler{handler});
        if (!added) {
            throw std::logic_error{"duplicate test handler"};
        }
    }

    template <typename ScalarType>
    void add_runtime_handler(void (*handler)(RecordingVisitor &, bg::Scalar<>)) {
        const auto added = handlers_.add(bg::cpp_scalar_runtime_type<ScalarType>(), Handler{handler});
        if (!added) {
            throw std::logic_error{"duplicate test handler"};
        }
    }

    void visit(bg::Scalar<> value) {
        value.visit(
            [&](const bg::Scalar<bg::List<bg::SCALAR>> *list) {
                ++structural_collections_;
                for (auto index = std::size_t{}; index < list->size(); ++index) {
                    const auto item = (*list)[index];
                    visit(bg::Scalar<>::from(item));
                }
            },
            [&](const bg::Scalar<bg::Set<bg::SCALAR>> *) { ++structural_collections_; },
            [&](const bg::Scalar<bg::Dict<bg::SCALAR, bg::SCALAR>> *) { ++structural_collections_; },
            [&](const bg::Scalar<> *scalar) {
                ++extension_lookups_;
                const auto *handler = handlers_.find(scalar->runtime_type());
                if (handler == nullptr) {
                    throw std::logic_error{"test scalar has no handler"};
                }
                handler->visit(*this, *scalar);
            });
    }

    void record(int value) { values_.push_back(value); }

    const std::vector<int> &values() const noexcept { return values_; }
    std::size_t structural_collections() const noexcept { return structural_collections_; }
    std::size_t extension_lookups() const noexcept { return extension_lookups_; }

  private:
    bg::ScalarDispatch<Handler> handlers_;
    std::vector<int> values_;
    std::size_t structural_collections_{};
    std::size_t extension_lookups_{};
};

void visit_integer(RecordingVisitor &visitor, bg::Scalar<> value) {
    visitor.record(*bg::unchecked_scalar_cast<std::int64_t>(value));
}

void visit_external_type(RecordingVisitor &visitor, bg::Scalar<> value) {
    visitor.record(-1);
    const auto external = bg::unchecked_scalar_cast<test_scalar_provider::value>(value);
    const auto nested = bg::ScalarValue{static_cast<std::int64_t>(external->number)};
    const auto nested_view = nested.view();
    visitor.visit(bg::Scalar<>::from(nested_view));
}

void visit_external_runtime(RecordingVisitor &visitor, bg::Scalar<> value) {
    visitor.record(-2);
    const auto external = bg::unchecked_scalar_cast<test_scalar_provider::value>(value);
    visitor.record(external->number);
}

void reject_structural_collection(RecordingVisitor &, bg::Scalar<>) {
    throw std::logic_error{"a structural collection reached extension dispatch"};
}

} // namespace

TEST_CASE("an external scalar provider exports stable type identities",
          "[feature:type-system][declaration:concrete]") {
    CHECK(&bg::cpp_scalar_type<test_scalar_provider::value>() ==
          &test_scalar_provider::semantic_type_from_provider());
    CHECK(&bg::cpp_scalar_runtime_type<test_scalar_provider::value>() ==
          &test_scalar_provider::runtime_type_from_provider());
}

TEST_CASE("scalar dispatch prefers an exact runtime type handler",
          "[feature:visitor][scalar:generic][declaration:runtime]") {
    auto visitor = RecordingVisitor{};
    visitor.add_type_handler<test_scalar_provider::value>(visit_external_type);
    visitor.add_runtime_handler<test_scalar_provider::value>(visit_external_runtime);
    const auto value = bg::ScalarValue<test_scalar_provider::value>{test_scalar_provider::value{42}};

    const auto view = value.view();
    visitor.visit(bg::Scalar<>::from(view));

    CHECK(visitor.values() == std::vector<int>{-2, 42});
}

TEST_CASE("a recursive scalar visitor dispatches extension values from a structural list",
          "[feature:visitor][feature:scalar-collection][scalar:list][declaration:runtime]") {
    auto visitor = RecordingVisitor{};
    visitor.add_type_handler<std::int64_t>(visit_integer);
    visitor.add_type_handler<test_scalar_provider::value>(visit_external_type);
    visitor.add_type_handler<bg::List<test_scalar_provider::value>>(reject_structural_collection);
    const auto value = bg::ScalarValue<bg::List<test_scalar_provider::value>>{
        std::vector<test_scalar_provider::value>{{1}, {2}}};

    const auto view = value.view();
    visitor.visit(bg::Scalar<>::from(view));

    CHECK(visitor.values() == std::vector<int>{-1, 1, -1, 2});
    CHECK(visitor.structural_collections() == 1);
    CHECK(visitor.extension_lookups() == 4);
}
