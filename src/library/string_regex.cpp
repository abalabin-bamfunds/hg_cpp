#include "registration.hpp"
#include "utf8.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/string_builder.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <unicode/regex.h>
#include <unicode/stringpiece.h>
#include <unicode/utext.h>
#include <unicode/utypes.h>
#include <unicode/unistr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::operators::details {

struct PreparedRegexPattern {
    std::string text;
    std::unique_ptr<const icu::RegexPattern> compiled;
    std::size_t capture_count;
};

struct RegexReplacementPiece {
    std::size_t offset;
    std::size_t length;
    std::int32_t group;
    bool is_group;
};

struct PreparedRegexReplacement {
    std::shared_ptr<const PreparedRegexPattern> pattern;
    std::string text;
    std::string literal;
    std::vector<RegexReplacementPiece> pieces;
};

using PreparedRegexPatternHandle = Opaque<std::shared_ptr<const PreparedRegexPattern>>;
using PreparedRegexReplacementHandle = Opaque<std::shared_ptr<const PreparedRegexReplacement>>;

} // namespace bg::operators::details

namespace bg {

template <> struct scalar_storage_traits<operators::details::PreparedRegexPatternHandle> {
    using stored_type = std::shared_ptr<const operators::details::PreparedRegexPattern>;
    using value_type = const stored_type &;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool supports_alternative_layouts = false;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = true;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value; }
    static std::size_t hash(value_type value) { return std::hash<std::string_view>{}(value->text); }
    static bool equals(value_type left, value_type right) { return left->text == right->text; }

    template <typename Source>
    static void construct(stored_type *destination, Source &&value, allocator &) {
        std::construct_at(destination, std::forward<Source>(value));
    }

    static void assign(stored_type &destination, const stored_type &value, allocator &) { destination = value; }
    static void destroy(stored_type *value, allocator &) noexcept { std::destroy_at(value); }
};

template <> struct scalar_storage_traits<operators::details::PreparedRegexReplacementHandle> {
    using stored_type = std::shared_ptr<const operators::details::PreparedRegexReplacement>;
    using value_type = const stored_type &;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool supports_alternative_layouts = false;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = true;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value; }
    static std::size_t hash(value_type value) {
        auto result = std::hash<std::string_view>{}(value->pattern->text);
        return details::combine_scalar_hash(result, std::hash<std::string_view>{}(value->text));
    }
    static bool equals(value_type left, value_type right) {
        return left->pattern->text == right->pattern->text && left->text == right->text;
    }

    template <typename Source>
    static void construct(stored_type *destination, Source &&value, allocator &) {
        std::construct_at(destination, std::forward<Source>(value));
    }

    static void assign(stored_type &destination, const stored_type &value, allocator &) { destination = value; }
    static void destroy(stored_type *value, allocator &) noexcept { std::destroy_at(value); }
};

} // namespace bg

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"REGEX_G">;
inline constexpr auto INPUT_G = guarantee_variable<"REGEX_INPUT_G">;

class RegexMatcherState {
  public:
    RegexMatcherState(const PreparedRegexPattern &pattern, UErrorCode &status)
        : matcher_{pattern.compiled->matcher(status)} {}

    static std::string_view text_view(std::string_view text) noexcept { return text; }
    static std::string_view text_view(symbol text) noexcept { return text.text(); }

    template <typename VARIABLE> static std::string_view scalar_text(Scalar<VARIABLE> text) {
        return scalar_text(Scalar<>::from(text));
    }

    static std::string_view scalar_text(Scalar<> text) {
        if (&text.type() == &string_type()) {
            return Scalar<string>::from(text).value();
        }
        return Scalar<symbol>::from(text).value().text();
    }

    static icu::UnicodeString unicode_pattern(std::string_view pattern) {
        static_cast<void>(Utf8View{pattern});
        if (pattern.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::length_error{"regular expression pattern exceeds ICU's 32-bit length limit"};
        }

        const auto *data = pattern.empty() ? "" : pattern.data();
        return icu::UnicodeString::fromUTF8(
            icu::StringPiece{data, static_cast<std::int32_t>(pattern.size())});
    }

