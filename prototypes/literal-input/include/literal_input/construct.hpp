#pragma once

#include <literal_input/scalar_literal.hpp>

#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <span>
#include <stdexcept>

namespace literal {
namespace details {

class ScalarConstructionContext {
  public:
    explicit ScalarConstructionContext(const ScalarLiteral &literal) noexcept : literal_{&literal} {}

    static void callback(void *context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                         bg::allocator &memory) {
        static_cast<const ScalarConstructionContext *>(context)->construct(runtime_type, destination, memory);
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
                            auto child_context = ScalarConstructionContext{child};
                            list_runtime_type.emplace_back_with(
                                destination, memory, {&child_context, &ScalarConstructionContext::callback});
                        }
                        rollback.release();
                    },
                    [&](const bg::DictScalarType *) {
                        const auto &dict_runtime_type = static_cast<const bg::ScalarDictRuntimeType &>(runtime_type);
                        dict_runtime_type.construct_runtime_value(destination, memory);
                        auto rollback = bg::util::scope_exit{
                            [&]() noexcept { dict_runtime_type.destroy(destination, memory); }};
                        for (const auto &entry : children) {
                            entry.visit(
                                [&](const ScalarLiteralItem &) {
                                    throw std::invalid_argument{
                                        "dictionary literal entries must contain a key and mapped value"};
                                },
                                [&](std::span<const ScalarLiteral> fields) {
                                    if (fields.size() != 2) {
                                        throw std::invalid_argument{
                                            "dictionary literal entries must contain exactly two values"};
                                    }

                                    auto key_context = ScalarConstructionContext{fields[0]};
                                    auto mapped_context = ScalarConstructionContext{fields[1]};
                                    if (!dict_runtime_type.try_emplace_with(
                                            destination, memory,
                                            {&key_context, &ScalarConstructionContext::callback},
                                            {&mapped_context, &ScalarConstructionContext::callback})) {
                                        throw std::invalid_argument{"dictionary literal contains duplicate keys"};
                                    }
                                });
                        }
                        rollback.release();
                    },
                    [&](auto *) {
                        throw std::invalid_argument{"literal sequence requires a list or dictionary scalar destination"};
                    });
            });
    }

  private:
    const ScalarLiteral *literal_;
};

} // namespace details

template <typename T> [[nodiscard]] bg::ScalarValue<T> construct(const ScalarLiteral &literal) {
    auto context = details::ScalarConstructionContext{literal};
    return bg::ScalarValue<T>::from_construction({&context, &details::ScalarConstructionContext::callback});
}

template <typename T> [[nodiscard]] bg::ScalarValue<T> construct(const ScalarLiteral &literal, bg::allocator &memory) {
    auto context = details::ScalarConstructionContext{literal};
    return bg::ScalarValue<T>::from_construction({&context, &details::ScalarConstructionContext::callback}, memory);
}

[[nodiscard]] inline bg::ScalarValue<> construct(const bg::ScalarRuntimeType &runtime_type,
                                                 const ScalarLiteral &literal) {
    auto context = details::ScalarConstructionContext{literal};
    return bg::ScalarValue<>::from_construction(
        runtime_type, {&context, &details::ScalarConstructionContext::callback});
}

[[nodiscard]] inline bg::ScalarValue<> construct(const bg::ScalarRuntimeType &runtime_type,
                                                 const ScalarLiteral &literal, bg::allocator &memory) {
    auto context = details::ScalarConstructionContext{literal};
    return bg::ScalarValue<>::from_construction(runtime_type, {&context, &details::ScalarConstructionContext::callback},
                                                memory);
}

} // namespace literal
