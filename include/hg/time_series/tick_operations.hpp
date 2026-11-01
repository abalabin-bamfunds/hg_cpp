#pragma once

#include <bg/allocator.hpp>
#include <bg/export.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/util/buffer.hpp>

#include <concepts>
#include <functional>
#include <utility>

namespace bg {

struct TimeSeriesTickOperations {
    using Write = std::function<void(const In<TS_T> &, ScalarBufferWriteContext &, allocator &)>;
    using Replay = std::function<void(util::buffer_reader &, OutputState &)>;
    using Discard = std::function<void(util::buffer_reader &)>;

    template <typename Series, typename Function> static TimeSeriesTickOperations for_write(Function &&function) {
        auto operations = TimeSeriesTickOperations{};
        operations.set_write<Series>(std::forward<Function>(function));
        return operations;
    }

    template <typename Output, typename Function> static TimeSeriesTickOperations for_replay(Function &&function) {
        auto operations = TimeSeriesTickOperations{};
        operations.set_replay<Output>(std::forward<Function>(function));
        return operations;
    }

    template <typename Function> static TimeSeriesTickOperations for_discard(Function &&function) {
        auto operations = TimeSeriesTickOperations{};
        operations.set_discard(std::forward<Function>(function));
        return operations;
    }

    template <typename Series, typename Output = Out<Series>, typename WriteHandler, typename ReplayHandler,
              typename DiscardHandler>
    static TimeSeriesTickOperations for_type(WriteHandler &&write, ReplayHandler &&replay,
                                             DiscardHandler &&discard) {
        auto operations = TimeSeriesTickOperations{};
        operations.set_write<Series>(std::forward<WriteHandler>(write));
        operations.set_replay<Output>(std::forward<ReplayHandler>(replay));
        operations.set_discard(std::forward<DiscardHandler>(discard));
        return operations;
    }

    template <typename Series, typename Function> bool set_write(Function &&function) {
        if (write_) {
            return false;
        }
        write_ = typed_write<Series>(std::forward<Function>(function));
        return true;
    }

    template <typename Output, typename Function> bool set_replay(Function &&function) {
        if (replay_) {
            return false;
        }
        replay_ = typed_replay<Output>(std::forward<Function>(function));
        return true;
    }

    template <typename Function> bool set_discard(Function &&function) {
        if (discard_) {
            return false;
        }
        discard_ = Discard{std::forward<Function>(function)};
        return true;
    }

    bool has_write() const noexcept { return static_cast<bool>(write_); }
    bool has_replay() const noexcept { return static_cast<bool>(replay_); }
    bool has_discard() const noexcept { return static_cast<bool>(discard_); }

    void write(const In<TS_T> &input, ScalarBufferWriteContext &writer, allocator &memory) const {
        write_(input, writer, memory);
    }

    void replay(util::buffer_reader &reader, OutputState &state) const { replay_(reader, state); }
    void discard(util::buffer_reader &reader) const { discard_(reader); }

  private:
    template <typename Series, typename Function> static Write typed_write(Function &&function) {
        return [function = std::forward<Function>(function)](const In<TS_T> &input,
                                                              ScalarBufferWriteContext &writer,
                                                              allocator &memory) {
            std::invoke(function, input.template as<Series>(), writer, memory);
        };
    }

    template <typename Output, typename Function> static Replay typed_replay(Function &&function) {
        return [function = std::forward<Function>(function)](util::buffer_reader &reader, OutputState &state) {
            const auto erased = Out<TS_T>{TimeSeriesOutputBinding{state}};
            if constexpr (std::same_as<Output, Out<TS_T>>) {
                std::invoke(function, reader, erased);
            } else {
                const auto output = erased.template as_output<Output>();
                std::invoke(function, reader, output);
            }
        };
    }

    Write write_;
    Replay replay_;
    Discard discard_;
};

template <> BG_EXPORT TimeSeriesDispatch<TimeSeriesTickOperations> &
TimeSeriesDispatch<TimeSeriesTickOperations>::instance() noexcept;

} // namespace bg