    static std::shared_ptr<const PreparedRegexPattern> prepare_pattern(std::string_view pattern) {
        const auto value = unicode_pattern(pattern);
        auto status = U_ZERO_ERROR;
        std::unique_ptr<icu::RegexPattern> compiled{icu::RegexPattern::compile(value, 0, status)};
        if (U_FAILURE(status) || compiled == nullptr) {
            throw_pattern_error(U_FAILURE(status) ? status : U_INTERNAL_PROGRAM_ERROR);
        }
        std::unique_ptr<icu::RegexMatcher> matcher{compiled->matcher(status)};
        if (U_FAILURE(status) || matcher == nullptr) {
            throw_pattern_error(U_FAILURE(status) ? status : U_INTERNAL_PROGRAM_ERROR);
        }
        const auto capture_count = static_cast<std::size_t>(matcher->groupCount());
        return std::make_shared<const PreparedRegexPattern>(
            PreparedRegexPattern{std::string{pattern}, std::move(compiled),
                                 capture_count});
    }

    [[noreturn]] static void throw_pattern_error(UErrorCode status) {
        if (status == U_MEMORY_ALLOCATION_ERROR) {
            throw std::bad_alloc{};
        }
        throw std::invalid_argument{"invalid regular expression: " + std::string{u_errorName(status)}};
    }

    [[noreturn]] static void throw_match_error(UErrorCode status) {
        if (status == U_MEMORY_ALLOCATION_ERROR) {
            throw std::bad_alloc{};
        }
        throw std::runtime_error{"regular expression operation failed: " + std::string{u_errorName(status)}};
    }

    static std::size_t capture_count(std::string_view pattern) {
        return prepare_pattern(pattern)->capture_count;
    }

    template <typename FUNCTION>
    decltype(auto) with_text(std::string_view text, FUNCTION &&function) {
        static_cast<void>(Utf8View{text});
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::length_error{"regular expression input exceeds ICU's 64-bit length limit"};
        }

        UText input = UTEXT_INITIALIZER;
        auto close_input = util::scope_exit{[&input]() noexcept { utext_close(&input); }};
        auto status = U_ZERO_ERROR;
        const auto *data = text.empty() ? "" : text.data();
        if (utext_openUTF8(&input, data, static_cast<std::int64_t>(text.size()), &status) == nullptr ||
            U_FAILURE(status)) {
            throw_match_error(U_FAILURE(status) ? status : U_INTERNAL_PROGRAM_ERROR);
        }

        matcher_->reset(&input);
        auto detach_input = util::scope_exit{[this]() noexcept { matcher_->reset(detached_text_); }};
        return std::forward<FUNCTION>(function)(*matcher_);
    }

    icu::RegexMatcher &matcher() noexcept { return *matcher_; }
    const icu::RegexMatcher &matcher() const noexcept { return *matcher_; }

  private:
    icu::UnicodeString detached_text_;
    std::unique_ptr<icu::RegexMatcher> matcher_;
};

class ReplaceMatcherState : public RegexMatcherState {
  public:
    ReplaceMatcherState(const PreparedRegexReplacement &replacement, UErrorCode &status)
        : RegexMatcherState{*replacement.pattern, status}, replacement_{replacement} {}

    void replace_all(std::string_view text, StringBuilder &output) {
        with_text(text, [&](icu::RegexMatcher &matcher) {
            auto status = U_ZERO_ERROR;
            auto copied_through = std::size_t{};
            while (matcher.find(status)) {
                if (U_FAILURE(status)) {
                    throw_match_error(status);
                }

                const auto start = matcher.start64(status);
                const auto end = matcher.end64(status);
                if (U_FAILURE(status)) {
                    throw_match_error(status);
                }
                const auto match_start = static_cast<std::size_t>(start);
                const auto match_end = static_cast<std::size_t>(end);
                output.append(text.substr(copied_through, match_start - copied_through));
                append_replacement(matcher, text, output, status);
                copied_through = match_end;
            }
            if (U_FAILURE(status)) {
                throw_match_error(status);
            }

            output.append(text.substr(copied_through));
        });
    }

