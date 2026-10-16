#include <bg/time_series/tick_serializer.hpp>

#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

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

namespace {

enum class LiteralMarker { value, skip, invalid, remove, removed };

LiteralMarker marker(const literal::TimeSeriesLiteral &literal) {
    const auto sentinel = literal::Sentinel::from(literal);
    if (!sentinel) {
        return LiteralMarker::value;
    }

    if (sentinel->name == literal::SKIP.name) {
        if (sentinel->argument != nullptr) {
            throw std::invalid_argument{"SKIP does not accept an argument"};
        }
        return LiteralMarker::skip;
    }
    if (sentinel->name == literal::INVALID.name) {
        if (sentinel->argument != nullptr) {
            throw std::invalid_argument{"INVALID does not accept an argument"};
        }
        return LiteralMarker::invalid;
    }
    if (sentinel->name == literal::REMOVE.name) {
        if (sentinel->argument != nullptr) {
            throw std::invalid_argument{"REMOVE does not accept an argument"};
        }
        return LiteralMarker::remove;
    }
    if (sentinel->name == literal::REMOVED.name) {
        if (sentinel->argument == nullptr) {
            throw std::invalid_argument{"REMOVED requires an argument"};
        }
        return LiteralMarker::removed;
    }
    return LiteralMarker::value;
}

class QueueMessageCodec {
  public:
    QueueMessageCodec(const TimeSeriesRuntimeType &runtime_type, ScalarBufferWriteContext *writer,
                      allocator &memory) noexcept
        : runtime_type_{&runtime_type}, writer_{writer}, memory_{&memory}, projection_{sizeof(QueueMessageHeader)} {}

    bool write_root(const literal::TimeSeriesLiteral &literal) {
        const auto value_marker = marker(literal);
        if (value_marker == LiteralMarker::skip) {
            return false;
        }
        if (value_marker == LiteralMarker::invalid) {
            if (!supports_literal_invalidation(runtime_type_->type())) {
                throw std::invalid_argument{"this time-series type does not support literal invalidation"};
            }
            write_header({TickOperation::tick, false});
            return true;
        }
        if (value_marker != LiteralMarker::value) {
            throw std::invalid_argument{"a root time-series literal cannot use REMOVE or REMOVED"};
        }

        write_header({TickOperation::tick, true});
        write_payload(*runtime_type_, literal);
        return true;
    }

    std::size_t frame_size() const {
        auto result = projection_;
        result.write_bytes(util::details::alignment_padding(result.position(), alignof(QueueMessageHeader)));
        return result.position();
    }

    void pad_to(std::size_t size) {
        if (size < projection_.position()) {
            throw std::logic_error{"queue message projection is smaller than its payload"};
        }
        write_zeroes(size - projection_.position());
    }

  private:
    static bool supports_literal_invalidation(const TimeSeriesType &type) {
        return type_cast<TimeSeriesValueType>(&type) != nullptr || type_cast<TimeSeriesListType>(&type) != nullptr ||
               type_cast<TimeSeriesBundleType>(&type) != nullptr;
    }

    void write_zeroes(std::size_t size) {
        if (size == 0) {
            return;
        }
        if (writer_ != nullptr) {
            const auto zeroes = std::array<std::byte, alignof(QueueMessageHeader)>{};
            while (size != 0) {
                const auto count = std::min(size, zeroes.size());
                writer_->write_bytes(std::span{zeroes}.first(count));
                size -= count;
                projection_.write_bytes(count);
            }
            return;
        }
        projection_.write_bytes(size);
    }

    template <typename T> void write_raw(const T &value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (writer_ != nullptr) {
            bg::details::write_bytes(*writer_, value);
        }
        projection_.write_bytes(sizeof(T));
    }

    void write_header(TickHeader header) {
        if (writer_ != nullptr) {
            header.write(*writer_);
        }
        projection_.write_bytes(sizeof(std::uint8_t));
    }

