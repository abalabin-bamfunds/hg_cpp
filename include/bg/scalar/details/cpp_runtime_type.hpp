#pragma once

#include <bg/scalar/bundle/storage_traits.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/struct/runtime_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/static_storage.hpp>

#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace bg::details {

template <typename T, typename Base> class CppScalarRuntimeTypeBase : public Base {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;
    using ReadValue = std::remove_cvref_t<Value>;

    static_assert(std::is_nothrow_destructible_v<Stored>);

  public:
    template <typename SemanticType, typename... Components>
    explicit CppScalarRuntimeTypeBase(const SemanticType &type, Components &&...components)
        : Base{type,
               sizeof(Stored),
               alignof(Stored),
               Traits::uses_variable_memory,
               Traits::requires_finalization,
               util::byte_copy_initializable<Stored>,
               std::forward<Components>(components)...} {}

    [[nodiscard]] bool copy_inline(ScalarDataMode mode, std::uintptr_t source,
                                   std::uintptr_t &destination) const noexcept override {
        if constexpr (inline_scalar_value<T>) {
            if (mode == ScalarDataMode::inline_value) {
                destination = source;
            } else {
                const auto value = scalar_data_value<T>(ScalarData{*this, source, mode});
                std::memcpy(std::addressof(destination), std::addressof(value), sizeof(Stored));
            }
            return true;
        } else {
            return false;
        }
    }

    void read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        if constexpr (std::constructible_from<ReadValue, Value>) {
            std::construct_at(static_cast<ReadValue *>(destination),
                              scalar_data_value<T>(ScalarData{*this, data, mode}));
        } else {
            Base::read_value(mode, data, destination);
        }
    }

    std::size_t hash(ScalarDataMode mode, std::uintptr_t data) const override {
        if constexpr (Traits::supports_hash) {
            return Traits::hash(scalar_data_value<T>(ScalarData{*this, data, mode}));
        } else {
            std::terminate();
        }
    }

    bool equals(ScalarDataMode mode, std::uintptr_t data,
                const details::ScalarData &other) const override {
        if constexpr (Traits::supports_equality) {
            return &this->type() == &other.type() &&
                   Traits::equals(scalar_data_value<T>(ScalarData{*this, data, mode}),
                                  scalar_data_value<T>(other));
        } else {
            std::terminate();
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        if constexpr (util::byte_copy_initializable<Stored>) {
            std::construct_at(static_cast<Stored *>(destination));
        } else {
            std::terminate();
        }
    }

    void construct_runtime_value(void *destination, allocator &memory) const override {
        if constexpr (std::default_initializable<ReadValue>) {
            Traits::construct(static_cast<Stored *>(destination), ReadValue{}, memory);
        } else {
            this->construct_initial_value(destination);
        }
    }

    void construct_from_value(void *destination, const void *value, allocator &memory) const override {
        Traits::construct(static_cast<Stored *>(destination), *static_cast<const ReadValue *>(value), memory);
    }

    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override {
        Traits::construct(static_cast<Stored *>(destination), scalar_data_value<T>(value), memory);
    }

    void copy_construct(void *destination, const void *source, allocator &memory) const override {
        if constexpr (requires {
                          Traits::copy_construct(static_cast<Stored *>(destination),
                                                 *static_cast<const Stored *>(source), memory);
                      }) {
            Traits::copy_construct(static_cast<Stored *>(destination), *static_cast<const Stored *>(source),
                                   memory);
        } else {
            std::uninitialized_construct_using_allocator(static_cast<Stored *>(destination),
                                                         std::pmr::polymorphic_allocator<Stored>{&memory.memory_resource()},
                                                         *static_cast<const Stored *>(source));
        }
    }

    void assign_from_value(void *destination, const void *value) const override {
        if constexpr (requires(Stored &target, const ReadValue &source, allocator &memory) {
                          Traits::assign(target, source, memory);
                      }) {
            auto memory = allocator{};
            if constexpr (Traits::assign_uses_memory_resource) {
                memory = current_scalar_allocator();
            }
            Traits::assign(*static_cast<Stored *>(destination), *static_cast<const ReadValue *>(value), memory);
        } else {
            std::terminate();
        }
    }

    void do_assign_from_input(void *destination, const details::ScalarData &value) const override {
        decltype(auto) current = scalar_data_value<T>(value);
        assign_from_value(destination, std::addressof(current));
    }

    std::size_t hash(const void *value) const override {
        if constexpr (Traits::supports_hash) {
            return Traits::hash(Traits::read(*static_cast<const Stored *>(value)));
        } else {
            std::terminate();
        }
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const override {
        if constexpr (Traits::supports_equality) {
            return Traits::equals(Traits::read(*static_cast<const Stored *>(value)), scalar_data_value<T>(other));
        } else {
            std::terminate();
        }
    }

    void destroy(void *value, allocator &memory) const noexcept override {
        Traits::destroy(static_cast<Stored *>(value), memory);
    }
};

template <typename T> class CppScalarRuntimeType final : public CppScalarRuntimeTypeBase<T, ScalarRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<T, ScalarRuntimeType>;

  public:
    CppScalarRuntimeType() : Base{cpp_scalar_type<T>()} {}
};

