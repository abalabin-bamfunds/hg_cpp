#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/time_series/tick_operations.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

enum class TickOperation : std::uint8_t { add, tick, remove, end };

struct TickHeader {
    static constexpr std::uint8_t operation_mask = 0x03;
    static constexpr std::uint8_t valid_mask = 0x04;
    static constexpr std::uint8_t known_mask = operation_mask | valid_mask;

    TickOperation operation;
    bool valid;

    static TickHeader read(util::buffer_reader &reader);
    void write(ScalarBufferWriteContext &writer) const;
};

template <typename T> void write_bytes(ScalarBufferWriteContext &writer, const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    writer.write_bytes(std::as_bytes(std::span{&value, std::size_t{1}}));
}

class TimeSeriesTickWrite {
  public:
    TimeSeriesTickWrite(ScalarBufferWriteContext &writer, allocator &memory,
                        const TimeSeriesDispatch<TimeSeriesTickOperations> &dispatch)
        : writer_{&writer}, memory_{&memory}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    bool write(const In<Variable, Properties...> &input, EvaluationTime now) {
        if (input.runtime_type().is_reference()) {
            throw std::invalid_argument{"retained reference time series ticks are not supported"};
        }
        if (input.last_updated() != now || (input.valid() ? !input.modified() : !input.became_invalid())) {
            return false;
        }

        TickHeader{TickOperation::tick, input.valid()}.write(*writer_);
        write_payload(input, now);
        return true;
    }

    void operator()(In<TS<SCALAR>> *input, EvaluationTime) {
        if (input->valid()) {
            write_scalar(input->value_view());
        }
    }

    void operator()(In<TSL<TS_T, SIZE>> *input, EvaluationTime now) {
        if (!input->valid()) {
            return;
        }

        input->for_each_updated([&](item_id_t index) {
            const auto &item = (*input)[index];
            TickHeader{TickOperation::tick, item.valid()}.write(*writer_);
            write_bytes(*writer_, index);
            if (item.valid()) {
                write_payload(item, now);
            }
        });
        TickHeader{TickOperation::end}.write(*writer_);
    }

    void operator()(In<TSB<SCHEMA>> *input, EvaluationTime now) {
        if (!input->valid()) {
            return;
        }

        input->for_each_updated([&](item_id_t index) {
            const auto &item = (*input)[index];
            TickHeader{TickOperation::tick, item.valid()}.write(*writer_);
            write_bytes(*writer_, index);
            if (item.valid()) {
                write_payload(item, now);
            }
        });
        TickHeader{TickOperation::end}.write(*writer_);
    }

    void operator()(In<TSS<SCALAR>> *input, EvaluationTime) {
        if (!input->valid()) {
            throw std::invalid_argument{"TSS invalidation cannot be replayed through its output API"};
        }

        input->for_each_added([&](const auto &key) {
            TickHeader{TickOperation::add}.write(*writer_);
            write_scalar(key);
        });
        input->for_each_removed([&](const auto &key) {
            TickHeader{TickOperation::remove}.write(*writer_);
            write_scalar(key);
        });
        TickHeader{TickOperation::end}.write(*writer_);
    }

    void operator()(In<TSD<SCALAR, TS_T>> *input, EvaluationTime now) {
        if (!input->valid()) {
            throw std::invalid_argument{"TSD invalidation cannot be replayed through its output API"};
        }

        input->for_each_removed([&](const auto &key) {
            TickHeader{TickOperation::remove}.write(*writer_);
            write_scalar(key);
        });
        input->for_each_added([&](const auto &key) {
            const auto item = (*input)[key];
            TickHeader{TickOperation::add, item.valid()}.write(*writer_);
            write_scalar(key);
            if (item.valid()) {
                write_payload(item, now);
            }
        });
        input->for_each_modified([&](const auto &key) {
            const auto item = (*input)[key];
            TickHeader{TickOperation::tick, item.valid()}.write(*writer_);
            write_scalar(key);
            if (item.valid()) {
                write_payload(item, now);
            }
        });
        TickHeader{TickOperation::end}.write(*writer_);
    }

