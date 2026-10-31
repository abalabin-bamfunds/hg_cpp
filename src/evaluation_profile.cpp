#include <bg/evaluation_profile.hpp>

#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/util/scope_exit.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace bg {
namespace {

constexpr auto record_tag_shift = 30;
constexpr auto record_tag_mask = std::uint32_t{3} << record_tag_shift;
constexpr auto record_value_mask = ~record_tag_mask;
constexpr auto node_tag = std::uint32_t{0} << record_tag_shift;
constexpr auto cycle_tag = std::uint32_t{1} << record_tag_shift;
constexpr auto graph_tag = std::uint32_t{2} << record_tag_shift;
constexpr auto cycle_start_event = cycle_tag | 1;
constexpr auto cycle_end_event = cycle_tag | 2;
constexpr auto graph_start_event = graph_tag | 1;
constexpr auto graph_start_with_label_event = graph_tag | 2;
constexpr auto graph_stop_event = graph_tag | 3;
constexpr auto graph_entry_event = graph_tag | 4;
constexpr auto graph_exit_event = graph_tag | 5;
constexpr auto stream_magic = std::uint64_t{0x424750524f46494c};
constexpr auto stream_version = std::uint32_t{3};

struct StreamHeader {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t clock;
};

struct CycleMetadata {
    std::int64_t evaluation_time_microseconds;
    std::int64_t wall_clock_time_microseconds;
    std::int64_t steady_clock_nanoseconds;
};

struct CycleEndMetadata {
    std::int64_t wall_clock_time_microseconds;
    std::int64_t steady_clock_nanoseconds;
};

static_assert(sizeof(StreamHeader) == 16);
static_assert(sizeof(CycleMetadata) == 24);
static_assert(sizeof(CycleEndMetadata) == 16);
static_assert(std::is_trivially_copyable_v<StreamHeader>);
static_assert(std::is_trivially_copyable_v<CycleMetadata>);
static_assert(std::is_trivially_copyable_v<CycleEndMetadata>);

constexpr auto checkpoint_clock = [] {
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
    return EvaluationProfileClock::timestamp_counter;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return EvaluationProfileClock::timestamp_counter;
#else
    return EvaluationProfileClock::steady_clock_nanoseconds;
#endif
}();

std::uint64_t read_checkpoint() noexcept {
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
    auto low = std::uint32_t{};
    auto high = std::uint32_t{};
    asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<std::uint64_t>(high) << 32) | low;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _ReadWriteBarrier();
    const auto result = __rdtsc();
    _ReadWriteBarrier();
    return result;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

std::int64_t steady_clock_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename T> T read_object(util::buffer_reader &reader) {
    auto value = T{};
    reader.read_bytes(std::as_writable_bytes(std::span{&value, std::size_t{1}}));
    return value;
}

} // namespace

EvaluationProfileReader::EvaluationProfileReader(util::buffer recording) : reader_{std::move(recording)} {
    if (reader_.readable().empty()) {
        throw std::runtime_error{"evaluation profile stream has no header"};
    }

    const auto header = read_object<StreamHeader>(reader_);
    if (header.magic != stream_magic || header.version != stream_version ||
        header.clock > static_cast<std::uint32_t>(EvaluationProfileClock::steady_clock_nanoseconds)) {
        throw std::runtime_error{"invalid evaluation profile stream header"};
    }
    clock_ = static_cast<EvaluationProfileClock>(header.clock);
}

EvaluationProfileReader::~EvaluationProfileReader() {
    try {
        while (read()) {
        }
    } catch (...) {
    }
}

