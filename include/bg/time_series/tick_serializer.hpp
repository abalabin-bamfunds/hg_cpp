#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/buffer.hpp>
#include <bg/util/hash_dictionary.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

namespace details {

struct TimeSeriesTickOperation {
    enum class Kind : std::uint8_t {
        header,
        invalidated,
        value,
        fixed_list,
        bundle,
        set,
        dictionary,
        index,
        set_key,
        dictionary_item,
        dictionary_remove,
        extension,
    };

    explicit TimeSeriesTickOperation(Kind kind) noexcept : kind{kind} {}

    Kind kind;
    bool flag{};
    std::uint32_t count{};
    item_id_t index{};
    const TimeSeriesType *type{};
    std::optional<ScalarValue<>> scalar;
};

static_assert(std::is_nothrow_move_constructible_v<TimeSeriesTickOperation>);

} // namespace details

// Extension handlers encode one provider-specific delta as an owned scalar value.
struct TimeSeriesTickOperations {
    // `snapshot` is true when capture must store the provider's complete value.
    void (*capture)(const In<TS_T> &, bool, std::optional<ScalarValue<>> &, allocator &){};
    void (*replay)(const ScalarValue<> &, const Out<TS_T> &){};
};

class TimeSeriesTickDispatch {
  public:
    explicit TimeSeriesTickDispatch(allocator memory = {}) noexcept : memory_{memory} {}

    ~TimeSeriesTickDispatch() {
        runtime_operations_.release(memory_.memory_resource());
        type_operations_.release(memory_.memory_resource());
    }

    TimeSeriesTickDispatch(const TimeSeriesTickDispatch &) = delete;
    TimeSeriesTickDispatch &operator=(const TimeSeriesTickDispatch &) = delete;

    bool add(const TimeSeriesRuntimeType &runtime_type, TimeSeriesTickOperations operations) {
        return runtime_operations_.try_emplace(&runtime_type, memory_.memory_resource(), operations).second;
    }

    bool add(const TimeSeriesType &type, TimeSeriesTickOperations operations) {
        return type_operations_.try_emplace(&type, memory_.memory_resource(), operations).second;
    }

    const TimeSeriesTickOperations *find(const TimeSeriesRuntimeType &runtime_type) const noexcept {
        if (const auto *operations = runtime_operations_.find(&runtime_type)) {
            return operations;
        }
        return type_operations_.find(&runtime_type.type());
    }

  private:
    allocator memory_;
    util::hash_dictionary<const TimeSeriesRuntimeType *, TimeSeriesTickOperations> runtime_operations_;
    util::hash_dictionary<const TimeSeriesType *, TimeSeriesTickOperations> type_operations_;
};

namespace details {

class TimeSeriesTickCapture {
  public:
    TimeSeriesTickCapture(std::pmr::vector<TimeSeriesTickOperation> &operations, allocator &memory,
                          const TimeSeriesTickDispatch &dispatch)
        : operations_{&operations}, memory_{&memory}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    bool capture(const In<Variable, Properties...> &input, EvaluationTime now, bool snapshot = false) {
        if (input.runtime_type().is_reference()) {
            throw std::invalid_argument{"retained reference time series ticks are not supported"};
        }
        if (!snapshot) {
            if (input.last_updated() != now) {
                return false;
            }
            if (input.valid() ? !input.modified() : !input.became_invalid()) {
                return false;
            }
        }

        snapshot = snapshot || input.became_valid();
        input.visit(ddv::serial{std::ref(*this)}, now, snapshot);
        return true;
    }

    void operator()(In<TS<SCALAR>> *input, EvaluationTime, bool snapshot) {
        if (!input->valid()) {
            if (snapshot || input->became_invalid()) {
                operations_->emplace_back(TimeSeriesTickOperation::Kind::invalidated);
            }
            return;
        }

        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::value);
        operation.scalar.emplace(input->value_view(), *memory_);
    }

