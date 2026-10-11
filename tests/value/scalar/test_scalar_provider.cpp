#include "test_scalar_provider.hpp"

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>

#include <exception>
#include <functional>
#include <memory>

namespace test_scalar_provider {
namespace {

bool &snapshot_construction_failure() {
    static auto value = false;
    return value;
}

class ExternalScalarType final : public bg::ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override { return true; }
};

class AlternativeRuntimeType final : public bg::ScalarRuntimeType {
  public:
    AlternativeRuntimeType()
        : ScalarRuntimeType{bg::cpp_scalar_type<value>(), sizeof(storage), alignof(storage), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<storage *>(destination));
    }

    void read_value(bg::ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        ++read_count;
        if (mode == bg::ScalarDataMode::inline_value || mode == bg::ScalarDataMode::shared_view ||
            mode == bg::ScalarDataMode::shared_value) {
            std::terminate();
        }

        const auto &source = *reinterpret_cast<const storage *>(data);
        std::construct_at(static_cast<large_source_view *>(destination),
                          large_source_view{.number = std::addressof(source.number)});
    }

    void copy_construct(void *destination, const void *source, bg::allocator &) const override {
        std::construct_at(static_cast<storage *>(destination), *static_cast<const storage *>(source));
    }

    std::size_t hash(const void *) const noexcept override { return 0; }

    void destroy(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<storage *>(value));
    }

    const bg::ScalarRuntimeType &runtime_type_for_copy(const bg::ScalarData &) const override {
        test_scalar_provider::record_source_runtime_type_for_copy_call();
        return test_scalar_provider::immutable_snapshot_runtime_type();
    }

    mutable int read_count{};

  private:
    void do_construct_from_input(void *destination, const bg::ScalarData &source, bg::allocator &) const override {
        const auto read = bg::scalar_storage_traits<value>::read_value(source);
        std::construct_at(static_cast<storage *>(destination), storage{read.get()});
    }

    void do_assign_from_input(void *destination, const bg::ScalarData &source) const override {
        const auto read = bg::scalar_storage_traits<value>::read_value(source);
        static_cast<storage *>(destination)->number = read.get();
    }

    bool equals_same_type(const void *stored, const bg::ScalarData &other) const noexcept override {
        return static_cast<const storage *>(stored)->number ==
               bg::scalar_storage_traits<value>::read_value(other).get();
    }
};

class ImmutableSnapshotRuntimeType final : public bg::ScalarRuntimeType {
  public:
    ImmutableSnapshotRuntimeType()
        : ScalarRuntimeType{bg::cpp_scalar_type<value>(), sizeof(immutable_snapshot_storage),
                            alignof(immutable_snapshot_storage), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<immutable_snapshot_storage *>(destination));
    }

    void read_value(bg::ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        if (mode == bg::ScalarDataMode::inline_value || mode == bg::ScalarDataMode::shared_view ||
            mode == bg::ScalarDataMode::shared_value) {
            std::terminate();
        }

        const auto &source = *reinterpret_cast<const immutable_snapshot_storage *>(data);
        std::construct_at(static_cast<large_source_view *>(destination),
                          large_source_view{.number = std::addressof(source.number)});
    }

    void copy_construct(void *destination, const void *source, bg::allocator &) const override {
        std::construct_at(static_cast<immutable_snapshot_storage *>(destination),
                          *static_cast<const immutable_snapshot_storage *>(source));
    }

    std::size_t hash(const void *stored) const noexcept override {
        return std::hash<int>{}(static_cast<const immutable_snapshot_storage *>(stored)->number);
    }

    void destroy(void *stored, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<immutable_snapshot_storage *>(stored));
    }

  private:
    void do_construct_from_input(void *destination, const bg::ScalarData &source, bg::allocator &) const override {
        if (snapshot_construction_failure()) {
            throw std::runtime_error{"snapshot construction failed"};
        }

        const auto read = bg::scalar_storage_traits<value>::read_value(source);
        std::construct_at(static_cast<immutable_snapshot_storage *>(destination),
                          immutable_snapshot_storage{.number = read.get()});
    }

    void do_assign_from_input(void *, const bg::ScalarData &) const override { std::terminate(); }

    bool equals_same_type(const void *stored, const bg::ScalarData &other) const override {
        return static_cast<const immutable_snapshot_storage *>(stored)->number ==
               bg::scalar_storage_traits<value>::read_value(other).get();
    }
};

