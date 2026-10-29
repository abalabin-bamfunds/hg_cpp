#include <bg/scalar/format.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tick_format_operations.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/static_storage.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace bg {
namespace {

template <typename... Arguments>
void write(fmt::format_context &context, fmt::format_string<Arguments...> pattern, Arguments &&...arguments) {
    context.advance_to(fmt::format_to(context.out(), pattern, std::forward<Arguments>(arguments)...));
}

void format_live_payload(const In<TS_T> &input, EvaluationTime now, fmt::format_context &context);
void format_serialized_payload(const TimeSeriesType &type, util::buffer_reader &reader,
                               fmt::format_context &context);
void format_live_child(const In<TS_T> &input, EvaluationTime now, fmt::format_context &context);
void format_serialized_child(const TimeSeriesType &type, details::TickHeader header, util::buffer_reader &reader,
                             fmt::format_context &context);

void format_scalar_value(const ScalarType &type, util::buffer_reader &reader, fmt::format_context &context) {
    const auto read = details::TimeSeriesTickScalarRead::visit(
        reader, type, [&](Scalar<> value) { format_scalar(value, context); });
    if (!read) {
        throw fmt::format_error{"the retained tick scalar could not be read"};
    }
}

class LiveTickFormatter {
  public:
    LiveTickFormatter(EvaluationTime now, fmt::format_context &context) noexcept : now_{now}, context_{&context} {}

    void operator()(const In<TS<SCALAR>> *input) const {
        const auto value = input->value_view();
        format_scalar(Scalar<>::from(value), *context_);
    }

    void operator()(const In<TSL<TS_T, SIZE>> *input) const {
        write(*context_, "{{");
        auto first = true;
        input->for_each_updated([&](item_id_t index) {
            const auto item = (*input)[index];
            append_separator(first);
            write(*context_, "{}: ", index);
            format_live_child(item, now_, *context_);
        });
        write(*context_, "}}");
    }

    void operator()(const In<TSB<SCHEMA>> *input) const {
        write(*context_, "{{");
        auto first = true;
        input->for_each_updated([&](item_id_t index) {
            const auto item = (*input)[index];
            append_separator(first);
            write(*context_, "{:?}: ", input->item_name(index).text());
            format_live_child(item, now_, *context_);
        });
        write(*context_, "}}");
    }

    void operator()(const In<TSS<SCALAR>> *input) const {
        write(*context_, "{{");
        auto first = true;
        input->for_each_added([&](const Scalar<SCALAR> &key) {
            append_separator(first);
            format_scalar(Scalar<>::from(key), *context_);
        });
        input->for_each_removed([&](const Scalar<SCALAR> &key) {
            append_separator(first);
            write(*context_, "REMOVED(");
            format_scalar(Scalar<>::from(key), *context_);
            write(*context_, ")");
        });
        write(*context_, "}}");
    }

    void operator()(const In<TSD<SCALAR, TS_T>> *input) const {
        write(*context_, "{{");
        auto first = true;
        input->for_each_removed([&](const Scalar<SCALAR> &key) {
            append_separator(first);
            format_scalar(Scalar<>::from(key), *context_);
            write(*context_, ": REMOVE");
        });
        input->for_each_added([&](const Scalar<SCALAR> &key) {
            append_separator(first);
            format_scalar(Scalar<>::from(key), *context_);
            write(*context_, ": ");
            format_live_child((*input)[key], now_, *context_);
        });
        input->for_each_updated([&](const Scalar<SCALAR> &key) {
            append_separator(first);
            format_scalar(Scalar<>::from(key), *context_);
            write(*context_, ": ");
            format_live_child((*input)[key], now_, *context_);
        });
        write(*context_, "}}");
    }

    void operator()(const In<REF<TS_T>> *) const {
        throw fmt::format_error{"live reference time series ticks are not supported"};
    }

    void operator()(const In<TS_T> *input) const {
        const auto &dispatch = TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance();
        const auto *operations = dispatch.find_exact(input->runtime_type());
        if (operations == nullptr || operations->live == nullptr) {
            operations = dispatch.find(input->runtime_type().type());
        }
        if (operations == nullptr || operations->live == nullptr) {
            auto name = std::string{};
            input->runtime_type().type().format_name(name);
            throw fmt::format_error{fmt::format("no live tick formatter registered for `{}`", name)};
        }
        operations->live(*input, now_, *context_);
    }

  private:
    void append_separator(bool &first) const {
        if (!first) {
            write(*context_, ", ");
        }
        first = false;
    }

    EvaluationTime now_;
    fmt::format_context *context_;
};

void format_live_child(const In<TS_T> &input, EvaluationTime now, fmt::format_context &context) {
    if (!input.valid()) {
        write(context, "INVALID");
        return;
    }
    format_live_payload(input, now, context);
}

void format_live_payload(const In<TS_T> &input, EvaluationTime now, fmt::format_context &context) {
    const auto formatter = LiveTickFormatter{now, context};
    input.visit(ddv::serial{std::cref(formatter)});
}

class SerializedTickFormatter {
  public:
    SerializedTickFormatter(util::buffer_reader &reader, fmt::format_context &context) noexcept
        : reader_{&reader}, context_{&context} {}

    void operator()(const TimeSeriesValueType *type) const {
        format_scalar_value(type->value_type(), *reader_, *context_);
    }

