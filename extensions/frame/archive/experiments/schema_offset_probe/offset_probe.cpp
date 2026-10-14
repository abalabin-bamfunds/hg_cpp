#include <bg/frame/frame.hpp>
#include <bg/frame/column.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/schema/reflection.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace frame = bg::frame;

struct Wide32 {
    alignas(32) std::byte bytes[32];
};

template <typename T, auto...> using PlainField = T;
template <typename T, auto...> using CellField = frame::Cell<T>;
template <typename T, auto...> using PatchCellField = frame::PatchCell<T>;
template <typename T, auto...> using ColumnField = frame::Column<T>;
template <typename T, auto...> using ColumnViewField = frame::ColumnView<T>;
template <typename T, auto...> using ColumnStorageField = frame::ColumnStorage<T>;

template <template <typename, auto...> typename Field> struct EmptySchema {};

template <template <typename, auto...> typename Field> struct RepeatedSchema {
    Field<std::int64_t> first;
    Field<std::int64_t> second;
    Field<std::int64_t> third;
};

template <template <typename, auto...> typename Field> struct LayoutBase {
    Field<std::int64_t> base_integer;
    Field<char> base_character;
};

template <template <typename, auto...> typename Field>
struct LayoutMiddle : bg::schema_extends<LayoutBase, Field> {
    Field<Wide32> middle_wide;
    Field<double> middle_double;
};

template <template <typename, auto...> typename Field>
struct LayoutLeaf : bg::schema_extends<LayoutMiddle, Field> {
    Field<char> leaf_character;
    Field<std::int64_t> leaf_integer;
};

template <template <typename, auto...> typename Field> struct ShadowBase {
    Field<std::int64_t> duplicate;
    Field<char> base_only;
};

template <template <typename, auto...> typename Field>
struct ShadowDerived : bg::schema_extends<ShadowBase, Field> {
    Field<double> duplicate;
};

template <template <typename, auto...> typename Field> struct TailBase {
    Field<std::int64_t> integer;
    Field<char> base_character;
};

template <template <typename, auto...> typename Field>
struct TailDerived : bg::schema_extends<TailBase, Field> {
    Field<char> derived_character;
};

template <template <typename, auto...> typename Field>
struct alignas(32) TailAlignedDerived : bg::schema_extends<TailBase, Field> {
    Field<char> derived_character;
};

struct CapturedField {
    std::size_t offset{};
    std::size_t size{};
    std::size_t alignment{};
    const std::type_info *type{};
};

struct ExpectedField {
    std::string_view name;
    std::size_t logical_index{};
    const std::type_info *type{};
};

// The probe template itself is deliberately not marked as a schema-reflection
// field. It follows schema_extends' ordinary inheritance path and has the
// size and alignment of the selected concrete field wrapper.
template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename ActualField>
struct LayoutReflector {
    template <typename T, auto... Properties> struct F;
    using ProbeSchema = Schema<F>;
    using Reflection = bg::details::hierarchical_scalar_schema_reflection<Schema>;

    static consteval std::size_t physical_field_count() {
        auto count = Reflection::direct_field_count;
        if constexpr (Reflection::has_base) {
            using Base = typename Reflection::base_schema;
            count += LayoutReflector<Base::template apply, ActualField>::physical_field_count();
        }
        return count;
    }

    static constexpr std::size_t field_capacity = physical_field_count();

    struct Recorder {
        ProbeSchema *root{};
        std::size_t count{};
        std::size_t capture_runs{};
        std::array<CapturedField, field_capacity> fields{};
    };

    inline static Recorder recorder{};

    template <typename T, auto... Properties> struct F {
        using Actual = ActualField<T, Properties...>;
        alignas(alignof(Actual)) std::byte bytes[sizeof(Actual)];

        F() noexcept {
            if (sizeof(F) != sizeof(Actual) || alignof(F) != alignof(Actual))
                std::terminate();
            LayoutReflector::record(this, typeid(Actual), sizeof(Actual), alignof(Actual));
        }
    };

