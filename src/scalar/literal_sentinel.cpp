#include <bg/literal/sentinel.hpp>

#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/type.hpp>
#include <bg/type/type_registry.hpp>

namespace bg {
namespace {

class SentinelScalarType final : public ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "literal::Sentinel"; }
};

} // namespace

const ScalarType &scalar_type_traits<literal::Sentinel>::type() noexcept {
    static const SentinelScalarType type;
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
    return type;
}

} // namespace bg

namespace literal {

const Sentinel SKIP{bg::symbol{"bg.skip"}};
const Sentinel INVALID{bg::symbol{"bg.invalid"}};
const Sentinel REMOVE{bg::symbol{"bg.remove"}};
const Sentinel REMOVED{bg::symbol{"bg.removed"}};

std::optional<Sentinel> Sentinel::from(const bg::ScalarData &value) {
    if (value.empty() || &value.type() != &bg::cpp_scalar_type<Sentinel>()) {
        return std::nullopt;
    }

    auto scalar = bg::Scalar<>::from(value);
    return bg::Scalar<Sentinel>::from(scalar).value();
}

std::optional<Sentinel> Sentinel::from(const ScalarLiteralItem &value) {
    return value.visit_scalar([](const auto *source) {
        return from(bg::details::scalar_data(*source));
    });
}

std::optional<Sentinel> Sentinel::from(const ScalarLiteral &value) {
    return value.visit(
        [](const ScalarLiteralItem &item) { return from(item); },
        [](std::span<const ScalarLiteral>) { return std::optional<Sentinel>{}; });
}

} // namespace literal
