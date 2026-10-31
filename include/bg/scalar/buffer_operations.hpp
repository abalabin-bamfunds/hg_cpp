#pragma once

#include <bg/allocator.hpp>
#include <bg/export.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/buffer.hpp>

#include <functional>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg {

class ScalarBufferWriteContext {
  public:
    // The context borrows its writer or reservation; the caller keeps it alive and commits a reservation.
    explicit ScalarBufferWriteContext(util::buffer_writer &writer) noexcept : writer_{&writer} {}
    explicit ScalarBufferWriteContext(util::concurrent_buffer_reservation &reservation) noexcept
        : reservation_{&reservation} {}

    ScalarBufferWriteContext(const ScalarBufferWriteContext &) = delete;
    ScalarBufferWriteContext &operator=(const ScalarBufferWriteContext &) = delete;

    void write_bytes(std::span<const std::byte> bytes) {
        if (reservation_ != nullptr) {
            reservation_->write_bytes(bytes);
            return;
        }
        writer_->write_bytes(bytes);
    }

    template <typename T, typename... Args> T &emplace(Args &&...args) {
        if (reservation_ != nullptr) {
            return reservation_->emplace<T>(std::forward<Args>(args)...);
        }
        return writer_->emplace<T>(std::forward<Args>(args)...);
    }

    template <typename T> auto &write(T &&value) {
        return emplace<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

  private:
    util::buffer_writer *writer_{};
    util::concurrent_buffer_reservation *reservation_{};
};

class ScalarBufferProjectionContext {
  public:
    // The initial position is relative to a max-aligned buffer origin and must match the writer's current position.
    explicit ScalarBufferProjectionContext(std::size_t initial_position = 0) noexcept
        : initial_position_{initial_position}, position_{initial_position} {}

    void write_bytes(std::size_t size) { advance(size); }

    template <typename T> void emplace() {
        util::details::require_supported_alignment(alignof(T));
        advance(util::details::alignment_padding(position_, alignof(T)));
        advance(sizeof(T));
    }

    std::size_t position() const noexcept { return position_; }
    std::size_t size() const noexcept { return position_ - initial_position_; }

  private:
    void advance(std::size_t size) {
        if (size > std::numeric_limits<std::size_t>::max() - position_) {
            throw std::length_error{"scalar buffer projection size overflows"};
        }
        position_ += size;
    }

    std::size_t initial_position_;
    std::size_t position_;
};

namespace details {

struct ScalarBufferValueConsumer {
    void *context;
    void (*consume)(void *, Scalar<>);

    void operator()(Scalar<> value) const { consume(context, value); }
};

} // namespace details

// Provider-defined scalar encodings shared by buffer-based facilities.
struct ScalarBufferOperations {
    using Write = std::function<void(Scalar<>, ScalarBufferWriteContext &, allocator &)>;
    using Project = std::function<void(Scalar<>, ScalarBufferProjectionContext &)>;
    using Read = std::function<void(util::buffer_reader &, details::ScalarBufferValueConsumer)>;
    using Discard = std::function<void(util::buffer_reader &)>;

    template <typename ScalarType, typename Function> static ScalarBufferOperations for_read(Function &&function) {
        auto operations = ScalarBufferOperations{};
        operations.set_read<ScalarType>(std::forward<Function>(function));
        return operations;
    }

    template <typename Function> static ScalarBufferOperations for_discard(Function &&function) {
        auto operations = ScalarBufferOperations{};
        operations.set_discard(std::forward<Function>(function));
        return operations;
    }

    template <typename ScalarType, typename Function>
    bool set_write(Function &&function) {
        if (write_) {
            return false;
        }
        write_ = typed_write<ScalarType>(std::forward<Function>(function));
        return true;
    }

    template <typename ScalarType, typename Function>
    bool set_project(Function &&function) {
        if (project_) {
            return false;
        }
        project_ = typed_project<ScalarType>(std::forward<Function>(function));
        return true;
    }

    template <typename ScalarType, typename Function>
    bool set_read(Function &&function) {
        if (read_) {
            return false;
        }
        read_ = typed_read<ScalarType>(std::forward<Function>(function));
        return true;
    }

    template <typename Function>
    bool set_discard(Function &&function) {
        if (discard_) {
            return false;
        }
        discard_ = Discard{std::forward<Function>(function)};
        return true;
    }

    bool has_write() const noexcept { return static_cast<bool>(write_); }
    bool has_project() const noexcept { return static_cast<bool>(project_); }
    bool has_read() const noexcept { return static_cast<bool>(read_); }
    bool has_discard() const noexcept { return static_cast<bool>(discard_); }

    void write(Scalar<> value, ScalarBufferWriteContext &writer, allocator &memory) const {
        write_(value, writer, memory);
    }

    void project(Scalar<> value, ScalarBufferProjectionContext &projection) const { project_(value, projection); }

    void read(util::buffer_reader &reader, details::ScalarBufferValueConsumer consume) const {
        read_(reader, consume);
    }

    void discard(util::buffer_reader &reader) const { discard_(reader); }

  private:
    template <typename ScalarType, typename Function> static Write typed_write(Function &&function) {
        return [function = std::forward<Function>(function)](Scalar<> value, ScalarBufferWriteContext &writer,
                                                              allocator &memory) {
            std::invoke(function, scalar_cast<ScalarType>(value), writer, memory);
        };
    }

    template <typename ScalarType, typename Function> static Project typed_project(Function &&function) {
        return [function = std::forward<Function>(function)](Scalar<> value,
                                                              ScalarBufferProjectionContext &projection) {
            std::invoke(function, scalar_cast<ScalarType>(value), projection);
        };
    }

    template <typename ScalarType, typename Function> static Read typed_read(Function &&function) {
        return [function = std::forward<Function>(function)](util::buffer_reader &reader,
                                                              details::ScalarBufferValueConsumer consume) {
            const auto typed_consumer = [consume](Scalar<ScalarType> value) {
                consume(Scalar<>::from(value));
            };
            std::invoke(function, reader, typed_consumer);
        };
    }

    Write write_;
    Project project_;
    Read read_;
    Discard discard_;
};

template <> BG_EXPORT ScalarDispatch<ScalarBufferOperations> &
ScalarDispatch<ScalarBufferOperations>::instance() noexcept;

} // namespace bg
