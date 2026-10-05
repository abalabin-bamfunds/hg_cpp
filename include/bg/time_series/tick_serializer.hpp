#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/buffer.hpp>
#include <bg/util/hash_dictionary.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace bg {

class TimeSeriesTickWriteContext {
  public:
    explicit TimeSeriesTickWriteContext(util::buffer_writer &writer) noexcept : writer_{&writer} {}

    TimeSeriesTickWriteContext(const TimeSeriesTickWriteContext &) = delete;
    TimeSeriesTickWriteContext &operator=(const TimeSeriesTickWriteContext &) = delete;

    void write_bytes(std::span<const std::byte> bytes) { writer_->write_bytes(bytes); }

    template <typename T, typename... Args> T &emplace(Args &&...args) {
        return writer_->emplace<T>(std::forward<Args>(args)...);
    }

    template <typename T> auto &write(T &&value) {
        return emplace<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

  private:
    util::buffer_writer *writer_;
};

namespace details {

struct ScalarTickValueConsumer {
    void *context;
    void (*consume)(void *, Scalar<>);

    void operator()(Scalar<> value) const { consume(context, value); }
};

struct TimeSeriesTickScalarOperations {
    using Write = std::function<void(Scalar<>, TimeSeriesTickWriteContext &, allocator &)>;
    using Replay = std::function<void(util::buffer_reader &, ScalarTickValueConsumer)>;
    using Discard = std::function<void(util::buffer_reader &)>;

    Write write;
    Replay replay;
    Discard discard;
};

struct TimeSeriesTickOperations {
    using Write = std::function<void(const In<TS_T> &, TimeSeriesTickWriteContext &, allocator &)>;
    struct Replay {
        std::type_index output_view{typeid(void)};
        std::function<void(util::buffer_reader &, const void *)> apply;
    };

    Write write;
    std::vector<Replay> replays;
    std::function<void(util::buffer_reader &)> discard;
};

} // namespace details

class TimeSeriesTickDispatch {
  public:
    explicit TimeSeriesTickDispatch(allocator memory = {}) noexcept : memory_{memory}, scalar_operations_{memory} {}

    ~TimeSeriesTickDispatch() {
        runtime_operations_.release(memory_.memory_resource());
        type_operations_.release(memory_.memory_resource());
    }

    TimeSeriesTickDispatch(const TimeSeriesTickDispatch &) = delete;
    TimeSeriesTickDispatch &operator=(const TimeSeriesTickDispatch &) = delete;

    // Register write, replay, and discard handlers for every runtime representation of this scalar type.
    template <typename ScalarType, typename Write, typename Replay, typename Discard>
    bool add_scalar_handlers(Write &&write, Replay &&replay, Discard &&discard) {
        auto operations = details::TimeSeriesTickScalarOperations{
            typed_scalar_write<ScalarType>(std::forward<Write>(write)),
            typed_scalar_replay<ScalarType>(std::forward<Replay>(replay)),
            details::TimeSeriesTickScalarOperations::Discard{std::forward<Discard>(discard)}};
        return scalar_operations_.add(cpp_scalar_type<ScalarType>(), std::move(operations));
    }

    void write_scalar(Scalar<> value, TimeSeriesTickWriteContext &writer, allocator &memory) const {
        if (const auto *operations = scalar_operations_.find(value.type()); operations != nullptr) {
            operations->write(value, writer, memory);
            return;
        }

        writer.write(ScalarValue<>{value, memory});
    }

    template <typename Function> bool replay_scalar(const ScalarType &type, util::buffer_reader &reader,
                                                    Function &&function) const {
        const auto *operations = scalar_operations_.find(type);
        if (operations == nullptr) {
            return false;
        }

        using FunctionType = std::remove_reference_t<Function>;
        auto consume = details::ScalarTickValueConsumer{
            const_cast<void *>(static_cast<const void *>(std::addressof(function))),
            [](void *context, Scalar<> value) {
                std::invoke(*static_cast<FunctionType *>(context), value);
            }};
        operations->replay(reader, consume);
        return true;
    }

