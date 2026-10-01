#pragma once
// NOTE: WLR_USE_UNSTABLE must be defined before any Hyprland header is included.
#ifndef WLR_USE_UNSTABLE
#  define WLR_USE_UNSTABLE
#endif

#include <string>
#include <map>
#include <utility>   // std::pair

// ---------------------------------------------------------------------------
// Layout modes
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

    // Runtime state access — creates entry on first access, restoring from
    // m_saved (slot-keyed) if available.
    SHAState&       get(int wsId, const std::string& monitor);
    const SHAState* peek(int wsId, const std::string& monitor) const;

    eHAMode         cycleMode(int wsId, const std::string& monitor);
    std::string     cycleVariant(int wsId, const std::string& monitor);

    // Persistence — state.json uses "slot@monitor" keys for stability across
    // monitor index changes (slot = (ws_id-1) % ws_per_monitor + 1).
    void load();
    void save() const;

    // Monitor lifecycle — call from monitorAdded/monitorRemoved hooks in main.cpp
    void onMonitorAdded(const std::string& monitorName);
    void onMonitorRemoved(const std::string& monitorName);

    std::string toJson(int wsId, const std::string& monitor) const;

    static std::string modeName(eHAMode m);
    static std::string variantName(const SHAState& s);
    static std::string masterOrientName(eHAMasterVariant v);

    // Slot helpers — public so main.cpp can use them for key resolution
    static int  wsPerMonitor();          // reads workspaces.conf, cached
    static int  wsIdToSlot(int wsId);    // ws_id → 1-based slot within monitor
    static std::string saveKey(int wsId, const std::string& monitor);  // "slot@mon"

  private:
    // Runtime state keyed by (ws_id, monitor_name)
    std::map<std::pair<int, std::string>, SHAState> m_state;

    // Saved state keyed by "slot@monitor" — loaded from disk, survives
    // monitor index changes.  Never cleared; m_state entries are cleared on
    // monitorRemoved.
    std::map<std::string, SHAState> m_saved;

    const SHAState* _savedFor(int wsId, const std::string& monitor) const;

    static std::string statePath();
};
