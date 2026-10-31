#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>

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
    enum class Form { column, parameter, literal, binding, operation };

    Form form;
    util::symbol name;
    std::size_t binding_index{};
    ExpressionOperation operation{};
    const ScalarType *conversion_target{};
    std::vector<std::shared_ptr<const ExpressionNode>> operands;
    std::optional<ScalarValue<>> literal;
};

namespace {

const TimeSeriesBundleType *mixed_expression_bundle(const Type &type) {
    const auto *series = type_cast<TimeSeriesType>(&type);
    if (series == nullptr)
        return nullptr;
    const auto &resolved = series->dereferenced_type();
    const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(std::addressof(resolved));
    if (bundle == nullptr || bundle->item_count() < 2 || bundle->item_count() > 3 ||
        bundle->item_name(0).text() != "expression")
        return nullptr;

    const auto &expression_type = bundle->item_type(0).dereferenced_type();
    const auto *expression_value = dynamic_cast<const TimeSeriesValueType *>(std::addressof(expression_type));
    if (expression_value == nullptr ||
        std::addressof(expression_value->value_type()) !=
            std::addressof(scalar_type_traits<Expression>::type()))
        return nullptr;

    for (auto index = std::size_t{1}; index != bundle->item_count(); ++index) {
        const auto binding_name = index == 1 ? "binding_0" : "binding_1";
        if (bundle->item_name(index).text() != binding_name ||
            type_cast<TimeSeriesType>(std::addressof(bundle->item_type(index))) == nullptr)
            return nullptr;
    }
    return bundle;
}

Expression read_mixed_expression(WiringArgument argument) {
    if (argument.is_scalar()) {
        const auto scalar = argument.scalar();
        return Scalar<Expression>::from(scalar).value();
    }
    throw WiringError{"mixed expression bundle has no scalar expression field"};
}

std::vector<WiringArgument> mixed_expression_bindings(WiringPoint point,
                                                    const TimeSeriesBundleType &bundle_type) {
    auto bindings = std::vector<WiringArgument>{};
    bindings.reserve(bundle_type.item_count() - 1);
    for (auto index = std::size_t{1}; index != bundle_type.item_count(); ++index)
        bindings.push_back(bg::details::time_series_bundle_argument(point, index));
    return bindings;
}

} // namespace

struct ExpressionBindingSet {
    struct Entry {
        const WiringArgument *argument;
        bool used{};
    };

