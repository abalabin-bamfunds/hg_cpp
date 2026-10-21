#pragma once

#include <bg/frame/column.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/details/graph_signature.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/library/operators.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/symbol.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace bg::frame {

// Extension-facing writable batch passed to column block functions. Callbacks set one validity bit per output row and
// write values only for valid cells. Bits after the final row are ignored.
template <typename T = void> struct MutableColumnBatch {
    static_assert(cell_type<T>);
    std::span<T> values;
    std::span<std::uint8_t> validity;

    std::size_t size() const noexcept { return values.size(); }
    T value_at(std::size_t position) const noexcept { return values[position]; }
    void set_value(std::size_t position, T value) const noexcept { values[position] = value; }
    bool is_valid(std::size_t position) const noexcept {
        return (validity[position / 8] & (std::uint8_t{1} << (position % 8))) != 0;
    }

    void set_valid(std::size_t position, bool valid) noexcept {
        auto &byte = validity[position / 8];
        const auto mask = std::uint8_t{1} << (position % 8);
        if (valid)
            byte |= mask;
        else
            byte &= static_cast<std::uint8_t>(~mask);
    }
};

template <> struct MutableColumnBatch<bool> {
    std::span<std::uint8_t> values;
    std::uint64_t value_bit_offset{};
    std::span<std::uint8_t> validity;

    MutableColumnBatch(std::span<std::uint8_t> values, std::size_t size, std::uint64_t value_bit_offset,
                       std::span<std::uint8_t> validity) noexcept
        : values{values}, size_{size}, value_bit_offset{value_bit_offset}, validity{validity} {}

    std::size_t size() const noexcept { return size_; }
    bool value_at(std::size_t position) const noexcept {
        const auto bit = value_bit_offset + position;
        return (values[bit / 8] & (std::uint8_t{1} << (bit % 8))) != 0;
    }
    void set_value(std::size_t position, bool value) const noexcept {
        const auto bit = value_bit_offset + position;
        auto &byte = values[bit / 8];
        const auto mask = std::uint8_t{1} << (bit % 8);
        if (value)
            byte |= mask;
        else
            byte &= static_cast<std::uint8_t>(~mask);
    }
    bool is_valid(std::size_t position) const noexcept {
        return (validity[position / 8] & (std::uint8_t{1} << (position % 8))) != 0;
    }
    void set_valid(std::size_t position, bool valid) const noexcept {
        auto &byte = validity[position / 8];
        const auto mask = std::uint8_t{1} << (position % 8);
        if (valid)
            byte |= mask;
        else
            byte &= static_cast<std::uint8_t>(~mask);
    }

  private:
    std::size_t size_{};
};

template <> struct MutableColumnBatch<void> {
    void *values{};
    std::size_t size{};
    std::span<std::uint8_t> validity;

    MutableColumnBatch() = default;
    MutableColumnBatch(void *values, std::size_t size, std::span<std::uint8_t> validity) noexcept
        : values{values}, size{size}, validity{validity} {}
};

// One expression operand is a row-aligned column batch or an always-valid scalar broadcast. Read a column value only
// when is_valid(position) returns true.
template <typename T = void> class ColumnOperandBatch {
    static_assert(cell_type<T>);
  public:
    ColumnOperandBatch(ColumnBatch<T> column) : value_{column} {}
    ColumnOperandBatch(T scalar) : value_{scalar} {}

    bool is_column() const noexcept { return std::holds_alternative<ColumnBatch<T>>(value_); }
    const ColumnBatch<T> &column() const { return std::get<ColumnBatch<T>>(value_); }
    T value(std::size_t position) const {
        const auto *column = std::get_if<ColumnBatch<T>>(&value_);
        return column == nullptr ? std::get<T>(value_) : column->value_at(position);
    }
    bool is_valid(std::size_t position) const noexcept {
        const auto *column = std::get_if<ColumnBatch<T>>(&value_);
        return column == nullptr || column->is_valid(position);
    }

  private:
    std::variant<ColumnBatch<T>, T> value_;
};

template <> class ColumnOperandBatch<void> {
  public:
    ColumnOperandBatch() = default;
    explicit ColumnOperandBatch(ColumnBatch<> column) : value_{column} {}
    explicit ColumnOperandBatch(const void *scalar) : value_{scalar} {}

    bool is_column() const noexcept { return std::holds_alternative<ColumnBatch<>>(value_); }
    const ColumnBatch<> &column() const { return std::get<ColumnBatch<>>(value_); }
    const void *scalar() const { return std::get<const void *>(value_); }

  private:
    std::variant<ColumnBatch<>, const void *> value_;
};