    void operator()(In<TSL<TS_T, SIZE>> *input, EvaluationTime now, bool snapshot) {
        if (!input->valid()) {
            if (snapshot || input->became_invalid()) {
                operations_->emplace_back(TimeSeriesTickOperation::Kind::invalidated);
            }
            return;
        }

        const auto begin = operations_->size();
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::fixed_list);
        auto entry_count = std::uint32_t{};
        if (snapshot) {
            for (auto index = item_id_t{}; index < input->size(); ++index) {
                entry_count += capture_index(index, (*input)[index], now, true);
            }
        } else {
            input->for_each_updated([&](item_id_t index) {
                entry_count += capture_index(index, (*input)[index], now, false);
            });
        }
        operations_->at(begin).count = entry_count;
    }

    void operator()(In<TSB<SCHEMA>> *input, EvaluationTime now, bool snapshot) {
        if (!input->valid()) {
            if (snapshot || input->became_invalid()) {
                operations_->emplace_back(TimeSeriesTickOperation::Kind::invalidated);
            }
            return;
        }

        const auto begin = operations_->size();
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::bundle);
        auto entry_count = std::uint32_t{};
        if (snapshot) {
            for (auto index = item_id_t{}; index < input->size(); ++index) {
                entry_count += capture_index(index, (*input)[index], now, true);
            }
        } else {
            input->for_each_updated([&](item_id_t index) {
                entry_count += capture_index(index, (*input)[index], now, false);
            });
        }
        operations_->at(begin).count = entry_count;
    }

    void operator()(In<TSS<SCALAR>> *input, EvaluationTime, bool snapshot) {
        if (!input->valid()) {
            if (snapshot || input->became_invalid()) {
                throw std::invalid_argument{"TSS invalidation cannot be replayed through its output API"};
            }
            return;
        }

        const auto begin = operations_->size();
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::set);
        operation.flag = snapshot;
        auto entry_count = std::uint32_t{};
        if (snapshot) {
            input->for_each([&](const auto &key) {
                capture_set_key(true, Scalar<>::from(key));
                ++entry_count;
            });
        } else {
            input->for_each_added([&](const auto &key) {
                capture_set_key(true, Scalar<>::from(key));
                ++entry_count;
            });
            input->for_each_removed([&](const auto &key) {
                capture_set_key(false, Scalar<>::from(key));
                ++entry_count;
            });
        }
        operations_->at(begin).count = entry_count;
    }

    void operator()(In<TSD<SCALAR, TS_T>> *input, EvaluationTime now, bool snapshot) {
        if (!input->valid()) {
            if (snapshot || input->became_invalid()) {
                throw std::invalid_argument{"TSD invalidation cannot be replayed through its output API"};
            }
            return;
        }

        const auto begin = operations_->size();
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::dictionary);
        operation.flag = snapshot;
        auto entry_count = std::uint32_t{};
        if (snapshot) {
            input->for_each_key_value([&](const auto &key, const auto &item) {
                entry_count += capture_dictionary_item(Scalar<>::from(key), item, now, true);
            });
        } else {
            input->for_each_removed([&](const auto &key) {
                capture_dictionary_remove(Scalar<>::from(key));
                ++entry_count;
            });
            input->for_each_added([&](const auto &key) {
                entry_count += capture_dictionary_item(Scalar<>::from(key), (*input)[key], now, true);
            });
            input->for_each_modified([&](const auto &key) {
                entry_count += capture_dictionary_item(Scalar<>::from(key), (*input)[key], now, false);
            });
        }
        operations_->at(begin).count = entry_count;
    }

    void operator()(In<REF<TS_T>> *, EvaluationTime, bool) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    void operator()(In<TS_T> *input, EvaluationTime, bool snapshot) {
        const auto *handler = dispatch_->find(input->runtime_type());
        if (handler == nullptr || handler->capture == nullptr) {
            throw std::invalid_argument{"no tick capture operation is registered for this time series type"};
        }
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::extension);
        handler->capture(*input, snapshot, operation.scalar, *memory_);
        if (!operation.scalar) {
            throw std::logic_error{"a time series extension did not write its tick payload"};
        }
    }

  private:
    bool capture_index(item_id_t index, const In<TS_T> &input, EvaluationTime now, bool snapshot) {
        const auto start = operations_->size();
        if (!capture(input, now, snapshot)) {
            return false;
        }
        auto entry = operations_->emplace(operations_->begin() + static_cast<std::ptrdiff_t>(start),
                                          TimeSeriesTickOperation::Kind::index);
        entry->index = index;
        return true;
    }

    void capture_set_key(bool added, const Scalar<> &key) {
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::set_key);
        operation.flag = added;
        operation.scalar.emplace(key, *memory_);
    }

    void capture_dictionary_remove(const Scalar<> &key) {
        auto &operation = operations_->emplace_back(TimeSeriesTickOperation::Kind::dictionary_remove);
        operation.scalar.emplace(key, *memory_);
    }

    bool capture_dictionary_item(const Scalar<> &key, const In<TS_T> &input, EvaluationTime now, bool snapshot) {
        const auto start = operations_->size();
        if (!capture(input, now, snapshot)) {
            return false;
        }
        auto entry = operations_->emplace(operations_->begin() + static_cast<std::ptrdiff_t>(start),
                                          TimeSeriesTickOperation::Kind::dictionary_item);
        entry->scalar.emplace(key, *memory_);
        return true;
    }

    std::pmr::vector<TimeSeriesTickOperation> *operations_;
    allocator *memory_;
    const TimeSeriesTickDispatch *dispatch_;
};