    explicit ExpressionBindingSet(std::span<const WiringArgument> arguments) {
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

WiringPoint bind_expression_tree(const Expression &expression, std::span<const WiringArgument> references,
                                 ExpressionBindingSet &binding_set,
                                 const ExpressionColumnResolver &column_resolver, GraphWiring &wiring) {
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

    const auto resolve = [&](const auto &self, const ExpressionNode &node,
                             std::span<const WiringArgument> current_references) -> WiringArgument {
        if (node.form != ExpressionNode::Form::binding) {
            if (const auto found = resolved.find(std::addressof(node)); found != resolved.end())
                return WiringArgument::borrow(found->second);
        }

        auto result = [&]() -> WiringArgument {
            switch (node.form) {
            case ExpressionNode::Form::column: {
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
            case ExpressionNode::Form::parameter: {
                const auto *binding = binding_set.find(node.name);
                if (binding == nullptr) {
                    throw WiringError{"expression parameter '" + std::string{node.name.text()} +
                                      "' has no binding"};
                }
                return WiringArgument::borrow(*binding);
            }
            case ExpressionNode::Form::literal:
                return WiringArgument{node.literal->view()};
            case ExpressionNode::Form::binding: {
                if (node.binding_index >= current_references.size())
                    throw WiringError{"mixed expression binding reference is outside its TSB"};
                const auto &binding = current_references[node.binding_index];
                if (!binding.is_time_series())
                    throw WiringError{"mixed expression binding must be a time series"};
                const auto point = binding.time_series();
                if (const auto *bundle_type = mixed_expression_bundle(point.type())) {
                    const auto nested_expression = read_mixed_expression(
                        bg::details::time_series_bundle_argument(point, 0));
                    const auto nested_bindings = mixed_expression_bindings(point, *bundle_type);
                    return self(self, *nested_expression.node_, nested_bindings);
                }
                return WiringArgument::borrow(binding);
            }
            case ExpressionNode::Form::operation: {
                auto arguments = std::vector<WiringArgument>{};
                arguments.reserve(node.operands.size());
                for (const auto &operand : node.operands)
                    arguments.push_back(self(self, *operand, current_references));
                for (auto &argument : arguments)
                    argument.clear_name();
                const auto invoke = [&](const WiringTemplate &operation, const TypeMap *type_map = nullptr) {
                    return wiring.invoke(operation, std::span<const WiringArgument>{arguments}, type_map);
                };
                const auto output = [&]() {
                    switch (node.operation) {
                    case ExpressionOperation::add:
                        return invoke(operators::add);
                    case ExpressionOperation::sub:
                        return invoke(operators::sub);
                    case ExpressionOperation::mul:
                        return invoke(operators::mul);
                    case ExpressionOperation::div:
                        return invoke(operators::div);
                    case ExpressionOperation::eq:
                        return invoke(operators::eq);
                    case ExpressionOperation::ne:
                        return invoke(operators::ne);
                    case ExpressionOperation::lt:
                        return invoke(operators::lt);
                    case ExpressionOperation::le:
                        return invoke(operators::le);
                    case ExpressionOperation::gt:
                        return invoke(operators::gt);
                    case ExpressionOperation::ge:
                        return invoke(operators::ge);
                    case ExpressionOperation::and_:
                        return invoke(operators::and_);
                    case ExpressionOperation::or_:
                        return invoke(operators::or_);
                    case ExpressionOperation::not_:
                        return invoke(operators::not_);
                    case ExpressionOperation::convert: {
                        auto type_map = TypeMap{};
                        const auto &target = column_series_type(*node.conversion_target,
                                                                TimeSeriesGuarantee::ordinary);
                        type_map.set(bg::details::type_parameter<operators::CONVERT_RESULT>(), std::addressof(target));
                        return invoke(operators::convert, std::addressof(type_map));
                    }
                    }
                    std::unreachable();
                }();
                if (!output)
                    throw WiringError{"expression operation did not produce a time series"};
                return WiringArgument{*output};
            }
            }
            throw WiringError{"expression contains an unknown operation"};
        }();

        if (node.form == ExpressionNode::Form::binding)
            return result;
        const auto entry = resolved.emplace(std::addressof(node), std::move(result)).first;
        return WiringArgument::borrow(entry->second);
    };

    auto result = resolve(resolve, *expression.node_, references);
    if (!result.is_time_series())
        throw WiringError{"a bound expression must produce a time series"};
    if (std::addressof(result.time_series().graph()) != std::addressof(wiring))
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    return result.time_series();
}

WiringPoint bind_expression(const Expression &expression, std::span<const WiringArgument> bindings,
                           const ExpressionColumnResolver &column_resolver) {
    auto &wiring = WiringContext::current().wiring();
    auto binding_set = ExpressionBindingSet{bindings};
    auto result = bind_expression_tree(expression, {}, binding_set, column_resolver, wiring);
    binding_set.validate_all_used();
    return result;
}

WiringPoint bind_expression(WiringPoint expression, std::span<const WiringArgument> bindings,
                           const ExpressionColumnResolver &column_resolver) {
    const auto *bundle_type = mixed_expression_bundle(expression.type());
    if (bundle_type == nullptr)
        throw WiringError{"bind_expression requires an expression or a mixed expression TSB"};

    const auto scalar_expression = read_mixed_expression(bg::details::time_series_bundle_argument(expression, 0));
    const auto references = mixed_expression_bindings(expression, *bundle_type);
    auto &wiring = WiringContext::current().wiring();
    auto binding_set = ExpressionBindingSet{bindings};
    auto result = bind_expression_tree(scalar_expression, references, binding_set, column_resolver, wiring);
    binding_set.validate_all_used();
    return result;
}

} // namespace bg::frame::details

namespace bg::frame {

Expression col(util::symbol name) {
    return Expression{std::make_shared<const details::ExpressionNode>(
        details::ExpressionNode{.form = details::ExpressionNode::Form::column, .name = name})};
}

Expression col(std::string_view name) { return col(util::symbol{name}); }

Expression param(util::symbol name) {
    return Expression{std::make_shared<const details::ExpressionNode>(
        details::ExpressionNode{.form = details::ExpressionNode::Form::parameter, .name = name})};
}

Expression param(std::string_view name) { return param(util::symbol{name}); }

Expression details::expression_literal(ScalarValue<> value) {
    return Expression{std::make_shared<const details::ExpressionNode>(details::ExpressionNode{
        .form = details::ExpressionNode::Form::literal,
        .literal = std::move(value),
    })};
}

Expression details::expression_operation(details::ExpressionOperation operation, std::span<const Expression> operands,
                                        const ScalarType *conversion_target) {
    auto children = std::vector<std::shared_ptr<const details::ExpressionNode>>{};
    children.reserve(operands.size());
    for (const auto &operand : operands)
        children.push_back(operand.node_);
    return Expression{std::make_shared<const details::ExpressionNode>(details::ExpressionNode{
        .form = details::ExpressionNode::Form::operation,
        .operation = operation,
        .conversion_target = conversion_target,
        .operands = std::move(children),
    })};
}

Expression details::expression_binding(std::size_t index) {
    return Expression{std::make_shared<const details::ExpressionNode>(details::ExpressionNode{
        .form = details::ExpressionNode::Form::binding,
        .binding_index = index,
    })};
}

WiringPoint details::make_mixed_expression(details::ExpressionOperation operation,
                                          std::span<const WiringArgument> operands) {
    auto expression_operands = std::vector<Expression>{};
    auto bindings = std::vector<WiringArgument>{};
    expression_operands.reserve(operands.size());
    bindings.reserve(operands.size());
    for (const auto &operand : operands) {
        if (operand.is_scalar() &&
            std::addressof(operand.type()) == std::addressof(scalar_type_traits<Expression>::type())) {
            const auto scalar = operand.scalar();
            expression_operands.push_back(Scalar<Expression>::from(scalar).value());
            continue;
        }
        if (operand.is_scalar()) {
            const auto scalar = operand.scalar();
            expression_operands.push_back(details::expression_literal(ScalarValue<>{scalar}));
            continue;
        }
        if (!operand.is_time_series())
            throw WiringError{"mixed expression operands must be expressions or time series"};
        expression_operands.push_back(details::expression_binding(bindings.size()));
        auto binding = WiringArgument::borrow(operand);
        binding.clear_name();
        bindings.push_back(std::move(binding));
    }

    const auto expression = details::expression_operation(operation, expression_operands);
    auto fields = std::vector<WiringArgument>{};
    fields.reserve(bindings.size() + 1);
    fields.emplace_back(util::symbol{"expression"}, expression);
    for (auto index = std::size_t{}; index != bindings.size(); ++index) {
        const auto name = index == 0 ? util::symbol{"binding_0"} : util::symbol{"binding_1"};
        fields.emplace_back(name, std::move(bindings[index]));
    }
    return bg::bundle(std::span<const WiringArgument>{fields});
}

bool details::is_mixed_expression_bundle_type(const Type &type) {
    return mixed_expression_bundle(type) != nullptr;
}

Expression operator+(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::add, operands);
}

Expression operator-(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::sub, operands);
}

Expression operator*(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::mul, operands);
}

Expression operator/(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::div, operands);
}

Expression operator==(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::eq, operands);
}

Expression operator!=(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::ne, operands);
}

Expression operator<(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::lt, operands);
}

Expression operator<=(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::le, operands);
}

Expression operator>(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::gt, operands);
}

Expression operator>=(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::ge, operands);
}

Expression operator&&(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::and_, operands);
}

Expression operator||(const Expression &lhs, const Expression &rhs) {
    const auto operands = std::array{lhs, rhs};
    return details::expression_operation(details::ExpressionOperation::or_, operands);
}

Expression operator!(const Expression &value) {
    const auto operands = std::array{value};
    return details::expression_operation(details::ExpressionOperation::not_, operands);
}

} // namespace bg::frame

namespace {

class ExpressionScalarType final : public bg::ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "Expression"; }
};

} // namespace

const bg::ScalarType &bg::scalar_type_traits<bg::frame::Expression>::type() noexcept {
    static const auto type = bg::util::static_storage<ExpressionScalarType>{};
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type.get());
    return type.get();
}

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
