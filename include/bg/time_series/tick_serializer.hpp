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
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

class TimeSeriesTickWriteContext {
  public:
    explicit TimeSeriesTickWriteContext(util::buffer_writer &writer) noexcept : writer_{&writer} {}

    TimeSeriesTickWriteContext(const TimeSeriesTickWriteContext &) = delete;
    TimeSeriesTickWriteContext &operator=(const TimeSeriesTickWriteContext &) = delete;

    ~TimeSeriesTickWriteContext() {
        if (committed_) {
            return;
        }

        for (auto value = owned_values_.rbegin(); value != owned_values_.rend(); ++value) {
            if (value->data != nullptr) {
                value->destroy(value->data);
            }
        }
        writer_->rollback();
    }

    void write_bytes(std::span<const std::byte> bytes) { writer_->write_bytes(bytes); }

    template <typename T, typename... Args> T &emplace(Args &&...args) {
        auto journal_index = std::size_t{};
        if constexpr (!std::is_trivially_destructible_v<T>) {
            journal_index = owned_values_.size();
            owned_values_.push_back({nullptr, [](void *value) noexcept { std::destroy_at(static_cast<T *>(value)); }});
        }

        auto &value = writer_->emplace<T>(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            owned_values_[journal_index].data = &value;
        }
        return value;
    }

    template <typename T> auto &write(T &&value) {
        return emplace<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

    void commit() noexcept {
        writer_->commit();
        committed_ = true;
    }

  private:
    struct OwnedValue {
        void *data;
        void (*destroy)(void *) noexcept;
    };

    util::buffer_writer *writer_;
    std::vector<OwnedValue> owned_values_;
    bool committed_{};
};

class TimeSeriesTickReadContext {
  public:
    explicit TimeSeriesTickReadContext(util::buffer_reader &reader) noexcept : reader_{&reader} {}

    std::span<const std::byte> readable() { return reader_->readable(); }

    void consume(std::size_t size) {
        while (size != 0) {
            const auto available = reader_->readable();
            if (available.empty()) {
                throw std::out_of_range{"incomplete time series tick record"};
            }

            const auto count = std::min(size, available.size());
            reader_->consume(count);
            size -= count;
        }
    }

    void read_bytes(std::span<std::byte> destination) {
        while (!destination.empty()) {
            const auto available = reader_->readable();
            if (available.empty()) {
                throw std::out_of_range{"incomplete time series tick record"};
            }

            const auto count = std::min(destination.size(), available.size());
            std::memcpy(destination.data(), available.data(), count);
            reader_->consume(count);
            destination = destination.subspan(count);
        }
    }

    template <typename T> T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        auto value = T{};
        read_bytes(std::as_writable_bytes(std::span{&value, std::size_t{1}}));
        return value;
    }

    template <typename T, typename Function> void visit_object(Function &&function) {
        std::invoke(std::forward<Function>(function), reader_->peek<T>());
        reader_->consume<T>();
    }

  protected:
    util::buffer_reader *reader_;
};

class TimeSeriesTickDiscardContext : public TimeSeriesTickReadContext {
  public:
    explicit TimeSeriesTickDiscardContext(util::buffer_reader &reader) noexcept : TimeSeriesTickReadContext{reader} {}

    template <typename T> void discard() { reader_->skip<T>(); }
};

namespace details {

struct ScalarTickValueConsumer {
    void *context;
    void (*consume)(void *, Scalar<>);

    void operator()(Scalar<> value) const { consume(context, value); }
};

struct TimeSeriesTickScalarOperations {
    using Capture = std::function<void(Scalar<>, TimeSeriesTickWriteContext &, allocator &)>;
    using Replay = std::function<void(TimeSeriesTickReadContext &, ScalarTickValueConsumer)>;
    using Discard = std::function<void(TimeSeriesTickDiscardContext &)>;

    Capture capture;
    Replay replay;
    Discard discard;
};

struct TimeSeriesTickOperations {
    using Capture = std::function<void(const In<TS_T> &, bool, TimeSeriesTickWriteContext &, allocator &)>;
    using DefaultReplay = std::function<void(TimeSeriesTickReadContext &, const Out<TS_T> &)>;
    using ViewReplay = std::function<void(TimeSeriesTickReadContext &, const Out<TS_T, view> &)>;

    Capture capture;
    DefaultReplay default_replay;
    ViewReplay view_replay;
    std::function<void(TimeSeriesTickDiscardContext &)> discard;
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

