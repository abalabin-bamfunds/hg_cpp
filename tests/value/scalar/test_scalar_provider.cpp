#include "test_scalar_provider.hpp"

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>

#include <memory>
#include <exception>

namespace test_scalar_provider {
namespace {

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

const AlternativeRuntimeType &preserving_runtime() {
    static const auto runtime = AlternativeRuntimeType{};
    return runtime;
}

const AlternativeRuntimeType &canonicalizing_runtime() {
    static const auto runtime = AlternativeRuntimeType{};
    return runtime;
}

const MismatchedLayoutRuntimeType &mismatched_layout_runtime() {
    static const auto runtime = MismatchedLayoutRuntimeType{};
    return runtime;
}

int &copy_runtime_calls() {
    static auto count = 0;
    return count;
}

int &generic_runtime_calls() {
    static auto count = 0;
    return count;
}

int &generic_copy_runtime_calls() {
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

int alternative_runtime_read_count() { return preserving_runtime().read_count + canonicalizing_runtime().read_count; }

int copy_runtime_type_call_count() noexcept { return copy_runtime_calls(); }

void record_copy_runtime_type_call() noexcept { ++copy_runtime_calls(); }

int generic_runtime_type_call_count() noexcept { return generic_runtime_calls(); }

void record_generic_runtime_type_call() noexcept { ++generic_runtime_calls(); }

int generic_copy_runtime_type_call_count() noexcept { return generic_copy_runtime_calls(); }

void record_generic_copy_runtime_type_call() noexcept { ++generic_copy_runtime_calls(); }

const bg::ScalarType &compound_semantic_type_from_provider() {
    return bg::cpp_scalar_type<compound_value>();
}

const bg::ScalarType &layout_semantic_type_from_provider() {
    static const ExternalScalarType type;
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type);
    return type;
}

const bg::ScalarRuntimeType &mismatched_layout_runtime_type() { return mismatched_layout_runtime(); }

} // namespace test_scalar_provider

namespace bg {

const ScalarType &scalar_type_traits<test_scalar_provider::value>::type() noexcept {
    return test_scalar_provider::semantic_type_from_provider();
}

const ScalarType &scalar_type_traits<test_scalar_provider::layout_value>::type() noexcept {
    return test_scalar_provider::layout_semantic_type_from_provider();
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

template const ScalarRuntimeType &cpp_scalar_runtime_type<test_scalar_provider::value>();

} // namespace bg
