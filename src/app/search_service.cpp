#include "panicast/app/search_service.h"

namespace panicast
{

void SearchService::reset() {
    search_query_.clear();
    search_matches_.clear();
    current_match_idx_ = -1;
    total_matches_ = 0;
}

} // namespace panicast