    static const ScalarBufferOperations *write_operations(const ScalarRuntimeType &runtime_type) {
        const auto &dispatch = ScalarDispatch<ScalarBufferOperations>::instance();
        const auto *runtime_operations = dispatch.find_exact(runtime_type);
        const auto *type_operations = dispatch.find_exact(runtime_type.type());
        auto *selected = static_cast<const ScalarBufferOperations *>(nullptr);
        if (runtime_operations != nullptr && runtime_operations->has_write()) {
            selected = runtime_operations;
        } else if (type_operations != nullptr && type_operations->has_write()) {
            selected = type_operations;
        }
        if (selected != nullptr) {
            if (!selected->has_project()) {
                throw std::invalid_argument{"queue messages require a matching scalar projection operation"};
            }
            if (type_operations == nullptr || !type_operations->has_read() || !type_operations->has_discard()) {
                throw std::invalid_argument{"queue messages require semantic scalar read and discard operations"};
            }
            return selected;
        }

        if (type_operations != nullptr && (type_operations->has_read() || type_operations->has_discard())) {
            throw std::invalid_argument{"queue message scalar fallback conflicts with semantic read or discard"};
        }
        return nullptr;
    }

    template <typename Function>
    void with_scalar_value(const ScalarRuntimeType &runtime_type, const literal::ScalarLiteral &literal,
                           Function &&function) {
        literal.visit(
            [&](const literal::ScalarLiteralItem &item) {
                item.visit_scalar([&](const auto *source) {
                    const auto &source_data = details::scalar_data(*source);
                    if (&source_data.runtime_type() == &runtime_type) {
                        std::invoke(function, Scalar<>::from(source_data));
                        return;
                    }

                    auto value = ScalarValue<>::from_literal(runtime_type, literal, *memory_);
                    std::invoke(function, Scalar<>::from(value));
                });
            },
            [&](std::span<const literal::ScalarLiteral>) {
                auto value = ScalarValue<>::from_literal(runtime_type, literal, *memory_);
                std::invoke(function, Scalar<>::from(value));
            });
    }

    void write_scalar(const ScalarRuntimeType &runtime_type, const literal::ScalarLiteral &literal) {
        if (type_cast<BuiltinScalarType>(&runtime_type.type()) == nullptr) {
            const auto *operations = write_operations(runtime_type);
            if (operations != nullptr) {
                with_scalar_value(runtime_type, literal, [&](Scalar<> value) {
                    operations->project(value, projection_);
                    if (writer_ != nullptr) {
                        operations->write(value, *writer_, *memory_);
                    }
                });
                return;
            }
        }

        auto inline_encoding = false;
        if (const auto *builtin = type_cast<BuiltinScalarType>(&runtime_type.type())) {
            builtin->visit_builtin_type([&]<typename Builtin>() {
                using Stored = scalar_stored_t<Builtin>;
                if constexpr (std::is_trivially_copyable_v<Stored> &&
                              !scalar_storage_traits<Builtin>::uses_shared_handle) {
                    inline_encoding = true;
                    if (writer_ != nullptr) {
                        auto value = ScalarValue<>::from_literal(runtime_type, literal, *memory_);
                        const auto canonical = scalar_cast<Builtin>(value.view()).value();
                        write_raw(canonical);
                    } else {
                        projection_.write_bytes(sizeof(Stored));
                    }
                }
            });
        }
        if (inline_encoding) {
            return;
        }

        projection_.emplace<ScalarValue<>>();
        if (writer_ != nullptr) {
            auto value = ScalarValue<>::from_literal(runtime_type, literal, *memory_);
            writer_->write(std::move(value));
        }
    }

    static std::span<const literal::ScalarLiteral> sequence(const literal::ScalarLiteral &literal,
                                                           const char *message) {
        return literal.visit(
            [message](const literal::ScalarLiteralItem &) -> std::span<const literal::ScalarLiteral> {
                throw std::invalid_argument{message};
            },
            [](std::span<const literal::ScalarLiteral> values) { return values; })
            .value();
    }

