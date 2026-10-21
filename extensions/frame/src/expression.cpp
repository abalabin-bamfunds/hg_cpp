#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/library/operators.hpp>
#include <bg/scalar/value.hpp>

#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bg {

template <> ScalarDispatch<frame::ColumnBlockFunctionAdapter> &
ScalarDispatch<frame::ColumnBlockFunctionAdapter>::instance() noexcept {
    static auto dispatch = ScalarDispatch<frame::ColumnBlockFunctionAdapter>{};
    return dispatch;
}

} // namespace bg

namespace bg::frame::details {

struct ExpressionNode {
    enum class Kind { column, parameter, capture, operation };

    Kind kind;
    util::symbol name;
    const WiringTemplate *operation{};
    std::vector<std::shared_ptr<const ExpressionNode>> operands;
    std::optional<WiringArgument> argument;
};

namespace {

struct BindingSet {
    struct Entry {
        const WiringArgument *argument;
        bool used{};
    };

    explicit BindingSet(std::span<const WiringArgument> arguments) {
        for (const auto &argument : arguments) {
            const auto name = argument.name();
            if (!name) {
                throw WiringError{"expression bindings must be named"};
            }
            const auto key = name->text();
            if (!values.emplace(key, Entry{std::addressof(argument)}).second) {
                throw WiringError{"expression binding name '" + std::string{key} + "' is supplied more than once"};
            }
        }
    }

    const WiringArgument *find(util::symbol name) {
        const auto found = values.find(name.text());
        if (found == values.end()) {
            return nullptr;
        }
        found->second.used = true;
        return found->second.argument;
    }

    void validate_all_used() const {
        for (const auto &[name, entry] : values) {
            if (!entry.used) {
                throw WiringError{"expression binding name '" + std::string{name} +
                                  "' does not match a column or parameter"};
            }
        }
    }