class TimeSeriesTickReplay {
  public:
    TimeSeriesTickReplay(util::buffer_reader &reader, const TimeSeriesTickDispatch &dispatch, std::size_t &remaining)
        : reader_{&reader}, dispatch_{&dispatch}, remaining_{&remaining} {}

    template <typename Variable, auto... Properties>
    void apply(const Out<Variable, Properties...> &output) {
        auto operation = next();
        output.visit([&](const auto *typed) { apply(operation, *typed); }, ddv::noop);
    }

    void operator()(TimeSeriesTickOperation &operation, const Out<TS<SCALAR>> &output) {
        if (operation.kind == TimeSeriesTickOperation::Kind::invalidated) {
            output.invalidate();
            return;
        }
        require(operation.kind == TimeSeriesTickOperation::Kind::value && operation.scalar.has_value());
        output.set_value(operation.scalar->view());
    }

    void operator()(TimeSeriesTickOperation &operation, const Out<TSL<TS_T, SIZE>> &output) {
        if (operation.kind == TimeSeriesTickOperation::Kind::invalidated) {
            output.invalidate();
            return;
        }
        require(operation.kind == TimeSeriesTickOperation::Kind::fixed_list);
        for (auto i = std::uint32_t{}; i < operation.count; ++i) {
            auto entry = next();
            require(entry.kind == TimeSeriesTickOperation::Kind::index);
            apply(output[entry.index]);
        }
    }

    void operator()(TimeSeriesTickOperation &operation, const Out<TSB<SCHEMA>> &output) {
        if (operation.kind == TimeSeriesTickOperation::Kind::invalidated) {
            output.invalidate();
            return;
        }
        require(operation.kind == TimeSeriesTickOperation::Kind::bundle);
        for (auto i = std::uint32_t{}; i < operation.count; ++i) {
            auto entry = next();
            require(entry.kind == TimeSeriesTickOperation::Kind::index);
            apply(output[entry.index]);
        }
    }

    void operator()(TimeSeriesTickOperation &operation, const Out<TSS<SCALAR>> &output) {
        require(operation.kind == TimeSeriesTickOperation::Kind::set);
        if (operation.flag) {
            output.clear();
        }
        for (auto i = std::uint32_t{}; i < operation.count; ++i) {
            auto key = next();
            require(key.kind == TimeSeriesTickOperation::Kind::set_key && key.scalar.has_value());
            if (key.flag) {
                output.insert(key.scalar->view());
            } else {
                output.erase(key.scalar->view());
            }
        }
    }

    void operator()(TimeSeriesTickOperation &operation, const Out<TSD<SCALAR, TS_T>> &output) {
        require(operation.kind == TimeSeriesTickOperation::Kind::dictionary);
        if (operation.flag) {
            output.clear();
        }
        for (auto i = std::uint32_t{}; i < operation.count; ++i) {
            auto entry = next();
            if (entry.kind == TimeSeriesTickOperation::Kind::dictionary_remove) {
                require(entry.scalar.has_value());
                output.erase(entry.scalar->view());
                continue;
            }
            require(entry.kind == TimeSeriesTickOperation::Kind::dictionary_item && entry.scalar.has_value());
            auto item = output[entry.scalar->view()];
            apply(item);
        }
    }

    void operator()(TimeSeriesTickOperation &, const Out<REF<TS_T>> &) {
        throw std::invalid_argument{"retained reference time series ticks are not supported"};
    }

    void operator()(TimeSeriesTickOperation &operation, const Out<TS_T> &output) {
        const auto *handler = dispatch_->find(output.runtime_type());
        if (operation.kind != TimeSeriesTickOperation::Kind::extension || handler == nullptr ||
            handler->replay == nullptr || !operation.scalar) {
            throw std::invalid_argument{"no tick replay operation is registered for this time series type"};
        }
        handler->replay(*operation.scalar, output);
    }

  private:
    static void require(bool condition) {
        if (!condition) {
            throw std::logic_error{"time series tick operations do not match their output shape"};
        }
    }

    TimeSeriesTickOperation next() {
        require(*remaining_ != 0);
        --*remaining_;
        return reader_->read<TimeSeriesTickOperation>();
    }

    template <typename Output> void apply(TimeSeriesTickOperation &operation, const Output &output) {
        (*this)(operation, output);
    }

