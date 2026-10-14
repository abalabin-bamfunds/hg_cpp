#include <literal_input/brace_element.hpp>
#include <literal_input/scalar_source.hpp>

#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/value.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {

std::atomic_size_t allocation_count{};
std::atomic_bool count_allocations{};
std::size_t free_function_visits{};

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

template <typename T> T read_value(const bg::ScalarData &source) {
    alignas(T) std::byte storage[sizeof(T)];
    source.runtime_type().read_value(source.mode(), source.data_word(), storage);
    auto *value = std::launder(reinterpret_cast<T *>(storage));
    auto result = std::move(*value);
    std::destroy_at(value);
    return result;
}

void free_function_visitor(const bg::ScalarData &source) {
    require(read_value<std::int64_t>(source) == 41, "function pointer visitor received the wrong value");
    ++free_function_visits;
}

void visit_tree(const literal_input::BraceElement<literal_input::ScalarSource> &element,
                std::int64_t &sum, std::size_t &leaves) {
    element.visit(
        [&](const literal_input::ScalarSource &source) {
            source.visit([&](const bg::ScalarData &scalar) {
                sum += read_value<std::int64_t>(scalar);
                ++leaves;
            });
        },
        [&](std::span<const literal_input::BraceElement<literal_input::ScalarSource>> children) {
            for (const auto &child : children) {
                visit_tree(child, sum, leaves);
            }
        });
}

struct ShapeStats {
    std::size_t sequences{};
    std::size_t empty_sequences{};
    std::size_t leaves{};
};

void inspect_shape(const literal_input::BraceElement<literal_input::ScalarSource> &element, ShapeStats &stats) {
    element.visit(
        [&](const literal_input::ScalarSource &) { ++stats.leaves; },
        [&](std::span<const literal_input::BraceElement<literal_input::ScalarSource>> children) {
            ++stats.sequences;
            if (children.empty()) {
                ++stats.empty_sequences;
            }
            for (const auto &child : children) {
                inspect_shape(child, stats);
            }
        });
}

ShapeStats inspect_shape(const literal_input::BraceElement<literal_input::ScalarSource> &element) {
    auto stats = ShapeStats{};
    inspect_shape(element, stats);
    return stats;
}

void require_same_descriptor(const literal_input::ScalarSource &source, const bg::ScalarData &expected) {
    source.visit([&](const bg::ScalarData &actual) {
        require(&actual.runtime_type() == &expected.runtime_type(), "runtime type identity was not preserved");
        require(actual.data_word() == expected.data_word(), "scalar data pointer was not aliased");
        require(actual.mode() == expected.mode(), "scalar data mode was not preserved");
    });
}

} // namespace