    void operator()(In<REF<TS_T>> *, EvaluationTime) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    void operator()(In<TS_T> *input, EvaluationTime) {
        if (input->valid()) {
            const auto &runtime_type = input->runtime_type();
            const auto *semantic_operations = dispatch_->find_exact(runtime_type.type());
            if (semantic_operations == nullptr || !semantic_operations->has_discard()) {
                throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
            }

            if (const auto *runtime_operations = dispatch_->find_exact(runtime_type);
                runtime_operations != nullptr && runtime_operations->has_write()) {
                runtime_operations->write(*input, *writer_, *memory_);
                return;
            }
            if (semantic_operations->has_write()) {
                semantic_operations->write(*input, *writer_, *memory_);
                return;
            }

            throw std::invalid_argument{"no tick write operation is registered for this time series type"};
        }
    }

  private:
    template <typename Input> void write_payload(const Input &input, EvaluationTime now) {
        input.visit(ddv::serial{std::ref(*this)}, now);
    }

    template <typename T> void write_scalar(const Scalar<T> &value) {
        if (const auto *builtin = type_cast<BuiltinScalarType>(&value.type())) {
            builtin->visit_builtin_type([&]<typename Builtin>() {
                using Stored = details::scalar_stored_t<Builtin>;
                if constexpr (std::is_trivially_copyable_v<Stored> &&
                              !scalar_storage_traits<Builtin>::uses_shared_handle) {
                    const auto canonical = scalar_cast<Builtin>(value).value();
                    write_bytes(*writer_, canonical);
                } else {
                    writer_->write(ScalarValue<>{value, *memory_});
                }
            });
            return;
        }

        const auto erased = Scalar<>::from(value);
        const auto &dispatch = ScalarDispatch<ScalarBufferOperations>::instance();
        const auto *runtime_operations = dispatch.find_exact(erased.runtime_type());
        const auto *type_operations = dispatch.find_exact(erased.type());
        if (runtime_operations != nullptr && runtime_operations->has_write()) {
            runtime_operations->write(erased, *writer_, *memory_);
        } else if (type_operations != nullptr && type_operations->has_write()) {
            type_operations->write(erased, *writer_, *memory_);
        } else {
            writer_->write(ScalarValue<>{erased, *memory_});
        }
    }

    ScalarBufferWriteContext *writer_;
    allocator *memory_;
    const TimeSeriesDispatch<TimeSeriesTickOperations> *dispatch_;
};

class TimeSeriesTickReplay {
  public:
    TimeSeriesTickReplay(util::buffer_reader &reader,
                         const TimeSeriesDispatch<TimeSeriesTickOperations> &dispatch)
        : reader_{&reader}, dispatch_{&dispatch} {}

    template <typename Variable>
    void apply_erased(const TimeSeriesType &type, const Out<Variable> &output) {
        const auto header = TickHeader::read(*reader_);
        if (header.operation != TickOperation::tick) {
            throw std::logic_error{"time series tick record does not start with a tick header"};
        }
        apply_payload(type, output, header);
    }

  private:
    template <typename Output> void apply_payload(const TimeSeriesType &type, const Output &output,
                                                  TickHeader header) {
        output.visit([&](const auto *typed) { apply_concrete(type, *typed, header); }, ddv::noop);
    }

