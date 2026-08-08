#include "panicast/app/app.h"
#include <set>

#include <nlohmann/json.hpp>

#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/storage/database.h"

namespace panicast
{
using json = nlohmann::json;

// Online mode search
// Search history node management
// Supports ESC to cancel search
void App::perform_online_search() {
    std::string query = ui.input_box("Search iTunes Podcasts");
    // Check whether the user cancelled
    if (UI::is_input_cancelled(query)) {
        EVENT_LOG("Search cancelled");
        return;
    }
    if (query.empty())
        return;

    OnlineState::instance().last_query = query;
    EVENT_LOG(fmt::format("Searching: '{}' in {}", query,
                          ITunesSearch::get_region_name(OnlineState::instance().current_region)));

    // Execute search
    auto results = ITunesSearch::instance().search(query, OnlineState::instance().current_region);

    // Add or update the search node
    OnlineState::instance().add_or_update_search_node(query, OnlineState::instance().current_region,
                                                      results);

    selected_idx = 0;
    view_start = 0;

    EVENT_LOG(fmt::format("Found {} podcasts", results.size()));
}

// Perform an online search from the FAVOURITE-mode online_root LINK node
// New searches sync to ONLINE's online_root
void App::perform_online_search_from_favourite() {
    std::string query = ui.input_box("Search iTunes Podcasts (sync to ONLINE)");
    if (UI::is_input_cancelled(query)) {
        EVENT_LOG("Search cancelled");
        return;
    }
    if (query.empty())
        return;

    OnlineState::instance().last_query = query;
    EVENT_LOG(fmt::format("Searching: '{}' in {}", query,
                          ITunesSearch::get_region_name(OnlineState::instance().current_region)));

    // Execute search
    auto results = ITunesSearch::instance().search(query, OnlineState::instance().current_region);

    // Add or update the search node to ONLINE's online_root
    OnlineState::instance().add_or_update_search_node(query, OnlineState::instance().current_region,
                                                      results);

    // Clear children of all online_root LINK nodes in FAVOURITE
    // They will re-sync from online_root on next expand
    // P1-5: mutate library_.fav_root() under tree_mutex (was unlocked — raced with pool loaders).
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        for (auto &f : library_.fav_root()) {
            if (f->is_link && f->url == "online_root") {
                f->children_loaded = false;
                f->children.clear();
            }
        }
    }

    // Expand and refresh the current LINK node
    if (selected_idx < (int)display_list.size()) {
        auto node = display_list[selected_idx].node;
        // P1-5: traverse + mutate library_.fav_root() under tree_mutex (recursive; the inner
        //   block re-acquires safely). Was unlocked — raced with pool loaders mutating library_.fav_root().
        std::lock_guard<std::recursive_mutex> outer_lock(tree_mutex);
        // Find the LINK containing the current node
        for (auto &f : library_.fav_root()) {
            if (f->is_link && f->url == "online_root") {
                bool is_under = (f.get() == node.get());
                if (!is_under) {
                    for (auto &child : f->children) {
                        if (child.get() == node.get()) {
                            is_under = true;
                            break;
                        }
                    }
                }
                if (is_under) {
                    // Sync children from online_root
                    f->children.clear();
                    for (auto &child : OnlineState::instance().online_root->children) {
                        // Do not reset child->parent — these children are shared_ptr
                        //   between online_root and f; resetting would break online_root's
                        //   parent-pointer invariant, causing parent.lock() to wrongly point at f
                        //   after switching back to ONLINE mode. f is a link; keep its
                        //   online_root parent pointer as-is.
                        f->children.push_back(child);
                    }
                    f->children_loaded = true;
                    f->expanded = true;
                    break;
                }
            }
        }
    }

    // Rebuild display_list
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        display_list.clear();
        flatten_items(cur_items());
    }

    selected_idx = 0;
    view_start = 0;

    EVENT_LOG(fmt::format("Found {} podcasts (synced to ONLINE)", results.size()));
}

