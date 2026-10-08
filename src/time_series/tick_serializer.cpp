#include <bg/time_series/tick_serializer.hpp>

namespace bg {

template <> TimeSeriesDispatch<TimeSeriesTickOperations> &
TimeSeriesDispatch<TimeSeriesTickOperations>::instance() noexcept {
    static auto dispatch = TimeSeriesDispatch<TimeSeriesTickOperations>{};
    return dispatch;
}

namespace details {

TickHeader TickHeader::read(util::buffer_reader &reader) {
    const auto raw = reader.read_raw<std::uint8_t>();
    if ((raw & ~known_mask) != 0) {
        throw std::logic_error{"invalid time series tick header"};
    }

    const auto operation = static_cast<TickOperation>(raw & operation_mask);
    const auto valid = (raw & valid_mask) != 0;
    if ((operation == TickOperation::remove || operation == TickOperation::end) && valid) {
        throw std::logic_error{"invalid time series tick header flags"};
    }
    return {operation, valid};
}

void TickHeader::write(ScalarBufferWriteContext &writer) const {
    if ((operation == TickOperation::remove || operation == TickOperation::end) && valid) {
        throw std::logic_error{"invalid time series tick header flags"};
    }

    const std::uint8_t raw = static_cast<std::uint8_t>(operation) | (valid ? valid_mask : 0);
    writer.write_bytes(std::as_bytes(std::span{&raw, std::size_t{1}}));
}

TimeSeriesTickDiscard::TimeSeriesTickDiscard(
    util::buffer_reader &reader, const TimeSeriesDispatch<TimeSeriesTickOperations> &dispatch)
    : reader_{&reader}, dispatch_{&dispatch} {}

void TimeSeriesTickDiscard::apply(const TimeSeriesType &type) {
    const auto header = TickHeader::read(*reader_);
    if (header.operation != TickOperation::tick) {
        throw std::logic_error{"time series tick record does not start with a tick header"};
    }
    if (header.valid) {
        apply_payload(type);
    }
}

void TimeSeriesTickDiscard::apply_payload(const TimeSeriesType &type) {
    type.visit(
        [&](const TimeSeriesValueType *value_type) { discard_scalar(value_type->value_type()); },
        [&](const TimeSeriesListType *list_type) {
            for (;;) {
                const auto item_header = TickHeader::read(*reader_);
                if (item_header.operation == TickOperation::end) {
                    break;
                }
                if (item_header.operation != TickOperation::tick) {
                    throw std::logic_error{"invalid list tick entry"};
                }
                const auto item_id = reader_->read_raw<item_id_t>();
                discard_child(list_type->item_type(item_id), item_header);
            }
        },
        [&](const TimeSeriesBundleType *bundle_type) {
            for (;;) {
                const auto item_header = TickHeader::read(*reader_);
                if (item_header.operation == TickOperation::end) {
                    break;
                }
                if (item_header.operation != TickOperation::tick) {
                    throw std::logic_error{"invalid bundle tick entry"};
                }
                const auto item_id = reader_->read_raw<item_id_t>();
                discard_child(bundle_type->item_type(item_id), item_header);
            }
        },
        [&](const TimeSeriesSetType *set_type) {
            for (;;) {
                const auto entry_header = TickHeader::read(*reader_);
                if (entry_header.operation == TickOperation::end) {
                    break;
                }
                if (entry_header.operation != TickOperation::add &&
                    entry_header.operation != TickOperation::remove) {
                    throw std::logic_error{"invalid TSS tick entry"};
                }
                discard_scalar(set_type->item_type());
            }
        },
        [&](const TimeSeriesDictionaryType *dictionary_type) {
            for (;;) {
                const auto entry_header = TickHeader::read(*reader_);
                if (entry_header.operation == TickOperation::end) {
                    break;
                }
                if (entry_header.operation != TickOperation::add && entry_header.operation != TickOperation::tick &&
                    entry_header.operation != TickOperation::remove) {
                    throw std::logic_error{"invalid TSD tick entry"};
                }
                discard_scalar(dictionary_type->key_type());
                if (entry_header.operation != TickOperation::remove) {
                    discard_child(dictionary_type->value_type(), entry_header);
                }
            }
        },
        [&](const TimeSeriesReferenceType *) {
            throw std::invalid_argument{"retained reference time series ticks are not supported"};
        },
        [&] {
            const auto *operations = dispatch_->find_exact(type);
            if (operations == nullptr || !operations->has_discard()) {
                throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
            }
            operations->discard(*reader_);
        });
}

void TimeSeriesTickDiscard::discard_child(const TimeSeriesType &type, TickHeader header) {
    if (header.operation != TickOperation::add && header.operation != TickOperation::tick) {
        throw std::logic_error{"invalid time series collection entry"};
    }
    if (header.valid) {
        apply_payload(type);
    }
}

void TimeSeriesTickDiscard::discard_scalar(const ScalarType &type) {
    if (const auto *builtin = type_cast<BuiltinScalarType>(&type)) {
        builtin->visit_builtin_type([&]<typename Builtin>() {
            using Stored = scalar_stored_t<Builtin>;
            if constexpr (std::is_trivially_copyable_v<Stored>) {
                reader_->consume(sizeof(Stored));
            } else {
                reader_->skip<ScalarValue<>>();
            }
        });
        return;
    }

    const auto &dispatch = ScalarDispatch<ScalarBufferOperations>::instance();
    const auto *operations = dispatch.find_exact(type);
    if (operations != nullptr && operations->has_discard()) {
        operations->discard(*reader_);
        return;
    }

    reader_->skip<ScalarValue<>>();
}

} // namespace details

} // namespace bg
