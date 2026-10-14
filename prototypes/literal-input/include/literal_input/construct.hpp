#pragma once

#include <literal_input/scalar_literal.hpp>

#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <span>
#include <stdexcept>

namespace literal_input {
namespace details {

class ConstructionContext {
  public:
    explicit ConstructionContext(const ScalarLiteral &literal) noexcept : literal_{&literal} {}

    static void callback(void *context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                         bg::allocator &memory) {
        static_cast<const ConstructionContext *>(context)->construct(runtime_type, destination, memory);
    }

    void construct(const bg::ScalarRuntimeType &runtime_type, void *destination, bg::allocator &memory) const {
        literal_->visit(
            [&](const ScalarLiteralItem &item) {
                item.visit([&](auto *source) {
                    runtime_type.construct_from_input(destination, bg::details::scalar_data(*source), memory);
                });
            },
            [&](std::span<const ScalarLiteral> children) {
                runtime_type.type().visit(
                    [&](const bg::ListScalarType *) {
                        const auto &list_runtime_type = static_cast<const bg::ScalarListRuntimeType &>(runtime_type);
                        list_runtime_type.construct_runtime_value(destination, memory);
                        auto rollback = bg::util::scope_exit{
                            [&]() noexcept { list_runtime_type.destroy(destination, memory); }};
                        for (const auto &child : children) {
                            auto child_context = ConstructionContext{child};
                            list_runtime_type.emplace_back_with(
                                destination, memory, {&child_context, &ConstructionContext::callback});
                        }
                        rollback.release();
                    },
                    [&](auto *) {
                        throw std::invalid_argument{"literal sequence requires a list scalar destination"};
                    });
            });
    }

  private:
    const ScalarLiteral *literal_;
};

} // namespace details

template <typename T> [[nodiscard]] bg::ScalarValue<T> construct(const ScalarLiteral &literal) {
    auto context = details::ConstructionContext{literal};
    return bg::ScalarValue<T>::from_construction({&context, &details::ConstructionContext::callback});
}

template <typename T> [[nodiscard]] bg::ScalarValue<T> construct(const ScalarLiteral &literal, bg::allocator &memory) {
    auto context = details::ConstructionContext{literal};
    return bg::ScalarValue<T>::from_construction({&context, &details::ConstructionContext::callback}, memory);
}

[[nodiscard]] inline bg::ScalarValue<> construct(const bg::ScalarRuntimeType &runtime_type,
                                                 const ScalarLiteral &literal) {
    auto context = details::ConstructionContext{literal};
    return bg::ScalarValue<>::from_construction(runtime_type, {&context, &details::ConstructionContext::callback});
}

[[nodiscard]] inline bg::ScalarValue<> construct(const bg::ScalarRuntimeType &runtime_type,
                                                 const ScalarLiteral &literal, bg::allocator &memory) {
    auto context = details::ConstructionContext{literal};
    return bg::ScalarValue<>::from_construction(runtime_type, {&context, &details::ConstructionContext::callback},
                                                memory);
}

} // namespace literal_input