    std::unordered_map<std::string_view, Entry> values;
};

} // namespace

WiringPoint bind_expression(const Expression &expression, std::span<const WiringArgument> bindings,
                            const ExpressionColumnResolver &column_resolver) {
    auto &wiring = WiringContext::current().wiring();
    auto binding_set = BindingSet{bindings};
    auto resolved = std::unordered_map<const ExpressionNode *, WiringArgument>{};

    const auto column_context = [](util::symbol name) {
        const auto *path = WiringContext::current().path();
        return WiringErrorContext{.stage = WiringFailureStage::resolver,
                                  .path = path == nullptr ? std::string{} : path->text(),
                                  .operation = util::symbol{"bind_expression"},
                                  .parameter = name,
                                  .expected_type = "TSC"};
    };
    const auto validate_column = [](const WiringArgument &argument, WiringErrorContext context) {
        if (!argument.is_time_series()) {
            argument.type().format_name(context.actual_type);
            throw WiringError{"a column expression binding must be a TSC"}.with_context(std::move(context));
        }
        const auto point = argument.time_series();
        if (!point) {
            argument.type().format_name(context.actual_type);
            throw WiringError{"a column expression binding must be a TSC"}.with_context(std::move(context));
        }
        const auto source = bg::details::WiringRecord::point(*point.record()->source_identity());
        const auto &source_type = static_cast<const TimeSeriesType &>(source.type()).dereferenced_type();
        if (dynamic_cast<const ColumnSeriesType *>(std::addressof(source_type)) == nullptr) {
            source.type().format_name(context.actual_type);
            throw WiringError{"a column expression binding must be a TSC"}.with_context(std::move(context));
        }
        return WiringArgument{source};
    };

    const auto resolve = [&](const auto &self, const ExpressionNode &node) -> WiringArgument {
        if (const auto found = resolved.find(std::addressof(node)); found != resolved.end()) {
            return WiringArgument::borrow(found->second);
        }

        auto result = [&]() -> WiringArgument {
            switch (node.kind) {
            case ExpressionNode::Kind::column: {
                const auto *binding = binding_set.find(node.name);
                if (binding != nullptr) {
                    auto context = column_context(node.name);
                    return validate_column(*binding, std::move(context));
                }
                if (!column_resolver) {
                    throw WiringError{"column '" + std::string{node.name.text()} +
                                      "' needs a frame or an explicit binding"};
                }
                auto context = column_context(node.name);
                auto selected = [&]() {
                    try {
                        return column_resolver(node.name);
                    } catch (const WiringError &error) {
                        throw error.with_context(context);
                    } catch (...) {
                        throw WiringError::from_exception("failed to resolve expression column '" +
                                                              std::string{node.name.text()} + "'",
                                                          context, std::current_exception());
                    }
                }();
                return validate_column(selected, std::move(context));
            }
            case ExpressionNode::Kind::parameter: {
                const auto *binding = binding_set.find(node.name);
                if (binding == nullptr) {
                    throw WiringError{"expression parameter '" + std::string{node.name.text()} +
                                      "' has no binding"};
                }
                return WiringArgument::borrow(*binding);
            }
            case ExpressionNode::Kind::capture:
                return WiringArgument::borrow(*node.argument);
            case ExpressionNode::Kind::operation: {
                auto arguments = std::vector<WiringArgument>{};
                arguments.reserve(node.operands.size());
                for (const auto &operand : node.operands) {
                    arguments.push_back(self(self, *operand));
                }
                for (auto &argument : arguments)
                    argument.clear_name();
                const auto output = wiring.invoke(*node.operation, std::span<const WiringArgument>{arguments});
                if (!output) {
                    throw WiringError{"expression operation '" + std::string{node.operation->signature().name().text()} +
                                      "' did not produce a time series"};
                }
                return WiringArgument{*output};
            }
            }
            throw WiringError{"expression contains an unknown operation"};
        }();

        const auto entry = resolved.emplace(std::addressof(node), std::move(result)).first;
        return WiringArgument::borrow(entry->second);
    };

    if (expression.node_ == nullptr) {
        throw WiringError{"cannot bind an empty expression"};
    }

    auto result = resolve(resolve, *expression.node_);
    binding_set.validate_all_used();
    if (!result.is_time_series()) {
        throw WiringError{"a bound expression must produce a time series"};
    }
    if (std::addressof(result.time_series().graph()) != std::addressof(wiring)) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }
    return result.time_series();
}

} // namespace bg::frame::details

namespace bg::frame {

Expression col(util::symbol name) {
    return Expression{std::make_shared<const details::ExpressionNode>(
        details::ExpressionNode{.kind = details::ExpressionNode::Kind::column, .name = name})};
}

Expression col(std::string_view name) { return col(util::symbol{name}); }

Expression param(util::symbol name) {
    return Expression{std::make_shared<const details::ExpressionNode>(
        details::ExpressionNode{.kind = details::ExpressionNode::Kind::parameter, .name = name})};
}

Expression param(std::string_view name) { return param(util::symbol{name}); }

Expression capture(WiringArgument argument) {
    if (argument.is_scalar() && argument.present()) {
        auto memory = allocator{};
        argument = WiringArgument{ScalarValue<>{argument.scalar(), memory}};
    }
    argument.clear_name();
    return Expression{std::make_shared<const details::ExpressionNode>(details::ExpressionNode{
        .kind = details::ExpressionNode::Kind::capture,
        .argument = std::move(argument),
    })};
}

Expression apply(const WiringTemplate &operation, std::span<const Expression> operands) {
    auto children = std::vector<std::shared_ptr<const details::ExpressionNode>>{};
    children.reserve(operands.size());
    for (const auto &operand : operands) {
        if (operand.node_ == nullptr) {
            throw std::invalid_argument{"an expression operation cannot contain an empty operand"};
        }
        children.push_back(operand.node_);
    }
    return Expression{std::make_shared<const details::ExpressionNode>(details::ExpressionNode{
        .kind = details::ExpressionNode::Kind::operation,
        .operation = std::addressof(operation),
        .operands = std::move(children),
    })};
}

Expression operator+(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::add, operands);
}