std::optional<EvaluationProfileRecord> EvaluationProfileReader::read() {
    if (reader_.readable().empty()) {
        return std::nullopt;
    }

    const auto event = reader_.read_raw<std::uint32_t>();
    const auto tag = event & record_tag_mask;
    const auto value = event & record_value_mask;
    if (tag == node_tag) {
        if (active_graphs_.empty()) {
            throw std::runtime_error{"evaluation profile node record is outside a graph evaluation"};
        }
        return EvaluationProfileNode{active_graphs_.back(), value, reader_.read_raw<std::uint64_t>()};
    }
    if (tag == cycle_tag) {
        if (value != 1 && value != 2) {
            throw std::runtime_error{"invalid evaluation profile cycle event"};
        }
        if (value == 1) {
            if (cycle_graph_id_.value != 0) {
                throw std::runtime_error{"evaluation profile cycle start is nested"};
            }
            cycle_graph_id_ = GraphInstanceId{reader_.read_raw<std::uint64_t>()};
            if (cycle_graph_id_.value == 0) {
                throw std::runtime_error{"evaluation profile cycle has no graph identity"};
            }
            active_graphs_.push_back(cycle_graph_id_);
            const auto checkpoint = reader_.read_raw<std::uint64_t>();
            const auto metadata = read_object<CycleMetadata>(reader_);
            cycle_evaluation_time_ =
                EvaluationTime{std::chrono::microseconds{metadata.evaluation_time_microseconds}};
            return EvaluationProfileCycle{cycle_graph_id_,
                                          EvaluationProfileCycle::Boundary::start,
                                          cycle_evaluation_time_,
                                          EvaluationTime{std::chrono::microseconds{
                                              metadata.wall_clock_time_microseconds}},
                                          checkpoint,
                                          metadata.steady_clock_nanoseconds};
        }

        if (cycle_graph_id_.value == 0 || active_graphs_.empty() || active_graphs_.back() != cycle_graph_id_) {
            throw std::runtime_error{"evaluation profile cycle end has no matching start"};
        }
        const auto checkpoint = reader_.read_raw<std::uint64_t>();
        const auto metadata = read_object<CycleEndMetadata>(reader_);
        const auto graph_id = cycle_graph_id_;
        active_graphs_.pop_back();
        cycle_graph_id_ = {};
        return EvaluationProfileCycle{graph_id,
                                      EvaluationProfileCycle::Boundary::end,
                                      cycle_evaluation_time_,
                                      EvaluationTime{std::chrono::microseconds{
                                          metadata.wall_clock_time_microseconds}},
                                      checkpoint,
                                      metadata.steady_clock_nanoseconds};
    }
    if (tag == graph_tag) {
        if (value == 1 || value == 2) {
            const auto graph_id = GraphInstanceId{reader_.read_raw<std::uint64_t>()};
            const auto *compiled_graph = reader_.read_raw<const CompiledGraph *>();
            const auto parent_graph_id = GraphInstanceId{reader_.read_raw<std::uint64_t>()};
            const auto owner_node_index = reader_.read_raw<std::uint32_t>();
            auto label = std::optional<ScalarValue<>>{};
            if (value == 2) {
                label.emplace(reader_.read<ScalarValue<>>());
            }
            return EvaluationProfileGraph{graph_id, compiled_graph, parent_graph_id, owner_node_index, std::move(label)};
        }
        if (value == 3) {
            return EvaluationProfileGraphStop{GraphInstanceId{reader_.read_raw<std::uint64_t>()}};
        }
        if (value == 4) {
            const auto graph_id = GraphInstanceId{reader_.read_raw<std::uint64_t>()};
            const auto checkpoint = reader_.read_raw<std::uint64_t>();
            active_graphs_.push_back(graph_id);
            return EvaluationProfileGraphEntry{graph_id, checkpoint};
        }
        if (value == 5) {
            if (active_graphs_.empty()) {
                throw std::runtime_error{"evaluation profile graph exit has no matching entry"};
            }
            const auto checkpoint = reader_.read_raw<std::uint64_t>();
            const auto graph_id = active_graphs_.back();
            active_graphs_.pop_back();
            return EvaluationProfileGraphExit{graph_id, checkpoint};
        }
        throw std::runtime_error{"invalid evaluation profile graph event"};
    }

    throw std::runtime_error{"unknown evaluation profile event"};
}

EvaluationProfile::EvaluationProfile(std::size_t block_capacity, allocator memory)
    : memory_{memory}, recording_{util::buffer::chain(block_capacity, memory)}, writer_{recording_} {
    const auto header = StreamHeader{stream_magic, stream_version, static_cast<std::uint32_t>(checkpoint_clock)};
    writer_.write_bytes(std::as_bytes(std::span{&header, std::size_t{1}}));
    writer_.commit();
    recorded_bytes_ = sizeof(header);
    committed_bytes_ = sizeof(header);
}

