#pragma once

#include <literal_input/scalar_literal.hpp>

#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/runtime_type.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/struct/runtime_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace literal {
namespace details {

class ScalarConstructionContext {
  public:
    explicit ScalarConstructionContext(const ScalarLiteral &literal, bool explicit_allocator = false) noexcept
        : literal_{&literal}, explicit_allocator_{explicit_allocator} {}

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
                            auto child_context = ScalarConstructionContext{child, explicit_allocator_};
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

                                    auto key_context = ScalarConstructionContext{fields[0], explicit_allocator_};
                                    auto mapped_context = ScalarConstructionContext{fields[1], explicit_allocator_};
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
                    [&](const bg::SetScalarType *) {
                        const auto &set_runtime_type = static_cast<const bg::ScalarSetRuntimeType &>(runtime_type);
                        set_runtime_type.construct_runtime_value(destination, memory);
                        auto rollback = bg::util::scope_exit{
                            [&]() noexcept { set_runtime_type.destroy(destination, memory); }};
                        for (const auto &child : children) {
                            auto child_context = ScalarConstructionContext{child, explicit_allocator_};
                            (void)set_runtime_type.insert_with(
                                destination, memory, {&child_context, &ScalarConstructionContext::callback});
                        }
                        rollback.release();
                    },
                    [&](const bg::StructScalarType *struct_type) {
                        auto fields = NamedFieldConstructionContext{*this, children, struct_type, nullptr};
                        fields.validate();
                        static_cast<const bg::StructScalarRuntimeType &>(runtime_type).construct_fields_with(
                            destination, memory, {&fields, &NamedFieldConstructionContext::callback});
                    },
                    [&](const bg::HierarchicalScalarType *) {
                        if (!explicit_allocator_ && bg::details::current_scalar_allocation_owner() == nullptr) {
                            throw std::logic_error{"constructing a Bundle literal requires an explicit allocator"};
                        }
                        const auto &hierarchical_type =
                            static_cast<const bg::HierarchicalScalarRuntimeType &>(runtime_type).type();
                        auto fields = NamedFieldConstructionContext{*this, children, nullptr, &hierarchical_type};
                        fields.validate();
                        static_cast<const bg::HierarchicalScalarRuntimeType &>(runtime_type).construct_fields_with(
                            destination, memory, {&fields, &NamedFieldConstructionContext::callback});
                    },
                    [&](auto *) {
                        throw std::invalid_argument{
                            "literal sequence requires a list, set, dictionary, struct, or Bundle scalar destination"};
                    });
            });
    }

  private:
    template <typename Value, typename ScalarView, typename Function>
    static bool read_name(const ScalarView &source, Function &&function) {
        const auto &data = bg::details::scalar_data(source);
        alignas(Value) std::byte storage[sizeof(Value)];
        data.runtime_type().read_value(data.mode(), data.data_word(), storage);
        auto *value = std::launder(reinterpret_cast<Value *>(storage));
        auto cleanup = bg::util::scope_exit{[value]() noexcept { std::destroy_at(value); }};
        return function(*value);
    }

    struct NamedField {
        const ScalarLiteral *name;
        const ScalarLiteral *value;
    };

    struct NamedFieldConstructionContext {
        const ScalarConstructionContext &parent;
        std::span<const ScalarLiteral> entries;
        const bg::StructScalarType *struct_type;
        const bg::ResolvedHierarchicalScalarType *hierarchical_type;

        std::size_t field_count() const noexcept {
            return struct_type != nullptr ? struct_type->field_count() : hierarchical_type->field_count();
        }

        std::string_view field_name(std::size_t index) const {
            const auto name = struct_type != nullptr ? struct_type->field(index).name : hierarchical_type->field(index).name;
            return name.text();
        }

        bool field_optional(std::size_t index) const {
            return hierarchical_type != nullptr && hierarchical_type->field(index).optional;
        }

        static bool callback(void *context, std::size_t field_index, const bg::ScalarRuntimeType &runtime_type,
                             void *destination, bg::allocator &memory) {
            const auto &fields = *static_cast<const NamedFieldConstructionContext *>(context);
            const auto expected_name = fields.field_name(field_index);
            for (const auto &entry : fields.entries) {
                const auto supplied = parse_entry(entry);
                if (name_matches(*supplied.name, expected_name)) {
                    auto child = ScalarConstructionContext{*supplied.value, fields.parent.explicit_allocator_};
                    child.construct(runtime_type, destination, memory);
                    return true;
                }
            }
            return false;
        }

        void validate() const {
            for (auto entry_index = std::size_t{}; entry_index < entries.size(); ++entry_index) {
                const auto supplied = parse_entry(entries[entry_index]);
                auto known = false;
                for (auto field_index = std::size_t{}; field_index < field_count(); ++field_index) {
                    known = known || name_matches(*supplied.name, field_name(field_index));
                }
                if (!known) {
                    throw std::invalid_argument{"named scalar literal contains an unknown field"};
                }

                for (auto previous_index = std::size_t{}; previous_index < entry_index; ++previous_index) {
                    if (same_name(*supplied.name, *parse_entry(entries[previous_index]).name)) {
                        throw std::invalid_argument{"named scalar literal contains a duplicate field"};
                    }
                }
            }

            for (auto field_index = std::size_t{}; field_index < field_count(); ++field_index) {
                auto supplied = false;
                for (const auto &entry : entries) {
                    supplied = supplied || name_matches(*parse_entry(entry).name, field_name(field_index));
                }
                if (!supplied && !field_optional(field_index)) {
                    throw std::invalid_argument{"named scalar literal omits a required field"};
                }
            }
        }
    };

    template <typename Function> static bool visit_name(const ScalarLiteral &literal, Function &&function) {
        return literal.visit(
            [&](const ScalarLiteralItem &item) {
                return item.visit([&](auto *source) -> bool {
                    using View = std::remove_cvref_t<decltype(*source)>;
                    if constexpr (std::same_as<View, bg::Scalar<bg::string>>) {
                        return read_name<std::string_view>(*source, std::forward<Function>(function));
                    } else if constexpr (std::same_as<View, bg::Scalar<bg::symbol>>) {
                        return read_name<bg::symbol>(*source, [&](const bg::symbol &value) {
                            return function(value.text());
                        });
                    } else if constexpr (std::same_as<View, bg::Scalar<>>) {
                        throw std::invalid_argument{"named scalar literal field name must be a string or symbol"};
                    } else {
                        throw std::invalid_argument{"named scalar literal field name must be a string or symbol"};
                    }
                });
            },
            [](std::span<const ScalarLiteral>) -> bool {
                throw std::invalid_argument{"named scalar literal field name must be a string or symbol"};
            }).value();
    }

    static bool name_matches(const ScalarLiteral &literal, std::string_view expected) {
        return visit_name(literal, [&](std::string_view actual) { return actual == expected; });
    }

    static bool same_name(const ScalarLiteral &left, const ScalarLiteral &right) {
        return visit_name(left, [&](std::string_view left_name) {
            return visit_name(right, [&](std::string_view right_name) { return left_name == right_name; });
        });
    }

    static NamedField parse_entry(const ScalarLiteral &entry) {
        return entry.visit(
            [](const ScalarLiteralItem &) -> NamedField {
                throw std::invalid_argument{"named scalar literal entries must contain a field name and value"};
            },
            [](std::span<const ScalarLiteral> fields) -> NamedField {
                if (fields.size() != 2) {
                    throw std::invalid_argument{"named scalar literal entries must contain exactly two values"};
                }
                (void)visit_name(fields[0], [](std::string_view) { return true; });
                return {&fields[0], &fields[1]};
            }).value();
    }

    const ScalarLiteral *literal_;
    bool explicit_allocator_;
};

} // namespace details

template <typename T> [[nodiscard]] bg::ScalarValue<T> construct(const ScalarLiteral &literal) {
    auto context = details::ScalarConstructionContext{literal};
    return bg::ScalarValue<T>::from_construction({&context, &details::ScalarConstructionContext::callback});
}

template <typename T> [[nodiscard]] bg::ScalarValue<T> construct(const ScalarLiteral &literal, bg::allocator &memory) {
    auto context = details::ScalarConstructionContext{literal, true};
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
    auto context = details::ScalarConstructionContext{literal, true};
    return bg::ScalarValue<>::from_construction(runtime_type, {&context, &details::ScalarConstructionContext::callback},
                                                memory);
}

} // namespace literal