  private:
    void append_replacement(icu::RegexMatcher &matcher, std::string_view text, StringBuilder &output,
                            UErrorCode &status) const {
        for (const auto &piece : replacement_.pieces) {
            if (!piece.is_group) {
                output.append(std::string_view{replacement_.literal.data() + piece.offset, piece.length});
                continue;
            }

            const auto start = matcher.start64(piece.group, status);
            const auto end = matcher.end64(piece.group, status);
            if (U_FAILURE(status)) {
                throw_match_error(status);
            }
            if (start == -1) {
                continue;
            }
            output.append(text.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
        }
    }

    const PreparedRegexReplacement &replacement_;
};

std::shared_ptr<const PreparedRegexReplacement>
prepare_replacement(std::shared_ptr<const PreparedRegexPattern> pattern, std::string_view replacement) {
    static_cast<void>(Utf8View{replacement});
    auto result = std::make_shared<PreparedRegexReplacement>();
    result->pattern = std::move(pattern);
    result->text = replacement;

    static constexpr auto is_octal = [](char value) { return value >= '0' && value <= '7'; };
    static constexpr auto is_digit = [](char value) { return value >= '0' && value <= '9'; };
    const auto reject = [&](std::string_view reason, std::size_t position) -> void {
        throw std::invalid_argument{"replace: " + std::string{reason} + " at position " + std::to_string(position) +
                                    " of replacement '" + std::string{replacement} + "'"};
    };
    const auto flush_literal = [&](std::size_t &literal_start) {
        const auto literal_end = result->literal.size();
        if (literal_end != literal_start) {
            result->pieces.push_back({literal_start, literal_end - literal_start, 0, false});
            literal_start = literal_end;
        }
    };
    const auto append_group = [&](std::int32_t group, std::size_t &literal_start, std::size_t position) {
        if (group < 0 || static_cast<std::size_t>(group) > result->pattern->capture_count) {
            reject("invalid group reference", position);
        }
        flush_literal(literal_start);
        result->pieces.push_back({0, 0, group, true});
    };
    const auto append_code_point = [&](std::uint16_t code_point) {
        if (code_point <= 0x7f) {
            result->literal.push_back(static_cast<char>(code_point));
            return;
        }
        result->literal.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        result->literal.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    };

    auto literal_start = std::size_t{};
    for (auto index = std::size_t{}; index < replacement.size(); ++index) {
        const auto value = replacement[index];
        if (value != '\\') {
            result->literal.push_back(value);
            continue;
        }
        if (++index == replacement.size()) {
            reject("trailing backslash", index - 1);
        }

        const auto escape = replacement[index];
        switch (escape) {
        case '\\': result->literal.push_back('\\'); continue;
        case 'a': result->literal.push_back('\a'); continue;
        case 'b': result->literal.push_back('\b'); continue;
        case 'f': result->literal.push_back('\f'); continue;
        case 'n': result->literal.push_back('\n'); continue;
        case 'r': result->literal.push_back('\r'); continue;
        case 't': result->literal.push_back('\t'); continue;
        case 'v': result->literal.push_back('\v'); continue;
        default: break;
        }

        if (escape == 'g') {
            const auto open = index + 1;
            if (open >= replacement.size() || replacement[open] != '<') {
                reject("missing < in \\g", index);
            }
            const auto close = replacement.find('>', open);
            if (close == std::string_view::npos) {
                reject("missing > in \\g", index);
            }
            const auto name = replacement.substr(open + 1, close - open - 1);
            if (name.empty()) {
                reject("missing group name in \\g<>", index);
            }

            if (std::ranges::all_of(name, is_digit)) {
                auto group = std::int64_t{};
                for (const auto digit : name) {
                    if (group > (std::numeric_limits<std::int32_t>::max() - (digit - '0')) / 10) {
                        reject("group reference out of range", index);
                    }
                    group = group * 10 + (digit - '0');
                }
                append_group(static_cast<std::int32_t>(group), literal_start, index);
            } else {
                if (name.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                    reject("group name is too long", index);
                }
                auto status = U_ZERO_ERROR;
                const auto group = result->pattern->compiled->groupNumberFromName(
                    name.data(), static_cast<std::int32_t>(name.size()), status);
                if (U_FAILURE(status)) {
                    reject("unknown group name", index);
                }
                append_group(group, literal_start, index);
            }

            index = close;
            continue;
        }

        if (escape == '0' || is_digit(escape)) {
            const auto leading_zero = escape == '0';
            const auto three_octal = is_octal(escape) && index + 2 < replacement.size() &&
                                     is_octal(replacement[index + 1]) && is_octal(replacement[index + 2]);
            if (leading_zero || three_octal) {
                const auto escape_start = index;
                auto code_point = static_cast<unsigned>(escape - '0');
                auto digits = 0;
                while (digits < 2 && index + 1 < replacement.size() && is_octal(replacement[index + 1])) {
                    code_point = code_point * 8 + static_cast<unsigned>(replacement[++index] - '0');
                    ++digits;
                }
                if (code_point > 0xff) {
                    reject("octal escape value outside of range 0-0o377", escape_start - 1);
                }
                append_code_point(static_cast<std::uint16_t>(code_point));
                continue;
            }

            auto group = static_cast<std::int32_t>(escape - '0');
            if (index + 1 < replacement.size() && is_digit(replacement[index + 1])) {
                group = group * 10 + static_cast<std::int32_t>(replacement[++index] - '0');
            }
            append_group(group, literal_start, index);
            continue;
        }

        const auto ascii_letter = (escape >= 'a' && escape <= 'z') || (escape >= 'A' && escape <= 'Z');
        if (ascii_letter) {
            reject("bad escape", index - 1);
        }
        result->literal.push_back('\\');
        result->literal.push_back(escape);
    }

    flush_literal(literal_start);
    return result;
}

struct string_regex_overloads {
    using SIZE_DECLARATION = std::remove_cv_t<decltype(SIZE)>;
    using PREPARED_PATTERN = PreparedRegexPatternHandle;
    using PREPARED_REPLACEMENT = PreparedRegexReplacementHandle;

