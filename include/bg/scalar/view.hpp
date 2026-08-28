#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/type/scalar_type.hpp>

#include <ddv/visitable.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>

namespace bg {

template <typename T> class List;

class ScalarView {
  public:
    [[nodiscard]] const void *data() const noexcept { return reinterpret_cast<const void *>(value_); }

  protected:
    void bind(const void *value) noexcept { value_ = reinterpret_cast<std::uintptr_t>(value); }
    void bind(std::uintptr_t value) noexcept { value_ = value; }

    std::uintptr_t value_{};
};

class ScalarValueInputView;
class ScalarListInputView;
class ScalarSetInputView;
class ScalarDictInputView;
template <typename T> class TypedScalarValueInputView;

using ScalarInputViewTypes =
    ddv::mux<ScalarListInputView, ScalarSetInputView, ScalarDictInputView, ScalarValueInputView>;

class ScalarInputView : public ScalarView, public ddv::visitable<ScalarInputView, ScalarInputViewTypes> {
  public:
    void bind(const void *value) noexcept { ScalarView::bind(value); }
    void bind(std::uintptr_t value) noexcept { ScalarView::bind(value); }

    [[nodiscard]] virtual const ScalarType &type() const noexcept = 0;

    virtual void copy_construct(void *destination, const ScalarStorageRepresentation &representation,
                                std::pmr::memory_resource &memory) const = 0;
    virtual void assign(void *destination, const ScalarStorageRepresentation &representation) const = 0;
    virtual void copy_value(void *destination, std::pmr::memory_resource *memory) const = 0;
    virtual void release() noexcept = 0;

  protected:
    template <typename> friend class Scalar;

    [[nodiscard]] virtual std::size_t erased_hash() const noexcept = 0;
    [[nodiscard]] virtual bool erased_equals(const ScalarValueInputView &other) const noexcept = 0;
};

class ScalarValueInputView : public ScalarInputView {
  public:
    VISITOR_SUPPORT()

    [[nodiscard]] std::size_t hash() const noexcept { return erased_hash(); }
    [[nodiscard]] bool equals(const ScalarValueInputView &other) const noexcept { return erased_equals(other); }
};

template <typename T, typename Base> class TypedScalarValueInputViewBase : public Base {
    using Value = details::scalar_value_t<T>;

  public:
    [[nodiscard]] virtual Value value() const noexcept = 0;

    [[nodiscard]] const ScalarType &type() const noexcept final { return cpp_scalar_type<T>(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_storage_traits<T>::supports_hash
    {
        return details::scalar_storage_traits<T>::hash(value());
    }

    [[nodiscard]] bool equals(const TypedScalarValueInputViewBase &other) const noexcept
        requires details::scalar_storage_traits<T>::supports_equality
    {
        return value() == other.value();
    }

    void copy_construct(void *destination, const ScalarStorageRepresentation &representation,
                        std::pmr::memory_resource &memory) const final {
        require_compatible(representation);
        representation.construct_from_input(destination, *this, memory);
    }

    void assign(void *destination, const ScalarStorageRepresentation &representation) const final {
        require_compatible(representation);
        representation.assign_from_input(destination, *this);
    }

    void copy_value(void *destination, std::pmr::memory_resource *memory) const final;

  private:
    [[nodiscard]] std::size_t erased_hash() const noexcept final {
        if constexpr (details::scalar_storage_traits<T>::supports_hash) {
            return details::scalar_storage_traits<T>::hash(value());
        } else {
            std::terminate();
        }
    }

    [[nodiscard]] bool erased_equals(const ScalarValueInputView &other) const noexcept final {
        if constexpr (details::scalar_storage_traits<T>::supports_equality) {
            if (&this->type() != &other.type()) {
                return false;
            }

            return value() == static_cast<const TypedScalarValueInputView<T> &>(other).value();
        } else {
            std::terminate();
        }
    }

    void require_compatible(const ScalarStorageRepresentation &representation) const {
        if (&type() != &representation.type()) {
            throw std::invalid_argument{"incompatible scalar types"};
        }
    }
};

template <typename T>
class TypedScalarValueInputView : public TypedScalarValueInputViewBase<T, ScalarValueInputView> {};

static_assert(sizeof(ScalarView) == sizeof(void *));
static_assert(sizeof(ScalarInputView) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValueInputView) == sizeof(ScalarInputView));

} // namespace bg

#include <bg/scalar/dict/view.hpp>
#include <bg/scalar/list/view.hpp>
#include <bg/scalar/set/view.hpp>
