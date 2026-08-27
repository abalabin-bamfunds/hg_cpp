#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/list_storage_representation.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/time_series/input_view.hpp>
#include <bg/time_series/output_view.hpp>
#include <bg/type/scalar_type.hpp>

#include <exception>
#include <memory>
#include <type_traits>

namespace bg::details {

template <typename T, typename Base> class CppScalarStorageRepresentationBase : public Base {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;

    static_assert(std::is_nothrow_destructible_v<Stored>);

  public:
    [[nodiscard]] const ScalarType &type() const noexcept override { return cpp_scalar_type<T>(); }

    [[nodiscard]] std::size_t size() const noexcept override { return sizeof(Stored); }

    [[nodiscard]] std::size_t alignment() const noexcept override { return alignof(Stored); }

    [[nodiscard]] bool uses_variable_memory() const noexcept override {
        return std::uses_allocator_v<Stored, std::pmr::polymorphic_allocator<Stored>>;
    }

    [[nodiscard]] bool requires_destruction() const noexcept override {
        return !std::is_trivially_destructible_v<Stored>;
    }

    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override {
        return std::is_trivially_copyable_v<Stored> && std::is_default_constructible_v<Stored>;
    }

    [[nodiscard]] const ScalarValueInputView &scalar_input_view_prototype() const noexcept override {
        using View = util::polymorphic_value<ScalarValueInputView>;
        static const auto prototype = View::template make<BorrowedScalarValue<T>>();

        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesValueInputView &input_view_prototype() const noexcept override {
        using View = util::polymorphic_value<TimeSeriesValueInputView>;
        static const auto prototype = View::template make<TypedTimeSeriesValueInputView<T>>();

        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesValueOutputView &output_view_prototype() const noexcept override {
        using View = util::polymorphic_value<TimeSeriesValueOutputView>;
        static const auto prototype = View::template make<TypedTimeSeriesValueOutputView<T>>();

        return prototype.get();
    }

    void construct_initial_value(void *destination) const noexcept override {
        if constexpr (std::is_trivially_copyable_v<Stored> && std::is_default_constructible_v<Stored>) {
            std::construct_at(static_cast<Stored *>(destination), Stored{});
        } else {
            std::terminate();
        }
    }

    void construct_from_value(void *destination, const void *value, std::pmr::memory_resource &memory) const override {
        Traits::construct(static_cast<Stored *>(destination), *static_cast<const Value *>(value), memory);
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        std::uninitialized_construct_using_allocator(static_cast<Stored *>(destination),
                                                     std::pmr::polymorphic_allocator<Stored>{&memory},
                                                     *static_cast<const Stored *>(source));
    }

    void assign_from_value(void *destination, const void *value) const override {
        if constexpr (requires(Stored &target, const Value &source) { Traits::assign(target, source); }) {
            Traits::assign(*static_cast<Stored *>(destination), *static_cast<const Value *>(value));
        } else {
            std::terminate();
        }
    }

    void destroy(void *value, std::pmr::memory_resource &) const noexcept override {
        std::destroy_at(static_cast<Stored *>(value));
    }
};

template <typename T>
class CppScalarStorageRepresentation final : public CppScalarStorageRepresentationBase<T, ScalarStorageRepresentation> {
};

template <typename T>
class CppScalarStorageRepresentation<Opaque<T>> final
    : public CppScalarStorageRepresentationBase<Opaque<T>, ScalarStorageRepresentation> {
  public:
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return false; }

    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return false; }

    void construct_initial_value(void *) const noexcept override { std::terminate(); }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &) const override {
        std::construct_at(static_cast<T *>(destination), *static_cast<const T *>(source));
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }
};

template <typename T>
class CppScalarStorageRepresentation<List<T>> final
    : public CppScalarStorageRepresentationBase<List<T>, ScalarListStorageRepresentation> {
  public:
    [[nodiscard]] const ScalarStorageRepresentation &item_storage_representation() const noexcept override {
        return cpp_scalar_storage_representation<T>();
    }
};

} // namespace bg::details

namespace bg {

template <typename T> const ScalarStorageRepresentation &cpp_scalar_storage_representation() noexcept {
    static const details::CppScalarStorageRepresentation<T> representation;

    return representation;
}

template <typename T> void TypedTimeSeriesValueOutputView<T>::set_value(const ScalarInputView &value) const {
    value.assign(mutable_value(), cpp_scalar_storage_representation<T>());
    publish();
}

} // namespace bg
