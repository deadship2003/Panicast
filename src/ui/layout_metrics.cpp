#include "podradio/ui/layout_metrics.h"

namespace podradio
{

LayoutMetrics& LayoutMetrics::instance() {
    static LayoutMetrics lm;
    return lm;
}

} // namespace podradio
