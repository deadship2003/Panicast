#pragma once

#include <vector>

#include "panicast/core/types.h" // TreeNodePtr

namespace panicast
{
// D10-4: LibraryService — owns the per-mode tree DATA MODEL (the 8 top-level item lists +
//   6 "loaded" flags). Extracted from App's god-object (ownership cut, mirrors D8b-1/D10-1/D10-2).
//
//   The members behind accessors are the same std::vector<TreeNodePtr> / bool App held; callers
//   read/write via the returned reference under tree_mutex (still in App for now — it also guards
//   view state display_list/selected_idx which is D11 territory, so it is not co-located here yet).
//
//   The tree-building METHODS (load_radio_root / load_podcast_root / …) stay in App for now: they
//   reach into tree_mutex + parser/storage + the UI, so their full relocation waits for D11
//   (UI pure-interaction). This cut only relocates the DATA so App stops owning the tree model.
class LibraryService
{
public:
    // ── 8 per-mode root item lists (top-level items live directly in the vector; the root NODE
    //   was eliminated — name kept xxx_root for historical reasons) ──
    std::vector<TreeNodePtr> &radio_root() { return radio_root_; }
    const std::vector<TreeNodePtr> &radio_root() const { return radio_root_; }
    std::vector<TreeNodePtr> &podcast_root() { return podcast_root_; }
    const std::vector<TreeNodePtr> &podcast_root() const { return podcast_root_; }
    std::vector<TreeNodePtr> &fav_root() { return fav_root_; }
    const std::vector<TreeNodePtr> &fav_root() const { return fav_root_; }
    std::vector<TreeNodePtr> &history_root() { return history_root_; }
    const std::vector<TreeNodePtr> &history_root() const { return history_root_; }
    std::vector<TreeNodePtr> &account_root() { return account_root_; }
    const std::vector<TreeNodePtr> &account_root() const { return account_root_; }
    std::vector<TreeNodePtr> &bilibili_root() { return bilibili_root_; }
    const std::vector<TreeNodePtr> &bilibili_root() const { return bilibili_root_; }
    std::vector<TreeNodePtr> &tiktok_root() { return tiktok_root_; }
    const std::vector<TreeNodePtr> &tiktok_root() const { return tiktok_root_; }
    std::vector<TreeNodePtr> &iptv_root() { return iptv_root_; }
    const std::vector<TreeNodePtr> &iptv_root() const { return iptv_root_; }

    // ── 6 per-mode "loaded" flags (were the root node's children_loaded before root removal) ──
    bool &radio_loaded() { return radio_loaded_; }
    bool radio_loaded() const { return radio_loaded_; }
    bool &podcast_loaded() { return podcast_loaded_; }
    bool podcast_loaded() const { return podcast_loaded_; }
    bool &account_loaded() { return account_loaded_; }
    bool account_loaded() const { return account_loaded_; }
    bool &bilibili_loaded() { return bilibili_loaded_; }
    bool bilibili_loaded() const { return bilibili_loaded_; }
    bool &tiktok_loaded() { return tiktok_loaded_; }
    bool tiktok_loaded() const { return tiktok_loaded_; }
    bool &iptv_loaded() { return iptv_loaded_; }
    bool iptv_loaded() const { return iptv_loaded_; }

private:
    std::vector<TreeNodePtr> radio_root_, podcast_root_, fav_root_, history_root_, account_root_,
        bilibili_root_, tiktok_root_, iptv_root_;
    bool radio_loaded_ = false, podcast_loaded_ = false, account_loaded_ = false,
         bilibili_loaded_ = false, tiktok_loaded_ = false, iptv_loaded_ = false;
};

} // namespace panicast