// Extension-facing typed block callback. Inputs are aligned by row ID; output storage is caller-owned.
template <cell_type RESULT, cell_type... ARGS> struct ColumnBlockFunction {
    static_assert(sizeof...(ARGS) == 1 || sizeof...(ARGS) == 2,
                  "column block functions support unary or binary operations");
    using function_type = void (*)(std::span<const row_id_t>, const ColumnOperandBatch<ARGS> &...,
                                   MutableColumnBatch<RESULT>);

    function_type function{};

    ColumnBlockFunction() = default;
    ColumnBlockFunction(function_type function) : function{function} {}

    void operator()(std::span<const row_id_t> ids, const ColumnOperandBatch<ARGS> &...operands,
                    MutableColumnBatch<RESULT> output) const {
        std::invoke(function, ids, operands..., output);
    }

    friend bool operator==(const ColumnBlockFunction &, const ColumnBlockFunction &) = default;
};

using ColumnBlockFunctionEntryPoint = void (*)(Scalar<>, std::span<const row_id_t>,
                                                std::span<const ColumnOperandBatch<>>,
                                                MutableColumnBatch<>);

struct ColumnBlockFunctionAdapter {
    ColumnBlockFunctionEntryPoint call{};
    std::size_t result_size{};
    std::size_t result_alignment{};
    bool packed_result{};
};

} // namespace bg::frame

namespace bg {
template <> ScalarDispatch<frame::ColumnBlockFunctionAdapter> &
ScalarDispatch<frame::ColumnBlockFunctionAdapter>::instance() noexcept;
} // namespace bg

namespace bg::frame {

class ColumnBlockFunctionType;
const ColumnBlockFunctionType &column_block_function_type(const ScalarType &result_type,
                                                           std::span<const ScalarType *const> operand_types);

template <cell_type RESULT, cell_type... ARGS> struct ColumnBlockFunctionAdapterFactory {
    using Function = ColumnBlockFunction<RESULT, ARGS...>;

    template <cell_type ARG>
    static ColumnOperandBatch<ARG> operand(const ColumnOperandBatch<> &source) {
        if (!source.is_column())
            return ColumnOperandBatch<ARG>{*static_cast<const ARG *>(source.scalar())};
        const auto &column = source.column();
        return ColumnOperandBatch<ARG>{ColumnBatch<ARG>{column.first_slot, column.size,
                                                        static_cast<typename ColumnValueTraits<ARG>::pointer>(
                                                            column.values),
                                                        column.value_bit_offset, column.valid}};
    }

    template <std::size_t... INDEX>
    static void invoke(Scalar<> value, std::span<const row_id_t> rows,
                       std::span<const ColumnOperandBatch<>> operands,
                       MutableColumnBatch<> output, std::index_sequence<INDEX...>) {
        const auto function = Scalar<Function>::from(value).value();
        const auto typed_operands = std::tuple{operand<ARGS>(operands[INDEX])...};
        std::apply(
            [&](const auto &...typed) {
                auto typed_output = [&] {
                    if constexpr (std::same_as<RESULT, bool>) {
                        auto *values = static_cast<std::uint8_t *>(output.values);
                        const auto value_bytes = (output.size + 7) / 8;
                        std::memset(values, 0, value_bytes);
                        return MutableColumnBatch<bool>{std::span<std::uint8_t>{values, value_bytes}, output.size, 0,
                                                        output.validity};
                    } else {
                        auto *values = static_cast<RESULT *>(output.values);
                        std::uninitialized_default_construct_n(values, output.size);
                        return MutableColumnBatch<RESULT>{std::span<RESULT>{values, output.size}, output.validity};
                    }
                }();
                function.function(rows, typed...,
                                  typed_output);
            },
            typed_operands);
    }

    static void call(Scalar<> value, std::span<const row_id_t> rows,
                     std::span<const ColumnOperandBatch<>> operands, MutableColumnBatch<> output) {
        invoke(value, rows, operands, output, std::index_sequence_for<ARGS...>{});
    }

};

template <cell_type RESULT, cell_type... ARGS> void register_column_block_function() {
    using Factory = ColumnBlockFunctionAdapterFactory<RESULT, ARGS...>;
    const auto type = std::array<const ScalarType *, sizeof...(ARGS)>{&cpp_scalar_type<ARGS>()...};
    const auto &semantic_type = column_block_function_type(cpp_scalar_type<RESULT>(), type);
    static_cast<void>(ScalarDispatch<ColumnBlockFunctionAdapter>::instance().add(
        semantic_type,
        ColumnBlockFunctionAdapter{&Factory::call, sizeof(RESULT), alignof(RESULT), packed_column_value<RESULT>}));
}

// Compiles an expression bundle selected by BG and returns the expression node's concrete output.
WiringPoint materialize_column_expression(WiringPoint result);

namespace details {
bool is_column_expression_bundle_type(const Type &type);
} // namespace details

class ColumnBlockFunctionType final : public ScalarType {
  public:
    ColumnBlockFunctionType(const ScalarType &result_type, std::vector<const ScalarType *> operand_types)
        : result_type_{result_type}, operand_types_{std::move(operand_types)} {}

