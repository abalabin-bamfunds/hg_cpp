#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>

namespace bg {
namespace {

class FloatType final : public ScalarType {
  public:
    [[nodiscard]] bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] bool match(const Type &supplied, TypeMap &) const override {
        return this == &supplied;
    }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

const ScalarType &float_type() noexcept {
    static const FloatType type;

    return type;
}

} // namespace bg
