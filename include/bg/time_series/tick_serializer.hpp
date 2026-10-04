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
    using Write = std::function<void(const In<TS_T> &, bool, TimeSeriesTickWriteContext &, allocator &)>;
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

    void write(const In<TS_T> &input, bool snapshot, TimeSeriesTickWriteContext &writer, allocator &memory) const {
        const auto *semantic_operations = type_operations_.find(&input.runtime_type().type());
        if (semantic_operations == nullptr || !semantic_operations->discard) {
            throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
        }

        if (const auto *operations = runtime_operations_.find(&input.runtime_type());
            operations != nullptr && operations->write) {
            operations->write(input, snapshot, writer, memory);
            return;
        }

        if (const auto *operations = type_operations_.find(&input.runtime_type().type());
            operations != nullptr && operations->write) {
            operations->write(input, snapshot, writer, memory);
            return;
        }

        throw std::invalid_argument{"no tick write operation is registered for this time series type"};
    }

    template <OutputSpecification Specification>
    void replay(util::buffer_reader &reader, const Out<TS_T, Specification> &output) const {
        const auto output_view = std::type_index{typeid(output)};
        if (const auto *operations = runtime_operations_.find(&output.runtime_type());
            operations != nullptr && replay_from(*operations, output_view, reader, std::addressof(output))) {
            return;
        }

        if (const auto *operations = type_operations_.find(&output.runtime_type().type());
            operations != nullptr && replay_from(*operations, output_view, reader, std::addressof(output))) {
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
        return [write = std::forward<Write>(write)](const In<TS_T> &input, bool snapshot,
                                                    TimeSeriesTickWriteContext &writer, allocator &memory) {
            std::invoke(write, input.template as<Series>(), snapshot, writer, memory);
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

enum class TickState : std::uint8_t { value, invalidated };

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

        input.visit(ddv::serial{std::ref(*this)}, now, input.became_valid());
        return true;
    }

    void operator()(In<TS<SCALAR>> *input, EvaluationTime, bool) {
        write_state(input->valid());
        if (input->valid()) {
            write_scalar(input->value_view());
        }
    }

    void operator()(In<TSL<TS_T, SIZE>> *input, EvaluationTime now, bool snapshot) {
        write_state(input->valid());
        if (!input->valid()) {
            return;
        }

        auto count = std::size_t{};
        if (snapshot) {
            count = input->size();
        } else {
            input->for_each_updated([&](item_id_t) { ++count; });
        }
        write_count(count);

        auto write_item = [&](item_id_t index) {
            write_bytes(*writer_, index);
            write_child((*input)[index], now, snapshot);
        };
        if (snapshot) {
            for (auto index = item_id_t{}; index < input->size(); ++index) {
                write_item(index);
            }
        } else {
            input->for_each_updated(write_item);
        }
    }

    void operator()(In<TSB<SCHEMA>> *input, EvaluationTime now, bool snapshot) {
        write_state(input->valid());
        if (!input->valid()) {
            return;
        }

        auto count = std::size_t{};
        if (snapshot) {
            count = input->size();
        } else {
            input->for_each_updated([&](item_id_t) { ++count; });
        }
        write_count(count);

        auto write_item = [&](item_id_t index) {
            write_bytes(*writer_, index);
            write_child((*input)[index], now, snapshot);
        };
        if (snapshot) {
            for (auto index = item_id_t{}; index < input->size(); ++index) {
                write_item(index);
            }
        } else {
            input->for_each_updated(write_item);
        }
    }

    void operator()(In<TSS<SCALAR>> *input, EvaluationTime, bool snapshot) {
        if (!input->valid()) {
            throw std::invalid_argument{"TSS invalidation cannot be replayed through its output API"};
        }

        write_bytes(*writer_, static_cast<std::uint8_t>(snapshot));
        auto added_count = std::size_t{};
        auto removed_count = std::size_t{};
        if (snapshot) {
            input->for_each([&](const auto &) { ++added_count; });
        } else {
            input->for_each_added([&](const auto &) { ++added_count; });
            input->for_each_removed([&](const auto &) { ++removed_count; });
        }
        write_count(added_count);
        write_count(removed_count);

        if (snapshot) {
            input->for_each([&](const auto &key) { write_scalar(key); });
        } else {
            input->for_each_added([&](const auto &key) { write_scalar(key); });
            input->for_each_removed([&](const auto &key) { write_scalar(key); });
        }
    }

    void operator()(In<TSD<SCALAR, TS_T>> *input, EvaluationTime now, bool snapshot) {
        if (!input->valid()) {
            throw std::invalid_argument{"TSD invalidation cannot be replayed through its output API"};
        }

        write_bytes(*writer_, static_cast<std::uint8_t>(snapshot));
        auto removed_count = std::size_t{};
        auto added_count = std::size_t{};
        auto modified_count = std::size_t{};
        if (snapshot) {
            added_count = input->size();
        } else {
            input->for_each_removed([&](const auto &) { ++removed_count; });
            input->for_each_added([&](const auto &) { ++added_count; });
            input->for_each_modified([&](const auto &) { ++modified_count; });
        }
        write_count(removed_count);
        write_count(added_count);
        write_count(modified_count);

        if (snapshot) {
            input->for_each_key_value([&](const auto &key, const auto &item) {
                write_scalar(key);
                write_child(item, now, true);
            });
        } else {
            input->for_each_removed([&](const auto &key) {
                write_scalar(key);
            });
            input->for_each_added([&](const auto &key) {
                write_scalar(key);
                write_child((*input)[key], now, true);
            });
            input->for_each_modified([&](const auto &key) {
                write_scalar(key);
                write_child((*input)[key], now, false);
            });
        }
    }

    void operator()(In<REF<TS_T>> *, EvaluationTime, bool) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    void operator()(In<TS_T> *input, EvaluationTime, bool snapshot) {
        dispatch_->write(*input, snapshot, *writer_, *memory_);
    }

  private:
    template <typename T> void write_scalar(const Scalar<T> &value) {
        const auto matched = visit_builtin_scalar_type(value.type(), [&]<typename Builtin>(std::type_identity<Builtin>) {
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

    void write_state(bool valid) { write_bytes(*writer_, static_cast<std::uint8_t>(valid ? TickState::value
                                                                                           : TickState::invalidated)); }

    void write_count(std::size_t count) {
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error{"time series tick has too many changed items"};
        }
        write_bytes(*writer_, static_cast<std::uint32_t>(count));
    }

    template <typename Input> void write_child(const Input &input, EvaluationTime now, bool snapshot) {
        input.visit(ddv::serial{std::ref(*this)}, now, snapshot);
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
        output.visit([&](const auto *typed) { apply_concrete(type, *typed); }, ddv::noop);
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TS<SCALAR>, Specification> &output) {
        const auto &value_type = type_cast<TimeSeriesValueType>(type);
        if (read_state() == TickState::invalidated) {
            output.invalidate();
            return;
        }

        visit_scalar(value_type.value_type(), [&](const auto &value) { output.set_value(value); });
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSL<TS_T, SIZE>, Specification> &output) {
        const auto &list_type = type_cast<TimeSeriesListType>(type);
        if (read_state() == TickState::invalidated) {
            output.invalidate();
            return;
        }

        const auto count = reader_->read_raw<std::uint32_t>();
        for (auto index = std::uint32_t{}; index < count; ++index) {
            const auto item_id = reader_->read_raw<bg::item_id_t>();
            apply_erased(list_type.item_type(item_id), output[item_id]);
        }
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSB<SCHEMA>, Specification> &output) {
        const auto &bundle_type = type_cast<TimeSeriesBundleType>(type);
        if (read_state() == TickState::invalidated) {
            output.invalidate();
            return;
        }

        const auto count = reader_->read_raw<std::uint32_t>();
        for (auto index = std::uint32_t{}; index < count; ++index) {
            const auto item_id = reader_->read_raw<bg::item_id_t>();
            apply_erased(bundle_type.item_type(item_id), output[item_id]);
        }
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSS<SCALAR>, Specification> &output) {
        const auto &set_type = type_cast<TimeSeriesSetType>(type);
        const auto snapshot = reader_->read_raw<std::uint8_t>() != 0;
        if (snapshot) {
            output.clear();
        }

        const auto added_count = reader_->read_raw<std::uint32_t>();
        const auto removed_count = reader_->read_raw<std::uint32_t>();
        for (auto index = std::uint32_t{}; index < added_count; ++index) {
            visit_scalar(set_type.item_type(), [&](const auto &key) { output.insert(key); });
        }
        for (auto index = std::uint32_t{}; index < removed_count; ++index) {
            visit_scalar(set_type.item_type(), [&](const auto &key) { output.erase(key); });
        }
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSD<SCALAR, TS_T>, Specification> &output) {
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(type);
        const auto snapshot = reader_->read_raw<std::uint8_t>() != 0;
        if (snapshot) {
            output.clear();
        }

        const auto removed_count = reader_->read_raw<std::uint32_t>();
        const auto added_count = reader_->read_raw<std::uint32_t>();
        const auto modified_count = reader_->read_raw<std::uint32_t>();
        for (auto index = std::uint32_t{}; index < removed_count; ++index) {
            visit_scalar(dictionary_type.key_type(), [&](const auto &key) { output.erase(key); });
        }
        for (auto index = std::uint32_t{}; index < added_count; ++index) {
            visit_scalar(dictionary_type.key_type(), [&](const auto &key) {
                apply_erased(dictionary_type.value_type(), output[key]);
            });
        }
        for (auto index = std::uint32_t{}; index < modified_count; ++index) {
            visit_scalar(dictionary_type.key_type(), [&](const auto &key) {
                apply_erased(dictionary_type.value_type(), output[key]);
            });
        }
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &, const Out<REF<TS_T>, Specification> &) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &, const Out<TS_T, Specification> &output) {
        dispatch_->replay(*reader_, output);
    }

  private:
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

    TickState read_state() {
        const auto state = reader_->read_raw<std::uint8_t>();
        if (state > static_cast<std::uint8_t>(TickState::invalidated)) {
            throw std::logic_error{"invalid time series tick state"};
        }
        return static_cast<TickState>(state);
    }

    util::buffer_reader *reader_;
    const TimeSeriesTickDispatch *dispatch_;
};

class TimeSeriesTickDiscard {
  public:
    TimeSeriesTickDiscard(util::buffer_reader &reader, const TimeSeriesTickDispatch &dispatch)
        : reader_{&reader}, dispatch_{&dispatch} {}

    void apply(const TimeSeriesType &type) {
        type.visit(
            [&](const TimeSeriesValueType *value_type) {
                if (read_state() == TickState::value) {
                    discard_scalar(value_type->value_type());
                }
            },
            [&](const TimeSeriesListType *list_type) {
                if (read_state() == TickState::invalidated) {
                    return;
                }
                const auto count = reader_->read_raw<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < count; ++index) {
                    const auto item_id = reader_->read_raw<item_id_t>();
                    apply(list_type->item_type(item_id));
                }
            },
            [&](const TimeSeriesBundleType *bundle_type) {
                if (read_state() == TickState::invalidated) {
                    return;
                }
                const auto count = reader_->read_raw<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < count; ++index) {
                    const auto item_id = reader_->read_raw<item_id_t>();
                    apply(bundle_type->item_type(item_id));
                }
            },
            [&](const TimeSeriesSetType *set_type) {
                (void)reader_->read_raw<std::uint8_t>(); // snapshot flag
                const auto added_count = reader_->read_raw<std::uint32_t>();
                const auto removed_count = reader_->read_raw<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < added_count; ++index) {
                    discard_scalar(set_type->item_type());
                }
                for (auto index = std::uint32_t{}; index < removed_count; ++index) {
                    discard_scalar(set_type->item_type());
                }
            },
            [&](const TimeSeriesDictionaryType *dictionary_type) {
                (void)reader_->read_raw<std::uint8_t>(); // snapshot flag
                const auto removed_count = reader_->read_raw<std::uint32_t>();
                const auto added_count = reader_->read_raw<std::uint32_t>();
                const auto modified_count = reader_->read_raw<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < removed_count; ++index) {
                    discard_scalar(dictionary_type->key_type());
                }
                for (auto index = std::uint32_t{}; index < added_count; ++index) {
                    discard_scalar(dictionary_type->key_type());
                    apply(dictionary_type->value_type());
                }
                for (auto index = std::uint32_t{}; index < modified_count; ++index) {
                    discard_scalar(dictionary_type->key_type());
                    apply(dictionary_type->value_type());
                }
            },
            [&](const TimeSeriesReferenceType *) {
                throw std::invalid_argument{"retained reference time series ticks are not supported"};
            },
            [&] { dispatch_->discard(type, *reader_); });
    }

  private:
    TickState read_state() {
        const auto state = reader_->read_raw<std::uint8_t>();
        if (state > static_cast<std::uint8_t>(TickState::invalidated)) {
            throw std::logic_error{"invalid time series tick state"};
        }
        return static_cast<TickState>(state);
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
