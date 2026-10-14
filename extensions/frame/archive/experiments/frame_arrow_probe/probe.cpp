#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>

namespace {

using value_type = std::int64_t;
using position_type = bg::util::occupancy_index::size_type;

struct benchmark_chunk {
    std::uintptr_t values;
    std::size_t length;
};

struct benchmark_state {
    bg::util::occupancy_index live;
    bg::util::sparse_bunch<value_type> values;
    std::pmr::memory_resource &memory{*std::pmr::new_delete_resource()};

    benchmark_state(position_type extent, const position_type *positions, std::size_t count) {
        for (auto i = std::size_t{}; i < count; ++i) {
            const auto position = positions[i];
            if (position >= extent) {
                throw std::out_of_range{"benchmark row position is outside the extent"};
            }

            const auto value = static_cast<value_type>((static_cast<std::uint64_t>(position) * 17 + 13) % 1009);
            values.emplace_at(position, memory, value);
            live.set(position, memory);
        }
    }

    ~benchmark_state() {
        values.release(live, memory);
        live.release(memory);
    }

    std::size_t export_chunks(benchmark_chunk *output, [[maybe_unused]] std::size_t capacity) const noexcept {
        auto output_size = std::size_t{};
        auto first = static_cast<const value_type *>(nullptr);
        auto length = std::size_t{};
        auto previous_position = position_type{};
        auto previous_address = std::uintptr_t{};

        const auto finish_chunk = [&] {
            if (length == 0) {
                return;
            }

            assert(output_size < capacity);
            output[output_size++] = {reinterpret_cast<std::uintptr_t>(first), length};
            first = nullptr;
            length = 0;
        };

        for (const auto position : live) {
            const auto *value = &values[position];
            const auto address = reinterpret_cast<std::uintptr_t>(value);
            const auto adjacent = length != 0 && position == previous_position + 1 &&
                                  address == previous_address + sizeof(value_type);
            if (!adjacent) {
                finish_chunk();
                first = value;
            }

            ++length;
            previous_position = position;
            previous_address = address;
        }

        finish_chunk();
        return output_size;
    }

    std::size_t copy_live_values(value_type *output) const noexcept {
        auto size = std::size_t{};
        for (const auto position : live) {
            output[size++] = values[position];
        }

        return size;
    }
};

std::unique_ptr<benchmark_state> benchmark;

struct chunk {
    const value_type *values{};
    std::size_t length{};
    std::vector<std::uint8_t> validity;
    std::size_t null_count{};
};

struct state {
    bg::util::occupancy_index live;
    bg::util::occupancy_index measurement_positions;
    bg::util::occupancy_index edge_live;
    bg::util::occupancy_index edge_constructed;
    bg::util::index<> nullable;
    bg::util::index<> edge_validity;
    bg::util::sparse_bunch<value_type> ids;
    bg::util::sparse_bunch<value_type> measurements;
    bg::util::sparse_bunch<value_type> edge_values;
    std::vector<chunk> id_chunks;
    std::vector<chunk> measurement_chunks;
    std::pmr::memory_resource &memory{*std::pmr::new_delete_resource()};

