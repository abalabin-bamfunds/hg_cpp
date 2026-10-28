#pragma once

#include <bg/export.hpp>

#include <quill/Logger.h>

namespace bg::logging {

// Returns a borrowed pointer to the process-level logger named `bg`. Quill owns the logger.
BG_EXPORT quill::Logger *logger();

// Waits until the backend has flushed log messages queued up to this call's timestamp.
BG_EXPORT void flush();

} // namespace bg::logging
