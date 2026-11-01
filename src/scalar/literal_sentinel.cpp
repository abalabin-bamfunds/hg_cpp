#include <hg/literal/sentinel.hpp>

#include <hg/literal/scalar_literal.hpp>
#include <hg/scalar/scalar.hpp>
#include <hg/scalar/type.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/util/static_storage.hpp>

namespace hg {
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

} // namespace hg

namespace literal {

const Sentinel SKIP{hg::symbol{"hg.skip"}};
const Sentinel INVALID{hg::symbol{"hg.invalid"}};
const Sentinel REMOVE{hg::symbol{"hg.remove"}};
const Sentinel REMOVED{hg::symbol{"hg.removed"}};

const Sentinel *Sentinel::from(const hg::ScalarData &value) noexcept {
    if (value.empty() || &value.type() != &hg::cpp_scalar_type<Sentinel>()) {
        return nullptr;
    }

    const auto erased = hg::Scalar<>::from(value);
    return std::addressof(hg::Scalar<Sentinel>::from(erased).value());
}

const Sentinel *Sentinel::from(const ScalarLiteralItem &value) {
    return value.visit_scalar([](const auto *scalar) {
        return from(hg::details::scalar_data(*scalar));
    }).value();
}

const Sentinel *Sentinel::from(const ScalarLiteral &value) {
    return value.visit(
                       [](const ScalarLiteralItem &item) { return from(item); },
                       [](std::span<const ScalarLiteral>) -> const Sentinel * { return nullptr; })
        .value();
}

std::optional<TaggedLiteral> TaggedLiteral::from(const hg::ScalarData &value) {
    if (value.empty() || &value.type() != &hg::cpp_scalar_type<TaggedLiteral>()) {
        return std::nullopt;
    }

    const auto erased = hg::Scalar<>::from(value);
    return hg::Scalar<TaggedLiteral>::from(erased).value();
}

std::optional<TaggedLiteral> TaggedLiteral::from(const ScalarLiteralItem &value) {
    std::optional<TaggedLiteral> result;
    value.visit_scalar([&](const auto *scalar) {
        const auto source = hg::details::scalar_data(*scalar);
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
