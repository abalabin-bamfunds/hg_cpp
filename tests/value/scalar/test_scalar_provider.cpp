#include "test_scalar_provider.hpp"

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>

namespace test_scalar_provider {
namespace {

class ExternalScalarType final : public bg::ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override { return true; }
};

} // namespace

const bg::ScalarType &semantic_type_from_provider() noexcept {
    static const ExternalScalarType type;
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type);

    return type;
}

const bg::ScalarRuntimeType &runtime_type_from_provider() {
    return bg::cpp_scalar_runtime_type<value>();
}

const bg::ScalarType &compound_semantic_type_from_provider() {
    return bg::cpp_scalar_type<compound_value>();
}

} // namespace test_scalar_provider

namespace bg {

const ScalarType &scalar_type_traits<test_scalar_provider::value>::type() noexcept {
    return test_scalar_provider::semantic_type_from_provider();
}

template const ScalarRuntimeType &cpp_scalar_runtime_type<test_scalar_provider::value>();

} // namespace bg
