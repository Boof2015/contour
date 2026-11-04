#pragma once

#include <contour/peq.h>
#include <optional>
#include <string_view>

namespace contour::eq {

struct ImportIssue {
    std::size_t line;
    bool error;
    std::string message;
};

struct ImportResult {
    // Present only if every processing line is understood and valid. Imported
    // PEQ is ordinary editable layer data; it has no special runtime category.
    std::optional<Layer> layer;
    std::vector<ImportIssue> issues;
};

// REW Generic / Equalizer APO-style parametric text, not an APO config-script
// interpreter. Never follows Include, applies Channel/Device, or performs I/O.
// Selection and name belong to the caller. No partial import on errors.
ImportResult import_peq_text(std::string_view text);

} // namespace contour::eq
