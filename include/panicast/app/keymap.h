// Keymap — key (int) → Action. handle_input looks the key up here FIRST; if bound, the Action is
// published on the message bus (UI never calls Core directly for bound keys). Unbound keys fall
// through to the legacy switch (complex flows not yet migrated). Defaults are built in code by
// App::build_keymap (matching the legacy hardcoded keys); the map is centralized here so bindings
// are rebindable + a future [keys] INI override has one place to land. (D7.)
#pragma once

#include <unordered_map>

#include "panicast/app/actions.h"

namespace panicast
{

class Keymap {
public:
    void bind(int key, Action a) { map_[key] = std::move(a); }
    const Action *lookup(int key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool contains(int key) const { return map_.count(key) > 0; }

private:
    std::unordered_map<int, Action> map_;
};

}  // namespace panicast