    // Register capture, replay, and discard handlers for every runtime representation of this scalar type.
    template <typename ScalarType, typename Capture, typename Replay, typename Discard>
    bool add_scalar_handlers(Capture &&capture, Replay &&replay, Discard &&discard) {
        auto operations = details::TimeSeriesTickScalarOperations{
            typed_scalar_capture<ScalarType>(std::forward<Capture>(capture)),
            typed_scalar_replay<ScalarType>(std::forward<Replay>(replay)),
            details::TimeSeriesTickScalarOperations::Discard{std::forward<Discard>(discard)}};
        return scalar_operations_.add(cpp_scalar_type<ScalarType>(), std::move(operations));
    }

    void capture_scalar(Scalar<> value, TimeSeriesTickWriteContext &writer, allocator &memory) const {
        if (const auto *operations = scalar_operations_.find(value.type()); operations != nullptr) {
            operations->capture(value, writer, memory);
            return;
        }

        writer.write(ScalarValue<>{value, memory});
    }

    template <typename Function> bool replay_scalar(const ScalarType &type, TimeSeriesTickReadContext &reader,
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

    bool discard_scalar(const ScalarType &type, TimeSeriesTickDiscardContext &reader) const {
        const auto *operations = scalar_operations_.find(type);
        if (operations == nullptr) {
            return false;
        }

        operations->discard(reader);
        return true;
    }

    template <typename Series, typename Capture>
    bool add_capture(const TimeSeriesRuntimeType &runtime_type, Capture &&capture) {
        const auto *existing = runtime_operations_.find(&runtime_type);
        if (existing != nullptr && existing->capture) {
            return false;
        }

        auto handler = details::TimeSeriesTickOperations::Capture{
            typed_capture<Series>(std::forward<Capture>(capture))};
        auto &operations = *runtime_operations_.try_emplace(&runtime_type, memory_.memory_resource()).first;
        operations.capture = std::move(handler);
        return true;
    }

    template <typename Series, typename Capture>
    bool add_capture(const TimeSeriesType &type, Capture &&capture) {
        const auto *existing = type_operations_.find(&type);
        if (existing != nullptr && existing->capture) {
            return false;
        }

        auto handler = details::TimeSeriesTickOperations::Capture{
            typed_capture<Series>(std::forward<Capture>(capture))};
        auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
        operations.capture = std::move(handler);
        return true;
    }

    template <typename Series, OutputSpecification Specification, typename Replay>
    bool add_replay(const TimeSeriesRuntimeType &runtime_type, Replay &&replay) {
        const auto *existing = runtime_operations_.find(&runtime_type);
        if (existing != nullptr && specific_replay<Specification>(*existing)) {
            return false;
        }

        if constexpr (Specification == OutputSpecification::default_) {
            auto handler = details::TimeSeriesTickOperations::DefaultReplay{
                typed_replay<Series, Specification>(std::forward<Replay>(replay))};
            auto &operations = *runtime_operations_.try_emplace(&runtime_type, memory_.memory_resource()).first;
            operations.default_replay = std::move(handler);
        } else {
            auto handler = details::TimeSeriesTickOperations::ViewReplay{
                typed_replay<Series, Specification>(std::forward<Replay>(replay))};
            auto &operations = *runtime_operations_.try_emplace(&runtime_type, memory_.memory_resource()).first;
            operations.view_replay = std::move(handler);
        }
        return true;
    }

    template <typename Series, OutputSpecification Specification, typename Replay>
    bool add_replay(const TimeSeriesType &type, Replay &&replay) {
        const auto *existing = type_operations_.find(&type);
        if (existing != nullptr && specific_replay<Specification>(*existing)) {
            return false;
        }

        if constexpr (Specification == OutputSpecification::default_) {
            auto handler = details::TimeSeriesTickOperations::DefaultReplay{
                typed_replay<Series, Specification>(std::forward<Replay>(replay))};
            auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
            operations.default_replay = std::move(handler);
        } else {
            auto handler = details::TimeSeriesTickOperations::ViewReplay{
                typed_replay<Series, Specification>(std::forward<Replay>(replay))};
            auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
            operations.view_replay = std::move(handler);
        }
        return true;
    }

    template <typename Discard>
    bool add_discard(const TimeSeriesType &type, Discard &&discard) {
        const auto *existing = type_operations_.find(&type);
        if (existing != nullptr && existing->discard) {
            return false;
        }

        auto handler = std::function<void(TimeSeriesTickDiscardContext &)>{std::forward<Discard>(discard)};
        auto &operations = *type_operations_.try_emplace(&type, memory_.memory_resource()).first;
        operations.discard = std::move(handler);
        return true;
    }

    void capture(const In<TS_T> &input, bool snapshot, TimeSeriesTickWriteContext &writer, allocator &memory) const {
        const auto *semantic_operations = type_operations_.find(&input.runtime_type().type());
        if (semantic_operations == nullptr || !semantic_operations->discard) {
            throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
        }

        if (const auto *operations = runtime_operations_.find(&input.runtime_type());
            operations != nullptr && operations->capture) {
            operations->capture(input, snapshot, writer, memory);
            return;
        }

        if (const auto *operations = type_operations_.find(&input.runtime_type().type());
            operations != nullptr && operations->capture) {
            operations->capture(input, snapshot, writer, memory);
            return;
        }

        throw std::invalid_argument{"no tick capture operation is registered for this time series type"};
    }

    template <OutputSpecification Specification>
    void replay(TimeSeriesTickReadContext &reader, const Out<TS_T, Specification> &output) const {
        if (const auto *operations = runtime_operations_.find(&output.runtime_type());
            operations != nullptr && replay_from<Specification>(*operations, reader, output)) {
            return;
        }

        if (const auto *operations = type_operations_.find(&output.runtime_type().type());
            operations != nullptr && replay_from<Specification>(*operations, reader, output)) {
            return;
        }

        throw std::invalid_argument{"no tick replay operation is registered for this time series type"};
    }

    void discard(const TimeSeriesType &type, TimeSeriesTickDiscardContext &reader) const {
        const auto *operations = type_operations_.find(&type);
        if (operations == nullptr || !operations->discard) {
            throw std::invalid_argument{"no tick discard operation is registered for this time series type"};
        }
        operations->discard(reader);
    }

  private:
    template <typename ScalarType, typename Capture> static auto typed_scalar_capture(Capture &&capture) {
        return [capture = std::forward<Capture>(capture)](Scalar<> value, TimeSeriesTickWriteContext &writer,
                                                          allocator &memory) {
            std::invoke(capture, scalar_cast<ScalarType>(value), writer, memory);
        };
    }

    template <typename ScalarType, typename Replay> static auto typed_scalar_replay(Replay &&replay) {
        return [replay = std::forward<Replay>(replay)](TimeSeriesTickReadContext &reader,
                                                       details::ScalarTickValueConsumer consume) {
            auto typed_consumer = [consume](Scalar<ScalarType> value) {
                consume(Scalar<>::from(value));
            };
            std::invoke(replay, reader, typed_consumer);
        };
    }

    template <OutputSpecification Specification, typename Operations>
    static auto &specific_replay(Operations &operations) {
        if constexpr (Specification == OutputSpecification::default_) {
            return operations.default_replay;
        } else {
            return operations.view_replay;
        }
    }

    template <typename Series, typename Capture>
    static auto typed_capture(Capture &&capture) {
        return [capture = std::forward<Capture>(capture)](const In<TS_T> &input, bool snapshot,
                                                          TimeSeriesTickWriteContext &writer, allocator &memory) {
            std::invoke(capture, input.template as<Series>(), snapshot, writer, memory);
        };
    }

    template <typename Series, OutputSpecification Specification, typename Replay>
    static auto typed_replay(Replay &&replay) {
        return [replay = std::forward<Replay>(replay)](TimeSeriesTickReadContext &reader,
                                                       const Out<TS_T, Specification> &output) {
            std::invoke(replay, reader, output.template as<Series>());
        };
    }

    template <OutputSpecification Specification>
    static bool replay_from(const details::TimeSeriesTickOperations &operations, TimeSeriesTickReadContext &reader,
                            const Out<TS_T, Specification> &output) {
        const auto &specific = specific_replay<Specification>(operations);
        if (specific) {
            specific(reader, output);
            return true;
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

class TimeSeriesTickCapture {
  public:
    TimeSeriesTickCapture(TimeSeriesTickWriteContext &writer, allocator &memory,
                          const TimeSeriesTickDispatch &dispatch)
        : writer_{&writer}, memory_{&memory}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    bool capture(const In<Variable, Properties...> &input, EvaluationTime now) {
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
        dispatch_->capture(*input, snapshot, *writer_, *memory_);
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
            dispatch_->capture_scalar(Scalar<>::from(value), *writer_, *memory_);
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
    TimeSeriesTickReplay(TimeSeriesTickReadContext &reader, const TimeSeriesTickDispatch &dispatch)
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

        const auto count = reader_->read<std::uint32_t>();
        for (auto index = std::uint32_t{}; index < count; ++index) {
            const auto item_id = reader_->read<bg::item_id_t>();
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

        const auto count = reader_->read<std::uint32_t>();
        for (auto index = std::uint32_t{}; index < count; ++index) {
            const auto item_id = reader_->read<bg::item_id_t>();
            apply_erased(bundle_type.item_type(item_id), output[item_id]);
        }
    }

    template <OutputSpecification Specification>
    void apply_concrete(const TimeSeriesType &type, const Out<TSS<SCALAR>, Specification> &output) {
        const auto &set_type = type_cast<TimeSeriesSetType>(type);
        const auto snapshot = reader_->read<std::uint8_t>() != 0;
        if (snapshot) {
            output.clear();
        }

        const auto added_count = reader_->read<std::uint32_t>();
        const auto removed_count = reader_->read<std::uint32_t>();
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
        const auto snapshot = reader_->read<std::uint8_t>() != 0;
        if (snapshot) {
            output.clear();
        }

        const auto removed_count = reader_->read<std::uint32_t>();
        const auto added_count = reader_->read<std::uint32_t>();
        const auto modified_count = reader_->read<std::uint32_t>();
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
                const auto stored = reader_->template read<Stored>();
                const auto value = Scalar<Builtin>::from(stored);
                std::invoke(function, Scalar<>::from(value));
            } else {
                reader_->template visit_object<ScalarValue<>>(
                    [&](const auto &value) { std::invoke(function, value.view()); });
            }
        });

        if (!matched && !dispatch_->replay_scalar(type, *reader_, function)) {
            reader_->template visit_object<ScalarValue<>>(
                [&](const auto &value) { std::invoke(function, value.view()); });
        }
    }

    TickState read_state() {
        const auto state = reader_->read<std::uint8_t>();
        if (state > static_cast<std::uint8_t>(TickState::invalidated)) {
            throw std::logic_error{"invalid time series tick state"};
        }
        return static_cast<TickState>(state);
    }

    TimeSeriesTickReadContext *reader_;
    const TimeSeriesTickDispatch *dispatch_;
};

class TimeSeriesTickDiscard {
  public:
    TimeSeriesTickDiscard(TimeSeriesTickDiscardContext &reader, const TimeSeriesTickDispatch &dispatch)
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
                const auto count = reader_->read<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < count; ++index) {
                    const auto item_id = reader_->read<item_id_t>();
                    apply(list_type->item_type(item_id));
                }
            },
            [&](const TimeSeriesBundleType *bundle_type) {
                if (read_state() == TickState::invalidated) {
                    return;
                }
                const auto count = reader_->read<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < count; ++index) {
                    const auto item_id = reader_->read<item_id_t>();
                    apply(bundle_type->item_type(item_id));
                }
            },
            [&](const TimeSeriesSetType *set_type) {
                (void)reader_->read<std::uint8_t>(); // snapshot flag
                const auto added_count = reader_->read<std::uint32_t>();
                const auto removed_count = reader_->read<std::uint32_t>();
                for (auto index = std::uint32_t{}; index < added_count; ++index) {
                    discard_scalar(set_type->item_type());
                }
                for (auto index = std::uint32_t{}; index < removed_count; ++index) {
                    discard_scalar(set_type->item_type());
                }
            },
            [&](const TimeSeriesDictionaryType *dictionary_type) {
                (void)reader_->read<std::uint8_t>(); // snapshot flag
                const auto removed_count = reader_->read<std::uint32_t>();
                const auto added_count = reader_->read<std::uint32_t>();
                const auto modified_count = reader_->read<std::uint32_t>();
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
        const auto state = reader_->read<std::uint8_t>();
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
                reader_->template discard<ScalarValue<>>();
            }
        });
        if (!matched && !dispatch_->discard_scalar(type, *reader_)) {
            reader_->template discard<ScalarValue<>>();
        }
    }

    TimeSeriesTickDiscardContext *reader_;
    const TimeSeriesTickDispatch *dispatch_;
};

} // namespace details

class TimeSeriesTickWriter {
  public:
    explicit TimeSeriesTickWriter(util::buffer buffer, allocator memory = {},
                                  const TimeSeriesTickDispatch &dispatch = default_dispatch())
        : writer_{std::move(buffer)}, memory_{memory}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    // Capture a change made at `now`. The caller supplies the evaluation time
    // so an older invalidation is not recorded again when another input ticks.
    bool write(const In<Variable, Properties...> &input, EvaluationTime now) {
        const auto &type = input.runtime_type().type();
        if (type_ != nullptr && type_ != &type) {
            throw std::invalid_argument{"a tick writer cannot mix semantic time series types"};
        }

        auto writer = TimeSeriesTickWriteContext{writer_};
        auto capture = details::TimeSeriesTickCapture{writer, memory_, *dispatch_};
        if (!capture.capture(input, now)) {
            return false;
        }

        writer.commit();
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
        auto read_context = TimeSeriesTickReadContext{replay_buffer};
        auto replay = details::TimeSeriesTickReplay{read_context, *dispatch_};
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
        auto context = TimeSeriesTickDiscardContext{reader_};
        details::TimeSeriesTickDiscard{context, *dispatch_}.apply(*type_);
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