    void write_indexed_child(const TimeSeriesRuntimeType &runtime_type,
                             const literal::TimeSeriesLiteral &literal, item_id_t index) {
        const auto value_marker = marker(literal);
        if (value_marker == LiteralMarker::skip) {
            return;
        }
        if (value_marker == LiteralMarker::invalid) {
            if (!supports_literal_invalidation(runtime_type.type())) {
                throw std::invalid_argument{"this time-series item does not support literal invalidation"};
            }
            write_header({TickOperation::tick, false});
            write_raw(index);
            return;
        }
        if (value_marker != LiteralMarker::value) {
            throw std::invalid_argument{"a nested time-series literal cannot use REMOVE or REMOVED"};
        }

        write_header({TickOperation::tick, true});
        write_raw(index);
        write_payload(runtime_type, literal);
    }

    void write_payload(const TimeSeriesRuntimeType &runtime_type, const literal::TimeSeriesLiteral &literal) {
        runtime_type.type().visit(
            [&](const TimeSeriesValueType *) {
                const auto &value_runtime = static_cast<const TimeSeriesValueRuntimeType &>(runtime_type);
                write_scalar(value_runtime.scalar_runtime_type(), literal);
            },
            [&](const TimeSeriesListType *list_type) {
                const auto values = sequence(literal, "a TSL update requires a sequence");
                if (list_type->item_count() == std::dynamic_extent) {
                    throw std::invalid_argument{"queue messages require a fixed-size TSL"};
                }
                const auto index_at = [&](std::size_t position) {
                    const auto entry = scalar_literal::pair(values[position],
                                                           "a TSL update entry requires an index and a value");
                    return scalar_literal::list_index(*entry.first);
                };
                for (auto position = std::size_t{}; position < values.size(); ++position) {
                    const auto index = index_at(position);
                    if (index >= list_type->item_count() || index > UINT32_MAX) {
                        throw std::invalid_argument{"a TSL update index is out of range"};
                    }
                    for (auto previous = std::size_t{}; previous < position; ++previous) {
                        if (index_at(previous) == index) {
                            throw std::invalid_argument{"a TSL update contains a duplicate index"};
                        }
                    }
                }
                for (auto position = std::size_t{}; position < values.size(); ++position) {
                    const auto &value = values[position];
                    const auto entry = scalar_literal::pair(value,
                                                           "a TSL update entry requires an index and a value");
                    const auto index = index_at(position);
                    const auto child = entry.second;
                    write_indexed_child(runtime_type.item_layout(static_cast<item_id_t>(index)).runtime_type, *child,
                                        static_cast<item_id_t>(index));
                }
                write_header({TickOperation::end});
            },
            [&](const TimeSeriesBundleType *bundle_type) {
                const auto values = sequence(literal, "a TSB update requires a sequence");
                const auto field_index = [&](std::string_view name) {
                    for (auto index = std::size_t{}; index < bundle_type->item_count(); ++index) {
                        if (bundle_type->item_name(index).text() == name) {
                            return index;
                        }
                    }
                    throw std::invalid_argument{"a TSB update contains an unknown field"};
                };
                const auto index_at = [&](std::size_t position) {
                    const auto entry = scalar_literal::pair(
                        values[position], "a TSB update entry requires a field name and a value");
                    const auto name = scalar_literal::field_name(*entry.first);
                    return field_index(name);
                };
                for (auto position = std::size_t{}; position < values.size(); ++position) {
                    const auto index = index_at(position);
                    for (auto previous = std::size_t{}; previous < position; ++previous) {
                        if (index_at(previous) == index) {
                            throw std::invalid_argument{"a TSB update contains a duplicate field"};
                        }
                    }
                }
                for (auto position = std::size_t{}; position < values.size(); ++position) {
                    const auto &value = values[position];
                    const auto entry = scalar_literal::pair(
                        value, "a TSB update entry requires a field name and a value");
                    const auto index = index_at(position);
                    const auto child = entry.second;
                    write_indexed_child(runtime_type.item_layout(static_cast<item_id_t>(index)).runtime_type, *child,
                                        static_cast<item_id_t>(index));
                }
                write_header({TickOperation::end});
            },
            [&](const TimeSeriesSetType *) {
                const auto values = sequence(literal, "a TSS update requires a sequence");
                const auto &set_runtime = static_cast<const TimeSeriesSetRuntimeType &>(runtime_type);
                for (const auto &value : values) {
                    const auto value_marker = marker(value);
                    if (value_marker == LiteralMarker::skip) {
                        continue;
                    }
                    if (value_marker == LiteralMarker::invalid || value_marker == LiteralMarker::remove) {
                        throw std::invalid_argument{"invalid marker in TSS update"};
                    }
                    if (value_marker == LiteralMarker::removed) {
                        const auto removed = literal::Sentinel::from(value)->argument;
                        write_header({TickOperation::remove});
                        write_scalar(set_runtime.key_runtime_type(), *removed);
                        continue;
                    }
                    write_header({TickOperation::add});
                    write_scalar(set_runtime.key_runtime_type(), value);
                }
                write_header({TickOperation::end});
            },
            [&](const TimeSeriesDictionaryType *) {
                const auto values = sequence(literal, "a TSD update requires a sequence");
                const auto &dictionary_runtime = static_cast<const TimeSeriesDictionaryRuntimeType &>(runtime_type);
                for (const auto &value : values) {
                    const auto entry = scalar_literal::pair(value, "a TSD update entry requires a key and a value");
                    const auto value_marker = marker(*entry.second);
                    if (value_marker == LiteralMarker::skip) {
                        continue;
                    }
                    if (value_marker == LiteralMarker::removed) {
                        throw std::invalid_argument{"REMOVED is only valid as a TSS item"};
                    }
                    if (value_marker == LiteralMarker::remove) {
                        write_header({TickOperation::remove});
                        write_scalar(dictionary_runtime.key_runtime_type(), *entry.first);
                        continue;
                    }
                    if (value_marker == LiteralMarker::invalid &&
                        !supports_literal_invalidation(dictionary_runtime.item_runtime_type().type())) {
                        throw std::invalid_argument{"this time-series item does not support literal invalidation"};
                    }
                    write_header({TickOperation::tick, value_marker != LiteralMarker::invalid});
                    write_scalar(dictionary_runtime.key_runtime_type(), *entry.first);
                    if (value_marker == LiteralMarker::value) {
                        write_payload(dictionary_runtime.item_runtime_type(), *entry.second);
                    }
                }
                write_header({TickOperation::end});
            },
            [&](const TimeSeriesReferenceType *) {
                throw std::invalid_argument{"retained reference time series ticks are not supported"};
            },
            [&] { throw std::invalid_argument{"queue messages do not support provider-defined time-series types"}; });
    }