// Load search-history node children from cache
void App::load_search_history_children(TreeNodePtr node) {
    if (!node || node->url.find("search:") != 0)
        return;

    // Parse URL format: search:region:query
    std::string search_id = node->url.substr(7); // strip "search:"
    size_t colon_pos = search_id.find(':');
    if (colon_pos == std::string::npos)
        return;

    std::string region = search_id.substr(0, colon_pos);
    std::string query = search_id.substr(colon_pos + 1);

    // Y23.1: youtube/bilibili records use the per-account src cache + typed result nodes.
    if (region == "youtube" || region == "bilibili") {
        load_search_record_children(node, region);
        return;
    }

    // Load cache from the database
    std::string cached = DatabaseManager::instance().load_search_cache(query, region);
    if (cached.empty()) {
        EVENT_LOG(fmt::format("[Online] No cache for '{}'", query));
        return;
    }

    // Parse JSON and build child nodes
    try {
        json j = json::parse(cached);
        if (j.contains("results") && j["results"].is_array()) {
            node->children.clear();
            for (const auto &item : j["results"]) {
                auto child = ITunesSearch::instance().parse_result(item);
                if (child) {
                    // Check whether it is already cached
                    child->is_db_cached = DatabaseManager::instance().is_podcast_cached(child->url);
                    child->parent = node; // set parent pointer
                    node->children.push_back(child);
                }
            }
            node->children_loaded = true;
            EVENT_LOG(fmt::format("[Online] Loaded {} cached results for '{}'",
                                  node->children.size(), query));
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[Online] Parse error: {}", e.what()));
    }
}

void App::reset_search() {
    search_.reset();
}

// Supports ESC to cancel search
void App::perform_search() {
    std::string q = ui.input_box("Search:");
    // Check whether the user cancelled
    if (UI::is_input_cancelled(q)) {
        EVENT_LOG("Search cancelled");
        return;
    }
    if (q.empty())
        return;
    reset_search();
    search_.search_query() = q;
    std::string ql = Utils::to_lower(q);
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);

        // F20: context-aware search — prioritize matches at the cursor's peer level (same parent),
        //   then the subtree, then the rest of the tree. This avoids "global search" when the user
        //   is browsing a specific feed/folder and expects results nearby first.
        TreeNodePtr cur_node = (selected_idx >= 0 && selected_idx < (int)display_list.size())
                                   ? display_list[selected_idx].node
                                   : nullptr;

        if (cur_node) {
            // 1. Search peers (same parent's children, same type priority)
            auto parent = cur_node->parent.lock();
            if (parent) {
                // Same-type peers first (e.g., if cursor is on an episode, search episodes first)
                for (auto &sib : parent->children) {
                    if (sib->type == cur_node->type &&
                        Utils::to_lower(sib->title).find(ql) != std::string::npos)
                        search_.search_matches().push_back(sib);
                }
                // Different-type siblings (e.g., feed nodes if cursor is on an episode)
                for (auto &sib : parent->children) {
                    if (sib->type != cur_node->type &&
                        Utils::to_lower(sib->title).find(ql) != std::string::npos)
                        search_.search_matches().push_back(sib);
                }
                // 2. Search the parent's subtree (children of siblings)
                for (auto &sib : parent->children) {
                    if (sib != cur_node) {
                        for (auto &child : sib->children)
                            search_recursive(child, ql, search_.search_matches());
                    }
                }
            }
            // 3. Search the current node's own subtree
            for (auto &child : cur_node->children)
                search_recursive(child, ql, search_.search_matches());
        }

        // 4. Global search (everything not yet covered) — but skip the subtree already searched
        for (auto &it : cur_items())
            search_recursive(it, ql, search_.search_matches());

        // Deduplicate (peers/subtree may overlap with global)
        std::set<TreeNodePtr> seen;
        std::vector<TreeNodePtr> unique;
        for (auto &m : search_.search_matches()) {
            if (seen.insert(m).second)
                unique.push_back(m);
        }
        search_.search_matches() = std::move(unique);
    }
    search_.set_total_matches(search_.search_matches().size());
    if (search_.total_matches() > 0) {
        search_.set_current_match_idx(0);
        jump_to_match(0);
        EVENT_LOG(fmt::format("Found {} matches", search_.total_matches()));
    } else {
        EVENT_LOG("No matches found");
    }
}