    bool discard_scalar(const ScalarType &type, util::buffer_reader &reader) const {
        const auto *operations = scalar_operations_.find(type);
        if (operations == nullptr) {
            return false;
        }

        operations->discard(reader);
        return true;
    }

    template <typename Series, typename Write>
    bool add_write(const TimeSeriesRuntimeType &runtime_type, Write &&write) {
        const auto *existing = runtime_operations_.find(&runtime_type);
        if (existing != nullptr && existing->write) {
            return false;
        }

        auto handler = details::TimeSeriesTickOperations::Write{
            typed_write<Series>(std::forward<Write>(write))};
        auto &operations = *runtime_operations_.try_emplace(&runtime_type, memory_.memory_resource()).first;
        operations.write = std::move(handler);
        return true;
    }

    template <typename Series, typename Write>
    bool add_write(const TimeSeriesType &type, Write &&write) {
        const auto *existing = type_operations_.find(&type);
        if (existing != nullptr && existing->write) {
            return false;
        }

        auto handler = details::TimeSeriesTickOperations::Write{
            typed_write<Series>(std::forward<Write>(write))};
        auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
        operations.write = std::move(handler);
        return true;
    }

    template <typename Series, OutputSpecification Specification, typename Replay>
    bool add_replay(const TimeSeriesRuntimeType &runtime_type, Replay &&replay) {
        auto &operations = *runtime_operations_.try_emplace(&runtime_type, memory_.memory_resource()).first;
        return add_replay<Series, Specification>(operations, std::forward<Replay>(replay));
    }

    template <typename Series, OutputSpecification Specification, typename Replay>
    bool add_replay(const TimeSeriesType &type, Replay &&replay) {
        auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
        return add_replay<Series, Specification>(operations, std::forward<Replay>(replay));
    }

    template <typename Discard>
    bool add_discard(const TimeSeriesType &type, Discard &&discard) {
        const auto *existing = type_operations_.find(&type);
        if (existing != nullptr && existing->discard) {
            return false;
        }

        auto handler = std::function<void(util::buffer_reader &)>{std::forward<Discard>(discard)};
        auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
        operations.discard = std::move(handler);
        return true;
    }

    void write(const In<TS_T> &input, TimeSeriesTickWriteContext &writer, allocator &memory) const {
        const auto *semantic_operations = type_operations_.find(&input.runtime_type().type());
        if (semantic_operations == nullptr || !semantic_operations->discard) {
            throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
        }

        if (const auto *operations = runtime_operations_.find(&input.runtime_type());
            operations != nullptr && operations->write) {
            operations->write(input, writer, memory);
            return;
        }

        if (const auto *operations = type_operations_.find(&input.runtime_type().type());
            operations != nullptr && operations->write) {
            operations->write(input, writer, memory);
            return;
        }

        throw std::invalid_argument{"no tick write operation is registered for this time series type"};
    }

    template <OutputSpecification Specification>
    void replay(util::buffer_reader &reader, const Out<TS_T, Specification> &output) const {
        const auto output_view = std::type_index{typeid(output)};
        if (const auto *operations = runtime_operations_.find(&output.runtime_type());
            operations != nullptr &&
            replay_from(*operations, output_view, reader, std::addressof(output))) {
            return;
        }

        if (const auto *operations = type_operations_.find(&output.runtime_type().type());
            operations != nullptr &&
            replay_from(*operations, output_view, reader, std::addressof(output))) {
            return;
        }

        throw std::invalid_argument{"no tick replay operation is registered for this output view"};
    }

    void discard(const TimeSeriesType &type, util::buffer_reader &reader) const {
        const auto *operations = type_operations_.find(&type);
        if (operations == nullptr || !operations->discard) {
            throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
        }
        operations->discard(reader);
    }

