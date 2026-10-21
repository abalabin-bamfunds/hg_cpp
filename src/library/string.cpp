#include "registration.hpp"
#include "utf8.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/string_builder.hpp>
#include <bg/type/time_series_type.hpp>

#include <unicode/bytestream.h>
#include <unicode/casemap.h>
#include <unicode/stringpiece.h>
#include <unicode/utypes.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;

class StringBuilderSink final : public icu::ByteSink {
  public:
    explicit StringBuilderSink(StringBuilder &builder) : builder_{builder} {}

    void Append(const char *bytes, std::int32_t length) override {
        builder_.append(std::string_view{bytes, static_cast<std::size_t>(length)});
    }

  private:
    StringBuilder &builder_;
};

struct string_overloads {
    static TimeSeriesGuarantee resolve_output(const WiringArguments &arguments) {
        auto guarantee = TimeSeriesGuarantee::const_;
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            const auto &type = arguments.resolved_type(index);
            if (!type.is_time_series()) {
                continue;
            }

            const auto &input = type_cast<TimeSeriesType>(type);
            guarantee = weakest(guarantee, *input.guarantee().value());
        }
        return guarantee;
    }

    static void repeat(std::string_view text, i64 count, Out<TS<string, G>> &output) {
        auto builder = StringBuilder{};
        if (count > 0 && !text.empty()) {
            const auto repetitions = static_cast<std::uint64_t>(count);
            if (repetitions > std::numeric_limits<std::size_t>::max() / text.size()) {
                throw std::length_error{"repeated string exceeds the maximum size"};
            }

            const auto size = text.size() * static_cast<std::size_t>(repetitions);
            builder.reserve(size);
            for (auto repetition = std::uint64_t{}; repetition < repetitions; ++repetition) {
                builder.append(text);
            }
        }
        output.set_value(std::move(builder).finish());
    }

    static void convert_case(CaseConversion conversion, std::string_view text, Out<TS<string, G>> &output) {
        const auto validated_text = Utf8View{text};
        auto builder = StringBuilder{};
        validated_text.append_case_conversion(conversion, builder);
        output.set_value(std::move(builder).finish());
    }

    // Addition.
    struct add_strings {
        std::string_view lhs;
        std::string_view rhs;
        Out<TS<string, G>> output;

        static TimeSeriesGuarantee resolve(decltype(G), const WiringArguments &arguments) {
            return resolve_output(arguments);
        }

        void evaluate() const {
            if (rhs.size() > std::numeric_limits<std::size_t>::max() - lhs.size()) {
                throw std::length_error{"string concatenation exceeds the maximum size"};
            }

            auto builder = StringBuilder{};
            builder.reserve(lhs.size() + rhs.size());
            builder.append(lhs);
            builder.append(rhs);

            output.set_value(std::move(builder).finish());
        }
    };

    overload<add, add_strings> add_strings;

    struct add_optional_strings {
        std::optional<std::string_view> lhs;
        std::optional<std::string_view> rhs;
        Out<TS<string>> output;

        void evaluate() const {
            const auto lhs_value = lhs.value_or(std::string_view{});
            const auto rhs_value = rhs.value_or(std::string_view{});
            if (rhs_value.size() > std::numeric_limits<std::size_t>::max() - lhs_value.size()) {
                throw std::length_error{"string concatenation exceeds the maximum size"};
            }

            auto builder = StringBuilder{};
            builder.reserve(lhs_value.size() + rhs_value.size());
            builder.append(lhs_value);
            builder.append(rhs_value);
            output.set_value(std::move(builder).finish());
        }
    };

    overload<add, add_optional_strings> add_optional_strings;
    overload<add> add_string_scalars{graph([](std::string_view lhs, std::string_view rhs) {
        auto builder = StringBuilder{};
        builder.reserve(lhs.size() + rhs.size());
        builder.append(lhs);
        builder.append(rhs);
        auto value = std::move(builder).finish();
        return operators::const_(value).as<TS<string, TimeSeriesGuarantee::const_>>();
    })};

    // Multiplication.
    overload<mul> mul_string_i64{node(
        eval = [](std::string_view text, i64 count, Out<TS<string, G>> output) { repeat(text, count, output); },
        resolve<decltype(G)> = resolve_output)};
    overload<mul> mul_i64_string{node(
        eval = [](i64 count, std::string_view text, Out<TS<string, G>> output) { repeat(text, count, output); },
        resolve<decltype(G)> = resolve_output)};
    overload<mul> mul_symbol_i64{node(
        eval = [](symbol text, i64 count, Out<TS<string, G>> output) { repeat(text.text(), count, output); },
        resolve<decltype(G)> = resolve_output)};
    overload<mul> mul_i64_symbol{node(
        eval = [](i64 count, symbol text, Out<TS<string, G>> output) { repeat(text.text(), count, output); },
        resolve<decltype(G)> = resolve_output)};

    // Substring selection.
    overload<substr> substr_with_end{node([](std::string_view text, i64 start, i64 end) {
        return Utf8View{text}.slice(start, end);
    })};
    overload<substr> substr_to_end{node([](std::string_view text, i64 start) {
        return Utf8View{text}.slice(start, std::numeric_limits<i64>::max());
    })};
    overload<substr> substr_symbol_with_end{node([](symbol text, i64 start, i64 end) {
        return Utf8View{text}.slice(start, end);
    })};
    overload<substr> substr_symbol_to_end{node([](symbol text, i64 start) {
        return Utf8View{text}.slice(start, std::numeric_limits<i64>::max());
    })};

    // String indexing.
    overload<bg::operators::getitem> getitem_string_index{node(
        [](std::string_view text, i64 index) { return Utf8View{text}.at(index); })};
    overload<bg::operators::getitem> getitem_symbol_index{node(
        [](symbol text, i64 index) { return Utf8View{text}.at(index); })};

    // Unicode casing.
    overload<lower> lower_string{node(
        eval = [](std::string_view text, Out<TS<string, G>> output) { convert_case(CaseConversion::lower, text, output); },
        resolve<decltype(G)> = resolve_output)};
    overload<lower> lower_symbol{node(
        eval = [](symbol text, Out<TS<string, G>> output) { convert_case(CaseConversion::lower, text.text(), output); },
        resolve<decltype(G)> = resolve_output)};
    overload<upper> upper_string{node(
        eval = [](std::string_view text, Out<TS<string, G>> output) { convert_case(CaseConversion::upper, text, output); },
        resolve<decltype(G)> = resolve_output)};
    overload<upper> upper_symbol{node(
        eval = [](symbol text, Out<TS<string, G>> output) { convert_case(CaseConversion::upper, text.text(), output); },
        resolve<decltype(G)> = resolve_output)};
    overload<casefold> casefold_string{node(
        eval = [](std::string_view text, Out<TS<string, G>> output) { convert_case(CaseConversion::fold, text, output); },
        resolve<decltype(G)> = resolve_output)};
    overload<casefold> casefold_symbol{node(
        eval = [](symbol text, Out<TS<string, G>> output) { convert_case(CaseConversion::fold, text.text(), output); },
        resolve<decltype(G)> = resolve_output)};

    // Equality.
    overload<eq> eq_string{[](std::string_view lhs, std::string_view rhs) { return lhs == rhs; }};
    overload<eq> eq_symbol{[](symbol lhs, symbol rhs) { return lhs == rhs; }};

    // Inequality.
    overload<ne> ne_string{[](std::string_view lhs, std::string_view rhs) { return lhs != rhs; }};
    overload<ne> ne_symbol{[](symbol lhs, symbol rhs) { return lhs != rhs; }};

    // Ordering.
    overload<lt> lt_string{[](std::string_view lhs, std::string_view rhs) { return lhs < rhs; }};
    overload<le> le_string{[](std::string_view lhs, std::string_view rhs) { return lhs <= rhs; }};
    overload<gt> gt_string{[](std::string_view lhs, std::string_view rhs) { return lhs > rhs; }};
    overload<ge> ge_string{[](std::string_view lhs, std::string_view rhs) { return lhs >= rhs; }};
};

} // namespace

void Utf8View::append_case_conversion(CaseConversion conversion, StringBuilder &builder) const {
    if (text_.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error{"Unicode case conversion input exceeds ICU's 32-bit length limit"};
    }

    auto sink = StringBuilderSink{builder};
    auto error = U_ZERO_ERROR;
    const auto source = icu::StringPiece{text_.data(), static_cast<std::int32_t>(text_.size())};

    switch (conversion) {
    case CaseConversion::lower:
        icu::CaseMap::utf8ToLower("", 0, source, sink, nullptr, error);
        break;
    case CaseConversion::upper:
        icu::CaseMap::utf8ToUpper("", 0, source, sink, nullptr, error);
        break;
    case CaseConversion::fold:
        icu::CaseMap::utf8Fold(0, source, sink, nullptr, error);
        break;
    }

    if (U_FAILURE(error)) {
        throw std::runtime_error{"Unicode case conversion failed: " + std::string{u_errorName(error)}};
    }
}

void register_string_operators() {
    register_overloads<string_overloads>();
}

} // namespace bg::operators::details