void App::search_recursive(const TreeNodePtr &node, const std::string &query,
                           std::vector<TreeNodePtr> &results) {
    if (Utils::to_lower(node->title).find(query) != std::string::npos)
        results.push_back(node);
    for (auto &child : node->children)
        search_recursive(child, query, results);
}

void App::jump_search(int dir) {
    if (search_.total_matches() == 0)
        return;
    search_.set_current_match_idx(
        (search_.current_match_idx() + dir + search_.total_matches()) % search_.total_matches());
    jump_to_match(search_.current_match_idx());
}

void App::jump_to_match(int idx) {
    if (idx < 0 || idx >= search_.total_matches())
        return;
    auto node = search_.search_matches()[idx];
    reveal_node(node);
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        display_list.clear();
        flatten_items(cur_items());
    }
    for (int i = 0; i < (int)display_list.size(); ++i) {
        if (display_list[i].node == node) {
            selected_idx = i;
            view_start = std::max(0, selected_idx - (LINES - 5) / 2);
            return;
        }
    }
}

void App::reveal_node(TreeNodePtr node) {
    std::function<bool(TreeNodePtr)> reveal = [&](TreeNodePtr curr) -> bool {
        if (curr == node)
            return true;
        for (auto &child : curr->children) {
            if (reveal(child)) {
                curr->expanded = true;
                return true;
            }
        }
        return false;
    };
    std::lock_guard<std::recursive_mutex> lock(
        tree_mutex); // traversal + changing expanded must be mutually exclusive
    for (auto &it : cur_items())
        reveal(it); // E: was reveal(current_root)
}

// ═════════════════════════════════════════════════════════════════════════
// Y23.1/Y23.2: B/Y search-record cache (mirror O-mode online_root) — UNIFIED across modes.
//   Mode is just a `source` selector ("youtube" | "bilibili"); all record/container/cache/icon
//   logic is shared here. Mode-specific code is only the fetch+serialize (the providers below).
// ═════════════════════════════════════════════════════════════════════════

// Y23.2: shared "Search History" container child — used by both Y (load_accounts_root) and
//   B (expand_bilibili_account) so the container shape is identical.
TreeNodePtr App::make_search_history_child(TreeNodePtr account_node, const std::string &source,
                                           int account_id) {
    auto shist = std::make_shared<TreeNode>();
    shist->title = "Search History";
    shist->type = NodeType::FOLDER;
    shist->is_search_parent = true;
    shist->url = "searchhist:" + source;
    shist->account_id = account_id;
    shist->children_loaded = false;
    shist->parent = account_node;
    return shist;
}

// Y23.2: shared "build result nodes from JSON + save to cache + add 🔍 record" — the providers
//   (perform_youtube_search / perform_bilibili_search) only differ in HOW they produce
//   results_json; this finalizer is mode-agnostic.
void App::finalize_search(const std::string &source, int account_id, TreeNodePtr account_node,
                          const std::string &query, const json &results_json) {
    std::vector<TreeNodePtr> nodes;
    nodes.reserve(results_json.size());
    for (const auto &elem : results_json)
        nodes.push_back(build_search_result_node(source, elem));
    add_search_record(source, account_id, account_node, query, "",
                      nodes); // P2 (Y23.7): keyword-only cache
}