    template <typename Output> void apply_child(const TimeSeriesType &type, const Output &output,
                                                TickHeader header) {
        if (header.operation != TickOperation::add && header.operation != TickOperation::tick) {
            throw std::logic_error{"invalid time series collection entry"};
        }
        if (header.operation == TickOperation::add && !header.valid && !output.valid()) {
            return;
        }
        apply_payload(type, output, header);
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TS<SCALAR>> &output, TickHeader header) {
        const auto &value_type = type_cast<TimeSeriesValueType>(type);
        if (!header.valid) {
            output.invalidate();
            return;
        }

        visit_scalar(value_type.value_type(), [&](const auto &value) { output.set_value(value); });
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSL<TS_T, SIZE>> &output, TickHeader header) {
        const auto &list_type = type_cast<TimeSeriesListType>(type);
        if (!header.valid) {
            output.invalidate();
            return;
        }

        for (;;) {
            const auto item_header = TickHeader::read(*reader_);
            if (item_header.operation == TickOperation::end) {
                break;
            }
            if (item_header.operation != TickOperation::tick) {
                throw std::logic_error{"invalid list tick entry"};
            }
            const auto item_id = reader_->read_raw<bg::item_id_t>();
            apply_child(list_type.item_type(item_id), output[item_id], item_header);
        }
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSB<SCHEMA>> &output, TickHeader header) {
        const auto &bundle_type = type_cast<TimeSeriesBundleType>(type);
        if (!header.valid) {
            output.invalidate();
            return;
        }

        for (;;) {
            const auto item_header = TickHeader::read(*reader_);
            if (item_header.operation == TickOperation::end) {
                break;
            }
            if (item_header.operation != TickOperation::tick) {
                throw std::logic_error{"invalid bundle tick entry"};
            }
            const auto item_id = reader_->read_raw<bg::item_id_t>();
            apply_child(bundle_type.item_type(item_id), output[item_id], item_header);
        }
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSS<SCALAR>> &output, TickHeader header) {
        const auto &set_type = type_cast<TimeSeriesSetType>(type);
        if (!header.valid) {
            throw std::logic_error{"TSS invalidation cannot be replayed through its output API"};
        }
        if (!output.valid()) {
            output.clear();
        }

        for (;;) {
            const auto entry_header = TickHeader::read(*reader_);
            if (entry_header.operation == TickOperation::end) {
                break;
            }
            if (entry_header.operation != TickOperation::add && entry_header.operation != TickOperation::remove) {
                throw std::logic_error{"invalid TSS tick entry"};
            }
            visit_scalar(set_type.item_type(), [&](const auto &key) {
                if (entry_header.operation == TickOperation::add) {
                    output.insert(key);
                } else {
                    output.erase(key);
                }
            });
        }
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSD<SCALAR, TS_T>> &output, TickHeader header) {
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(type);
        if (!header.valid) {
            throw std::logic_error{"TSD invalidation cannot be replayed through its output API"};
        }
        if (!output.valid()) {
            output.clear();
        }

        for (;;) {
            const auto entry_header = TickHeader::read(*reader_);
            if (entry_header.operation == TickOperation::end) {
                break;
            }
            if (entry_header.operation != TickOperation::add && entry_header.operation != TickOperation::tick &&
                entry_header.operation != TickOperation::remove) {
                throw std::logic_error{"invalid TSD tick entry"};
            }
            visit_scalar(dictionary_type.key_type(), [&](const auto &key) {
                if (entry_header.operation == TickOperation::remove) {
                    output.erase(key);
                } else {
                    apply_child(dictionary_type.value_type(), output[key], entry_header);
                }
            });
        }
    }

    void apply_concrete(const TimeSeriesType &, const Out<REF<TS_T>> &, TickHeader) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    void apply_concrete(const TimeSeriesType &, const Out<TS_T> &output, TickHeader header) {
        if (!header.valid) {
            output.invalidate();
            return;
        }
        const auto &runtime_type = output.runtime_type();
        if (const auto *runtime_operations = dispatch_->find_exact(runtime_type);
            runtime_operations != nullptr && runtime_operations->has_replay()) {
            runtime_operations->replay(*reader_, output.output_state());
            return;
        }
        if (const auto *type_operations = dispatch_->find_exact(runtime_type.type());
            type_operations != nullptr && type_operations->has_replay()) {
            type_operations->replay(*reader_, output.output_state());
            return;
        }
        throw std::invalid_argument{"no tick replay operation is registered for this time series representation"};
    }

    template <typename Function> void visit_scalar(const ScalarType &type, Function &&function) {
        if (const auto *builtin = type_cast<BuiltinScalarType>(&type)) {
            builtin->visit_builtin_type([&]<typename Builtin>() {
                using Stored = details::scalar_stored_t<Builtin>;
                if constexpr (std::is_trivially_copyable_v<Stored> &&
                              !scalar_storage_traits<Builtin>::uses_shared_handle) {
                    const auto stored = reader_->template read_raw<Stored>();
                    const auto value = Scalar<Builtin>::from(stored);
                    std::invoke(function, Scalar<>::from(value));
                } else {
                    const auto &value = reader_->template peek<ScalarValue<>>();
                    std::invoke(function, value.view());
                    reader_->template consume<ScalarValue<>>();
                }
            });
            return;
        }

        const auto &dispatch = ScalarDispatch<ScalarBufferOperations>::instance();
        const auto *type_operations = dispatch.find_exact(type);
        if (type_operations != nullptr && type_operations->has_read()) {
            using FunctionType = std::remove_reference_t<Function>;
            const auto consume = details::ScalarBufferValueConsumer{
                const_cast<void *>(static_cast<const void *>(std::addressof(function))),
                [](void *context, Scalar<> value) {
                    std::invoke(*static_cast<FunctionType *>(context), value);
                }};
            type_operations->read(*reader_, consume);
            return;
        }

        const auto &value = reader_->template peek<ScalarValue<>>();
        std::invoke(function, value.view());
        reader_->template consume<ScalarValue<>>();
    }

