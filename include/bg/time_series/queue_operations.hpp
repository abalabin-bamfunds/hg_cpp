#pragma once

#include <bg/allocator.hpp>
#include <bg/export.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/time_series/dispatch.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/util/buffer.hpp>

#include <functional>
#include <utility>

namespace bg {

// Queue operations encode literal payloads and consume queued payloads for a provider-defined time series.
// Project, write, conflicts, and apply belong to the exact runtime registration. Discard belongs to the semantic
// registration because it defines the payload grammar for every runtime representation.
struct TimeSeriesQueueOperations {
    using Project = std::function<void(const literal::TimeSeriesLiteral &, ScalarBufferProjectionContext &)>;
    using Write = std::function<void(const literal::TimeSeriesLiteral &, ScalarBufferWriteContext &, allocator &)>;
    using Conflicts = std::function<bool(const TimeSeriesRuntimeType::ConstItem &, EvaluationTime, bool,
                                         util::buffer_reader &)>;
    // Consume the complete payload for this provider leaf before returning or throwing. Replay does not retry a failed
    // leaf; it discards only later siblings.
    using Apply = std::function<void(util::buffer_reader &, OutputState &)>;
    using Discard = std::function<void(util::buffer_reader &)>;

    template <typename ProjectHandler, typename WriteHandler, typename ConflictsHandler, typename ApplyHandler>
    static TimeSeriesQueueOperations for_runtime(ProjectHandler &&project, WriteHandler &&write,
                                                  ConflictsHandler &&conflicts, ApplyHandler &&apply) {
        auto operations = TimeSeriesQueueOperations{};
        operations.set_project(std::forward<ProjectHandler>(project));
        operations.set_write(std::forward<WriteHandler>(write));
        operations.set_conflicts(std::forward<ConflictsHandler>(conflicts));
        operations.set_apply(std::forward<ApplyHandler>(apply));
        return operations;
    }

    template <typename DiscardHandler>
    static TimeSeriesQueueOperations for_type(DiscardHandler &&discard) {
        auto operations = TimeSeriesQueueOperations{};
        operations.set_discard(std::forward<DiscardHandler>(discard));
        return operations;
    }

    template <typename Function> bool set_project(Function &&function) {
        if (project_) {
            return false;
        }
        project_ = Project{std::forward<Function>(function)};
        return true;
    }

    template <typename Function> bool set_write(Function &&function) {
        if (write_) {
            return false;
        }
        write_ = Write{std::forward<Function>(function)};
        return true;
    }

    template <typename Function> bool set_conflicts(Function &&function) {
        if (conflicts_) {
            return false;
        }
        conflicts_ = Conflicts{std::forward<Function>(function)};
        return true;
    }

    template <typename Function> bool set_apply(Function &&function) {
        if (apply_) {
            return false;
        }
        apply_ = Apply{std::forward<Function>(function)};
        return true;
    }

    template <typename Function> bool set_discard(Function &&function) {
        if (discard_) {
            return false;
        }
        discard_ = Discard{std::forward<Function>(function)};
        return true;
    }

    bool has_project() const noexcept { return static_cast<bool>(project_); }
    bool has_write() const noexcept { return static_cast<bool>(write_); }
    bool has_conflicts() const noexcept { return static_cast<bool>(conflicts_); }
    bool has_apply() const noexcept { return static_cast<bool>(apply_); }
    bool has_discard() const noexcept { return static_cast<bool>(discard_); }

    void project(const literal::TimeSeriesLiteral &literal, ScalarBufferProjectionContext &projection) const {
        project_(literal, projection);
    }
    void write(const literal::TimeSeriesLiteral &literal, ScalarBufferWriteContext &writer,
               allocator &memory) const {
        write_(literal, writer, memory);
    }
    bool conflicts(const TimeSeriesRuntimeType::ConstItem &item, EvaluationTime now, bool incoming_valid,
                   util::buffer_reader &reader) const {
        return conflicts_(item, now, incoming_valid, reader);
    }
    void apply(util::buffer_reader &reader, OutputState &output) const { apply_(reader, output); }
    void discard(util::buffer_reader &reader) const { discard_(reader); }

  private:
    Project project_;
    Write write_;
    Conflicts conflicts_;
    Apply apply_;
    Discard discard_;
};

template <> BG_EXPORT TimeSeriesDispatch<TimeSeriesQueueOperations> &
TimeSeriesDispatch<TimeSeriesQueueOperations>::instance() noexcept;

} // namespace bg
