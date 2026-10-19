#include <bg/literal/sentinel.hpp>

#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

namespace bg {
namespace {

class LiteralScalarType final : public ScalarType {
  public:
    explicit LiteralScalarType(std::string_view name) noexcept : name_{name} {}
    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += name_; }

  private:
    std::string_view name_;
};

} // namespace

const ScalarType &scalar_type_traits<literal::Sentinel>::type() noexcept {
    static const auto type = util::static_storage<LiteralScalarType>{"literal::Sentinel"};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

const ScalarType &scalar_type_traits<literal::TaggedLiteral>::type() noexcept {
    static const auto type = util::static_storage<LiteralScalarType>{"literal::TaggedLiteral"};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

} // namespace bg

namespace literal {

const Sentinel SKIP{bg::symbol{"bg.skip"}};
const Sentinel INVALID{bg::symbol{"bg.invalid"}};
const Sentinel REMOVE{bg::symbol{"bg.remove"}};
const Sentinel REMOVED{bg::symbol{"bg.removed"}};

const Sentinel *Sentinel::from(const bg::ScalarData &value) noexcept {
    if (value.empty() || &value.type() != &bg::cpp_scalar_type<Sentinel>()) {
        return nullptr;
    }

    const auto erased = bg::Scalar<>::from(value);
    return std::addressof(bg::Scalar<Sentinel>::from(erased).value());
}

const Sentinel *Sentinel::from(const ScalarLiteralItem &value) {
    return value.visit_scalar([](const auto *scalar) {
        return from(bg::details::scalar_data(*scalar));
    }).value();
}

const Sentinel *Sentinel::from(const ScalarLiteral &value) {
    return value.visit(
                       [](const ScalarLiteralItem &item) { return from(item); },
                       [](std::span<const ScalarLiteral>) -> const Sentinel * { return nullptr; })
        .value();
}

std::optional<TaggedLiteral> TaggedLiteral::from(const bg::ScalarData &value) {
    if (value.empty() || &value.type() != &bg::cpp_scalar_type<TaggedLiteral>()) {
        return std::nullopt;
    }

    const auto erased = bg::Scalar<>::from(value);
    return bg::Scalar<TaggedLiteral>::from(erased).value();
}

std::optional<TaggedLiteral> TaggedLiteral::from(const ScalarLiteralItem &value) {
    std::optional<TaggedLiteral> result;
    value.visit_scalar([&](const auto *scalar) {
        const auto source = bg::details::scalar_data(*scalar);
        if (auto tagged = from(source)) {
            // DDV assigns visitor results, but TaggedLiteral is not move-assignable.
            result.emplace(*tagged);
        }
    });
    return result;
}

std::optional<TaggedLiteral> TaggedLiteral::from(const ScalarLiteral &value) {
    return value.visit(
        [](const ScalarLiteralItem &item) { return from(item); },
        [](std::span<const ScalarLiteral>) -> std::optional<TaggedLiteral> { return std::nullopt; });
}

} // namespace literal