    state() {
        constexpr auto positions = std::array<position_type, 13>{0, 1, 3, 4, 7, 8, 9, 10, 22, 23, 24, 25, 40};
        constexpr auto null_position = position_type{9};

        for (const auto position : positions) {
            live.set(position, memory);
            ids.emplace_at(position, memory, 10'000 + position);
            if (position != null_position) {
                measurements.emplace_at(position, memory, 20'000 + position);
                measurement_positions.set(position, memory);
                nullable.set(position, memory);
            }
        }

        for (auto position = position_type{6}; position < 10; ++position) {
            edge_live.set(position, memory);
            if (position < 8) {
                continue;
            }

            edge_values.emplace_at(position, memory, 30'000 + position);
            edge_constructed.set(position, memory);
            edge_validity.set(position, memory);
        }

        id_chunks = make_chunks(ids, live, false);
        measurement_chunks = make_chunks(measurements, measurement_positions, true);
    }

    ~state() {
        ids.release(live, memory);
        measurements.release(measurement_positions, memory);
        edge_values.release(edge_constructed, memory);
        edge_constructed.release(memory);
        measurement_positions.release(memory);
        edge_live.release(memory);
        live.release(memory);
        nullable.release(memory);
        edge_validity.release(memory);
    }

    template <typename Column>
    std::vector<chunk> make_chunks(const Column &column, const bg::util::occupancy_index &constructed,
                                   bool with_validity) const {
        std::vector<chunk> result;
        auto current = chunk{};
        auto first_position = position_type{};
        auto previous_position = position_type{};
        auto first_address = std::uintptr_t{};

        const auto finish_chunk = [&] {
            if (current.length == 0) {
                return;
            }

            if (with_validity && current.null_count != 0) {
                auto bitmap = std::vector<std::uint8_t>((current.length + 7) / 8, 0);
                for (auto offset = std::size_t{}; offset < current.length; ++offset) {
                    if (nullable.contains(first_position + static_cast<position_type>(offset))) {
                        bitmap[offset / 8] |= std::uint8_t{1} << (offset % 8);
                    }
                }
                current.validity = std::move(bitmap);
            }

            result.push_back(std::move(current));
            current = chunk{};
        };

        for (const auto position : live) {
            const auto has_value = constructed.contains(position);
            if (current.length != 0 && position != previous_position + 1) {
                finish_chunk();
            }

            if (current.length != 0 && has_value) {
                const auto address = reinterpret_cast<std::uintptr_t>(&column[position]);
                const auto expected_address = first_address +
                                              static_cast<std::uintptr_t>(position - first_position) *
                                                  sizeof(value_type);
                if (address != expected_address) {
                    if (current.null_count != 0) {
                        throw std::logic_error{"an unconstructed cell crosses a value block boundary"};
                    }
                    finish_chunk();
                }
            }

            if (current.length == 0) {
                if (!has_value) {
                    throw std::logic_error{"a zero-copy chunk cannot start with an unconstructed cell"};
                }

                current.values = &column[position];
                first_position = position;
                first_address = reinterpret_cast<std::uintptr_t>(current.values);
            }

            if (!has_value && (!with_validity || nullable.contains(position))) {
                throw std::logic_error{"an unconstructed cell must be null"};
            }

            ++current.length;
            if (with_validity && !nullable.contains(position)) {
                ++current.null_count;
            }

            previous_position = position;
        }

        finish_chunk();
        return result;
    }

    bool null_slot_is_unconstructed() const noexcept {
        return measurements.get(9, measurement_positions) == nullptr;
    }

    bool edge_null_run_is_unconstructed() const noexcept {
        return edge_live.contains(6) && edge_live.contains(7) && !edge_validity.contains(6) &&
               !edge_validity.contains(7) && edge_values.get(6, edge_constructed) == nullptr &&
               edge_values.get(7, edge_constructed) == nullptr;
    }
};

std::unique_ptr<state> current;

const std::vector<chunk> &chunks(int column) {
    return column == 0 ? current->id_chunks : current->measurement_chunks;
}

} // namespace

extern "C" {

void benchmark_create(std::uint32_t extent, const std::uint32_t *positions, std::size_t count) {
    benchmark = std::make_unique<benchmark_state>(extent, positions, count);
}

void benchmark_destroy() { benchmark.reset(); }

std::size_t benchmark_live_count() { return benchmark->live.size(); }

std::size_t benchmark_export_chunks(benchmark_chunk *output, std::size_t capacity) {
    return benchmark->export_chunks(output, capacity);
}

std::size_t benchmark_copy_live_values(std::int64_t *output) {
    return benchmark->copy_live_values(output);
}

int probe_init() {
    current = std::make_unique<state>();
    return 0;
}

void probe_destroy() { current.reset(); }

std::size_t probe_live_count() { return current->live.size(); }

int probe_null_slot_is_unconstructed() { return current->null_slot_is_unconstructed(); }

int probe_edge_null_run_is_unconstructed() { return current->edge_null_run_is_unconstructed(); }

std::size_t probe_edge_live_count() { return current->edge_live.size(); }

std::uintptr_t probe_edge_values_address() {
    return reinterpret_cast<std::uintptr_t>(&current->edge_values[8]);
}

std::size_t probe_edge_values_length() { return current->edge_constructed.size(); }

std::size_t probe_chunk_count(int column) { return chunks(column).size(); }

std::size_t probe_chunk_length(int column, std::size_t index) { return chunks(column).at(index).length; }

std::uintptr_t probe_values_address(int column, std::size_t index) {
    return reinterpret_cast<std::uintptr_t>(chunks(column).at(index).values);
}

std::uintptr_t probe_validity_address(int column, std::size_t index) {
    const auto &bitmap = chunks(column).at(index).validity;
    return bitmap.empty() ? 0 : reinterpret_cast<std::uintptr_t>(bitmap.data());
}

std::size_t probe_validity_size(int column, std::size_t index) {
    return chunks(column).at(index).validity.size();
}

std::size_t probe_null_count(int column, std::size_t index) { return chunks(column).at(index).null_count; }

} // extern "C"
