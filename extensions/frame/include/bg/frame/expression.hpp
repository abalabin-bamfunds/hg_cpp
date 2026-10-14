#pragma once

#include <bg/frame/column.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace bg::frame {

// Extension-facing writable batch passed to column block functions. Callbacks set one validity bit per output row and
// write values only for valid cells. Bits after the final row are ignored.
template <cell_type T> struct MutableColumnBatch {
    std::span<T> values;
    std::span<std::uint8_t> validity;

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

// Extension-facing typed block callback. The input rows are aligned by row ID; output storage is caller-owned.
template <cell_type T> struct ColumnBlockFunction {
    using batch_type = AlignedColumnBatch<T, T>;
    using function_type = void (*)(const batch_type &, MutableColumnBatch<T>);

    function_type function{};

    void operator()(const batch_type &batch, MutableColumnBatch<T> output) const {
        if (function == nullptr)
            throw std::logic_error{"column block function is empty"};
        if (output.values.size() != batch.ids.size() || output.validity.size() < (batch.ids.size() + 7) / 8)
            throw std::invalid_argument{"column block output layout does not match its input batch"};
        std::invoke(function, batch, output);
    }

    friend bool operator==(const ColumnBlockFunction &, const ColumnBlockFunction &) = default;
};

class ColumnBlockFunctionType final : public ScalarType {
  public:
    explicit ColumnBlockFunctionType(const ScalarType &result_type) noexcept : result_type_{result_type} {}

    void format_name(std::string &result) const override;
    const ScalarType &result_type() const noexcept { return result_type_; }
    bool is_resolved() const noexcept override { return result_type_.is_resolved(); }
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType &result_type_;
};

const ColumnBlockFunctionType &column_block_function_type(const ScalarType &result_type);

// Extension-facing schema for one lazy binary expression. Fields preserve the immediate operand declarations;
// they do not recursively embed upstream expression schemas.
template <typename LHS, typename RHS, cell_type RESULT> struct BinaryColumnExpression {
    template <template <typename, auto...> typename Field> struct schema {
        Field<LHS> lhs;
        Field<RHS> rhs;
        Field<TS<ColumnBlockFunction<RESULT>, TimeSeriesGuarantee::const_>> compute;
    };
};

} // namespace bg::frame

namespace bg {

template <frame::cell_type T> struct scalar_type_traits<frame::ColumnBlockFunction<T>> {
    static const ScalarType &type();
};

template <frame::cell_type T> struct scalar_declaration_traits<frame::ColumnBlockFunction<T>> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = true;
};

extern template const ScalarType &scalar_type_traits<frame::ColumnBlockFunction<i64>>::type();
extern template const ScalarType &scalar_type_traits<frame::ColumnBlockFunction<double>>::type();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::ColumnBlockFunction<i64>>();
extern template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::ColumnBlockFunction<double>>();

} // namespace bg