    void format_name(std::string &result) const override;
    const ScalarType &result_type() const noexcept { return result_type_; }
    std::span<const ScalarType *const> operand_types() const noexcept { return operand_types_; }
    bool is_resolved() const noexcept override {
        return result_type_.is_resolved() &&
               std::ranges::all_of(operand_types_, [](const auto *type) { return type->is_resolved(); });
    }
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType &result_type_;
    std::vector<const ScalarType *> operand_types_;
};

// Extension-facing schemas preserve immediate operand declarations; they do not recursively embed upstream schemas.
template <typename T> struct column_expression_cell;
template <typename T, auto... Properties> struct column_expression_cell<TS<T, Properties...>> { using type = T; };
template <typename T, auto... Properties> struct column_expression_cell<TSC<T, Properties...>> { using type = T; };
template <typename T> using column_expression_cell_t = typename column_expression_cell<T>::type;

template <typename ARG, cell_type RESULT> struct UnaryColumnExpression {
    template <template <typename, auto...> typename Field> struct schema {
        Field<ARG> arg;
        Field<TS<ColumnBlockFunction<RESULT, column_expression_cell_t<ARG>>, TimeSeriesGuarantee::const_>> compute;
    };
};

template <typename LHS, typename RHS, cell_type RESULT> struct BinaryColumnExpression {
    template <template <typename, auto...> typename Field> struct schema {
        Field<LHS> lhs;
        Field<RHS> rhs;
        Field<TS<ColumnBlockFunction<RESULT, column_expression_cell_t<LHS>, column_expression_cell_t<RHS>>,
                 TimeSeriesGuarantee::const_>> compute;
    };
};

namespace details {

template <cell_type ARG> WiringPoint column_expression_operand_point(WiringPoint point) {
    point = bg::details::WiringRecord::point(*point.record()->source_identity());
    if (TS<ARG>::type().accepts(point.type()))
        return static_cast<WiringPoint>(wp<TS<ARG>>{point});
    if (TSC<ARG>::type().accepts(point.type()))
        return static_cast<WiringPoint>(wp<TSC<ARG>>{point});
    throw WiringError{"column expression operand does not match its callback cell type"};
}

template <cell_type... ARGS>
WiringPoint column_expression_bundle_point(WiringPoint compute, std::array<WiringPoint, sizeof...(ARGS)> operands) {
    auto fields = std::vector<WiringArgument>{};
    fields.reserve(sizeof...(ARGS) + 1);
    auto index = std::size_t{};
    ([&] {
        const auto name = sizeof...(ARGS) == 1 ? std::string_view{"arg"}
                                                : index == 0 ? std::string_view{"lhs"} : std::string_view{"rhs"};
        fields.emplace_back(util::symbol{name}, column_expression_operand_point<ARGS>(operands[index]));
        ++index;
    }(), ...);
    fields.emplace_back(util::symbol{"compute"}, compute);
    return bg::bundle(std::span<const WiringArgument>{fields});
}

} // namespace details

// Creates one deferred column expression from a typed callback and its BG operand points.
template <cell_type RESULT, cell_type... ARGS, typename... OPERANDS>
wp<TSC<RESULT>> make_column_expression(ColumnBlockFunction<RESULT, ARGS...> callback,
                                       wp<OPERANDS>... operands) {
    static_assert(sizeof...(ARGS) == sizeof...(OPERANDS));
    static_assert(sizeof...(ARGS) == 1 || sizeof...(ARGS) == 2);
    using Function = ColumnBlockFunction<RESULT, ARGS...>;
    const auto operand_points = std::array<WiringPoint, sizeof...(OPERANDS)>{static_cast<WiringPoint>(operands)...};
    auto &wiring = operand_points.front().record()->wiring();
    auto result = wiring.template create_wp<TSC<RESULT>>(operands...);
    const auto compute = operators::const_(ScalarValue<Function>{callback})
                             .template as<TS<Function, TimeSeriesGuarantee::const_>>();
    const auto expression_builder = [compute](wp<OPERANDS>... input) {
        const auto input_points = std::array<WiringPoint, sizeof...(OPERANDS)>{static_cast<WiringPoint>(input)...};
        return details::column_expression_bundle_point<ARGS...>(static_cast<WiringPoint>(compute), input_points);
    };
    const auto concrete_builder = [result](wp<OPERANDS>...) { return materialize_column_expression(result); };
    const auto expression_target = expression_builder(operands...);
    using ExpressionBuilder = decltype(expression_builder);
    auto expression_signature =
        bg::details::make_graph_input_signature<ExpressionBuilder>(util::symbol{"materialisation"});
    auto erased_expression_builder = [expression_builder](std::span<const WiringArgument> arguments) {
        return bg::details::invoke_graph_function(expression_builder, arguments);
    };
    static_cast<WiringPoint>(result).add_materialisation_option(
        static_cast<const TimeSeriesType &>(expression_target.type()), 0.0, std::move(expression_signature),
        WiringMaterialisationBuilder{std::move(erased_expression_builder)});
    result.template materialisation_option<TSC<RESULT>>(concrete_builder, 1.0);
    return wp<TSC<RESULT>>{result};
}

class Expression;

namespace details {
struct ExpressionNode;
using ExpressionColumnResolver = std::function<WiringArgument(util::symbol)>;

WiringPoint bind_expression(const Expression &expression, std::span<const WiringArgument> bindings,
                           const ExpressionColumnResolver &column_resolver);
} // namespace details

// An immutable wiring expression. The tree owns scalar captures and retains graph-bound WiringPoints. A retained
// WiringPoint requires its GraphWiring to remain alive through binding and graph compilation. Binding resolves names
// and invokes BG operators; the expression is not a runtime scalar or evaluator.
class Expression {
  private:
    explicit Expression(std::shared_ptr<const details::ExpressionNode> node) noexcept : node_{std::move(node)} {}