  private:
    template <typename Series, OutputSpecification Specification, typename Replay>
    bool add_replay(details::TimeSeriesTickOperations &operations, Replay &&replay) {
        const auto output_view = std::type_index{typeid(Out<TS_T, Specification>)};
        if (std::ranges::any_of(operations.replays,
                                [&](const auto &registered) { return registered.output_view == output_view; })) {
            return false;
        }

        operations.replays.push_back(
            {output_view, typed_replay<Series, Specification>(std::forward<Replay>(replay))});
        return true;
    }

    template <typename ScalarType, typename Write> static auto typed_scalar_write(Write &&write) {
        return [write = std::forward<Write>(write)](Scalar<> value, TimeSeriesTickWriteContext &writer,
                                                    allocator &memory) {
            std::invoke(write, scalar_cast<ScalarType>(value), writer, memory);
        };
    }

    template <typename ScalarType, typename Replay> static auto typed_scalar_replay(Replay &&replay) {
        return [replay = std::forward<Replay>(replay)](util::buffer_reader &reader,
                                                       details::ScalarTickValueConsumer consume) {
            auto typed_consumer = [consume](Scalar<ScalarType> value) {
                consume(Scalar<>::from(value));
            };
            std::invoke(replay, reader, typed_consumer);
        };
    }

    template <typename Series, typename Write>
    static auto typed_write(Write &&write) {
        return [write = std::forward<Write>(write)](const In<TS_T> &input, TimeSeriesTickWriteContext &writer,
                                                    allocator &memory) {
            std::invoke(write, input.template as<Series>(), writer, memory);
        };
    }

    template <typename Series, OutputSpecification Specification, typename Replay>
    static auto typed_replay(Replay &&replay) {
        return [replay = std::forward<Replay>(replay)](util::buffer_reader &reader, const void *output) {
            const auto &erased = *static_cast<const Out<TS_T, Specification> *>(output);
            std::invoke(replay, reader, erased.template as<Series>());
        };
    }

    static bool replay_from(const details::TimeSeriesTickOperations &operations, std::type_index output_view,
                            util::buffer_reader &reader, const void *output) {
        for (const auto &registered : operations.replays) {
            if (registered.output_view == output_view) {
                registered.apply(reader, output);
                return true;
            }
        }
        return false;
    }

    allocator memory_;
    ScalarDispatch<details::TimeSeriesTickScalarOperations> scalar_operations_;
    util::hash_dictionary<const TimeSeriesRuntimeType *, details::TimeSeriesTickOperations> runtime_operations_;
    util::hash_dictionary<const TimeSeriesType *, details::TimeSeriesTickOperations> type_operations_;
};

namespace details {

enum class TickOperation : std::uint8_t { add, tick, remove, end };

struct TickHeader {
    static constexpr std::uint8_t operation_mask = 0x03;
    static constexpr std::uint8_t valid_mask = 0x04;
    static constexpr std::uint8_t known_mask = operation_mask | valid_mask;

    TickOperation operation;
    bool valid;

    static TickHeader read(util::buffer_reader &reader) {
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

    void write(TimeSeriesTickWriteContext &writer) const {
        if ((operation == TickOperation::remove || operation == TickOperation::end) && valid) {
            throw std::logic_error{"invalid time series tick header flags"};
        }

        const std::uint8_t raw = static_cast<std::uint8_t>(operation) | (valid ? valid_mask : 0);
        writer.write_bytes(std::as_bytes(std::span{&raw, std::size_t{1}}));
    }
};

template <typename Visitor> bool visit_builtin_scalar_type(const ScalarType &type, Visitor &&visitor) {
    auto matched = false;
    tp::for_each(builtin_scalar_declarations, [&]<typename Builtin>(tp::unit<Builtin>) {
        if (!matched && &type == &cpp_scalar_type<Builtin>()) {
            std::invoke(visitor, std::type_identity<Builtin>{});
            matched = true;
        }
    });
    return matched;
}

template <typename T> void write_bytes(TimeSeriesTickWriteContext &writer, const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    writer.write_bytes(std::as_bytes(std::span{&value, std::size_t{1}}));
}

class TimeSeriesTickWrite {
  public:
    TimeSeriesTickWrite(TimeSeriesTickWriteContext &writer, allocator &memory, const TimeSeriesTickDispatch &dispatch)
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
            dispatch_->write(*input, *writer_, *memory_);
        }
    }

