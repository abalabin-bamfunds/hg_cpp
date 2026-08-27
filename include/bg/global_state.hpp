#pragma once

#include <bg/scalar/value.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <functional>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

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

    using Values = std::pmr::unordered_map<util::symbol, ScalarValue<>, SymbolHash, SymbolEqual>;

  public:
    explicit GlobalState(std::pmr::memory_resource &upstream = *std::pmr::get_default_resource())
        : memory_{&upstream}, values_{&memory_} {}

    GlobalState(const GlobalState &) = delete;
    GlobalState &operator=(const GlobalState &) = delete;
    GlobalState(GlobalState &&) = delete;
    GlobalState &operator=(GlobalState &&) = delete;

    [[nodiscard]] bool contains(util::symbol key) const noexcept { return values_.contains(key); }
    [[nodiscard]] bool contains(std::string_view key) const noexcept { return values_.contains(key); }

    [[nodiscard]] const ScalarValue<> *find(util::symbol key) const noexcept {
        const auto found = values_.find(key);

        return found == values_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const ScalarValue<> *find(std::string_view key) const noexcept {
        const auto found = values_.find(key);

        return found == values_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] Scalar<> at(util::symbol key) const {
        const auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"global state key was not found"};
        }

        return Scalar<>::from(value->view());
    }

    [[nodiscard]] Scalar<> at(std::string_view key) const {
        const auto *value = find(key);
        if (value == nullptr) {
            throw std::out_of_range{"global state key was not found"};
        }

        return Scalar<>::from(value->view());
    }

    template <details::ScalarHandle Value> void set(util::symbol key, const Value &value) {
        values_.insert_or_assign(key, ScalarValue<>{value, memory_});
    }

    bool erase(util::symbol key) { return values_.erase(key) != 0; }
    bool erase(std::string_view key) {
        const auto found = values_.find(key);
        if (found == values_.end()) {
            return false;
        }

        values_.erase(found);
        return true;
    }

    [[nodiscard]] std::pmr::memory_resource &memory_resource() noexcept { return memory_; }

  private:
    std::pmr::unsynchronized_pool_resource memory_;
    Values values_;
};

} // namespace bg