template <> class CppScalarRuntimeType<string> final : public CppScalarRuntimeTypeBase<string, ScalarRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<string, ScalarRuntimeType>;

  public:
    CppScalarRuntimeType();

  private:
    ScalarData borrow_shared(const void *storage) const noexcept override;
    const ScalarRuntimeType *retain_shared(const ScalarData &value, std::uintptr_t &data) const noexcept override;
    void release_shared(std::uintptr_t data) const noexcept override;
    void read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const override;
    void do_construct_from_input(void *destination, const ScalarData &value, allocator &memory) const override;
    void do_assign_from_input(void *destination, const ScalarData &value) const override;
};

template <> class CppScalarRuntimeType<bytes> final : public CppScalarRuntimeTypeBase<bytes, ScalarRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<bytes, ScalarRuntimeType>;

  public:
    CppScalarRuntimeType();

  private:
    ScalarData borrow_shared(const void *storage) const noexcept override;
    const ScalarRuntimeType *retain_shared(const ScalarData &value, std::uintptr_t &data) const noexcept override;
    void release_shared(std::uintptr_t data) const noexcept override;
    void read_value(ScalarDataMode mode, std::uintptr_t data, void *destination) const override;
    void do_construct_from_input(void *destination, const ScalarData &value, allocator &memory) const override;
    void do_assign_from_input(void *destination, const ScalarData &value) const override;
};

template <native_struct T>
class CppScalarRuntimeType<T> final : public CppScalarRuntimeTypeBase<T, StructScalarRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<T, StructScalarRuntimeType>;
    using FieldFunction = ScalarData (*)(const ScalarData &);

    template <typename Field> struct FieldStorage {
        alignas(Field) std::byte bytes[sizeof(Field)];

        void *storage() noexcept { return bytes; }
        Field *value() noexcept { return std::launder(reinterpret_cast<Field *>(bytes)); }
    };

    template <std::size_t Index>
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, T>>;

    template <std::size_t Index> static ScalarData field_value(const ScalarData &source) {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, T>>;

        if constexpr (inline_scalar_value<T>) {
            if (source.mode() == ScalarDataMode::inline_value) {
                const auto value = scalar_data_value<T>(source);
                const auto members = util::aggregate_members(value);
                const auto &field = std::get<Index>(members);
                static_assert(inline_scalar_value<Field>);

                auto data = std::uintptr_t{};
                std::memcpy(std::addressof(data), std::addressof(field), sizeof(Field));
                return {cpp_scalar_runtime_type<Field>(), data, ScalarDataMode::inline_value};
            }
        }

        const auto &value = *static_cast<const T *>(source.data());
        const auto members = util::aggregate_members(value);
        return {cpp_scalar_runtime_type<Field>(), std::addressof(std::get<Index>(members))};
    }

    template <std::size_t... Index>
    static constexpr auto field_functions(std::index_sequence<Index...>) {
        return std::array<FieldFunction, sizeof...(Index)>{&field_value<Index>...};
    }

  private:
    ScalarData field(const ScalarData &value, std::size_t index) const override {
        static constexpr auto functions =
            field_functions(std::make_index_sequence<util::aggregate_member_count<T>>{});
        if (index >= functions.size()) {
            throw std::out_of_range{"compound scalar field index"};
        }
        return functions[index](value);
    }

  public:
    CppScalarRuntimeType() : Base{static_cast<const StructScalarType &>(cpp_scalar_type<T>())} {
        static_assert(sizeof(CppScalarRuntimeType) == sizeof(StructScalarRuntimeType));
    }

    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override {
        auto memory = current_scalar_allocator();
        alignas(T) std::byte replacement_bytes[sizeof(T)];
        construction.construct(construction.context, *this, replacement_bytes, memory);
        auto *replacement = std::launder(reinterpret_cast<T *>(replacement_bytes));
        auto release_replacement = util::scope_exit{[&]() noexcept { this->destroy(replacement, memory); }};
        const auto source = this->borrow(replacement);

        if (suppress_equal && this->ScalarRuntimeType::equals(destination, source)) {
            return false;
        }

        this->assign_from_input(destination, source);
        return true;
    }

    void construct_fields_with(void *destination, allocator &memory,
                               ScalarFieldConstruction construction) const override {
        constexpr auto indices = std::make_index_sequence<util::aggregate_member_count<T>>{};
        construct_fields(static_cast<T *>(destination), memory, construction, indices);
    }

  private:
    template <std::size_t Index, typename Slots>
    void construct_staged_field(Slots &slots, std::array<bool, util::aggregate_member_count<T>> &constructed,
                                allocator &memory, ScalarFieldConstruction construction) const {
        const auto &runtime_type = cpp_scalar_runtime_type<Field<Index>>();
        auto *field = std::get<Index>(slots).storage();
        if (!construction.construct(construction.context, Index, runtime_type, field, memory)) {
            throw std::invalid_argument{"a required compound scalar field was not supplied"};
        }
        constructed[Index] = true;
    }

    template <std::size_t Index, typename Slots>
    void destroy_staged_field(Slots &slots, const std::array<bool, util::aggregate_member_count<T>> &constructed,
                              allocator &memory) const noexcept {
        if (constructed[Index]) {
            cpp_scalar_runtime_type<Field<Index>>().destroy(std::get<Index>(slots).value(), memory);
        }
    }

    template <std::size_t... Index>
    void construct_fields(T *destination, allocator &memory, ScalarFieldConstruction construction,
                          std::index_sequence<Index...>) const {
        auto slots = std::tuple<FieldStorage<Field<Index>>...>{};
        auto constructed = std::array<bool, sizeof...(Index)>{};
        auto destroy_slots = util::scope_exit{[&]() noexcept {
            (destroy_staged_field<Index>(slots, constructed, memory), ...);
        }};
        (construct_staged_field<Index>(slots, constructed, memory, construction), ...);
        std::construct_at(destination, *std::get<Index>(slots).value()...);
    }
};