    struct Result {
        std::array<CapturedField, field_capacity> fields{};
        std::size_t count{};
        std::size_t capture_runs{};
    };

    static void record(const void *field, const std::type_info &type, std::size_t size,
                       std::size_t alignment) noexcept {
        if (recorder.root == nullptr || recorder.count >= field_capacity)
            std::terminate();
        const auto root_address = reinterpret_cast<std::uintptr_t>(recorder.root);
        const auto field_address = reinterpret_cast<std::uintptr_t>(field);
        recorder.fields[recorder.count++] = {field_address - root_address, size, alignment, &type};
    }

    static Result capture() {
        alignas(ProbeSchema) static std::byte storage[sizeof(ProbeSchema)]{};
        auto *sample = reinterpret_cast<ProbeSchema *>(storage);
        recorder.root = sample;
        recorder.count = 0;
        ++recorder.capture_runs;
        std::construct_at(sample);

        Result result{recorder.fields, recorder.count, recorder.capture_runs};
        std::destroy_at(sample);
        recorder.root = nullptr;
        return result;
    }

    static const Result &result() {
        // Function-local static initialization serializes the one capture for
        // this Schema/ActualField specialization.
        static const auto captured = capture();
        return captured;
    }

    template <template <template <typename, auto...> typename> typename Current>
    static void append_expected(std::vector<ExpectedField> &fields) {
        using CurrentReflection = bg::details::hierarchical_scalar_schema_reflection<Current>;
        if constexpr (CurrentReflection::has_base) {
            using Base = typename CurrentReflection::base_schema;
            append_expected<Base::template apply>(fields);
        }

        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (fields.push_back(ExpectedField{
                 CurrentReflection::template direct_field_name<Index>(),
                 CurrentReflection::field_index(CurrentReflection::template direct_field_name<Index>()),
                 &typeid(ActualField<typename CurrentReflection::template direct_field<Index>::scalar_type>)}),
             ...);
        }(std::make_index_sequence<CurrentReflection::direct_field_count>{});
    }
};

template <template <typename, auto...> typename Field, typename T> Field<T> make_field() {
    if constexpr (std::is_default_constructible_v<Field<T>>)
        return Field<T>{};
    else
        return Field<T>{T{}};
}

template <template <typename, auto...> typename Field> LayoutBase<Field> make_layout_base() {
    return {make_field<Field, std::int64_t>(), make_field<Field, char>()};
}

template <template <typename, auto...> typename Field> LayoutMiddle<Field> make_layout_middle() {
    using Base = bg::schema_extends<LayoutBase, Field>;
    return {Base{make_layout_base<Field>()}, make_field<Field, Wide32>(), make_field<Field, double>()};
}

template <template <typename, auto...> typename Field> LayoutLeaf<Field> make_layout_leaf() {
    using Base = bg::schema_extends<LayoutMiddle, Field>;
    return {Base{make_layout_middle<Field>()}, make_field<Field, char>(), make_field<Field, std::int64_t>()};
}

template <template <typename, auto...> typename Field> ShadowDerived<Field> make_shadow_derived() {
    using Base = bg::schema_extends<ShadowBase, Field>;
    auto base = ShadowBase<Field>{make_field<Field, std::int64_t>(), make_field<Field, char>()};
    return {Base{std::move(base)}, make_field<Field, double>()};
}