    const TimeSeriesRuntimeType *runtime_type_;
    ScalarBufferWriteContext *writer_;
    allocator *memory_;
    ScalarBufferProjectionContext projection_;
};

} // namespace

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
            if constexpr (std::is_trivially_copyable_v<Stored> &&
                          !scalar_storage_traits<Builtin>::uses_shared_handle) {
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

namespace {

class QueueTickInspect {
  public:
    explicit QueueTickInspect(util::buffer_reader &reader) noexcept : reader_{reader} {}

    bool conflicts(const OutputState &output) {
        evaluation_time_ = output.evaluation_time();
        const auto header = TickHeader::read(reader_);
        if (header.operation != TickOperation::tick) {
            throw std::logic_error{"time series tick record does not start with a tick header"};
        }
        return inspect(output.runtime_type().type(), output.runtime_type(), output.mutable_last_updated(),
                       output.data(), header);
    }

  private:
    bool inspect(const TimeSeriesType &type, const TimeSeriesRuntimeType &runtime_type,
                 const TimeSeriesState &last_updated, const void *value, TickHeader header) {
        if (runtime_type.is_reference()) {
            throw std::invalid_argument{"retained reference time series ticks are not supported"};
        }

        if (type_cast<TimeSeriesValueType>(&type) != nullptr) {
            return inspect_value(runtime_type, last_updated, value, header);
        }
        if (type_cast<TimeSeriesListType>(&type) != nullptr || type_cast<TimeSeriesBundleType>(&type) != nullptr) {
            return inspect_items(runtime_type, value, header);
        }
        if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
            return inspect_set(*set, runtime_type, last_updated, value, header);
        }
        if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
            return inspect_dictionary(*dictionary, runtime_type, last_updated, value, header);
        }
        throw std::invalid_argument{"standard queue inspection does not support this time series type"};
    }

    bool inspect_value(const TimeSeriesRuntimeType &runtime_type, const TimeSeriesState &last_updated,
                       const void *value, TickHeader header) {
        if (!header.valid) {
            return last_updated.valid() && last_updated.time() == evaluation_time_;
        }

        const auto deduplicated = runtime_type.type().guarantee().value() == TimeSeriesGuarantee::deduplicated;
        if (!deduplicated && last_updated.time() == evaluation_time_) {
            return true;
        }

        const auto &scalar_runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(runtime_type).scalar_runtime_type();
        auto changes_value = true;
        TimeSeriesTickScalarRead::visit(reader_, type_cast<TimeSeriesValueType>(runtime_type.type()).value_type(),
                                        [&](const Scalar<> &incoming) {
                                            if (deduplicated && last_updated.valid() &&
                                                scalar_runtime_type.equals(value, scalar_data(incoming))) {
                                                changes_value = false;
                                            }
                                        });
        return changes_value && last_updated.time() == evaluation_time_;
    }

    bool inspect_items(const TimeSeriesRuntimeType &runtime_type, const void *value, TickHeader header) {
        if (!header.valid) {
            return inspect_invalidation(runtime_type, value);
        }

        for (;;) {
            const auto item_header = TickHeader::read(reader_);
            if (item_header.operation == TickOperation::end) {
                return false;
            }
            if (item_header.operation != TickOperation::tick) {
                throw std::logic_error{"invalid time series collection tick entry"};
            }
            const auto id = reader_.read_raw<item_id_t>();
            if (value == nullptr) {
                const auto &item_runtime_type = runtime_type.item_layout(id).runtime_type;
                if (inspect(item_runtime_type.type(), item_runtime_type, initial_time_series_state, nullptr,
                            item_header)) {
                    return true;
                }
                continue;
            }

            const auto item = runtime_type.item(value, id);
            if (inspect(item.runtime_type.type(), item.runtime_type, item.last_updated, item.value, item_header)) {
                return true;
            }
        }
    }

    bool inspect_invalidation(const TimeSeriesRuntimeType &runtime_type, const void *value) {
        if (value == nullptr) {
            return false;
        }

        const auto *list = type_cast<TimeSeriesListType>(&runtime_type.type());
        const auto *bundle = type_cast<TimeSeriesBundleType>(&runtime_type.type());
        if (type_cast<TimeSeriesSetType>(&runtime_type.type()) != nullptr ||
            type_cast<TimeSeriesDictionaryType>(&runtime_type.type()) != nullptr) {
            return false;
        }
        if (list == nullptr && bundle == nullptr) {
            throw std::invalid_argument{"standard queue inspection does not support this invalidation"};
        }

        const auto count = runtime_type.item_count(value);
        for (auto id = item_id_t{}; id < count; ++id) {
            const auto item = runtime_type.item(value, id);
            if (inspect_invalidation_item(item.runtime_type.type(), item.runtime_type, item.last_updated,
                                          item.value)) {
                return true;
            }
        }
        return false;
    }

    bool inspect_invalidation_item(const TimeSeriesType &type, const TimeSeriesRuntimeType &runtime_type,
                                   const TimeSeriesState &last_updated, const void *value) {
        if (type_cast<TimeSeriesValueType>(&type) != nullptr) {
            return last_updated.valid() && last_updated.time() == evaluation_time_;
        }
        if (type_cast<TimeSeriesSetType>(&type) != nullptr ||
            type_cast<TimeSeriesDictionaryType>(&type) != nullptr) {
            return false;
        }
        return inspect_invalidation(runtime_type, value);
    }

    bool inspect_set(const TimeSeriesSetType &, const TimeSeriesRuntimeType &runtime_type,
                     const TimeSeriesState &last_updated, const void *value, TickHeader header) {
        if (!header.valid) {
            throw std::logic_error{"TSS invalidation cannot be replayed through its output API"};
        }

        const auto &set_runtime = static_cast<const TimeSeriesSetRuntimeType &>(runtime_type);
        auto changes_set = !last_updated.valid();

        for (;;) {
            const auto entry_header = TickHeader::read(reader_);
            if (entry_header.operation == TickOperation::end) {
                return changes_set && last_updated.time() == evaluation_time_;
            }
            if (entry_header.operation != TickOperation::add && entry_header.operation != TickOperation::remove) {
                throw std::logic_error{"invalid TSS tick entry"};
            }

            TimeSeriesTickScalarRead::visit(reader_, type_cast<TimeSeriesSetType>(runtime_type.type()).item_type(),
                                            [&](const Scalar<> &key) {
                                                const auto present = last_updated.valid() &&
                                                                     set_runtime.set_contains(value, scalar_data(key));
                                                const auto present_after = entry_header.operation == TickOperation::add;
                                                changes_set = changes_set || present != present_after;
                                            });
            if (changes_set && last_updated.time() == evaluation_time_) {
                return true;
            }
        }
    }

    bool inspect_dictionary(const TimeSeriesDictionaryType &dictionary_type,
                            const TimeSeriesRuntimeType &runtime_type, const TimeSeriesState &last_updated,
                            const void *value, TickHeader header) {
        if (!header.valid) {
            throw std::logic_error{"TSD invalidation cannot be replayed through its output API"};
        }

        const auto &dictionary_runtime = static_cast<const TimeSeriesDictionaryRuntimeType &>(runtime_type);
        for (;;) {
            const auto entry_header = TickHeader::read(reader_);
            if (entry_header.operation == TickOperation::end) {
                return false;
            }
            if (entry_header.operation != TickOperation::add && entry_header.operation != TickOperation::tick &&
                entry_header.operation != TickOperation::remove) {
                throw std::logic_error{"invalid TSD tick entry"};
            }

            auto entry_conflicts = false;
            TimeSeriesTickScalarRead::visit(reader_, dictionary_type.key_type(), [&](const Scalar<> &key) {
                const auto key_data = scalar_data(key);
                const auto id = value == nullptr ? util::occupancy_index::npos
                                                 : dictionary_runtime.find_item(value, key_data);
                if (entry_header.operation == TickOperation::remove) {
                    entry_conflicts = id != util::occupancy_index::npos && last_updated.valid() &&
                                      last_updated.time() == evaluation_time_ &&
                                      runtime_type.item_updated(last_updated, value, id);
                    return;
                }

                if (id == util::occupancy_index::npos) {
                    entry_conflicts = value != nullptr && last_updated.valid() &&
                                      last_updated.time() == evaluation_time_ &&
                                      dictionary_runtime.find_removed(last_updated, value, key_data) !=
                                          util::occupancy_index::npos;
                    if (!entry_conflicts) {
                        const auto &item_runtime_type = dictionary_runtime.item_runtime_type();
                        entry_conflicts = inspect(item_runtime_type.type(), item_runtime_type,
                                                  initial_time_series_state, nullptr, entry_header);
                    }
                    return;
                }

                const auto item = runtime_type.item(value, id);
                entry_conflicts = inspect(item.runtime_type.type(), item.runtime_type, item.last_updated, item.value,
                                          entry_header);
            });
            if (entry_conflicts) {
                return true;
            }
        }
    }

    util::buffer_reader &reader_;
    EvaluationTime evaluation_time_{MIN_DT};
};

} // namespace