  private:
    template <typename Input> void write_payload(const Input &input, EvaluationTime now) {
        input.visit(ddv::serial{std::ref(*this)}, now);
    }

    template <typename T> void write_scalar(const Scalar<T> &value) {
        const auto matched = visit_builtin_scalar_type(
            value.type(), [&]<typename Builtin>(std::type_identity<Builtin>) {
                using Stored = details::scalar_stored_t<Builtin>;
                if constexpr (std::is_trivially_copyable_v<Stored>) {
                    const auto canonical = scalar_cast<Builtin>(value).value();
                    write_bytes(*writer_, canonical);
                } else {
                    writer_->write(ScalarValue<>{value, *memory_});
                }
            });

        if (!matched) {
            dispatch_->write_scalar(Scalar<>::from(value), *writer_, *memory_);
        }
    }

    TimeSeriesTickWriteContext *writer_;
    allocator *memory_;
    const TimeSeriesTickDispatch *dispatch_;
};

class TimeSeriesTickReplay {
  public:
    TimeSeriesTickReplay(util::buffer_reader &reader, const TimeSeriesTickDispatch &dispatch)
        : reader_{&reader}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    void apply_erased(const TimeSeriesType &type, const Out<Variable, Properties...> &output) {
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

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TS<SCALAR>, Specification> &output,
                        TickHeader header) {
        const auto &value_type = type_cast<TimeSeriesValueType>(type);
        if (!header.valid) {
            output.invalidate();
            return;
        }

        visit_scalar(value_type.value_type(), [&](const auto &value) { output.set_value(value); });
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSL<TS_T, SIZE>, Specification> &output,
                        TickHeader header) {
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

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSB<SCHEMA>, Specification> &output,
                        TickHeader header) {
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

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSS<SCALAR>, Specification> &output,
                        TickHeader header) {
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

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSD<SCALAR, TS_T>, Specification> &output,
                        TickHeader header) {
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

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &, const Out<REF<TS_T>, Specification> &, TickHeader) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &, const Out<TS_T, Specification> &output, TickHeader header) {
        if (!header.valid) {
            output.invalidate();
            return;
        }
        dispatch_->replay(*reader_, output);
    }

    template <typename Function> void visit_scalar(const ScalarType &type, Function &&function) {
        const auto matched = visit_builtin_scalar_type(type, [&]<typename Builtin>(std::type_identity<Builtin>) {
            using Stored = details::scalar_stored_t<Builtin>;
            if constexpr (std::is_trivially_copyable_v<Stored>) {
                const auto stored = reader_->template read_raw<Stored>();
                const auto value = Scalar<Builtin>::from(stored);
                std::invoke(function, Scalar<>::from(value));
            } else {
                const auto &value = reader_->template peek<ScalarValue<>>();
                std::invoke(function, value.view());
                reader_->template consume<ScalarValue<>>();
            }
        });

        if (!matched && !dispatch_->replay_scalar(type, *reader_, function)) {
            const auto &value = reader_->template peek<ScalarValue<>>();
            std::invoke(function, value.view());
            reader_->template consume<ScalarValue<>>();
        }
    }

    util::buffer_reader *reader_;
    const TimeSeriesTickDispatch *dispatch_;
};

class TimeSeriesTickDiscard {
  public:
    TimeSeriesTickDiscard(util::buffer_reader &reader, const TimeSeriesTickDispatch &dispatch)
        : reader_{&reader}, dispatch_{&dispatch} {}

