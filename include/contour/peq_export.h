#pragma once

#include <contour/peq.h>

namespace contour::eq {

// Equalizer APO parametric text: layer gain and all filters, including OFF
// filters. Routing and the layer's own bypass are intentionally not exported.
// No I/O; rejects invalid parameters instead of writing a partial document.
std::string export_peq_text(const Layer& layer);

} // namespace contour::eq
