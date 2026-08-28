#pragma once

#include <bg/scalar/dict/storage_representation.hpp>
#include <bg/scalar/list/storage_representation.hpp>
#include <bg/scalar/mutable_collection.hpp>
#include <bg/scalar/mutable_view.hpp>
#include <bg/scalar/set/storage_representation.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <type_traits>

namespace bg {

template <typename T> class State {
    static_assert(std::is_nothrow_default_constructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);

  public:
    explicit State(T &value) noexcept : value_(&value) {}

    [[nodiscard]] T *operator->() const noexcept { return value_; }
    [[nodiscard]] T &operator*() const noexcept { return *value_; }

    using value_type = T;

  private:
    T *value_;
};

template <typename T>
    requires(!details::generic_scalar<List<T>>)
class State<List<T>> {
  public:
    explicit State(details::scalar_list_storage<T> &value) noexcept : value_{value} {}

    [[nodiscard]] MutableList<T> *operator->() noexcept { return &value_; }
    [[nodiscard]] const MutableList<T> *operator->() const noexcept { return &value_; }
    [[nodiscard]] MutableList<T> &operator*() noexcept { return value_; }
    [[nodiscard]] const MutableList<T> &operator*() const noexcept { return value_; }

    using value_type = List<T>;

  private:
    MutableList<T> value_;
};

template <typename T>
    requires details::generic_scalar<List<T>>
class State<List<T>> {
  public:
    State(const ScalarStorageRepresentation &representation, void *value) noexcept
        : representation_{&static_cast<const ScalarListStorageRepresentation &>(representation)}, value_{value} {}

    [[nodiscard]] State *operator->() noexcept { return this; }
    [[nodiscard]] const State *operator->() const noexcept { return this; }

    [[nodiscard]] std::size_t size() const noexcept { return representation_->item_count(value_); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] auto operator[](std::size_t index) const {
        auto *data = const_cast<void *>(representation_->item(value_, index));
        const auto &item = representation_->item_storage_representation();
        if constexpr (is_list_v<T> || is_set_v<T> || is_dict_v<T>) {
            return State<T>{item, data};
        } else {
            auto view = details::ErasedScalarView::copy(*item.scalar_input_view_prototype());
            view->bind(data);
            return Scalar<T>::from(view);
        }
    }

    void append(const Scalar<T> &value) const {
        representation_->append(value_, value.input_view(), details::current_scalar_memory_resource());
    }
    void set(std::size_t index, const Scalar<T> &value) const {
        representation_->set(value_, index, value.input_view());
    }
    void erase(std::size_t index) const {
        representation_->erase(value_, index, details::current_scalar_memory_resource());
    }
    void clear() const { representation_->clear(value_, details::current_scalar_memory_resource()); }

    using value_type = List<T>;

  private:
    const ScalarListStorageRepresentation *representation_;
    void *value_;
};

template <typename T>
    requires(!details::generic_scalar<Set<T>>)
class State<Set<T>> {
  public:
    explicit State(details::scalar_set_storage<T> &value) noexcept : value_{value} {}

    [[nodiscard]] MutableSet<T> *operator->() noexcept { return &value_; }
    [[nodiscard]] const MutableSet<T> *operator->() const noexcept { return &value_; }
    [[nodiscard]] MutableSet<T> &operator*() noexcept { return value_; }
    [[nodiscard]] const MutableSet<T> &operator*() const noexcept { return value_; }

    using value_type = Set<T>;

  private:
    MutableSet<T> value_;
};

template <typename T>
    requires details::generic_scalar<Set<T>>
class State<Set<T>> {
  public:
    State(const ScalarStorageRepresentation &representation, void *value) noexcept
        : representation_{&static_cast<const ScalarSetStorageRepresentation &>(representation)}, value_{value} {}