util::buffer EvaluationProfile::take_recording() {
    if (recording_taken_) {
        throw std::logic_error{"evaluation profile recording has already been taken"};
    }
    recording_taken_ = true;
    labels_.erase(labels_.begin(), labels_.begin() + static_cast<std::ptrdiff_t>(committed_label_count_));
    committed_label_count_ = 0;
    return std::move(recording_);
}

void EvaluationProfile::start_graph(GraphInstanceId graph_id, const CompiledGraph &compiled_graph,
                                    GraphInstanceId parent_graph_id, std::uint32_t owner_node_index,
                                    std::optional<Scalar<>> label) noexcept {
    if (failed_ || finished_) {
        return;
    }
    if (graph_id.value == 0 || compiled_graph.node_count() > record_value_mask) {
        fail();
        return;
    }

    try {
        const auto event = label ? graph_start_with_label_event : graph_start_event;
        const auto compiled_graph_pointer = &compiled_graph;
        auto bytes = std::array<std::byte, 32>{};
        std::memcpy(bytes.data(), &event, sizeof(event));
        std::memcpy(bytes.data() + 4, &graph_id.value, sizeof(graph_id.value));
        std::memcpy(bytes.data() + 12, &compiled_graph_pointer, sizeof(compiled_graph_pointer));
        std::memcpy(bytes.data() + 20, &parent_graph_id.value, sizeof(parent_graph_id.value));
        std::memcpy(bytes.data() + 28, &owner_node_index, sizeof(owner_node_index));
        writer_.write_bytes(std::span{std::as_const(bytes)});
        recorded_bytes_ += bytes.size();

        if (label) {
            auto &stored_label = writer_.emplace<ScalarValue<>>(label.value(), memory_);
            auto destroy_label = util::scope_exit{[&]() noexcept { std::destroy_at(&stored_label); }};
            labels_.push_back(&stored_label);
            destroy_label.release();
            recorded_bytes_ += sizeof(stored_label);
        }
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::write_graph_stop(GraphInstanceId graph_id) {
    const auto event = graph_stop_event;
    const auto record = std::array<std::byte, 12>{};
    auto bytes = record;
    std::memcpy(bytes.data(), &event, sizeof(event));
    std::memcpy(bytes.data() + 4, &graph_id.value, sizeof(graph_id.value));
    writer_.write_bytes(std::span{std::as_const(bytes)});
    recorded_bytes_ += bytes.size();
}

void EvaluationProfile::stop_graph(GraphInstanceId graph_id) noexcept {
    if (failed_ || finished_) {
        return;
    }
    try {
        write_graph_stop(graph_id);
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::enter_graph(GraphInstanceId graph_id) noexcept {
    if (failed_ || finished_) {
        return;
    }
    try {
        const auto event = graph_entry_event;
        const auto checkpoint = read_checkpoint();
        const auto record = std::array<std::byte, 20>{};
        auto bytes = record;
        std::memcpy(bytes.data(), &event, sizeof(event));
        std::memcpy(bytes.data() + 4, &graph_id.value, sizeof(graph_id.value));
        std::memcpy(bytes.data() + 12, &checkpoint, sizeof(checkpoint));
        writer_.write_bytes(std::span{std::as_const(bytes)});
        recorded_bytes_ += bytes.size();
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::exit_graph(GraphInstanceId graph_id) noexcept {
    if (failed_ || finished_) {
        return;
    }
    try {
        const auto event = graph_exit_event;
        const auto checkpoint = read_checkpoint();
        const auto record = std::array<std::byte, 12>{};
        auto bytes = record;
        std::memcpy(bytes.data(), &event, sizeof(event));
        std::memcpy(bytes.data() + 4, &checkpoint, sizeof(checkpoint));
        writer_.write_bytes(std::span{std::as_const(bytes)});
        recorded_bytes_ += bytes.size();
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::begin_cycle(GraphInstanceId graph_id, EvaluationTime evaluation_time,
                                    EvaluationTime wall_clock_time) noexcept {
    if (failed_ || finished_) {
        return;
    }
    if (graph_id.value == 0) {
        fail();
        return;
    }
    if (cycle_open_) {
        fail();
        return;
    }
    cycle_open_ = true;
    try {
        write_cycle_start(graph_id, evaluation_time, wall_clock_time);
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::record_node(std::size_t node_index) noexcept {
    if (failed_ || finished_) {
        return;
    }
    try {
        const auto event = node_tag | static_cast<std::uint32_t>(node_index);
        const auto checkpoint = read_checkpoint();
        const auto record = std::array<std::byte, 12>{};
        auto bytes = record;
        std::memcpy(bytes.data(), &event, sizeof(event));
        std::memcpy(bytes.data() + 4, &checkpoint, sizeof(checkpoint));
        writer_.write_bytes(std::span{std::as_const(bytes)});
        recorded_bytes_ += bytes.size();
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::end_cycle(EvaluationTime wall_clock_time) noexcept {
    if (failed_ || finished_ || !cycle_open_) {
        return;
    }
    try {
        write_cycle_end(wall_clock_time);
        commit();
        cycle_open_ = false;
    } catch (...) {
        fail();
    }
}

void EvaluationProfile::abort_cycle() noexcept {
    if (!cycle_open_) {
        return;
    }

    destroy_uncommitted_labels();
    writer_.rollback();
    recorded_bytes_ = committed_bytes_;
    cycle_open_ = false;
    failed_ = true;
}

void EvaluationProfile::write_cycle_start(GraphInstanceId graph_id, EvaluationTime evaluation_time,
                                          EvaluationTime wall_clock_time) {
    const auto checkpoint = read_checkpoint();
    const auto metadata = CycleMetadata{
        std::chrono::duration_cast<std::chrono::microseconds>(evaluation_time.time_since_epoch()).count(),
        std::chrono::duration_cast<std::chrono::microseconds>(wall_clock_time.time_since_epoch()).count(),
        steady_clock_nanoseconds()};
    const auto record = std::array<std::byte, 44>{};
    auto bytes = record;
    std::memcpy(bytes.data(), &cycle_start_event, sizeof(cycle_start_event));
    std::memcpy(bytes.data() + 4, &graph_id.value, sizeof(graph_id.value));
    std::memcpy(bytes.data() + 12, &checkpoint, sizeof(checkpoint));
    std::memcpy(bytes.data() + 20, &metadata, sizeof(metadata));
    writer_.write_bytes(std::span{std::as_const(bytes)});
    recorded_bytes_ += bytes.size();
}

void EvaluationProfile::write_cycle_end(EvaluationTime wall_clock_time) {
    const auto checkpoint = read_checkpoint();
    const auto metadata = CycleEndMetadata{
        std::chrono::duration_cast<std::chrono::microseconds>(wall_clock_time.time_since_epoch()).count(),
        steady_clock_nanoseconds()};
    const auto record = std::array<std::byte, 28>{};
    auto bytes = record;
    std::memcpy(bytes.data(), &cycle_end_event, sizeof(cycle_end_event));
    std::memcpy(bytes.data() + 4, &checkpoint, sizeof(checkpoint));
    std::memcpy(bytes.data() + 12, &metadata, sizeof(metadata));
    writer_.write_bytes(std::span{std::as_const(bytes)});
    recorded_bytes_ += bytes.size();
}

void EvaluationProfile::destroy_uncommitted_labels() noexcept {
    while (labels_.size() > committed_label_count_) {
        std::destroy_at(labels_.back());
        labels_.pop_back();
    }
}

void EvaluationProfile::commit() noexcept {
    writer_.commit();
    committed_bytes_ = recorded_bytes_;
    if (recording_taken_) {
        labels_.clear();
        committed_label_count_ = 0;
        return;
    }
    committed_label_count_ = labels_.size();
}

void EvaluationProfile::fail() noexcept {
    if (failed_) {
        return;
    }
    failed_ = true;
    destroy_uncommitted_labels();
    writer_.rollback();
    recorded_bytes_ = committed_bytes_;
    cycle_open_ = false;
}

void EvaluationProfile::finish() noexcept {
    if (finished_) {
        return;
    }
    if (cycle_open_) {
        destroy_uncommitted_labels();
        writer_.rollback();
        recorded_bytes_ = committed_bytes_;
        cycle_open_ = false;
        failed_ = true;
    } else if (!failed_) {
        commit();
    }
    finished_ = true;
}

EvaluationProfile::~EvaluationProfile() noexcept {
    if (recording_taken_) {
        destroy_uncommitted_labels();
        return;
    }
    while (!labels_.empty()) {
        std::destroy_at(labels_.back());
        labels_.pop_back();
    }
}

} // namespace bg