class MismatchedLayoutRuntimeType final : public bg::ScalarRuntimeType {
  public:
    MismatchedLayoutRuntimeType()
        : ScalarRuntimeType{bg::cpp_scalar_type<layout_value>(), sizeof(int) + 1, alignof(int), false, false, true} {}

    void construct_initial_value(void *) const noexcept override { std::terminate(); }
    void copy_construct(void *, const void *, bg::allocator &) const override { std::terminate(); }
    std::size_t hash(const void *) const noexcept override { return 0; }
    void destroy(void *, bg::allocator &) const noexcept override { std::terminate(); }

  private:
    void do_construct_from_input(void *, const bg::ScalarData &, bg::allocator &) const override {
        std::terminate();
    }

    void do_assign_from_input(void *, const bg::ScalarData &) const override { std::terminate(); }
    bool equals_same_type(const void *, const bg::ScalarData &) const noexcept override { return false; }
};

class PreservingCopyOnlyRuntimeType final : public bg::ScalarRuntimeType {
  public:
    PreservingCopyOnlyRuntimeType()
        : ScalarRuntimeType{bg::cpp_scalar_type<copy_only_value>(), sizeof(copy_only_runtime_storage),
                            alignof(copy_only_runtime_storage), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<copy_only_runtime_storage *>(destination));
    }

    void read_value(bg::ScalarDataMode mode, std::uintptr_t data, void *destination) const override {
        if (mode == bg::ScalarDataMode::inline_value || mode == bg::ScalarDataMode::shared_view ||
            mode == bg::ScalarDataMode::shared_value) {
            std::terminate();
        }

        const auto &source = *reinterpret_cast<const copy_only_runtime_storage *>(data);
        std::construct_at(static_cast<copy_only_value *>(destination), source.value);
    }

    void copy_construct(void *destination, const void *source, bg::allocator &) const override {
        std::construct_at(static_cast<copy_only_runtime_storage *>(destination),
                          *static_cast<const copy_only_runtime_storage *>(source));
    }

    std::size_t hash(const void *) const noexcept override { return 0; }

    void destroy(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<copy_only_runtime_storage *>(value));
    }

  private:
    void do_construct_from_input(void *destination, const bg::ScalarData &source, bg::allocator &) const override {
        if (&source.runtime_type() != this) {
            std::terminate();
        }
        std::construct_at(static_cast<copy_only_runtime_storage *>(destination),
                          *static_cast<const copy_only_runtime_storage *>(source.data()));
    }

    void do_assign_from_input(void *destination, const bg::ScalarData &source) const override {
        static_cast<copy_only_runtime_storage *>(destination)->value =
            bg::details::scalar_data_value<copy_only_value>(source);
    }

    bool equals_same_type(const void *, const bg::ScalarData &) const noexcept override { return false; }
};

const AlternativeRuntimeType &preserving_runtime() {
    static const auto runtime = AlternativeRuntimeType{};
    return runtime;
}

const AlternativeRuntimeType &canonicalizing_runtime() {
    static const auto runtime = AlternativeRuntimeType{};
    return runtime;
}

const ImmutableSnapshotRuntimeType &immutable_snapshot_runtime() {
    static const auto runtime = ImmutableSnapshotRuntimeType{};
    return runtime;
}

const MismatchedLayoutRuntimeType &mismatched_layout_runtime() {
    static const auto runtime = MismatchedLayoutRuntimeType{};
    return runtime;
}

const PreservingCopyOnlyRuntimeType &preserving_copy_only_runtime() {
    static const auto runtime = PreservingCopyOnlyRuntimeType{};
    return runtime;
}

int &trait_runtime_type_for_copy_calls() {
    static auto count = 0;
    return count;
}

int &source_runtime_type_for_copy_calls() {
    static auto count = 0;
    return count;
}