Expression operator-(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::sub, operands);
}

Expression operator==(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::eq, operands);
}

Expression operator!=(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::ne, operands);
}

Expression operator<(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::lt, operands);
}

Expression operator<=(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::le, operands);
}

Expression operator>(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::gt, operands);
}

Expression operator>=(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return bg::frame::apply(operators::ge, operands);
}

} // namespace bg::frame

namespace {

struct ColumnBlockFunctionTypeKey {
    const bg::ScalarType *result_type{};
    std::vector<const bg::ScalarType *> operand_types;

    bool operator==(const ColumnBlockFunctionTypeKey &) const = default;
};

struct ColumnBlockFunctionTypeKeyHash {
    std::size_t operator()(const ColumnBlockFunctionTypeKey &key) const noexcept {
        auto result = std::hash<const bg::ScalarType *>{}(key.result_type);
        for (const auto *operand_type : key.operand_types)
            result = (result * 31U) ^ std::hash<const bg::ScalarType *>{}(operand_type);
        return result;
    }
};

} // namespace

const bg::frame::ColumnBlockFunctionType &bg::frame::column_block_function_type(
    const bg::ScalarType &result_type, std::span<const bg::ScalarType *const> operand_types) {
    auto key = ColumnBlockFunctionTypeKey{&result_type, {operand_types.begin(), operand_types.end()}};
    return bg::TypeRegistry::instance().get_or_create<ColumnBlockFunctionType, ColumnBlockFunctionTypeKeyHash>(
        key, [&] { return std::make_unique<ColumnBlockFunctionType>(result_type, key.operand_types); });
}

void bg::frame::ColumnBlockFunctionType::format_name(std::string &result) const {
    result += "ColumnBlockFunction<";
    for (std::size_t index = 0; index != operand_types().size(); ++index) {
        if (index != 0)
            result += ", ";
        operand_types()[index]->format_name(result);
    }
    result += " -> ";
    result_type().format_name(result);
    result += '>';
}

const bg::Type &bg::frame::ColumnBlockFunctionType::resolve(const bg::TypeMap &type_map) const {
    const auto &resolved_result_type = result_type().resolve(type_map);
    const auto *scalar_type = bg::type_cast<bg::ScalarType>(&resolved_result_type);
    if (scalar_type == nullptr)
        throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map,
                                      bg::util::symbol{"ColumnBlockFunction"},
                                      "column block function result did not resolve to a scalar type"};
    auto resolved_operands = std::vector<const bg::ScalarType *>{};
    resolved_operands.reserve(operand_types().size());
    for (const auto *operand_type : operand_types()) {
        const auto &resolved_type = operand_type->resolve(type_map);
        const auto *resolved_scalar_type = bg::type_cast<bg::ScalarType>(&resolved_type);
        if (resolved_scalar_type == nullptr)
            throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map,
                                          bg::util::symbol{"ColumnBlockFunction"},
                                          "column block function operand did not resolve to a scalar type"};
        resolved_operands.push_back(resolved_scalar_type);
    }
    return column_block_function_type(*scalar_type, resolved_operands);
}

bool bg::frame::ColumnBlockFunctionType::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *function = dynamic_cast<const ColumnBlockFunctionType *>(std::addressof(supplied));
    if (function == nullptr || !function->is_resolved() || operand_types().size() != function->operand_types().size())
        return false;
    auto trial = type_map.overlay();
    if (!result_type().match(function->result_type(), trial))
        return false;
    for (auto index = std::size_t{}; index != operand_types().size(); ++index)
        if (!operand_types()[index]->match(*function->operand_types()[index], trial))
            return false;
    type_map.apply(std::move(trial));
    return true;
}