    util::buffer_reader *reader_;
    const TimeSeriesDispatch<TimeSeriesTickOperations> *dispatch_;
};

class TimeSeriesTickDiscard {
  public:
    TimeSeriesTickDiscard(util::buffer_reader &reader,
                          const TimeSeriesDispatch<TimeSeriesTickOperations> &dispatch);
    void apply(const TimeSeriesType &type);

  private:
    void apply_payload(const TimeSeriesType &type);
    void discard_child(const TimeSeriesType &type, TickHeader header);
    void discard_scalar(const ScalarType &type);

    util::buffer_reader *reader_;
    const TimeSeriesDispatch<TimeSeriesTickOperations> *dispatch_;
};

} // namespace details

class TimeSeriesTickWriter {
  public:
    explicit TimeSeriesTickWriter(util::buffer buffer, allocator memory = {})
        : writer_{std::move(buffer)}, memory_{memory} {}

    template <typename Variable, auto... Properties>
    // Write a change made at `now`. The caller supplies the evaluation time
    // so an older invalidation is not recorded again when another input ticks.
    bool write(const In<Variable, Properties...> &input, EvaluationTime now) {
        const auto &type = input.runtime_type().type();
        const auto &dispatch = TimeSeriesDispatch<TimeSeriesTickOperations>::instance();

        try {
            auto writer = ScalarBufferWriteContext{writer_};
            auto write = details::TimeSeriesTickWrite{writer, memory_, dispatch};
            if (!write.write(input, now)) {
                return false;
            }
        } catch (...) {
            const auto failure = std::current_exception();
            try {
                writer_.rollback([&](util::buffer_reader &reader) {
                    if (reader.readable().empty()) {
                        return;
                    }

                    try {
                        details::TimeSeriesTickDiscard{reader, dispatch}.apply(type);
                    } catch (const std::out_of_range &) {
                        // The input ended inside a raw field or typed value. Complete typed values
                        // before that point have been destroyed by the same discard walk.
                    }
                });
            } catch (...) {
                std::terminate();
            }
            std::rethrow_exception(failure);
        }

        writer_.commit();
        return true;
    }

  private:
    util::buffer_writer writer_;
    allocator memory_;
};

class TimeSeriesTickReader {
  public:
    TimeSeriesTickReader(util::buffer buffer, const TimeSeriesType &type)
        : reader_{std::move(buffer)}, type_{&type} {}

    ~TimeSeriesTickReader() {
        if (discard_failed_) {
            return;
        }

        try {
            while (has_next()) {
                discard();
            }
        } catch (...) {
            std::terminate();
        }
    }

    // The reader destroys unread records on destruction.
    bool has_next() { return !reader_.readable().empty(); }

    template <typename Variable> bool replay(const Out<Variable> &output) {
        if (!has_next()) {
            return false;
        }

        auto replay_buffer = reader_.fork();
        auto replay = details::TimeSeriesTickReplay{
            replay_buffer, TimeSeriesDispatch<TimeSeriesTickOperations>::instance()};
        try {
            replay.apply_erased(*type_, output);
        } catch (...) {
            try {
                discard_record();
            } catch (...) {
                discard_failed_ = true;
            }
            throw;
        }

        try {
            discard_record();
        } catch (...) {
            discard_failed_ = true;
            throw;
        }
        return true;
    }

    bool discard() {
        if (!has_next()) {
            return false;
        }

        try {
            discard_record();
        } catch (...) {
            discard_failed_ = true;
            throw;
        }
        return true;
    }

  private:
    void discard_record() {
        details::TimeSeriesTickDiscard{reader_, TimeSeriesDispatch<TimeSeriesTickOperations>::instance()}.apply(*type_);
    }

    util::buffer_reader reader_;
    const TimeSeriesType *type_;
    bool discard_failed_{};
};

} // namespace bg