QueueTickApply::QueueTickApply(util::buffer_reader &reader, QueueMode mode) noexcept
    : reader_{&reader}, mode_{mode} {}

bool QueueTickApply::apply(OutputState &output) {
    if (mode_ == QueueMode::batch) {
        throw std::invalid_argument{"batch queue mode does not apply individual ticks"};
    }
    if (reader_->readable().empty()) {
        return false;
    }

    const auto &type = output.runtime_type().type();
    if (mode_ == QueueMode::standard) {
        auto inspection_reader = reader_->fork();
        auto inspection = QueueTickInspect{inspection_reader};
        if (inspection.conflicts(output)) {
            return false;
        }
    }

    auto replay = TimeSeriesTickReplay{*reader_, TimeSeriesDispatch<TimeSeriesTickOperations>::instance(), true};
    auto replay_started = false;
    try {
        auto erased_output = output_factory::make<Out<TS_T>>(output);
        replay_started = true;
        replay.apply_erased(type, erased_output);
    } catch (...) {
        const auto failure = std::current_exception();
        if (!replay_started) {
            try {
                TimeSeriesTickDiscard{*reader_, TimeSeriesDispatch<TimeSeriesTickOperations>::instance()}.apply(type);
            } catch (...) {
                std::terminate();
            }
        }
        std::rethrow_exception(failure);
    }

    return true;
}

