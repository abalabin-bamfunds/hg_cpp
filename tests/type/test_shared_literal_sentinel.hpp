#pragma once

#include <bg/literal/sentinel.hpp>

namespace test_shared_literal_sentinel {

const literal::Sentinel *skip() noexcept;
const literal::Sentinel *invalid() noexcept;
const literal::Sentinel *remove() noexcept;
const literal::Sentinel *removed() noexcept;
const literal::Sentinel *extension() noexcept;
const literal::TaggedLiteral *extension_call() noexcept;
const literal::ScalarLiteral *extension_argument() noexcept;
const bg::ScalarType *semantic_type() noexcept;
const bg::ScalarRuntimeType *runtime_type() noexcept;
const bg::ScalarType *tagged_semantic_type() noexcept;
const bg::ScalarRuntimeType *tagged_runtime_type() noexcept;

} // namespace test_shared_literal_sentinel
