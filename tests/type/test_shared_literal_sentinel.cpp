#include "test_shared_literal_sentinel.hpp"

#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>

namespace test_shared_literal_sentinel {
namespace {

const bg::i64 extension_argument_value{71};
const literal::ScalarLiteral extension_argument_literal(extension_argument_value);
const literal::Sentinel extension_sentinel{bg::symbol{"test.literal.extension"}};
const literal::TaggedLiteral extension_tagged_literal{extension_sentinel, extension_argument_literal};

} // namespace

const literal::Sentinel *skip() noexcept { return &literal::SKIP; }
const literal::Sentinel *invalid() noexcept { return &literal::INVALID; }
const literal::Sentinel *remove() noexcept { return &literal::REMOVE; }
const literal::Sentinel *removed() noexcept { return &literal::REMOVED; }
const literal::Sentinel *extension() noexcept { return &extension_sentinel; }
const literal::TaggedLiteral *extension_call() noexcept { return &extension_tagged_literal; }
const literal::ScalarLiteral *extension_argument() noexcept { return &extension_argument_literal; }
const bg::ScalarType *semantic_type() noexcept { return &bg::cpp_scalar_type<literal::Sentinel>(); }
const bg::ScalarRuntimeType *runtime_type() noexcept { return &bg::cpp_scalar_runtime_type<literal::Sentinel>(); }
const bg::ScalarType *tagged_semantic_type() noexcept { return &bg::cpp_scalar_type<literal::TaggedLiteral>(); }
const bg::ScalarRuntimeType *tagged_runtime_type() noexcept {
    return &bg::cpp_scalar_runtime_type<literal::TaggedLiteral>();
}

} // namespace test_shared_literal_sentinel