    void operator()(const TimeSeriesListType *type) const {
        write(*context_, "{{");
        auto first = true;
        for (;;) {
            const auto header = details::TickHeader::read(*reader_);
            if (header.operation == details::TickOperation::end) {
                break;
            }
            if (header.operation != details::TickOperation::tick) {
                throw fmt::format_error{"invalid TSL tick entry"};
            }
            const auto index = reader_->read_raw<item_id_t>();
            append_separator(first);
            write(*context_, "{}: ", index);
            format_serialized_child(type->item_type(index), header, *reader_, *context_);
        }
        write(*context_, "}}");
    }

    void operator()(const TimeSeriesBundleType *type) const {
        write(*context_, "{{");
        auto first = true;
        for (;;) {
            const auto header = details::TickHeader::read(*reader_);
            if (header.operation == details::TickOperation::end) {
                break;
            }
            if (header.operation != details::TickOperation::tick) {
                throw fmt::format_error{"invalid TSB tick entry"};
            }
            const auto index = reader_->read_raw<item_id_t>();
            append_separator(first);
            write(*context_, "{:?}: ", type->item_name(index).text());
            format_serialized_child(type->item_type(index), header, *reader_, *context_);
        }
        write(*context_, "}}");
    }

    void operator()(const TimeSeriesSetType *type) const {
        write(*context_, "{{");
        auto first = true;
        for (;;) {
            const auto header = details::TickHeader::read(*reader_);
            if (header.operation == details::TickOperation::end) {
                break;
            }
            if (header.operation != details::TickOperation::add && header.operation != details::TickOperation::remove) {
                throw fmt::format_error{"invalid TSS tick entry"};
            }
            append_separator(first);
            if (header.operation == details::TickOperation::remove) {
                write(*context_, "REMOVED(");
            }
            format_scalar_value(type->item_type(), *reader_, *context_);
            if (header.operation == details::TickOperation::remove) {
                write(*context_, ")");
            }
        }
        write(*context_, "}}");
    }

    void operator()(const TimeSeriesDictionaryType *type) const {
        write(*context_, "{{");
        auto first = true;
        for (;;) {
            const auto header = details::TickHeader::read(*reader_);
            if (header.operation == details::TickOperation::end) {
                break;
            }
            if (header.operation != details::TickOperation::remove && header.operation != details::TickOperation::add &&
                header.operation != details::TickOperation::tick) {
                throw fmt::format_error{"invalid TSD tick entry"};
            }
            append_separator(first);
            format_scalar_value(type->key_type(), *reader_, *context_);
            write(*context_, ": ");
            if (header.operation == details::TickOperation::remove) {
                write(*context_, "REMOVE");
            } else {
                format_serialized_child(type->value_type(), header, *reader_, *context_);
            }
        }
        write(*context_, "}}");
    }

    void operator()(const TimeSeriesReferenceType *) const {
        throw fmt::format_error{"retained reference time series ticks are not supported"};
    }

    void operator()(const TimeSeriesType *type) const {
        const auto *operations = TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance().find(*type);
        if (operations == nullptr || operations->serialized == nullptr) {
            auto name = std::string{};
            type->format_name(name);
            throw fmt::format_error{fmt::format("no retained tick formatter registered for `{}`", name)};
        }
        operations->serialized(*type, *reader_, *context_);
    }

  private:
    void append_separator(bool &first) const {
        if (!first) {
            write(*context_, ", ");
        }
        first = false;
    }

    util::buffer_reader *reader_;
    fmt::format_context *context_;
};

void format_serialized_child(const TimeSeriesType &type, details::TickHeader header, util::buffer_reader &reader,
                             fmt::format_context &context) {
    if (!header.valid) {
        write(context, "INVALID");
        return;
    }
    format_serialized_payload(type, reader, context);
}

void format_serialized_payload(const TimeSeriesType &type, util::buffer_reader &reader,
                               fmt::format_context &context) {
    const auto formatter = SerializedTickFormatter{reader, context};
    type.visit(ddv::serial{std::cref(formatter), ddv::noop});
}

} // namespace

template <> TimeSeriesDispatch<TimeSeriesTickFormatOperations> &
TimeSeriesDispatch<TimeSeriesTickFormatOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<TimeSeriesDispatch<TimeSeriesTickFormatOperations>>{};
    return dispatch.get();
}

void format_live_tick(const TimeSeriesInputData &input_data, EvaluationTime now, fmt::format_context &context) {
    const auto input = In<TS_T>{input_data};
    if (input.runtime_type().is_reference()) {
        throw fmt::format_error{"live reference time series ticks are not supported"};
    }
    if (input.last_updated() != now || !input.modified()) {
        write(context, "SKIP");
        return;
    }
    if (!input.valid()) {
        write(context, "INVALID");
        return;
    }
    format_live_payload(input, now, context);
}

void format_serialized_tick(const TimeSeriesType &type, const util::buffer_reader &source,
                            fmt::format_context &context) {
    if (type_cast<TimeSeriesReferenceType>(&type) != nullptr) {
        throw fmt::format_error{"retained reference time series ticks are not supported"};
    }
    auto reader = source.fork();
    const auto header = details::TickHeader::read(reader);
    if (header.operation != details::TickOperation::tick) {
        throw fmt::format_error{"retained time series tick does not start with a tick header"};
    }
    if (!header.valid) {
        write(context, "INVALID");
        return;
    }
    format_serialized_payload(type, reader, context);
}

void format_serialized_tick_payload(const TimeSeriesType &type, bool valid, const util::buffer_reader &source,
                                    fmt::format_context &context) {
    if (type_cast<TimeSeriesReferenceType>(&type) != nullptr) {
        throw fmt::format_error{"retained reference time series ticks are not supported"};
    }
    if (!valid) {
        write(context, "INVALID");
        return;
    }

    auto reader = source.fork();
    format_serialized_payload(type, reader, context);
}

} // namespace bg