    [[nodiscard]] State *operator->() noexcept { return this; }
    [[nodiscard]] const State *operator->() const noexcept { return this; }
    [[nodiscard]] std::size_t size() const noexcept { return representation_->item_count(value_); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool contains(const Scalar<T> &value) const {
        return representation_->contains(value_, value.input_view());
    }
    [[nodiscard]] bool insert(const Scalar<T> &value) const {
        return representation_->insert(value_, value.input_view(), details::current_scalar_memory_resource());
    }
    [[nodiscard]] bool erase(const Scalar<T> &value) const {
        return representation_->erase(value_, value.input_view(), details::current_scalar_memory_resource());
    }
    void clear() const { representation_->clear(value_, details::current_scalar_memory_resource()); }

    using value_type = Set<T>;

  private:
    const ScalarSetStorageRepresentation *representation_;
    void *value_;
};

template <typename K, typename V>
    requires(!details::generic_scalar<Dict<K, V>>)
class State<Dict<K, V>> {
  public:
    explicit State(details::scalar_dict_storage<K, V> &value) noexcept : value_{value} {}

    [[nodiscard]] MutableDict<K, V> *operator->() noexcept { return &value_; }
    [[nodiscard]] const MutableDict<K, V> *operator->() const noexcept { return &value_; }
    [[nodiscard]] MutableDict<K, V> &operator*() noexcept { return value_; }
    [[nodiscard]] const MutableDict<K, V> &operator*() const noexcept { return value_; }

    using value_type = Dict<K, V>;

  private:
    MutableDict<K, V> value_;
};

template <typename K, typename V>
    requires details::generic_scalar<Dict<K, V>>
class State<Dict<K, V>> {
  public:
    State(const ScalarStorageRepresentation &representation, void *value) noexcept
        : representation_{&static_cast<const ScalarDictStorageRepresentation &>(representation)}, value_{value} {}

    [[nodiscard]] State *operator->() noexcept { return this; }
    [[nodiscard]] const State *operator->() const noexcept { return this; }
    [[nodiscard]] std::size_t size() const noexcept { return representation_->item_count(value_); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool contains(const Scalar<K> &key) const {
        return representation_->contains(value_, key.input_view());
    }

    [[nodiscard]] auto at(const Scalar<K> &key) const {
        const auto item = representation_->at(value_, key.input_view());
        if constexpr (is_list_v<V> || is_set_v<V> || is_dict_v<V>) {
            if constexpr (details::generic_scalar<V>) {
                return State<V>{item.representation, item.data};
            } else if constexpr (is_list_v<V>) {
                using Storage = details::scalar_stored_t<V>;
                return MutableList<typename V::item_type>{*static_cast<Storage *>(item.data)};
            } else if constexpr (is_set_v<V>) {
                using Storage = details::scalar_stored_t<V>;
                return MutableSet<typename V::item_type>{*static_cast<Storage *>(item.data)};
            } else {
                using Storage = details::scalar_stored_t<V>;
                return MutableDict<typename V::key_type, typename V::mapped_type>{*static_cast<Storage *>(item.data)};
            }
        } else if constexpr (details::scalar_variable<V>) {
            auto view = details::ErasedScalarView::copy(*item.representation.scalar_input_view_prototype());
            view->bind(item.data);
            return Scalar<V>::from(view);
        } else {
            auto view = details::TypedScalarView<V>::copy(
                static_cast<const TypedScalarValueInputView<V> &>(*item.representation.scalar_input_view_prototype()));
            view->bind(item.data);
            return Scalar<V>::from(view.get());
        }
    }

    void insert_or_assign(const Scalar<K> &key, const Scalar<V> &value) const {
        representation_->insert_or_assign(value_, key.input_view(), value.input_view(),
                                          details::current_scalar_memory_resource());
    }
    [[nodiscard]] bool erase(const Scalar<K> &key) const {
        return representation_->erase(value_, key.input_view(), details::current_scalar_memory_resource());
    }
    void clear() const { representation_->clear(value_, details::current_scalar_memory_resource()); }

    using value_type = Dict<K, V>;

  private:
    const ScalarDictStorageRepresentation *representation_;
    void *value_;
};

static_assert(sizeof(State<int>) == sizeof(void *));
static_assert(sizeof(State<List<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(State<Set<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(State<Dict<SCALAR, SCALAR>>) == 2 * sizeof(void *));

} // namespace bg