// Build a typed search-result node from one JSON result element (used by both the fresh-search
//   path and the cache-reload path, so the node shape is identical).
TreeNodePtr App::build_search_result_node(const std::string &source, const json &r) {
    auto c = std::make_shared<TreeNode>();
    c->is_yt_search_result = true;
    std::string kind = r.value("kind", "");
    c->url = r.value("url", "");
    c->art_url = r.value("thumbnail", "");
    if (c->art_url.empty())
        c->art_url = r.value("upic", "");
    if (c->art_url.empty())
        c->art_url = r.value("pic", "");

    if (source == "youtube") {
        c->is_youtube = true;
        c->title = r.value("title", "Untitled");
        c->channel_name = r.value("channel_title", "");
        if (!c->channel_name.empty() && kind != "channel")
            c->subtext = c->channel_name;
        if (kind == "channel") {
            c->is_yt_channel = true;
            c->channel_id = r.value("id", "");
            c->type = NodeType::FOLDER;
            c->children_loaded = false;
        } else if (kind == "playlist") {
            c->is_yt_channel = true;
            c->is_yt_playlist = true;
            c->channel_id = r.value("id", "");
            c->type = NodeType::FOLDER;
            c->children_loaded = false;
        } else { // video or music
            c->type = NodeType::PODCAST_EPISODE;
            c->children_loaded = true;
            if (kind == "music")
                c->is_yt_music = true;
        }
    } else { // bilibili
        c->is_youtube = false;
        if (kind == "up") {
            c->is_yt_channel = true;
            c->is_bili_up = true;
            c->channel_id = r.value("mid", "");
            c->channel_name = r.value("uname", "");
            int fans = r.value("fans", 0);
            c->title =
                fans > 0 ? fmt::format("{} ({} fans)", c->channel_name, fans) : c->channel_name;
            c->subtext = r.value("sign", "");
            c->type = NodeType::FOLDER;
            c->children_loaded = false;
        } else { // video
            c->title = r.value("title", "Untitled");
            c->type = NodeType::PODCAST_EPISODE;
            c->children_loaded = true;
            c->duration = r.value("duration", 0);
        }
    }
    return c;
}

// Expand a 🔍 record: load its results from the cache (lazy).
void App::load_search_record_children(TreeNodePtr node, const std::string &source) {
    if (!node || node->url.rfind("search:" + source + ":", 0) != 0)
        return;
    size_t qstart = 7 + source.size() + 1;
    if (qstart >= node->url.size())
        return;
    std::string query = node->url.substr(qstart);
    std::string cached =
        DatabaseManager::instance().load_search_cache_src(source, node->account_id, query);
    // P2 (Y23.7): keyword-only cache — if no cached results (empty), re-fetch from the API.
    if (cached.empty()) {
        EVENT_LOG(fmt::format("Re-fetching search results for '{}'", query));
        rerun_search_record(node);
        return;
    }
    try {
        json j = json::parse(cached);
        if (!j.contains("results") || !j["results"].is_array())
            return;
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        node->children.clear();
        for (const auto &r : j["results"]) {
            auto c = build_search_result_node(source, r);
            if (c) {
                c->parent = node;
                node->children.push_back(c);
            }
        }
        node->children_loaded = true;
        node->expanded = true;
        EVENT_LOG(fmt::format("Loaded {} cached results for '{}'", node->children.size(), query));
    } catch (const std::exception &e) {
        LOG(fmt::format("[Search] record load error: {}", e.what()));
    }
}

// Expand a "Search History" container: load the account's past 🔍 records from the cache.
void App::expand_search_history(TreeNodePtr node) {
    if (!node || !node->is_search_parent)
        return;
    std::string source;
    if (node->url.rfind("searchhist:", 0) == 0)
        source = node->url.substr(11);
    if (source.empty())
        return;
    auto items = DatabaseManager::instance().load_search_history_src(source, node->account_id);
    std::lock_guard<std::recursive_mutex> lock(tree_mutex);
    node->children.clear();
    for (const auto &it : items) {
        auto rec = std::make_shared<TreeNode>();
        rec->title = fmt::format("🔍 {} ({})", it.query, it.result_count);
        rec->url = "search:" + source + ":" + it.query;
        rec->type = NodeType::FOLDER;
        rec->is_yt_search = true;
        rec->account_id = node->account_id;
        rec->children_loaded = false; // lazy: expand loads results from cache
        rec->parent = node;
        node->children.push_back(rec);
    }
    node->children_loaded = true;
    node->expanded = true;
    EVENT_LOG(
        fmt::format("Loaded {} search records [{}/{}]", items.size(), source, node->account_id));
}

