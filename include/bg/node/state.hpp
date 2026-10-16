#pragma once

#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/mutable_collection.hpp>
#include <bg/scalar/set/runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/byte_copy.hpp>

#include <type_traits>

namespace bg {

template <typename T> class State {
    static_assert(util::byte_copy_initializable<T>);

  public:
    explicit State(T &value) noexcept : value_(&value) {}

    T *operator->() const noexcept { return value_; }
    T &operator*() const noexcept { return *value_; }

    using value_type = T;

  private:
    T *value_;
};

template <details::mutable_scalar T> class State<T> {
    using Traits = details::scalar_mutable_traits<T>;
    using View = typename Traits::view_type;

  public:
    explicit State(typename Traits::storage_type &value) noexcept : value_{value} {}

    View *operator->() noexcept { return &value_; }
    const View *operator->() const noexcept { return &value_; }
    View &operator*() noexcept { return value_; }
    const View &operator*() const noexcept { return value_; }

    using value_type = T;

  private:
    View value_;
};

template <details::scalar_variable T> class State<T> {
  public:
    State(const ScalarRuntimeType &runtime_type, void *value) noexcept : runtime_type_{&runtime_type}, value_{value} {}

    Scalar<T> value() const noexcept { return Scalar<T>::from(*runtime_type_, value_); }

    template <typename Source> void set(const Scalar<Source> &value) const {
        runtime_type_->assign_from_input(value_, value);
    }

    void reset() const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->destroy(value_, memory);
        runtime_type_->construct_initial_value(value_);
    }

    using value_type = T;

  private:
    const ScalarRuntimeType *runtime_type_;
    void *value_;
};

template <typename T>
    requires details::generic_scalar<List<T>>
class State<List<T>> {
  public:
    State(const ScalarRuntimeType &runtime_type, void *value) noexcept
        : runtime_type_{&static_cast<const ScalarListRuntimeType &>(runtime_type)}, value_{value} {}

    State *operator->() noexcept { return this; }
    const State *operator->() const noexcept { return this; }

    std::size_t size() const noexcept { return runtime_type_->item_count(value_); }
    bool empty() const noexcept { return size() == 0; }

    auto operator[](std::size_t index) const {
        auto *data = runtime_type_->item(value_, index);
        const auto &item = runtime_type_->item_runtime_type();
        if constexpr (scalar_declaration_traits<T>::is_collection) {
            return State<T>{item, data};
        } else {
            return Scalar<T>::from(item, data);
        }
    }

    void append(const Scalar<T> &value) const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->append(value_, value, memory);
    }
    void set(std::size_t index, const Scalar<T> &value) const { runtime_type_->set(value_, index, value); }
    void erase(std::size_t index) const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->erase(value_, index, memory);
    }
    void clear() const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->clear(value_, memory);
    }

    using value_type = List<T>;

  private:
    const ScalarListRuntimeType *runtime_type_;
    void *value_;
};

template <typename T>
    requires details::generic_scalar<Set<T>>
class State<Set<T>> {
  public:
    State(const ScalarRuntimeType &runtime_type, void *value) noexcept
        : runtime_type_{&static_cast<const ScalarSetRuntimeType &>(runtime_type)}, value_{value} {}

    State *operator->() noexcept { return this; }
    const State *operator->() const noexcept { return this; }
    std::size_t size() const noexcept { return runtime_type_->item_count(value_); }
    bool empty() const noexcept { return size() == 0; }
    bool contains(const Scalar<T> &value) const { return runtime_type_->contains(value_, value); }
    [[nodiscard]] bool insert(const Scalar<T> &value) const {
        auto memory = details::current_scalar_allocator();
        return runtime_type_->insert(value_, value, memory);
    }
    [[nodiscard]] bool erase(const Scalar<T> &value) const {
        auto memory = details::current_scalar_allocator();
        return runtime_type_->erase(value_, value, memory);
    }
    void clear() const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->clear(value_, memory);
    }

    using value_type = Set<T>;

  private:
    const ScalarSetRuntimeType *runtime_type_;
    void *value_;
};

template <typename K, typename V>
    requires details::generic_scalar<Dict<K, V>>
class State<Dict<K, V>> {
  public:
    State(const ScalarRuntimeType &runtime_type, void *value) noexcept
        : runtime_type_{&static_cast<const ScalarDictRuntimeType &>(runtime_type)}, value_{value} {}

    State *operator->() noexcept { return this; }
    const State *operator->() const noexcept { return this; }
    std::size_t size() const noexcept { return runtime_type_->item_count(value_); }
    bool empty() const noexcept { return size() == 0; }
    bool contains(const Scalar<K> &key) const { return runtime_type_->contains(value_, key); }

    auto at(const Scalar<K> &key) const {
        const auto item = runtime_type_->at(value_, key);
        if constexpr (scalar_declaration_traits<V>::is_collection) {
            if constexpr (details::generic_scalar<V>) {
                return State<V>{item.runtime_type, item.data};
            } else {
                using Storage = details::scalar_stored_t<V>;
                return details::mutable_scalar_view<V>(*static_cast<Storage *>(item.data));
            }
        } else {
            return Scalar<V>::from(item.runtime_type, item.data);
        }
    }

    void insert_or_assign(const Scalar<K> &key, const Scalar<V> &value) const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->insert_or_assign(value_, key, value, memory);
    }
    [[nodiscard]] bool erase(const Scalar<K> &key) const {
        auto memory = details::current_scalar_allocator();
        return runtime_type_->erase(value_, key, memory);
    }
    void clear() const {
        auto memory = details::current_scalar_allocator();
        runtime_type_->clear(value_, memory);
    }

    using value_type = Dict<K, V>;

  private:
    const ScalarDictRuntimeType *runtime_type_;
    void *value_;
};

static_assert(sizeof(State<int>) == sizeof(void *));
static_assert(sizeof(State<List<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(State<Set<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(State<Dict<SCALAR, SCALAR>>) == 2 * sizeof(void *));

} // namespace bg