template <typename T>
class CppScalarRuntimeType<Opaque<T>> final : public CppScalarRuntimeTypeBase<Opaque<T>, ScalarRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<Opaque<T>, ScalarRuntimeType>;

  public:
    CppScalarRuntimeType() : Base{cpp_scalar_type<Opaque<T>>()} {}

    void copy_construct(void *destination, const void *source, allocator &) const override {
        std::construct_at(static_cast<T *>(destination), *static_cast<const T *>(source));
    }
};

} // namespace bg::details

namespace bg {

template <typename T> const ScalarRuntimeType &cpp_scalar_runtime_type() {
    if constexpr (requires(const ScalarType &type) {
                      { scalar_storage_traits<T>::runtime_type(type) } -> std::same_as<const ScalarRuntimeType &>;
                  }) {
        static const auto &runtime_type = []() -> const ScalarRuntimeType & {
            using Stored = typename scalar_storage_traits<T>::stored_type;

            const auto &type = cpp_scalar_type<T>();
            const auto &runtime_type = scalar_storage_traits<T>::runtime_type(type);
            if (&runtime_type.type() != &type) {
                throw std::invalid_argument{"a scalar provider runtime must match its C++ scalar type"};
            }
            if (runtime_type.size() != sizeof(Stored) || runtime_type.alignment() != alignof(Stored)) {
                throw std::invalid_argument{"a scalar provider runtime must match its C++ storage layout"};
            }
            return runtime_type;
        }();
        static const auto registered = (register_scalar_runtime_type(runtime_type), true);
        (void)registered;

        return runtime_type;
    } else if constexpr (details::hierarchical_scalar_declaration<T> || is_tuple_scalar_v<T>) {
        return resolve_scalar_runtime_type(cpp_scalar_type<T>(), {});
    } else {
        static const auto runtime_type = util::static_storage<details::CppScalarRuntimeType<T>>{};
        static const auto registered = (register_scalar_runtime_type(runtime_type.get()), true);
        (void)registered;

        return runtime_type.get();
    }
}

extern template const ScalarRuntimeType &cpp_scalar_runtime_type<bool>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<byte>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<int>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<i64>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<float>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<double>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<date_time_t>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<timedelta_t>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<string>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<bytes>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<symbol>();

} // namespace bg