// Save a search to cache + add a 🔍 record under the account's "Search History" container.
void App::add_search_record(const std::string &source, int account_id, TreeNodePtr account_node,
                            const std::string &query, const std::string &results_json,
                            std::vector<TreeNodePtr> result_nodes) {
    DatabaseManager::instance().save_search_cache_src(source, account_id, query, results_json);
    std::lock_guard<std::recursive_mutex> lock(tree_mutex);
    TreeNodePtr sp;
    for (auto &c : account_node->children)
        if (c->is_search_parent) {
            sp = c;
            break;
        }
    if (!sp) {
        sp = std::make_shared<TreeNode>();
        sp->title = "Search History";
        sp->type = NodeType::FOLDER;
        sp->is_search_parent = true;
        sp->url = "searchhist:" + source;
        sp->account_id = account_id;
        sp->children_loaded = true;
        sp->parent = account_node;
        account_node->children.push_back(sp);
    }
    std::string rec_url = "search:" + source + ":" + query;
    sp->children.erase(
        std::remove_if(sp->children.begin(), sp->children.end(),
                       [&](const TreeNodePtr &n) { return n->is_yt_search && n->url == rec_url; }),
        sp->children.end());
    auto rec = std::make_shared<TreeNode>();
    rec->title = fmt::format("🔍 {} ({})", query, result_nodes.size());
    rec->url = rec_url;
    rec->type = NodeType::FOLDER;
    rec->is_yt_search = true;
    rec->account_id = account_id;
    rec->children_loaded = true; // results in memory
    rec->expanded = false;
    rec->parent = sp;
    for (auto &r : result_nodes) {
        r->parent = rec;
        rec->children.push_back(r);
    }
    sp->children.insert(sp->children.begin(), rec);
    sp->expanded = true;
    sp->children_loaded = true;
    account_node->expanded = true;
    pending_select_ = rec;
}

// Y23.1: r on a 🔍 record — re-run the search (re-fetch + update cache).
void App::rerun_search_record(TreeNodePtr node) {
    if (!node || !node->is_yt_search || node->url.rfind("search:", 0) != 0)
        return;
    size_t s1 = node->url.find(':', 7); // colon after "search:<source>"
    if (s1 == std::string::npos)
        return;
    std::string source = node->url.substr(7, s1 - 7);
    std::string query = node->url.substr(s1 + 1);
    if (source == "youtube")
        perform_youtube_search(query);
    else if (source == "bilibili")
        perform_bilibili_search(query);
}

// Y23.1: d on a 🔍 record — delete from cache + remove the node.
void App::delete_search_record(TreeNodePtr node) {
    if (!node || !node->is_yt_search || node->url.rfind("search:", 0) != 0)
        return;
    size_t s1 = node->url.find(':', 7);
    if (s1 == std::string::npos)
        return;
    std::string source = node->url.substr(7, s1 - 7);
    std::string query = node->url.substr(s1 + 1);
    DatabaseManager::instance().delete_search_history_src(source, node->account_id, query);
    TreeNodePtr p = node->parent.lock();
    if (p) {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        p->children.erase(
            std::remove_if(p->children.begin(), p->children.end(),
                           [&](const TreeNodePtr &n) { return n.get() == node.get(); }),
            p->children.end());
    }
    EVENT_LOG(fmt::format("Deleted search record: {}", query));
}

} // namespace panicast