    static std::size_t resolve_capture_size(const WiringArguments &arguments) {
        const auto pattern = RegexMatcherState::scalar_text(*arguments.scalar("pattern"));
        const auto count = RegexMatcherState::capture_count(pattern);
        if (count == 0) {
            throw WiringError{"match_ requires at least one capture group; use matches()"};
        }
        return count;
    }

    static std::size_t resolve_prepared_capture_size(const WiringArguments &arguments) {
        const auto argument = *arguments.scalar("pattern");
        const auto pattern = Scalar<PREPARED_PATTERN>::from(argument);
        return pattern.value()->capture_count;
    }

    template <typename SUBJECT> struct matches_impl {
        Scalar<PREPARED_PATTERN> pattern;
        In<TS<SUBJECT, G>> text;
        Out<TS<bool, G>> output;
        State<RegexMatcherState *> state;

        void start(EvaluationContext *context) const {
            auto status = U_ZERO_ERROR;
            auto memory = context->runtime_graph().allocation();
            auto *matcher = memory.new_object<RegexMatcherState>(*pattern.value(), status);
            if (U_FAILURE(status)) {
                memory.delete_object(matcher);
                RegexMatcherState::throw_pattern_error(status);
            }
            *state = matcher;
        }

        void evaluate() const {
            const auto result = (*state)->with_text(RegexMatcherState::text_view(text.value()),
                                                    [](icu::RegexMatcher &matcher) {
                auto status = U_ZERO_ERROR;
                const auto found = matcher.find(status);
                if (U_FAILURE(status)) {
                    RegexMatcherState::throw_match_error(status);
                }
                return static_cast<bool>(found);
            });
            output.set(result);
        }

        void finalize(EvaluationContext *context) const noexcept {
            auto memory = context->runtime_graph().allocation();
            memory.delete_object(*state);
            *state = nullptr;
        }
    };