    std::shared_ptr<const details::ExpressionNode> node_;

    friend Expression col(util::symbol name);
    friend Expression param(util::symbol name);
    friend Expression capture(WiringArgument argument);
    friend Expression apply(const WiringTemplate &operation, std::span<const Expression> operands);
    friend WiringPoint details::bind_expression(const Expression &, std::span<const WiringArgument>,
                                                const details::ExpressionColumnResolver &);
};

Expression col(util::symbol name);
Expression col(std::string_view name);
Expression param(util::symbol name);
Expression param(std::string_view name);

// Construct one operation node. The operation must outlive binding and graph compilation that use the expression.
// The node retains the operation identity and shared operand nodes.
Expression apply(const WiringTemplate &operation, std::span<const Expression> operands);

// Capture a graph argument or scalar value in an expression. Scalar data is copied into owned storage.
Expression capture(WiringArgument argument);

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression capture(Argument &&argument) {
    return capture(WiringArgument{std::forward<Argument>(argument)});
}

Expression operator+(const Expression &lhs, const Expression &rhs);
Expression operator-(const Expression &lhs, const Expression &rhs);
Expression operator==(const Expression &lhs, const Expression &rhs);
Expression operator!=(const Expression &lhs, const Expression &rhs);
Expression operator<(const Expression &lhs, const Expression &rhs);
Expression operator<=(const Expression &lhs, const Expression &rhs);
Expression operator>(const Expression &lhs, const Expression &rhs);
Expression operator>=(const Expression &lhs, const Expression &rhs);

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator+(const Expression &lhs, Argument &&rhs) {
    return lhs + capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator+(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) + rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator-(const Expression &lhs, Argument &&rhs) {
    return lhs - capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator-(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) - rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator==(const Expression &lhs, Argument &&rhs) {
    return lhs == capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator==(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) == rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator!=(const Expression &lhs, Argument &&rhs) {
    return lhs != capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator!=(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) != rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator<(const Expression &lhs, Argument &&rhs) {
    return lhs < capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator<(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) < rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator<=(const Expression &lhs, Argument &&rhs) {
    return lhs <= capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator<=(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) <= rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator>(const Expression &lhs, Argument &&rhs) {
    return lhs > capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator>(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) > rhs;
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator>=(const Expression &lhs, Argument &&rhs) {
    return lhs >= capture(std::forward<Argument>(rhs));
}

template <typename Argument>
    requires bg::details::graph_code_argument<Argument>
Expression operator>=(Argument &&lhs, const Expression &rhs) {
    return capture(std::forward<Argument>(lhs)) >= rhs;
}

// Bind explicit column names and parameters. Every supplied name must be used by the expression.
template <typename... Arguments>
    requires(bg::details::graph_code_argument<Arguments> && ...)
wp<> bind_expression(const Expression &expression, Arguments &&...arguments) {
    auto bindings = bg::details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
    return wp<>{details::bind_expression(expression, bindings, {})};
}

} // namespace bg::frame

namespace bg {

template <frame::cell_type RESULT, frame::cell_type... ARGS>
struct scalar_type_traits<frame::ColumnBlockFunction<RESULT, ARGS...>> {
    static const ScalarType &type() {
        const auto operand_types = std::array<const ScalarType *, sizeof...(ARGS)>{&cpp_scalar_type<ARGS>()...};
        return frame::column_block_function_type(cpp_scalar_type<RESULT>(), operand_types);
    }
};

template <frame::cell_type RESULT, frame::cell_type... ARGS>
struct scalar_declaration_traits<frame::ColumnBlockFunction<RESULT, ARGS...>> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = true;
};

} // namespace bg
