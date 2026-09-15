#pragma once
// NOTE: WLR_USE_UNSTABLE must be defined before any Hyprland header is included.
// When this file is included from main.cpp it is already defined. When included
// from LayoutState.cpp we define it here as a safety net (it has no effect if
// already defined).
#ifndef WLR_USE_UNSTABLE
#  define WLR_USE_UNSTABLE
#endif

#include <string>
#include <map>
#include <utility>   // std::pair

// ---------------------------------------------------------------------------
// Layout modes — plain enum in namespace avoids C++23/Wayland header conflicts
// with 'enum class : base_type' elaborated-type-specifier parsing.
// ---------------------------------------------------------------------------

namespace HA {
    enum Mode : int {
        DWINDLE = 0,
        MONOCLE = 1,
        MASTER  = 2,
    };

    enum MasterOrientation : int {
        LEFT   = 0,
        TOP    = 1,
        RIGHT  = 2,
        BOTTOM = 3,
    };
}

using eHAMode           = HA::Mode;
using eHAMasterVariant  = HA::MasterOrientation;

// ---------------------------------------------------------------------------
// Per workspace×monitor state
// ---------------------------------------------------------------------------

struct SHAState {
    eHAMode          mode              = HA::DWINDLE;
    bool             dwindleVertical   = false;
    eHAMasterVariant masterOrientation = HA::LEFT;
};

// ---------------------------------------------------------------------------
// CLayoutState
// ---------------------------------------------------------------------------

class CLayoutState {
  public:
    CLayoutState();
    ~CLayoutState() = default;

    SHAState&       get(int wsId, const std::string& monitor);
    const SHAState* peek(int wsId, const std::string& monitor) const;

    eHAMode         cycleMode(int wsId, const std::string& monitor);
    std::string     cycleVariant(int wsId, const std::string& monitor);

    void load();
    void save() const;

    std::string toJson(int wsId, const std::string& monitor) const;

    static std::string modeName(eHAMode m);
    static std::string variantName(const SHAState& s);

  private:
    std::map<std::pair<int, std::string>, SHAState> m_state;

    static std::string statePath();
};
