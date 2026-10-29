#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/queue_mode.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/time_series/tick_operations.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/buffer.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

struct alignas(std::max_align_t) QueueMessageHeader {
    std::uint32_t target;
    std::uint32_t size;
};

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
        if (input.last_updated() != now || !input.modified()) {
            return false;
        }

        const auto valid = tick_validity(input);
        if (!valid) {
            return false;
        }

        TickHeader{TickOperation::tick, *valid}.write(*writer_);
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
            write_child(item, TickOperation::tick, now, [&] { write_bytes(*writer_, index); });
        });
        TickHeader{TickOperation::end}.write(*writer_);
    }

    void operator()(In<TSB<SCHEMA>> *input, EvaluationTime now) {
        if (!input->valid()) {
            return;
        }

        input->for_each_updated([&](item_id_t index) {
            const auto &item = (*input)[index];
            write_child(item, TickOperation::tick, now, [&] { write_bytes(*writer_, index); });
        });
        TickHeader{TickOperation::end}.write(*writer_);
    }

    void operator()(In<TSS<SCALAR>> *input, EvaluationTime) {
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
        input->for_each_removed([&](const auto &key) {
            TickHeader{TickOperation::remove}.write(*writer_);
            write_scalar(key);
        });
        if (!input->valid()) {
            TickHeader{TickOperation::end}.write(*writer_);
            return;
        }

        input->for_each_added([&](const auto &key) {
            const auto item = (*input)[key];
            write_child(item, TickOperation::add, now, [&] { write_scalar(key); });
        });
        input->for_each_updated([&](const auto &key) {
            const auto item = (*input)[key];
            write_child(item, TickOperation::tick, now, [&] { write_scalar(key); });
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
    template <typename Input> std::optional<bool> tick_validity(const Input &input) const {
        if (input.valid()) {
            return true;
        }

        // nullopt means no event, false means invalidation, and true means a value or collection delta.
        auto valid = std::optional<bool>{false};
        input.visit(
            [&](const In<TSS<SCALAR>> *set) {
                if (set->next_removed_id(0) != util::occupancy_index::npos) {
                    valid = true;
                } else {
                    valid.reset();
                }
            },
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
                if (dictionary->next_removed_id(0) != util::occupancy_index::npos) {
                    valid = true;
                } else {
                    valid.reset();
                }
            },
            ddv::noop);
        return valid;
    }

    template <typename Input, typename WriteIdentity>
    void write_child(const Input &input, TickOperation operation, EvaluationTime now,
                     WriteIdentity &&write_identity) {
        const auto valid = operation == TickOperation::add ? std::optional{input.valid()} : tick_validity(input);
        if (!valid) {
            return;
        }

        TickHeader{operation, *valid}.write(*writer_);
        write_identity();
        if (*valid) {
            write_payload(input, now);
        }
    }

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

class TimeSeriesTickScalarRead {
  public:
    template <typename Function>
    static bool visit(util::buffer_reader &reader, const ScalarType &type, Function &&function,
                      bool consume = false, std::exception_ptr *failure = nullptr) {
        if (const auto *builtin = type_cast<BuiltinScalarType>(&type)) {
            builtin->visit_builtin_type([&]<typename Builtin>() {
                using Stored = details::scalar_stored_t<Builtin>;
                if constexpr (std::is_trivially_copyable_v<Stored> &&
                              !scalar_storage_traits<Builtin>::uses_shared_handle) {
                    const auto stored = reader.template read_raw<Stored>();
                    const auto value = Scalar<Builtin>::from(stored);
                    invoke(function, Scalar<>::from(value));
                } else {
                    if (consume) {
                        auto value = reader.template read<ScalarValue<>>();
                        invoke(function, value.view(), std::addressof(value));
                        return;
                    }
                    const auto &value = reader.template peek<ScalarValue<>>();
                    [[maybe_unused]] const auto value_reader = reader.fork();
                    reader.template consume<ScalarValue<>>();
                    invoke(function, value.view());
                }
            });
            return true;
        }

        const auto &dispatch = ScalarDispatch<ScalarBufferOperations>::instance();
        const auto *type_operations = dispatch.find_exact(type);
        if (type_operations != nullptr && type_operations->has_read()) {
            if (!consume) {
                using FunctionType = std::remove_reference_t<Function>;
                const auto consume_value = details::ScalarBufferValueConsumer{
                    const_cast<void *>(static_cast<const void *>(std::addressof(function))),
                    [](void *context, Scalar<> value) {
                        invoke(*static_cast<FunctionType *>(context), value);
                    }};
                type_operations->read(reader, consume_value);
                return true;
            }

            auto decoded_reader = reader.fork();
            auto scalar_start_reader = reader.fork();
            const auto readable_size = decoded_reader.readable().size();
            auto callback_called = false;
            using FunctionType = std::remove_reference_t<Function>;
            struct State {
                FunctionType *function;
                util::buffer_reader *reader;
                util::buffer_reader *original;
                std::size_t readable_size;
                bool &callback_called;
            };
            const auto state = State{std::addressof(function), std::addressof(decoded_reader),
                                     std::addressof(reader), readable_size, callback_called};
            const auto state_consumer = details::ScalarBufferValueConsumer{
                const_cast<State *>(std::addressof(state)),
                [](void *context, Scalar<> value) {
                    auto &state = *static_cast<State *>(context);
                    state.callback_called = true;
                    // Queue frames are contiguous, so the fork reports the exact encoded extent.
                    const auto consumed_size = state.readable_size - state.reader->readable().size();
                    state.original->consume(consumed_size);
                    invoke(*state.function, value);
                }};
            auto read_failure = std::exception_ptr{};
            try {
                type_operations->read(decoded_reader, state_consumer);
            } catch (...) {
                read_failure = std::current_exception();
            }

            const auto unread_size = scalar_start_reader.readable().size();
            try {
                type_operations->discard(scalar_start_reader);
            } catch (...) {
                std::terminate();
            }
            if (!callback_called) {
                reader.consume(unread_size - scalar_start_reader.readable().size());
            }
            if (read_failure) {
                if (failure != nullptr) {
                    if (!*failure) {
                        *failure = read_failure;
                    }
                    return callback_called;
                }
                std::rethrow_exception(read_failure);
            }
            return callback_called;
        }

        if (consume) {
            auto value = reader.template read<ScalarValue<>>();
            invoke(function, value.view(), std::addressof(value));
            return true;
        }
        const auto &value = reader.template peek<ScalarValue<>>();
        [[maybe_unused]] const auto value_reader = reader.fork();
        reader.template consume<ScalarValue<>>();
        invoke(function, value.view());
        return true;
    }

  private:
    template <typename Function>
    static void invoke(Function &function, Scalar<> value, ScalarValue<> *owned_value = nullptr) {
        if constexpr (std::invocable<Function &, Scalar<>, ScalarValue<> *>) {
            std::invoke(function, value, owned_value);
        } else {
            std::invoke(function, value);
        }
    }
};

class TimeSeriesTickDiscard {
  public:
    TimeSeriesTickDiscard(util::buffer_reader &reader,
                          const TimeSeriesDispatch<TimeSeriesTickOperations> &dispatch,
                          QueueMode mode = QueueMode::standard);
    void apply(const TimeSeriesType &type);
    void discard_child(const TimeSeriesType &type, TickHeader header);
    void discard_scalar(const ScalarType &type);

  private:
    void apply_payload(const TimeSeriesType &type);

    util::buffer_reader *reader_;
    const TimeSeriesDispatch<TimeSeriesTickOperations> *dispatch_;
    QueueMode mode_;
};

class TimeSeriesTickReplay {
  public:
    TimeSeriesTickReplay(util::buffer_reader &reader,
                         const TimeSeriesDispatch<TimeSeriesTickOperations> &dispatch, bool consume = false,
                         QueueMode mode = QueueMode::standard)
        : reader_{&reader}, dispatch_{&dispatch}, consume_{consume}, mode_{mode} {}

    template <typename Variable>
    void apply_erased(const TimeSeriesType &type, const Out<Variable> &output) {
        const auto header = TickHeader::read(*reader_);
        if (header.operation != TickOperation::tick) {
            throw std::logic_error{"time series tick record does not start with a tick header"};
        }
        apply_payload(type, output, header);
        if (failure_) {
            std::rethrow_exception(failure_);
        }
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
        if (failure_) {
            TimeSeriesTickDiscard{*reader_, *dispatch_, mode_}.discard_child(type, header);
            return;
        }
        apply_payload(type, output, header);
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TS<SCALAR>> &output, TickHeader header) {
        const auto &value_type = type_cast<TimeSeriesValueType>(type);
        if (mode_ == QueueMode::batch) {
            apply_batch_leaf(value_type, output, header);
            return;
        }
        if (!header.valid) {
            mutate([&] { output.invalidate(); });
            return;
        }

        visit_scalar(value_type.value_type(), [&](const Scalar<> &value, ScalarValue<> *owned) {
            mutate([&] {
                if (owned != nullptr) {
                    output.set_value(std::move(*owned));
                } else {
                    output.set_value(value);
                }
            });
        });
    }

    struct BatchListConstruction {
        const Scalar<> *value;
        ScalarValue<> *owned;
    };

    static void construct_batch_item(void *context, const ScalarRuntimeType &runtime_type, void *destination,
                                     allocator &memory) {
        auto &construction = *static_cast<BatchListConstruction *>(context);
        if (construction.owned == nullptr) {
            runtime_type.construct_from_input(destination, *construction.value, memory);
            return;
        }

        runtime_type.construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { runtime_type.destroy(destination, memory); }};
        std::move(*construction.owned).move_to(runtime_type, destination);
        rollback.release();
    }

    static void construct_batch_list(void *context, const ScalarRuntimeType &runtime_type, void *destination,
                                     allocator &memory) {
        auto &construction = *static_cast<BatchListConstruction *>(context);
        const auto &list_type = static_cast<const ScalarListRuntimeType &>(runtime_type);
        list_type.construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { list_type.destroy(destination, memory); }};
        list_type.emplace_back_with(destination, memory, {&construction, &construct_batch_item});
        rollback.release();
    }

    void apply_batch_leaf(const TimeSeriesValueType &value_type, const Out<TS<SCALAR>> &output,
                          TickHeader header) {
        const auto *list_type = type_cast<ListScalarType>(&value_type.value_type());
        if (list_type == nullptr) {
            throw std::invalid_argument{"batch queue leaves require a scalar list"};
        }
        if (!header.valid) {
            mutate([&] { output.invalidate(); });
            return;
        }

        const auto &scalar_runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(output.runtime_type()).scalar_runtime_type();
        const auto &list_runtime_type = static_cast<const ScalarListRuntimeType &>(scalar_runtime_type);
        visit_scalar(list_type->item_type(), [&](const Scalar<> &value, ScalarValue<> *owned) {
            mutate([&] {
                auto &state = output.output_state();
                const auto append = output.valid() && output.last_updated() == state.evaluation_time();
                auto construction = BatchListConstruction{&value, owned};
                if (append) {
                    list_runtime_type.emplace_back_with(state.mutable_data(), state.allocator(),
                                                        {&construction, &construct_batch_item});
                    state.publish();
                    return;
                }

                (void)scalar_runtime_type.assign_with(state.mutable_data(), {&construction, &construct_batch_list},
                                                      false);
                state.publish();
            });
        });
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSL<TS_T, SIZE>> &output, TickHeader header) {
        const auto &list_type = type_cast<TimeSeriesListType>(type);
        if (!header.valid) {
            mutate([&] { output.invalidate(); });
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
            apply_child_at(list_type.item_type(item_id), output, item_id, item_header);
        }
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSB<SCHEMA>> &output, TickHeader header) {
        const auto &bundle_type = type_cast<TimeSeriesBundleType>(type);
        if (!header.valid) {
            mutate([&] { output.invalidate(); });
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
            apply_child_at(bundle_type.item_type(item_id), output, item_id, item_header);
        }
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSS<SCALAR>> &output, TickHeader header) {
        const auto &set_type = type_cast<TimeSeriesSetType>(type);
        if (!header.valid) {
            throw std::logic_error{"TSS invalidation cannot be replayed through its output API"};
        }
        if (!output.valid()) {
            mutate([&] { output.clear(); });
        }

        for (;;) {
            const auto entry_header = TickHeader::read(*reader_);
            if (entry_header.operation == TickOperation::end) {
                break;
            }
            if (entry_header.operation != TickOperation::add && entry_header.operation != TickOperation::remove) {
                throw std::logic_error{"invalid TSS tick entry"};
            }
            visit_scalar(set_type.item_type(), [&](const Scalar<> &key) {
                mutate([&] {
                    if (entry_header.operation == TickOperation::add) {
                        output.insert(key);
                    } else {
                        output.erase(key);
                    }
                });
            });
        }
    }

    void apply_concrete(const TimeSeriesType &type, const Out<TSD<SCALAR, TS_T>> &output, TickHeader header) {
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(type);
        if (!header.valid) {
            throw std::logic_error{"TSD invalidation cannot be replayed through its output API"};
        }
        if (!output.valid()) {
            mutate([&] { output.clear(); });
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
            const auto key_read = visit_scalar(dictionary_type.key_type(), [&](const Scalar<> &key) {
                if (failure_) {
                    return;
                }
                if (entry_header.operation == TickOperation::remove) {
                    mutate([&] { output.erase(key); });
                    return;
                }

                auto child = [&]() -> std::optional<Out<TS_T>> {
                    try {
                        return output[key];
                    } catch (...) {
                        if (!consume_) {
                            throw;
                        }
                        record_failure();
                        TimeSeriesTickDiscard{*reader_, *dispatch_, mode_}
                            .discard_child(dictionary_type.value_type(), entry_header);
                        return std::nullopt;
                    }
                }();
                if (child) {
                    apply_child(dictionary_type.value_type(), *child, entry_header);
                }
            });
            if (!key_read && entry_header.operation != TickOperation::remove) {
                TimeSeriesTickDiscard{*reader_, *dispatch_, mode_}
                    .discard_child(dictionary_type.value_type(), entry_header);
            }
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

    template <typename Function> bool visit_scalar(const ScalarType &type, Function &&function) {
        if (failure_) {
            TimeSeriesTickDiscard{*reader_, *dispatch_}.discard_scalar(type);
            return false;
        }
        try {
            return TimeSeriesTickScalarRead::visit(*reader_, type, std::forward<Function>(function), consume_,
                                                   std::addressof(failure_));
        } catch (...) {
            if (!consume_) {
                throw;
            }
            record_failure();
            return false;
        }
    }

    template <typename Function> void mutate(Function &&function) {
        if (failure_) {
            return;
        }
        try {
            std::invoke(std::forward<Function>(function));
        } catch (...) {
            if (!consume_) {
                throw;
            }
            record_failure();
        }
    }

    void record_failure() noexcept {
        if (!failure_) {
            failure_ = std::current_exception();
        }
    }

    template <typename Output> void apply_child_at(const TimeSeriesType &type, const Output &output, item_id_t item_id,
                                                   TickHeader header) {
        if (failure_) {
            TimeSeriesTickDiscard{*reader_, *dispatch_, mode_}.discard_child(type, header);
            return;
        }
        auto child = [&]() -> std::optional<decltype(output[item_id])> {
            try {
                return output[item_id];
            } catch (...) {
                if (!consume_) {
                    throw;
                }
                record_failure();
                return std::nullopt;
            }
        }();
        if (!child) {
            TimeSeriesTickDiscard{*reader_, *dispatch_, mode_}.discard_child(type, header);
            return;
        }
        apply_child(type, *child, header);
    }

    util::buffer_reader *reader_;
    const TimeSeriesDispatch<TimeSeriesTickOperations> *dispatch_;
    bool consume_{};
    QueueMode mode_;
    std::exception_ptr failure_;
};

class QueueTickApply {
  public:
    QueueTickApply(util::buffer_reader &reader, QueueMode mode) noexcept;
    bool apply(OutputState &output);

  private:
    util::buffer_reader *reader_;
    QueueMode mode_;
};

class QueueMessageReader {
  public:
    explicit QueueMessageReader(util::buffer buffer);

    // Skips aborted frames, leaves the next message unread, and defers malformed-frame errors to next_target().
    bool has_pending_target() noexcept;
    std::optional<std::uint32_t> next_target();
    bool apply(OutputState &output, QueueMode mode);
    bool discard(const TimeSeriesType &type, QueueMode mode = QueueMode::standard);
    // Returns a handle at the reader position, including when no bytes are committed. Writers use the block's write
    // position and retain the chain independently of the reader.
    util::buffer current_block();

  private:
    struct Frame {
        QueueMessageHeader header;
        util::buffer body;
        std::size_t extent;
    };

    std::optional<Frame> next_frame();

    util::buffer_reader reader_;
};

class QueueMessageWriter {
  public:
    // The runtime type must outlive each write call. The buffer must be a concurrent buffer chain.
    QueueMessageWriter(util::buffer buffer, std::size_t target, const TimeSeriesRuntimeType &runtime_type,
                       allocator memory = {}, QueueMode mode = QueueMode::standard);

    // Copy only when the source has no active reservation and is not being mutated.
    QueueMessageWriter(const QueueMessageWriter &other)
        : writer_{other.writer_}, target_{other.target_}, runtime_type_{other.runtime_type_}, memory_{other.memory_},
          mode_{other.mode_} {}
    QueueMessageWriter &operator=(const QueueMessageWriter &) = delete;

    // Write one framed message. Root SKIP returns false without reserving a frame.
    bool write(const literal::TimeSeriesLiteral &literal);

  private:
    std::optional<std::size_t> project(const literal::TimeSeriesLiteral &literal);

    util::concurrent_buffer_writer writer_;
    std::uint32_t target_;
    const TimeSeriesRuntimeType *runtime_type_;
    allocator memory_;
    QueueMode mode_;
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
