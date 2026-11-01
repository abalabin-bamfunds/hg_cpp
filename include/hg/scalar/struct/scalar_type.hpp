#pragma once

#include <bg/scalar/declaration.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/struct/declaration.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/symbol.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace bg {

struct StructScalarField {
    util::symbol name;
    const ScalarType *type;
};

class StructScalarType : public ScalarType {
  public:
    using generic_scalar_view = Scalar<STRUCT>;

    VISITOR_SUPPORT()

    void format_name(std::string &result) const override { result += name_.text(); }
    util::symbol full_name() const noexcept { return full_name_; }
    util::symbol name() const noexcept { return name_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t alignment() const noexcept { return alignment_; }
    std::span<const StructScalarField> fields() const noexcept { return fields_; }
    std::size_t field_count() const noexcept { return fields_.size(); }
    const StructScalarField &field(std::size_t index) const { return fields_[index]; }

    bool supports_equality() const noexcept final { return true; }
    bool supports_hash() const noexcept final { return true; }

  protected:
    StructScalarType(util::symbol full_name, util::symbol name, std::size_t size, std::size_t alignment,
                     std::vector<StructScalarField> fields)
        : full_name_{full_name}, name_{name}, size_{size}, alignment_{alignment}, fields_{std::move(fields)} {}

  private:
    util::symbol full_name_;
    util::symbol name_;
    std::size_t size_;
    std::size_t alignment_;
    std::vector<StructScalarField> fields_;
};

namespace details {

const StructScalarType &canonical_struct_scalar_type(const std::type_info &identity, std::string_view full_name,
                                                     std::string_view name, std::size_t size, std::size_t alignment,
                                                     std::span<const StructScalarField> fields);

inline bool is_struct_scalar(const ScalarType &type) noexcept {
    return type_cast<StructScalarType>(&type) != nullptr;
}

template <native_struct T, std::size_t... Index>
auto native_struct_fields(std::index_sequence<Index...>) {
    return std::array<StructScalarField, sizeof...(Index)>{StructScalarField{
        util::symbol{util::aggregate_member_names<T>[Index]},
        &cpp_scalar_type<std::remove_cv_t<util::aggregate_member_type<Index, T>>>()}...};
}

} // namespace details

template <native_struct T> struct scalar_type_traits<T> {
    static const ScalarType &type() {
        static const auto fields = details::native_struct_fields<T>(
            std::make_index_sequence<util::aggregate_member_count<T>>{});

        const auto &type = details::canonical_struct_scalar_type(typeid(T), util::type_name<T>(),
                                                                 util::unqualified_type_name<T>(), sizeof(T),
                                                                 alignof(T), fields);
        type.set_default_runtime_type_factory(&details::cpp_scalar_runtime_type_for_type<T>);
        return type;
    }
};

} // namespace bg

#include <bg/scalar/cpp_runtime_type.hpp>
