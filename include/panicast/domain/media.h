// Media — the single domain handle modules exchange (architecture: modules pass a media
// identity, not URLs/paths). This is a thin ADAPTER over the current TreeNodePtr carrier (the
// de-facto media object); it does NOT replace or modify TreeNode. Later milestones narrow
// Media's surface and decouple it from the tree. Header-only.
//
// (D4 — new-arch M0.)
#pragma once

#include <memory>
#include <string>

#include "panicast/core/types.h"  // TreeNode, TreeNodePtr, TreeNodeWeakPtr

namespace panicast
{

// Opaque, comparable media identity. Today backed by the underlying TreeNode's address
// (stable for a given node, cheap to copy). Held weakly: expires if the node is destroyed.
class MediaID {
public:
    MediaID() = default;
    explicit MediaID(TreeNodePtr n) : node_(std::move(n)) {}

    bool operator==(const MediaID &o) const noexcept {
        return node_.lock().get() == o.node_.lock().get();
    }
    bool operator!=(const MediaID &o) const noexcept { return !(*this == o); }

    bool valid() const noexcept { return !node_.expired(); }
    // Resolve to the underlying node (nullptr if expired).
    TreeNodePtr lock() const noexcept { return node_.lock(); }

private:
    TreeNodeWeakPtr node_;
};

// Read-only convenience view onto a media item. Holds copies of the TreeNode's url/title plus
// the MediaID; does not own or modify the node. Modules that only need identity + url/title
// use this instead of the full TreeNode (80+ fields).
struct Media {
    MediaID id;
    std::string url;
    std::string title;
};

// Build a Media view from a tree node (nullptr → empty/invalid Media).
inline Media media_from_node(const TreeNodePtr &n) {
    Media m;
    if (n) {
        m.id = MediaID{n};
        m.url = n->url;
        m.title = n->title;
    }
    return m;
}

}  // namespace panicast