    template <typename SUBJECT> struct matches_graph {
        Scalar<REGEX_PATTERN> pattern;
        wp<TS<SUBJECT, G>> text;

        wp<TS<bool, G>> wire() const {
            auto prepared = ScalarValue<PREPARED_PATTERN>{
                RegexMatcherState::prepare_pattern(RegexMatcherState::scalar_text(pattern))};
            static const auto implementation = node<matches_impl<SUBJECT>>();
            return implementation(std::move(prepared), text);
        }
    };

    overload<matches> matches_string{graph<matches_graph<string>>()};
    overload<matches> matches_symbol{graph<matches_graph<symbol>>()};
    overload<matches> matches_scalars{graph([](Scalar<REGEX_PATTERN> pattern, Scalar<REGEX_TEXT> text)
                                                -> wp<TS<bool, TimeSeriesGuarantee::const_>> {
        return wp<TS<bool, TimeSeriesGuarantee::const_>>{operators::matches(pattern, operators::const_(text))};
    })};

    template <typename SUBJECT> struct match_impl {
        Scalar<PREPARED_PATTERN> pattern;
        In<TS<SUBJECT, INPUT_G>> text;
        Out<TSL<TS<string, G>, SIZE, G>> output;
        State<RegexMatcherState *> state;

        static TimeSeriesGuarantee resolve(decltype(G), const WiringArguments &arguments) {
            const auto &text_type = type_cast<TimeSeriesType>(arguments.resolved_type("text"));
            return weakest(*text_type.guarantee().value(), TimeSeriesGuarantee::final);
        }

        static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &arguments) {
            return string_regex_overloads::resolve_prepared_capture_size(arguments);
        }

        void start(EvaluationContext *context) const {
            auto status = U_ZERO_ERROR;
            auto memory = context->runtime_graph().allocation();
            auto *matcher = memory.new_object<RegexMatcherState>(*pattern.value(), status);
            if (U_FAILURE(status)) {
                memory.delete_object(matcher);
                RegexMatcherState::throw_pattern_error(status);
            }
            *state = matcher;
        }

        void evaluate() const {
            const auto subject = RegexMatcherState::scalar_text(text.value_view());
            (*state)->with_text(subject, [&](icu::RegexMatcher &matcher) {
                auto status = U_ZERO_ERROR;
                if (!matcher.find(status)) {
                    if (U_FAILURE(status)) {
                        RegexMatcherState::throw_match_error(status);
                    }
                    output.invalidate();
                    return;
                }
                if (U_FAILURE(status)) {
                    RegexMatcherState::throw_match_error(status);
                }

                for (auto group = std::int32_t{1}; group <= matcher.groupCount(); ++group) {
                    const auto start = matcher.start64(group, status);
                    const auto end = matcher.end64(group, status);
                    if (U_FAILURE(status)) {
                        RegexMatcherState::throw_match_error(status);
                    }
                    const auto index = static_cast<std::size_t>(group - 1);
                    if (start == -1) {
                        output[index].invalidate();
                        continue;
                    }
                    output[index].set(subject.substr(static_cast<std::size_t>(start),
                                                     static_cast<std::size_t>(end - start)));
                }
            });
        }

        void finalize(EvaluationContext *context) const noexcept {
            auto memory = context->runtime_graph().allocation();
            memory.delete_object(*state);
            *state = nullptr;
        }
    };

    template <typename SUBJECT> struct match_graph {
        Scalar<REGEX_PATTERN> pattern;
        wp<TS<SUBJECT, INPUT_G>> text;

        static TimeSeriesGuarantee resolve(decltype(G), const WiringArguments &arguments) {
            const auto &text_type = type_cast<TimeSeriesType>(arguments.resolved_type("text"));
            return weakest(*text_type.guarantee().value(), TimeSeriesGuarantee::final);
        }

        static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &arguments) {
            return string_regex_overloads::resolve_capture_size(arguments);
        }

