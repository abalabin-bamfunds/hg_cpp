#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/string_builder.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"STRING_COLLECTION_G">;
using SIZE_DECLARATION = std::remove_cv_t<decltype(SIZE)>;

class SplitText {
  public:
    template <typename TEXT, typename SEPARATOR>
    static std::vector<std::string_view> parts(const TEXT &text, const SEPARATOR &separator, i64 maxsplit,
                                               std::size_t maximum_parts = std::dynamic_extent) {
        const auto text_view = view(text);
        const auto separator_view = view(separator);
        if (separator_view.empty()) {
            throw std::invalid_argument{"split separator must not be empty"};
        }

        auto split_limit = std::numeric_limits<std::size_t>::max();
        if (maxsplit >= 0) {
            const auto requested_limit = static_cast<std::uint64_t>(maxsplit);
            if (requested_limit < split_limit) {
                split_limit = static_cast<std::size_t>(requested_limit);
            }
        }
        if (maximum_parts != std::dynamic_extent && maximum_parts - 1 < split_limit) {
            split_limit = maximum_parts - 1;
        }

        auto result = std::vector<std::string_view>{};
        auto start = std::size_t{};
        auto split_count = std::size_t{};
        while (split_count < split_limit) {
            const auto position = text_view.find(separator_view, start);
            if (position == std::string_view::npos) {
                break;
            }

            result.push_back(text_view.substr(start, position - start));
            start = position + separator_view.size();
            ++split_count;
        }
        result.push_back(text_view.substr(start));
        return result;
    }

  private:
    static std::string_view view(std::string_view text) { return text; }
    static std::string_view view(const symbol &text) { return text.text(); }
};

struct string_collection_overloads {
    static std::string_view separator_text(std::string_view separator) { return separator; }
    static std::string_view separator_text(symbol separator) { return separator.text(); }

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

    static std::size_t resolve_fixed_size(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(bg::details::type_parameter<OUT>());
        const auto *requested = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        const auto *result = requested == nullptr || *requested == nullptr
                                 ? nullptr
                                 : type_cast<TimeSeriesType>(*requested);
        const auto *list = result == nullptr ? nullptr : type_cast<TimeSeriesListType>(&result->target_type());
        return list == nullptr ? std::dynamic_extent : list->item_count();
    }

    template <typename ITEM, typename VALUE>
    static void append_join_item(StringBuilder &builder, const VALUE &value) {
        if constexpr (std::same_as<ITEM, string>) {
            builder.append(Scalar<string>::from(value).value());
        } else {
            builder.append(Scalar<symbol>::from(value).value().text());
        }
    }

    template <typename VALUE> static void append_join_value(StringBuilder &builder, const VALUE &value) {
        if (&value.type() == &string_type()) {
            builder.append(Scalar<string>::from(value).value());
        } else {
            builder.append(Scalar<symbol>::from(value).value().text());
        }
    }

    static RequirementResult require_fixed_output(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(bg::details::type_parameter<OUT>());
        const auto *requested = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        const auto *result = requested == nullptr || *requested == nullptr
                                 ? nullptr
                                 : type_cast<TimeSeriesType>(*requested);
        const auto *list = result == nullptr ? nullptr : type_cast<TimeSeriesListType>(&result->target_type());
        if (list == nullptr || list->item_count() == std::dynamic_extent) {
            return reject("fixed split requires an explicit fixed TSL result type");
        }
        if (list->item_count() == 0) {
            return reject("fixed split requires at least one output item");
        }

        const auto *item = type_cast<TimeSeriesValueType>(&list->item_type());
        if (item == nullptr || &item->value_type() != &string_type() || item->contains_reference()) {
            return reject("fixed split requires string time series items");
        }
        if (!result->accepts(*arguments.resolved_output_type())) {
            return reject("the requested fixed split result type is incompatible with the resolved output type");
        }
        return accept;
    }

    static RequirementResult require_list_output(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(bg::details::type_parameter<OUT>());
        if (mapping == nullptr) {
            return accept;
        }

        const auto *requested_mapping = std::get_if<const Type *>(mapping);
        const auto *requested = requested_mapping == nullptr || *requested_mapping == nullptr
                                    ? nullptr
                                    : type_cast<TimeSeriesType>(*requested_mapping);
        if (requested == nullptr || arguments.resolved_output_type() == nullptr ||
            !requested->accepts(*arguments.resolved_output_type())) {
            return reject("the requested split result type is incompatible with the resolved output type");
        }
        return accept;
    }

