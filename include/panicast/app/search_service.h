// SearchService — the Application Service (功能抽象层) for the in-tree search.
//   Owns the local search STATE (the query, the match list, the match cursor) that previously
//   lived as bare App members. This is the D10-2 state-holder cut (mirrors D8b-1): the search
//   METHODS stay in App for now (app_search.cpp) because they are UI/tree-coupled — they walk the
//   tree under tree_mutex and, on a match jump, rewrite App's display_list / selected_idx /
//   view_start. That UI-navigation coupling is what D11 (UI pure-interaction) removes; once the
//   cursor is driven by events, the search logic (perform_search / search_recursive / jump_* /
//   reveal_node) relocates into this service. Until then App reads/writes the state via the
//   accessors below (mechanical redirect, behaviour-identical). reset() — the one pure piece of
//   search logic (clears all four members) — moves in now.
//   The online search-record cache (perform_online_search*, build_search_result_node,
//   add_search_record, …) is account-coupled and stays in App (AccountService territory).
//   (D10-2 — UI-decoupling M1.)
#pragma once

#include <string>
#include <vector>

#include "panicast/core/types.h" // TreeNodePtr

namespace panicast
{

class SearchService {
public:
    // ── Search state (D10-2, moved out of App) ───────────────────────────────
    // Mutable refs for the query / match list (reassigned / push_back / cleared / indexed in place,
    //   like D8b-1's playlist()); const + setter for the two ints (like D8b-1's current_index()).
    std::string &search_query() {
        return search_query_;
    }
    const std::string &search_query() const {
        return search_query_;
    }
    std::vector<TreeNodePtr> &search_matches() {
        return search_matches_;
    }
    const std::vector<TreeNodePtr> &search_matches() const {
        return search_matches_;
    }
    int current_match_idx() const {
        return current_match_idx_;
    }
    void set_current_match_idx(int idx) {
        current_match_idx_ = idx;
    }
    int total_matches() const {
        return total_matches_;
    }
    void set_total_matches(int n) {
        total_matches_ = n;
    }

    // Clear all search state (new search / mode switch). The one pure piece of search logic.
    void reset();

private:
    std::string search_query_;
    std::vector<TreeNodePtr> search_matches_;
    int current_match_idx_ = -1;
    int total_matches_ = 0;
};

} // namespace panicast
