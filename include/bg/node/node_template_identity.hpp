#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace bg {

// Owns the information needed to compare a node definition after its template object is gone.
class NodeTemplateIdentity {
  public:
    NodeTemplateIdentity() = default;

    [[nodiscard]] static NodeTemplateIdentity unique() {
        return from_value(Unique{std::make_shared<const char>()});
    }

    [[nodiscard]] static NodeTemplateIdentity persistent(const void *value) {
        return from_value(Persistent{value});
    }

    template <typename Value>
        requires std::move_constructible<std::remove_cvref_t<Value>> &&
                 requires(const std::remove_cvref_t<Value> &left,
                          const std::remove_cvref_t<Value> &right) {
                     { left.hash() } -> std::convertible_to<std::size_t>;
                     { left == right } -> std::convertible_to<bool>;
                 }
    [[nodiscard]] static NodeTemplateIdentity from_value(Value &&value) {
        using Stored = std::remove_cvref_t<Value>;
        return NodeTemplateIdentity{std::make_shared<Model<Stored>>(std::forward<Value>(value))};
    }

    std::size_t hash() const { return value_ == nullptr ? 0 : value_->hash(); }
    bool cacheable() const { return value_ != nullptr && value_->cacheable(); }

    friend bool operator==(const NodeTemplateIdentity &left, const NodeTemplateIdentity &right) {
        if (left.value_ == right.value_) {
            return true;
        }
        return left.value_ != nullptr && right.value_ != nullptr && left.value_->equals(*right.value_);
    }

  private:
    struct Interface {
        virtual ~Interface() = default;
        virtual std::size_t hash() const = 0;
        virtual bool cacheable() const = 0;
        virtual bool equals(const Interface &) const = 0;
    };

    template <typename Value> struct Model final : Interface {
        template <typename Source> explicit Model(Source &&source) : value{std::forward<Source>(source)} {}

        std::size_t hash() const override { return value.hash(); }

        bool cacheable() const override {
            if constexpr (requires { { value.cacheable() } -> std::convertible_to<bool>; }) {
                return value.cacheable();
            }
            return true;
        }

        bool equals(const Interface &other) const override {
            const auto *candidate = dynamic_cast<const Model *>(&other);
            return candidate != nullptr && value == candidate->value;
        }

        Value value;
    };

    struct Unique {
        std::shared_ptr<const char> value;

        std::size_t hash() const noexcept { return std::hash<const void *>{}(value.get()); }
        bool operator==(const Unique &other) const noexcept { return value == other.value; }
    };

    struct Persistent {
        const void *value;

        std::size_t hash() const noexcept { return std::hash<const void *>{}(value); }
        bool operator==(const Persistent &) const noexcept = default;
    };

    explicit NodeTemplateIdentity(std::shared_ptr<const Interface> value) : value_{std::move(value)} {}

    std::shared_ptr<const Interface> value_;
};

} // namespace bg