QueueMessageWriter::QueueMessageWriter(util::buffer buffer, std::size_t target,
                                       const TimeSeriesRuntimeType &runtime_type, allocator memory)
    : writer_{std::move(buffer)}, runtime_type_{&runtime_type}, memory_{memory} {
    if (target > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument{"queue message target is outside the 32-bit range"};
    }
    if (target == std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument{"queue message target uses the reserved sentinel"};
    }
    target_ = static_cast<std::uint32_t>(target);
}

std::optional<std::size_t> QueueMessageWriter::project(const literal::TimeSeriesLiteral &literal) {
    auto codec = QueueMessageCodec{*runtime_type_, nullptr, memory_};
    if (!codec.write_root(literal)) {
        return std::nullopt;
    }
    const auto size = codec.frame_size();
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error{"queue message frame exceeds the 32-bit size field"};
    }
    return size;
}

bool QueueMessageWriter::write(const literal::TimeSeriesLiteral &literal) {
    const auto size = project(literal);
    if (!size) {
        return false;
    }

    auto reservation = writer_.reserve(*size, alignof(QueueMessageHeader));
    auto cleanup = util::scope_exit{[&]() noexcept {
        try {
            reservation.read_written([&](util::buffer_reader &reader) {
                try {
                    reader.consume<QueueMessageHeader>();
                    TimeSeriesTickDiscard{reader, TimeSeriesDispatch<TimeSeriesTickOperations>::instance()}
                        .apply(runtime_type_->type());
                } catch (const std::out_of_range &) {
                    // The reader destroys each completed typed value before reaching an incomplete field.
                }
            });
        } catch (...) {
            std::terminate();
        }

        auto bytes = reservation.bytes();
        const auto tombstone = std::numeric_limits<std::uint32_t>::max();
        auto *header = std::launder(reinterpret_cast<QueueMessageHeader *>(bytes.data()));
        header->target = tombstone;
        reservation.commit();
    }};

    auto writer = ScalarBufferWriteContext{reservation};
    const auto frame_size = static_cast<std::uint32_t>(*size);
    writer.write(QueueMessageHeader{target_, frame_size});

    auto codec = QueueMessageCodec{*runtime_type_, &writer, memory_};
    if (!codec.write_root(literal)) {
        throw std::logic_error{"queue message projection changed during writing"};
    }
    codec.pad_to(*size);

    reservation.commit();
    cleanup.release();
    return true;
}

} // namespace details

} // namespace bg
