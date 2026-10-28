#pragma once

#include <bg/export.hpp>

#include <quill/backend/BackendOptions.h>
#include <quill/core/PatternFormatterOptions.h>
#include <quill/Logger.h>
#include <quill/sinks/Sink.h>

#include <memory>
#include <vector>

namespace bg::logging {

// Returns BG's default Quill pattern options.
BG_EXPORT quill::PatternFormatterOptions default_pattern_options();

// Creates the process-level logger named `bg` and starts Quill with the supplied configuration.
// An empty sink list selects stdout. Call this before logger() or starting Quill's backend.
// Quill owns the returned logger.
BG_EXPORT quill::Logger *initialize(
    std::vector<std::shared_ptr<quill::Sink>> sinks = {},
    const quill::PatternFormatterOptions &pattern = default_pattern_options(),
    const quill::BackendOptions &backend = {});

// Returns a borrowed pointer to the process-level logger named `bg`. Quill owns the logger.
BG_EXPORT quill::Logger *logger();

// Waits until the backend has flushed log messages queued up to this call's timestamp.
BG_EXPORT void flush();

} // namespace bg::logging