    template <typename TEXT, typename SEPARATOR>
    static void split_list_maxsplit(TEXT text, SEPARATOR separator, i64 maxsplit,
                                    const Out<TS<List<string>, G>> &output) {
        const auto pieces = SplitText::parts(text, separator, maxsplit);
        output.set_value(ScalarValue<List<string>>{pieces});
    }

    template <typename TEXT, typename SEPARATOR>
    static void split_fixed_maxsplit(TEXT text, SEPARATOR separator, i64 maxsplit,
                                     const Out<TSL<TS<string, G>, SIZE, G>> &output) {
        const auto count = output.size();
        const auto pieces = SplitText::parts(text, separator, maxsplit, count);
        if (pieces.size() < count) {
            throw std::invalid_argument{"split produced fewer pieces than the fixed output size"};
        }
        for (auto index = std::size_t{}; index < count; ++index) {
            output[index].set(pieces[index]);
        }
    }

    // Split.
    overload<split> split_list_default_string_string{node(
        eval = [](std::string_view text, std::string_view separator, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_default_string_symbol{node(
        eval = [](std::string_view text, symbol separator, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_default_symbol_string{node(
        eval = [](symbol text, std::string_view separator, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_default_symbol_symbol{node(
        eval = [](symbol text, symbol separator, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_maxsplit_string_string{node(
        eval = [](std::string_view text, std::string_view separator, i64 maxsplit,
                 Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_maxsplit_string_symbol{node(
        eval = [](std::string_view text, symbol separator, i64 maxsplit, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_maxsplit_symbol_string{node(
        eval = [](symbol text, std::string_view separator, i64 maxsplit, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};
    overload<split> split_list_maxsplit_symbol_symbol{node(
        eval = [](symbol text, symbol separator, i64 maxsplit, Out<TS<List<string>, G>> output) {
            split_list_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, require = require_list_output)};

    overload<split> split_fixed_default_string_string{node(
        eval = [](std::string_view text, std::string_view separator, Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_default_string_symbol{node(
        eval = [](std::string_view text, symbol separator, Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_default_symbol_string{node(
        eval = [](symbol text, std::string_view separator, Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_default_symbol_symbol{node(
        eval = [](symbol text, symbol separator, Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, i64{-1}, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_maxsplit_string_string{node(
        eval = [](std::string_view text, std::string_view separator, i64 maxsplit,
                 Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_maxsplit_string_symbol{node(
        eval = [](std::string_view text, symbol separator, i64 maxsplit,
                 Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_maxsplit_symbol_string{node(
        eval = [](symbol text, std::string_view separator, i64 maxsplit,
                 Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};
    overload<split> split_fixed_maxsplit_symbol_symbol{node(
        eval = [](symbol text, symbol separator, i64 maxsplit, Out<TSL<TS<string, G>, SIZE, G>> output) {
            split_fixed_maxsplit(text, separator, maxsplit, output);
        },
        resolve<decltype(G)> = resolve_output, resolve<SIZE_DECLARATION> = resolve_fixed_size,
        require = require_fixed_output)};

    // Join.
    template <typename SEPARATOR> struct join_list_impl {
        In<TS<List<TEXT>, G>> values;
        SEPARATOR separator;
        Out<TS<string, G>> output;

        static TimeSeriesGuarantee resolve(decltype(G), const WiringArguments &arguments) {
            return resolve_output(arguments);
        }

        void evaluate() const {
            const auto items = values.value_view();
            const auto text = separator_text(separator);
            auto builder = StringBuilder{};
            for (auto index = std::size_t{}; index < items.size(); ++index) {
                if (index != 0) {
                    builder.append(text);
                }
                const auto item = items[index];
                append_join_value(builder, item);
            }
            output.set_value(std::move(builder).finish());
        }
    };

    instantiate_overloads<join, join_list_impl, std::string_view, symbol> join_list;

    template <typename ITEM>
    static void join_scalar_list(Scalar<List<ITEM>> values, std::string_view separator,
                                 const Out<TS<string, G>> &output) {
        auto builder = StringBuilder{};
        for (auto index = std::size_t{}; index < values.size(); ++index) {
            if (index != 0) {
                builder.append(separator);
            }
            append_join_item<ITEM>(builder, values[index]);
        }
        output.set_value(std::move(builder).finish());
    }

    overload<join> join_scalar_list_string_string{node(
        eval = [](Scalar<List<string>> values, std::string_view separator, Out<TS<string, G>> output) {
            join_scalar_list(values, separator, output);
        },
        resolve<decltype(G)> = resolve_output)};
    overload<join> join_scalar_list_string_symbol{node(
        eval = [](Scalar<List<string>> values, symbol separator, Out<TS<string, G>> output) {
            join_scalar_list(values, separator_text(separator), output);
        },
        resolve<decltype(G)> = resolve_output)};
    overload<join> join_scalar_list_symbol_string{node(
        eval = [](Scalar<List<symbol>> values, std::string_view separator, Out<TS<string, G>> output) {
            join_scalar_list(values, separator, output);
        },
        resolve<decltype(G)> = resolve_output)};
    overload<join> join_scalar_list_symbol_symbol{node(
        eval = [](Scalar<List<symbol>> values, symbol separator, Out<TS<string, G>> output) {
            join_scalar_list(values, separator_text(separator), output);
        },
        resolve<decltype(G)> = resolve_output)};

    template <typename SEPARATOR> struct join_optional_list_impl {
        In<TS<List<TEXT>, G>, optional> values;
        SEPARATOR separator;
        Out<TS<string, TimeSeriesGuarantee::ordinary>> output;

        void evaluate() const {
            auto builder = StringBuilder{};
            if (values.valid()) {
                const auto items = values.value_view();
                const auto text = separator_text(separator);
                for (auto index = std::size_t{}; index < items.size(); ++index) {
                    if (index != 0) {
                        builder.append(text);
                    }
                    const auto item = items[index];
                    append_join_value(builder, item);
                }
            }
            output.set_value(std::move(builder).finish());
        }
    };

    instantiate_overloads<join, join_optional_list_impl, std::string_view, symbol> join_optional_list;

    template <typename SEPARATOR> struct join_tsl_impl {
        In<TSL<TS<TEXT, G>, SIZE, G>, InputValidity::all_valid> values;
        SEPARATOR separator;
        Out<TS<string, G>> output;

        static TimeSeriesGuarantee resolve(decltype(G), const WiringArguments &arguments) {
            return resolve_output(arguments);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto &values_type = type_cast<TimeSeriesType>(arguments.resolved_type("values"));
            const auto &list = type_cast<TimeSeriesListType>(values_type.target_type());
            if (list.item_count() == std::dynamic_extent) {
                return reject("join requires a fixed TSL or a scalar list");
            }
            return accept;
        }

        void evaluate() const {
            const auto text = separator_text(separator);
            auto builder = StringBuilder{};
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                if (index != 0) {
                    builder.append(text);
                }
                const auto item = values[index].value_view();
                append_join_value(builder, item);
            }
            output.set_value(std::move(builder).finish());
        }
    };

    instantiate_overloads<join, join_tsl_impl, std::string_view, symbol> join_tsl;

    template <typename SEPARATOR> struct join_optional_tsl_impl {
        In<TSL<TS<TEXT, G>, SIZE, G>, optional> values;
        SEPARATOR separator;
        Out<TS<string, TimeSeriesGuarantee::ordinary>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            const auto &values_type = type_cast<TimeSeriesType>(arguments.resolved_type("values"));
            const auto &list = type_cast<TimeSeriesListType>(values_type.target_type());
            if (list.item_count() == std::dynamic_extent) {
                return reject("join requires a fixed TSL or a scalar list");
            }
            return accept;
        }

        void evaluate() const {
            auto builder = StringBuilder{};
            auto first = true;
            const auto text = string_collection_overloads::separator_text(separator);
            if (values.valid()) {
                for (auto index = std::size_t{}; index < values.size(); ++index) {
                    const auto value = values[index];
                    if (!value.valid()) {
                        continue;
                    }
                    if (!first) {
                        builder.append(text);
                    }
                    const auto item = value.value_view();
                    string_collection_overloads::append_join_value(builder, item);
                    first = false;
                }
            }
            output.set_value(std::move(builder).finish());
        }
    };

    instantiate_overloads<join, join_optional_tsl_impl, std::string_view, symbol> join_optional_tsl;
};

} // namespace

void register_string_collection_operators() { register_overloads<string_collection_overloads>(); }

} // namespace bg::operators::details