template <typename Reflector, template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename Field>
std::vector<std::size_t> logical_offsets(const typename Reflector::Result &result,
                                         const std::vector<std::size_t> &named_offsets) {
    auto expected = std::vector<ExpectedField>{};
    Reflector::template append_expected<Schema>(expected);
    if (expected.size() != result.count)
        throw std::runtime_error{"probe constructor count differs from reflected physical field count"};

    auto logical = std::vector<std::size_t>(named_offsets.size(), static_cast<std::size_t>(-1));
    for (auto position = std::size_t{}; position != expected.size(); ++position) {
        const auto &field = result.fields[position];
        const auto &declaration = expected[position];
        if (*field.type != *declaration.type || field.size == 0 || field.alignment == 0)
            throw std::runtime_error{"probe constructor order does not match BG schema_reflection traversal"};
        if (declaration.logical_index >= logical.size())
            throw std::runtime_error{"BG schema_reflection returned an invalid logical field index"};

        // A direct derived declaration replaces the inherited logical field.
        // Constructor traversal sees the base first, so the later address wins.
        logical[declaration.logical_index] = field.offset;
    }

    for (auto index = std::size_t{}; index != logical.size(); ++index) {
        if (logical[index] != named_offsets[index])
            throw std::runtime_error{"probe offset differs from the named real member address"};
    }
    return logical;
}

template <typename Reflector> void check_empty(std::string_view label) {
    const auto &result = Reflector::result();
    if (Reflector::field_capacity != 0 || result.count != 0 || result.capture_runs != 1)
        throw std::runtime_error{"zero-column schema capture failed"};
    std::printf("PASS %-22.*s empty fields=%zu capture_runs=%zu\n", static_cast<int>(label.size()), label.data(),
                result.count, result.capture_runs);
}

template <template <typename, auto...> typename Actual, typename Reflector>
void check_repeated(std::string_view label) {
    const auto &result = Reflector::result();
    if (result.count != 3 || result.capture_runs != 1)
        throw std::runtime_error{"repeated-cell schema capture failed"};
    auto sample = RepeatedSchema<Actual>{make_field<Actual, std::int64_t>(), make_field<Actual, std::int64_t>(),
                                         make_field<Actual, std::int64_t>()};
    const auto root = reinterpret_cast<std::uintptr_t>(std::addressof(sample));
    auto offsets = std::vector<std::size_t>{
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.first)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.second)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.third)) - root};
    const auto actual = logical_offsets<Reflector, RepeatedSchema, Actual>(result, offsets);
    if (actual[0] == actual[1] || actual[1] == actual[2] || actual[0] == actual[2])
        throw std::runtime_error{"identical repeated cell types did not retain distinct addresses"};
    std::printf("PASS %-22.*s fields=%zu offsets=%zu,%zu,%zu capture_runs=%zu\n",
                static_cast<int>(label.size()), label.data(), result.count, actual[0], actual[1], actual[2],
                result.capture_runs);
}

template <template <typename, auto...> typename Actual, typename Reflector>
void check_multilevel(std::string_view label) {
    const auto &result = Reflector::result();
    if (result.count != 6 || result.capture_runs != 1)
        throw std::runtime_error{"multi-level schema capture failed"};
    auto sample = make_layout_leaf<Actual>();
    const auto root = reinterpret_cast<std::uintptr_t>(std::addressof(sample));
    const auto &base = static_cast<const LayoutBase<Actual> &>(sample);
    const auto &middle = static_cast<const LayoutMiddle<Actual> &>(sample);
    auto offsets = std::vector<std::size_t>{
        reinterpret_cast<std::uintptr_t>(std::addressof(base.base_integer)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(base.base_character)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(middle.middle_wide)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(middle.middle_double)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.leaf_character)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.leaf_integer)) - root};
    const auto actual = logical_offsets<Reflector, LayoutLeaf, Actual>(result, offsets);
    std::printf("PASS %-22.*s fields=%zu offsets=", static_cast<int>(label.size()), label.data(), result.count);
    for (const auto offset : actual)
        std::printf("%zu,", offset);
    std::printf(" capture_runs=%zu sizeof=%zu alignof=%zu\n", result.capture_runs, sizeof(sample), alignof(decltype(sample)));
}