int &generic_runtime_calls() {
    static auto count = 0;
    return count;
}

int &generic_trait_runtime_type_for_copy_calls() {
    static auto count = 0;
    return count;
}

} // namespace

const bg::ScalarType &semantic_type_from_provider() noexcept {
    static const ExternalScalarType type;
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type);

    return type;
}

const bg::ScalarRuntimeType &runtime_type_from_provider() {
    return bg::cpp_scalar_runtime_type<value>();
}

const bg::ScalarRuntimeType &preserving_runtime_type() { return preserving_runtime(); }

const bg::ScalarRuntimeType &canonicalizing_runtime_type() { return canonicalizing_runtime(); }

const bg::ScalarRuntimeType &immutable_snapshot_runtime_type() { return immutable_snapshot_runtime(); }

int alternative_runtime_read_count() { return preserving_runtime().read_count + canonicalizing_runtime().read_count; }

int trait_runtime_type_for_copy_call_count() noexcept { return trait_runtime_type_for_copy_calls(); }

void record_trait_runtime_type_for_copy_call() noexcept { ++trait_runtime_type_for_copy_calls(); }

int source_runtime_type_for_copy_call_count() noexcept { return source_runtime_type_for_copy_calls(); }

void record_source_runtime_type_for_copy_call() noexcept { ++source_runtime_type_for_copy_calls(); }

void set_snapshot_construct_throws(bool value) noexcept { snapshot_construction_failure() = value; }

int generic_runtime_type_call_count() noexcept { return generic_runtime_calls(); }

void record_generic_runtime_type_call() noexcept { ++generic_runtime_calls(); }

int generic_trait_runtime_type_for_copy_call_count() noexcept { return generic_trait_runtime_type_for_copy_calls(); }

void record_generic_trait_runtime_type_for_copy_call() noexcept { ++generic_trait_runtime_type_for_copy_calls(); }

const bg::ScalarType &compound_semantic_type_from_provider() {
    return bg::cpp_scalar_type<compound_value>();
}

const bg::ScalarType &layout_semantic_type_from_provider() {
    static const ExternalScalarType type;
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type);
    return type;
}

const bg::ScalarRuntimeType &mismatched_layout_runtime_type() { return mismatched_layout_runtime(); }

const bg::ScalarType &copy_only_semantic_type() noexcept {
    static const ExternalScalarType type;
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type);
    return type;
}

const bg::ScalarRuntimeType &preserving_copy_only_runtime_type() { return preserving_copy_only_runtime(); }

} // namespace test_scalar_provider

namespace bg {

const ScalarType &scalar_type_traits<test_scalar_provider::value>::type() noexcept {
    return test_scalar_provider::semantic_type_from_provider();
}

const ScalarType &scalar_type_traits<test_scalar_provider::layout_value>::type() noexcept {
    return test_scalar_provider::layout_semantic_type_from_provider();
}

const ScalarType &scalar_type_traits<test_scalar_provider::copy_only_value>::type() noexcept {
    return test_scalar_provider::copy_only_semantic_type();
}

const ScalarType &scalar_type_traits<test_scalar_provider::bundle_provider_value>::type() noexcept {
    return cpp_scalar_type<test_scalar_provider::bundle_provider_base_type>();
}

const ScalarType &scalar_type_traits<test_scalar_provider::generic_bundle_provider_value>::type() noexcept {
    return cpp_scalar_type<test_scalar_provider::generic_bundle_provider_type>();
}

const ScalarRuntimeType &scalar_storage_traits<test_scalar_provider::layout_value>::runtime_type(const ScalarType &type) {
    if (&type != &cpp_scalar_type<test_scalar_provider::layout_value>()) {
        throw std::invalid_argument{"unexpected scalar type"};
    }
    return test_scalar_provider::mismatched_layout_runtime_type();
}

const ScalarRuntimeType &scalar_storage_traits<test_scalar_provider::copy_only_value>::runtime_type_for_copy(
    const details::ScalarData &source) {
    return source.runtime_type();
}

template const ScalarRuntimeType &cpp_scalar_runtime_type<test_scalar_provider::value>();

} // namespace bg