    void apply(const TimeSeriesType &type) {
        const auto header = TickHeader::read(*reader_);
        if (header.operation != TickOperation::tick) {
            throw std::logic_error{"time series tick record does not start with a tick header"};
        }
        if (!header.valid) {
            return;
        }
        apply_payload(type);
    }

  private:
    void apply_payload(const TimeSeriesType &type) {
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
            [&] { dispatch_->discard(type, *reader_); });
    }

    void discard_child(const TimeSeriesType &type, TickHeader header) {
        if (header.operation != TickOperation::add && header.operation != TickOperation::tick) {
            throw std::logic_error{"invalid time series collection entry"};
        }
        if (header.valid) {
            apply_payload(type);
        }
    }

    void discard_scalar(const ScalarType &type) {
        const auto matched = visit_builtin_scalar_type(type, [&]<typename Builtin>(std::type_identity<Builtin>) {
            using Stored = scalar_stored_t<Builtin>;
            if constexpr (std::is_trivially_copyable_v<Stored>) {
                reader_->consume(sizeof(Stored));
            } else {
                reader_->template skip<ScalarValue<>>();
            }
        });
        if (!matched && !dispatch_->discard_scalar(type, *reader_)) {
            reader_->template skip<ScalarValue<>>();
        }
    }

    util::buffer_reader *reader_;
    const TimeSeriesTickDispatch *dispatch_;
};

} // namespace details

class TimeSeriesTickWriter {
  public:
    explicit TimeSeriesTickWriter(util::buffer buffer, allocator memory = {},
                                  const TimeSeriesTickDispatch &dispatch = default_dispatch())
        : writer_{std::move(buffer)}, memory_{memory}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    // Write a change made at `now`. The caller supplies the evaluation time
    // so an older invalidation is not recorded again when another input ticks.
    bool write(const In<Variable, Properties...> &input, EvaluationTime now) {
        const auto &type = input.runtime_type().type();
        if (type_ != nullptr && type_ != &type) {
            throw std::invalid_argument{"a tick writer cannot mix semantic time series types"};
        }

        try {
            auto writer = TimeSeriesTickWriteContext{writer_};
            auto write = details::TimeSeriesTickWrite{writer, memory_, *dispatch_};
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
                        details::TimeSeriesTickDiscard{reader, *dispatch_}.apply(type);
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
        type_ = &type;
        return true;
    }

  private:
    static const TimeSeriesTickDispatch &default_dispatch() {
        static const TimeSeriesTickDispatch dispatch;
        return dispatch;
    }

    util::buffer_writer writer_;
    allocator memory_;
    const TimeSeriesTickDispatch *dispatch_;
    const TimeSeriesType *type_{};
};

class TimeSeriesTickReader {
  public:
    TimeSeriesTickReader(util::buffer buffer, const TimeSeriesType &type,
                         const TimeSeriesTickDispatch &dispatch = default_dispatch())
        : reader_{std::move(buffer)}, dispatch_{&dispatch}, type_{&type} {}

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

    template <typename Variable, auto... Properties>
    bool replay(const Out<Variable, Properties...> &output) {
        if (!has_next()) {
            return false;
        }

        if (&output.runtime_type().type() != type_) {
            throw std::invalid_argument{"time series tick record has a different semantic output type"};
        }

        auto replay_buffer = reader_.fork();
        auto replay = details::TimeSeriesTickReplay{replay_buffer, *dispatch_};
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
        details::TimeSeriesTickDiscard{reader_, *dispatch_}.apply(*type_);
    }

    static const TimeSeriesTickDispatch &default_dispatch() {
        static const TimeSeriesTickDispatch dispatch;
        return dispatch;
    }

    util::buffer_reader reader_;
    const TimeSeriesTickDispatch *dispatch_;
    const TimeSeriesType *type_;
    bool discard_failed_{};
};

} // namespace bg
