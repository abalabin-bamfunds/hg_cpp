#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/mutable_collection.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/symbol.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <typeinfo>
#include <utility>

namespace bg {

class GlobalState {
    struct SymbolHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(util::symbol value) const noexcept {
            return std::hash<std::string_view>{}(value.text());
        }

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct SymbolEqual {
        using is_transparent = void;

        [[nodiscard]] bool operator()(util::symbol left, util::symbol right) const noexcept { return left == right; }

        [[nodiscard]] bool operator()(util::symbol left, std::string_view right) const noexcept {
            return left.text() == right;
        }

        [[nodiscard]] bool operator()(std::string_view left, util::symbol right) const noexcept {
            return left == right.text();
        }
    };

    using Values = util::hash_dictionary<util::symbol, ScalarValue<>, SymbolHash, SymbolEqual>;

  public:
    class ReadAccess {
      public:
        ReadAccess(const ReadAccess &) = delete;
        ReadAccess &operator=(const ReadAccess &) = delete;

        [[nodiscard]] bool contains(util::symbol key) const noexcept { return state_->contains(key); }
        [[nodiscard]] bool contains(std::string_view key) const noexcept { return state_->contains(key); }

        [[nodiscard]] Scalar<> at(util::symbol key) const & { return state_->at(key); }
        [[nodiscard]] Scalar<> at(std::string_view key) const & { return state_->at(key); }
        [[nodiscard]] Scalar<> at(util::symbol) const && = delete;
        [[nodiscard]] Scalar<> at(std::string_view) const && = delete;

      private:
        friend GlobalState;

        explicit ReadAccess(const GlobalState &state) : state_{&state}, lock_{state.mutex_} {}

        const GlobalState *state_;
        std::shared_lock<std::shared_mutex> lock_;
    };

    class WriteAccess {
      public:
        WriteAccess(const WriteAccess &) = delete;
        WriteAccess &operator=(const WriteAccess &) = delete;

        [[nodiscard]] bool contains(util::symbol key) const noexcept { return state_->contains(key); }
        [[nodiscard]] bool contains(std::string_view key) const noexcept { return state_->contains(key); }

        [[nodiscard]] Scalar<> at(util::symbol key) const & { return state_->at(key); }
        [[nodiscard]] Scalar<> at(std::string_view key) const & { return state_->at(key); }
        [[nodiscard]] Scalar<> at(util::symbol) const && = delete;
        [[nodiscard]] Scalar<> at(std::string_view) const && = delete;

        template <typename T>
            requires details::mutable_scalar<T>
        [[nodiscard]] auto mutable_at(util::symbol key) const & {
            return state_->template mutable_at<T>(key);
        }

        template <typename T>
            requires details::mutable_scalar<T>
        [[nodiscard]] auto mutable_at(std::string_view key) const & {
            return state_->template mutable_at<T>(key);
        }

        template <typename T>
            requires details::mutable_scalar<T>
        [[nodiscard]] auto mutable_at(util::symbol) const && = delete;

        template <typename T>
            requires details::mutable_scalar<T>
        [[nodiscard]] auto mutable_at(std::string_view) const && = delete;

        template <details::ScalarHandle Value> void set(util::symbol key, const Value &value) {
            state_->set(key, value);
        }

        bool erase(util::symbol key) { return state_->erase(key); }
        bool erase(std::string_view key) { return state_->erase(key); }

      private:
        friend GlobalState;

        explicit WriteAccess(GlobalState &state)
            : state_{&state}, lock_{state.mutex_}, allocation_context_{state.memory_, &state, 0} {}

        GlobalState *state_;
        std::unique_lock<std::shared_mutex> lock_;
        details::ScalarAllocationContext allocation_context_;
    };

    explicit GlobalState(std::pmr::memory_resource &upstream = *std::pmr::get_default_resource())
        : memory_{&upstream} {}

    GlobalState(const GlobalState &) = delete;
    GlobalState &operator=(const GlobalState &) = delete;
    GlobalState(GlobalState &&) = delete;
    GlobalState &operator=(GlobalState &&) = delete;

    ~GlobalState() { values_.release(memory_); }

    template <typename Function>
        requires std::invocable<Function, const ReadAccess &> &&
                 std::same_as<std::invoke_result_t<Function, const ReadAccess &>, void>
    void read(Function &&function) const {
        const auto access = ReadAccess{*this};
        std::invoke(std::forward<Function>(function), access);
    }

    template <typename Function>
        requires std::invocable<Function, WriteAccess &> &&
                 std::same_as<std::invoke_result_t<Function, WriteAccess &>, void>
    void write(Function &&function) {
        auto access = WriteAccess{*this};
        std::invoke(std::forward<Function>(function), access);
    }

  private:
    [[nodiscard]] bool contains(util::symbol key) const noexcept { return values_.contains(key); }
    [[nodiscard]] bool contains(std::string_view key) const noexcept { return values_.contains(key); }

    [[nodiscard]] const ScalarValue<> *find(util::symbol key) const noexcept { return values_.find(key); }

    [[nodiscard]] const ScalarValue<> *find(std::string_view key) const noexcept { return values_.find(key); }

    [[nodiscard]] ScalarValue<> *find(util::symbol key) noexcept { return values_.find(key); }

    [[nodiscard]] ScalarValue<> *find(std::string_view key) noexcept { return values_.find(key); }

    [[nodiscard]] Scalar<> at(util::symbol key) const {
        const auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"global state key was not found"};
        }

        return value->view();
    }

    [[nodiscard]] Scalar<> at(std::string_view key) const {
        const auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"global state key was not found"};
        }

        return value->view();
    }

    template <details::ScalarHandle Value> void set(util::symbol key, const Value &value) {
        values_.insert_or_assign(key, ScalarValue<>{value, memory_}, memory_);
    }

    template <typename T, typename Key>
        requires details::mutable_scalar<T>
    [[nodiscard]] auto mutable_at(Key key) {
        auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"global state key was not found"};
        }
        if (&value->input_view().type() != &cpp_scalar_type<T>()) {
            throw std::bad_cast{};
        }

        using Stored = typename details::scalar_mutable_traits<T>::storage_type;
        auto &storage = *static_cast<Stored *>(const_cast<void *>(value->input_view().data()));
        return details::mutable_scalar_view<T>(storage);
    }

    bool erase(util::symbol key) { return values_.erase(key, memory_); }
    bool erase(std::string_view key) { return values_.erase(key, memory_); }

    std::pmr::unsynchronized_pool_resource memory_;
    Values values_;
    mutable std::shared_mutex mutex_;
};

} // namespace bg