namespace {

void deallocate_aligned(void *memory) noexcept {
#ifdef _MSC_VER
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

void *allocate_aligned(std::size_t size, std::size_t alignment) {
#ifdef _MSC_VER
    if (auto *memory = _aligned_malloc(size == 0 ? alignment : size, alignment); memory != nullptr) {
#else
    const auto requested_size = size == 0 ? alignment : size;
    const auto rounded_size = (requested_size + alignment - 1) / alignment * alignment;
    if (auto *memory = std::aligned_alloc(alignment, rounded_size); memory != nullptr) {
#endif
        if (count_allocations.load(std::memory_order_relaxed)) {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
        }
        return memory;
    }
    throw std::bad_alloc{};
}

} // namespace

void *operator new(std::size_t size) {
    if (auto *memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
        if (count_allocations.load(std::memory_order_relaxed)) {
            allocation_count.fetch_add(1, std::memory_order_relaxed);
        }
        return memory;
    }
    throw std::bad_alloc{};
}

void *operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    ::operator delete(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void *operator new(std::size_t size, std::align_val_t alignment) {
    const auto alignment_size = static_cast<std::size_t>(alignment);
    return allocate_aligned(size, alignment_size);
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void *memory, std::align_val_t) noexcept {
    deallocate_aligned(memory);
}

void operator delete[](void *memory, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete(void *memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete[](void *memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

int main() {
    using literal_input::ScalarSource;

    const auto input = std::int64_t{41};
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    const ScalarSource source{input};
    count_allocations.store(false, std::memory_order_relaxed);
    const auto source_construction_allocations = allocation_count.load(std::memory_order_relaxed);

    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    source.visit([](const bg::ScalarData &) {});
    count_allocations.store(false, std::memory_order_relaxed);
    const auto first_visit_allocations = allocation_count.load(std::memory_order_relaxed);

    auto observed = std::int64_t{};
    const auto const_lvalue_visitor = [&](const bg::ScalarData &data) {
        observed = read_value<std::int64_t>(data);
    };
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    source.visit(const_lvalue_visitor);
    count_allocations.store(false, std::memory_order_relaxed);
    const auto warm_visit_allocations = allocation_count.load(std::memory_order_relaxed);
    require(source_construction_allocations == 0, "ScalarSource construction called global operator new");
    require(warm_visit_allocations == 0, "warmed scalar visit called global operator new");
    require(observed == input, "const lvalue visitor received the wrong value");

    auto *function_pointer = &free_function_visitor;
    source.visit(function_pointer);
    require(free_function_visits == 1, "function pointer visitor was not called");
    source.visit(free_function_visitor);
    require(free_function_visits == 2, "plain function visitor was not called");

    std::string lvalue_string{"an lvalue string"};
    auto string_view_data = std::string_view{};
    ScalarSource{lvalue_string}.visit([&](const bg::ScalarData &data) {
        string_view_data = read_value<std::string_view>(data);
    });
    require(string_view_data == lvalue_string, "lvalue string contents were not preserved");
    require(string_view_data.data() == lvalue_string.data(), "lvalue string storage was copied");

    std::string mutable_string{"before"};
    const ScalarSource mutable_string_source{mutable_string};
    mutable_string.replace(0, mutable_string.size(), "after!");
    mutable_string_source.visit([&](const bg::ScalarData &data) {
        const auto view = read_value<std::string_view>(data);
        require(view == mutable_string, "source did not observe a later lvalue mutation");
        require(view.data() == mutable_string.data(), "mutable lvalue string storage was copied");
    });

    std::string temporary_string_result;
    ScalarSource{std::string{"a temporary string"}}.visit([&](const bg::ScalarData &data) {
        temporary_string_result = read_value<std::string_view>(data);
    });
    require(temporary_string_result == "a temporary string", "temporary string contents were not preserved");

    const auto lvalue_view = std::string_view{"an lvalue string view"};
    auto observed_view = std::string_view{};
    ScalarSource{lvalue_view}.visit([&](const bg::ScalarData &data) {
        observed_view = read_value<std::string_view>(data);
    });
    require(observed_view.data() == lvalue_view.data(), "lvalue string_view storage was copied");

    ScalarSource{std::string_view{"a temporary string_view"}}.visit([&](const bg::ScalarData &data) {
        require(read_value<std::string_view>(data) == "a temporary string_view",
                "temporary string_view contents were not preserved");
    });

    bg::ScalarValue<std::int64_t> scalar_value{input};
    const auto &scalar_descriptor = bg::details::scalar_data(scalar_value);
    require_same_descriptor(ScalarSource{scalar_value}, scalar_descriptor);
    const auto scalar = scalar_value.view();
    require_same_descriptor(ScalarSource{scalar}, bg::details::scalar_data(scalar));
    require_same_descriptor(ScalarSource{scalar_descriptor}, scalar_descriptor);

    auto sum = std::int64_t{};
    auto leaves = std::size_t{};
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    visit_tree(literal_input::BraceElement<ScalarSource>{{1, {{2, 3}, 4}, {{5}}}}, sum, leaves);
    count_allocations.store(false, std::memory_order_relaxed);
    const auto recursive_allocations = allocation_count.load(std::memory_order_relaxed);
    require(recursive_allocations == 0, "recursive brace construction/traversal called global operator new");
    require(sum == 15, "nested brace values produced the wrong sum");
    require(leaves == 5, "nested brace values produced the wrong leaf count");

    const auto empty_shape = inspect_shape(literal_input::BraceElement<ScalarSource>{});
    const auto nested_empty_shape = inspect_shape(literal_input::BraceElement<ScalarSource>{{}});
    const auto one_element_sequence_shape = inspect_shape(literal_input::BraceElement<ScalarSource>{7});
    const auto scalar_leaf_shape = inspect_shape(literal_input::BraceElement<ScalarSource>(7));
    require(empty_shape.sequences == 1 && empty_shape.empty_sequences == 1 && empty_shape.leaves == 0,
            "empty shape was not preserved");
    require(nested_empty_shape.sequences == 2 && nested_empty_shape.empty_sequences == 1 &&
                nested_empty_shape.leaves == 0,
            "nested-empty shape was not preserved");
    require(one_element_sequence_shape.sequences == 1 && one_element_sequence_shape.leaves == 1,
            "one-element sequence shape was not preserved");
    require(scalar_leaf_shape.sequences == 0 && scalar_leaf_shape.leaves == 1,
            "scalar leaf shape was not preserved");

    std::cout << "ScalarSource size/alignment: " << sizeof(ScalarSource) << '/' << alignof(ScalarSource) << '\n';
    std::cout << "BraceElement<ScalarSource> size/alignment: " << sizeof(literal_input::BraceElement<ScalarSource>)
              << '/' << alignof(literal_input::BraceElement<ScalarSource>) << '\n';
    std::cout << "operator new calls: first visit=" << first_visit_allocations
              << ", source construction=" << source_construction_allocations
              << ", warmed visit=" << warm_visit_allocations
              << ", recursive construction/traversal=" << recursive_allocations << '\n';
    std::cout << "nested shape: leaves=" << leaves << ", sum=" << sum << '\n';
}