template <template <typename, auto...> typename Actual, typename Reflector>
void check_shadow(std::string_view label) {
    const auto &result = Reflector::result();
    if (result.count != 3 || result.capture_runs != 1)
        throw std::runtime_error{"shadowed-field schema capture failed"};
    auto sample = make_shadow_derived<Actual>();
    const auto root = reinterpret_cast<std::uintptr_t>(std::addressof(sample));
    const auto &base = static_cast<const ShadowBase<Actual> &>(sample);
    auto offsets = std::vector<std::size_t>{
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.duplicate)) - root,
        reinterpret_cast<std::uintptr_t>(std::addressof(base.base_only)) - root};
    const auto actual = logical_offsets<Reflector, ShadowDerived, Actual>(result, offsets);
    const auto base_duplicate = reinterpret_cast<std::uintptr_t>(std::addressof(base.duplicate)) - root;
    std::printf("PASS %-22.*s physical=%zu logical=%zu base_duplicate=%zu derived_duplicate=%zu base_only=%zu\n",
                static_cast<int>(label.size()), label.data(), result.count, actual.size(), base_duplicate,
                actual[0], actual[1]);
}

template <typename Reflector> void check_concurrent_once(std::string_view label) {
    auto threads = std::array<std::thread, 8>{};
    for (auto &thread : threads)
        thread = std::thread{[] { static_cast<void>(Reflector::result()); }};
    for (auto &thread : threads)
        thread.join();
    const auto &result = Reflector::result();
    if (result.capture_runs != 1)
        throw std::runtime_error{"concurrent first requests did not share one capture"};
    std::printf("PASS %-22.*s thread_safe_capture_runs=%zu\n", static_cast<int>(label.size()), label.data(),
                result.capture_runs);
}

void check_plain_tail_padding() {
    using Reflector = LayoutReflector<TailDerived, PlainField>;
    using ProbeBase = TailBase<Reflector::template F>;
    const auto &result = Reflector::result();
    using Base = bg::schema_extends<TailBase, PlainField>;
    auto base = TailBase<PlainField>{std::int64_t{7}, 'b'};
    auto sample = TailDerived<PlainField>{Base{base}, 'd'};

    const auto root = reinterpret_cast<std::uintptr_t>(std::addressof(sample));
    const auto &real_base = static_cast<const TailBase<PlainField> &>(sample);
    const auto real_base_character = reinterpret_cast<std::uintptr_t>(std::addressof(real_base.base_character)) - root;
    const auto real_derived_character =
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.derived_character)) - root;

    auto expected = std::vector<ExpectedField>{};
    Reflector::template append_expected<TailDerived>(expected);
    auto probed_derived_character = static_cast<std::size_t>(-1);
    for (auto position = std::size_t{}; position != expected.size(); ++position) {
        if (expected[position].name == "derived_character")
            probed_derived_character = result.fields[position].offset;
    }

    std::printf("%s plain POD tail-padding: base.size=%zu probe_base.size=%zu "
                "real derived char=%zu probed derived char=%zu actual schema.size=%zu probe schema.size=%zu\n",
                real_derived_character == probed_derived_character ? "MATCH" : "MISMATCH",
                sizeof(TailBase<PlainField>), sizeof(ProbeBase), real_derived_character,
                probed_derived_character, sizeof(sample), sizeof(typename Reflector::ProbeSchema));
    if (real_derived_character == probed_derived_character)
        throw std::runtime_error{"plain POD tail-padding case did not expose the requested probe caveat"};
    if (real_base_character != result.fields[1].offset)
        throw std::runtime_error{"plain POD base-character offset differs before derived tail padding"};
}