        wp<TSL<TS<string, G>, SIZE, G>> wire() const {
            auto prepared = ScalarValue<PREPARED_PATTERN>{
                RegexMatcherState::prepare_pattern(RegexMatcherState::scalar_text(pattern))};
            static const auto implementation = node<match_impl<SUBJECT>>();
            return implementation(std::move(prepared), text);
        }
    };

    overload<match_> match_string{graph<match_graph<string>>()};
    overload<match_> match_symbol{graph<match_graph<symbol>>()};
    struct match_scalars_graph {
        Scalar<REGEX_PATTERN> pattern;
        Scalar<REGEX_TEXT> text;

        static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &arguments) {
            return string_regex_overloads::resolve_capture_size(arguments);
        }

        wp<TSL<TS<string, TimeSeriesGuarantee::final>, SIZE, TimeSeriesGuarantee::final>> wire() const {
            auto prepared = ScalarValue<PREPARED_PATTERN>{
                RegexMatcherState::prepare_pattern(RegexMatcherState::scalar_text(pattern))};
            static const auto implementation = node<match_impl<REGEX_TEXT>>();
            return wp<TSL<TS<string, TimeSeriesGuarantee::final>, SIZE, TimeSeriesGuarantee::final>>{
                static_cast<WiringPoint>(implementation(std::move(prepared), operators::const_(text)))};
        }
    };

    overload<match_> match_scalars{graph<match_scalars_graph>()};

    template <typename SUBJECT> struct replace_impl {
        Scalar<PREPARED_REPLACEMENT> replacement;
        In<TS<SUBJECT, G>> text;
        Out<TS<string, G>> output;
        State<ReplaceMatcherState *> state;

        void start(EvaluationContext *context) const {
            auto status = U_ZERO_ERROR;
            auto memory = context->runtime_graph().allocation();
            auto *matcher = memory.new_object<ReplaceMatcherState>(*replacement.value(), status);
            if (U_FAILURE(status)) {
                memory.delete_object(matcher);
                RegexMatcherState::throw_pattern_error(status);
            }
            *state = matcher;
        }

        void evaluate() const {
            auto builder = StringBuilder{};
            const auto input = RegexMatcherState::text_view(text.value());
            builder.reserve(input.size());
            (*state)->replace_all(input, builder);
            output.set_value(std::move(builder).finish());
        }

        void finalize(EvaluationContext *context) const noexcept {
            auto memory = context->runtime_graph().allocation();
            memory.delete_object(*state);
            *state = nullptr;
        }
    };

    template <typename SUBJECT> struct replace_graph {
        Scalar<REGEX_PATTERN> pattern;
        Scalar<REGEX_REPLACEMENT> replacement;
        wp<TS<SUBJECT, G>> text;

        wp<TS<string, G>> wire() const {
            auto prepared_pattern =
                RegexMatcherState::prepare_pattern(RegexMatcherState::scalar_text(pattern));
            auto prepared = ScalarValue<PREPARED_REPLACEMENT>{
                prepare_replacement(std::move(prepared_pattern), RegexMatcherState::scalar_text(replacement))};
            static const auto implementation = node<replace_impl<SUBJECT>>();
            return implementation(std::move(prepared), text);
        }
    };

    overload<replace> replace_string{graph<replace_graph<string>>()};
    overload<replace> replace_symbol{graph<replace_graph<symbol>>()};
    overload<replace> replace_scalars{graph([](Scalar<REGEX_PATTERN> pattern,
                                               Scalar<REGEX_REPLACEMENT> replacement,
                                               Scalar<REGEX_TEXT> text)
                                                -> wp<TS<string, TimeSeriesGuarantee::const_>> {
        return wp<TS<string, TimeSeriesGuarantee::const_>>{
            operators::replace(pattern, replacement, operators::const_(text))};
    })};
};

} // namespace

void register_string_regex_operators() {
    register_overloads<string_regex_overloads>();
}

} // namespace bg::operators::details