    util::buffer_reader *reader_;
    const TimeSeriesTickDispatch *dispatch_;
    std::size_t *remaining_;
};

} // namespace details

class TimeSeriesTickWriter {
  public:
    explicit TimeSeriesTickWriter(util::buffer buffer, allocator memory = {},
                                  const TimeSeriesTickDispatch &dispatch = default_dispatch())
        : writer_{std::move(buffer)}, memory_{memory}, operations_{&memory_.memory_resource()}, dispatch_{&dispatch} {}

    template <typename Variable, auto... Properties>
    // Capture a change made at `now`. The caller supplies the evaluation time
    // so an older invalidation is not recorded again when another input ticks.
    bool write(const In<Variable, Properties...> &input, EvaluationTime now) {
        operations_.clear();
        try {
            auto capture = details::TimeSeriesTickCapture{operations_, memory_, *dispatch_};
            if (!capture.capture(input, now)) {
                return false;
            }

            const auto count = operations_.size();
            if (count >= std::numeric_limits<std::uint32_t>::max() ||
                count + 1 > std::numeric_limits<std::size_t>::max() / sizeof(details::TimeSeriesTickOperation)) {
                throw std::length_error{"time series tick has too many operations"};
            }

            const auto byte_count = (count + 1) * sizeof(details::TimeSeriesTickOperation);
            auto reservation = writer_.reserve(byte_count, alignof(details::TimeSeriesTickOperation));
            auto header = details::TimeSeriesTickOperation{details::TimeSeriesTickOperation::Kind::header};
            header.type = &input.runtime_type().type();
            header.count = static_cast<std::uint32_t>(count);
            reservation.write(std::move(header));
            for (auto &operation : operations_) {
                reservation.write(std::move(operation));
            }
            reservation.commit();
            operations_.clear();
            return true;
        } catch (...) {
            operations_.clear();
            throw;
        }
    }

  private:
    static const TimeSeriesTickDispatch &default_dispatch() {
        static const TimeSeriesTickDispatch dispatch;
        return dispatch;
    }

    util::buffer_writer writer_;
    allocator memory_;
    std::pmr::vector<details::TimeSeriesTickOperation> operations_;
    const TimeSeriesTickDispatch *dispatch_;
};

class TimeSeriesTickReader {
  public:
    explicit TimeSeriesTickReader(util::buffer buffer, const TimeSeriesTickDispatch &dispatch = default_dispatch())
        : reader_{std::move(buffer)}, dispatch_{&dispatch} {}

    ~TimeSeriesTickReader() {
        try {
            while (has_next()) {
                discard();
            }
        } catch (...) {
            std::terminate();
        }
    }

    // The reader destroys unread records on destruction. A reader must be
    // created for a buffer containing records with owned scalar payloads.
    bool has_next() { return !reader_.readable().empty(); }

    template <typename Variable, auto... Properties>
    bool replay(const Out<Variable, Properties...> &output) {
        if (!has_next()) {
            return false;
        }

        const auto &header = reader_.peek<details::TimeSeriesTickOperation>();
        require_header(header);
        if (!output.runtime_type().type().accepts(*header.type)) {
            throw std::invalid_argument{"time series tick record has a different semantic output type"};
        }

        auto consumed_header = reader_.read<details::TimeSeriesTickOperation>();
        auto remaining = static_cast<std::size_t>(consumed_header.count);
        auto replay = details::TimeSeriesTickReplay{reader_, *dispatch_, remaining};
        try {
            output.visit([&](const auto *typed) { replay.apply(*typed); }, ddv::noop);
            if (remaining != 0) {
                throw std::logic_error{"time series tick has unconsumed operations"};
            }
        } catch (...) {
            while (remaining != 0) {
                --remaining;
                reader_.skip<details::TimeSeriesTickOperation>();
            }
            throw;
        }
        return true;
    }

    bool discard() {
        if (!has_next()) {
            return false;
        }
        const auto header = reader_.read<details::TimeSeriesTickOperation>();
        require_header(header);
        for (auto i = std::uint32_t{}; i < header.count; ++i) {
            reader_.skip<details::TimeSeriesTickOperation>();
        }
        return true;
    }

  private:
    static const TimeSeriesTickDispatch &default_dispatch() {
        static const TimeSeriesTickDispatch dispatch;
        return dispatch;
    }

    static void require_header(const details::TimeSeriesTickOperation &header) {
        if (header.kind != details::TimeSeriesTickOperation::Kind::header || header.type == nullptr) {
            throw std::logic_error{"buffer does not contain a time series tick record"};
        }
    }

    util::buffer_reader reader_;
    const TimeSeriesTickDispatch *dispatch_;
};

} // namespace bg