void check_aligned_tail_padding() {
    using Reflector = LayoutReflector<TailAlignedDerived, PlainField>;
    const auto &result = Reflector::result();
    using Base = bg::schema_extends<TailBase, PlainField>;
    auto base = TailBase<PlainField>{std::int64_t{7}, 'b'};
    auto sample = TailAlignedDerived<PlainField>{Base{base}, 'd'};

    auto expected = std::vector<ExpectedField>{};
    Reflector::template append_expected<TailAlignedDerived>(expected);
    auto probed_derived_character = static_cast<std::size_t>(-1);
    for (auto position = std::size_t{}; position != expected.size(); ++position) {
        if (expected[position].name == "derived_character")
            probed_derived_character = result.fields[position].offset;
    }

    const auto root = reinterpret_cast<std::uintptr_t>(std::addressof(sample));
    const auto actual_derived_character =
        reinterpret_cast<std::uintptr_t>(std::addressof(sample.derived_character)) - root;
    const auto equal_size = sizeof(sample) == sizeof(typename Reflector::ProbeSchema);
    const auto equal_alignment = alignof(decltype(sample)) == alignof(typename Reflector::ProbeSchema);
    std::printf("%s aligned POD tail-padding: actual/probe char=%zu/%zu size=%zu/%zu align=%zu/%zu\n",
                actual_derived_character == probed_derived_character && equal_size && equal_alignment ? "MATCH"
                                                                                                      : "MISMATCH",
                actual_derived_character, probed_derived_character, sizeof(sample),
                sizeof(typename Reflector::ProbeSchema), alignof(decltype(sample)),
                alignof(typename Reflector::ProbeSchema));
    if (actual_derived_character == probed_derived_character || !equal_size || !equal_alignment)
        throw std::runtime_error{"aligned POD case did not preserve whole layout while changing the field offset"};
}

template <template <typename, auto...> typename Actual, typename MultiReflector, typename ShadowReflector>
void check_all_layouts(std::string_view label) {
    check_multilevel<Actual, MultiReflector>(label);
    check_shadow<Actual, ShadowReflector>(label);
}

int main() {
    try {
        check_empty<LayoutReflector<EmptySchema, PlainField>>("plain empty");
        check_empty<LayoutReflector<EmptySchema, CellField>>("Cell empty");
        check_empty<LayoutReflector<EmptySchema, PatchCellField>>("PatchCell empty");
        check_empty<LayoutReflector<EmptySchema, ColumnField>>("Column empty");
        check_empty<LayoutReflector<EmptySchema, ColumnViewField>>("ColumnView empty");
        check_empty<LayoutReflector<EmptySchema, ColumnStorageField>>("ColumnStorage empty");

        using Concurrent = LayoutReflector<RepeatedSchema, ColumnViewField>;
        check_concurrent_once<Concurrent>("ColumnView first-use");
        check_repeated<PlainField, LayoutReflector<RepeatedSchema, PlainField>>("plain repeated");
        check_repeated<CellField, LayoutReflector<RepeatedSchema, CellField>>("Cell repeated");
        check_repeated<PatchCellField, LayoutReflector<RepeatedSchema, PatchCellField>>("PatchCell repeated");
        check_repeated<ColumnField, LayoutReflector<RepeatedSchema, ColumnField>>("Column repeated");
        check_repeated<ColumnViewField, Concurrent>("ColumnView repeated");
        check_repeated<ColumnStorageField, LayoutReflector<RepeatedSchema, ColumnStorageField>>(
            "ColumnStorage repeated");

        check_all_layouts<PlainField, LayoutReflector<LayoutLeaf, PlainField>,
                          LayoutReflector<ShadowDerived, PlainField>>("plain multi-level");
        check_all_layouts<CellField, LayoutReflector<LayoutLeaf, CellField>,
                          LayoutReflector<ShadowDerived, CellField>>("Cell multi-level");
        check_all_layouts<PatchCellField, LayoutReflector<LayoutLeaf, PatchCellField>,
                          LayoutReflector<ShadowDerived, PatchCellField>>("PatchCell multi-level");
        check_all_layouts<ColumnField, LayoutReflector<LayoutLeaf, ColumnField>,
                          LayoutReflector<ShadowDerived, ColumnField>>("Column multi-level");
        check_all_layouts<ColumnViewField, LayoutReflector<LayoutLeaf, ColumnViewField>,
                          LayoutReflector<ShadowDerived, ColumnViewField>>("ColumnView multi-level");
        check_all_layouts<ColumnStorageField, LayoutReflector<LayoutLeaf, ColumnStorageField>,
                          LayoutReflector<ShadowDerived, ColumnStorageField>>("ColumnStorage multi-level");

        check_plain_tail_padding();
        check_aligned_tail_padding();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
