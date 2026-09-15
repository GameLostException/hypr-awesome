#define WLR_USE_UNSTABLE

// Hyprland headers must come before any user headers that use Hyprland types
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/master/MasterAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/monocle/MonocleAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/floating/default/DefaultFloatingAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

// Standard library
#include <memory>
#include <string>
#include <fstream>
#include <sstream>

// User headers after Hyprland headers
#include "LayoutState.hpp"
#include "Dispatchers.hpp"

// ---------------------------------------------------------------------------
// Plugin globals
// ---------------------------------------------------------------------------

inline HANDLE       PHANDLE = nullptr;
inline CLayoutState g_state;

// ---------------------------------------------------------------------------
// Algorithm factory helpers
// ---------------------------------------------------------------------------

static UP<Layout::ITiledAlgorithm> makeTiledAlgorithm(eHAMode mode) {
    using namespace Layout::Tiled;
    switch (mode) {
        case HA::MONOCLE: return makeUnique<CMonocleAlgorithm>();
        case HA::MASTER:  return makeUnique<CMasterAlgorithm>();
        case HA::DWINDLE:
        default:          return makeUnique<CDwindleAlgorithm>();
    }
}

static SP<Layout::CAlgorithm> makeAlgorithm(eHAMode mode, SP<Layout::CSpace> space) {
    using namespace Layout::Floating;
    return Layout::CAlgorithm::create(
        makeTiledAlgorithm(mode),
        makeUnique<CDefaultFloatingAlgorithm>(),
        space);
}

// ---------------------------------------------------------------------------
// Apply state to a workspace
// ---------------------------------------------------------------------------

// Returns true if this workspace should be managed by hawesome.
static bool isManagedWorkspace(PHLWORKSPACE ws) {
    if (!ws || !ws->m_space)    return false;
    if (ws->m_isSpecialWorkspace) return false;
    if (ws->inert())            return false;
    if (ws->m_id <= 0)          return false;
    if (ws->m_id >= 800)        return false; // temp workspaces used by old daemon
    return true;
}

void applyStateToWorkspace(PHLWORKSPACE ws, bool recalc = true) {
    if (!isManagedWorkspace(ws)) return;

    int         wsId = ws->m_id;
    std::string mon  = ws->m_monitor ? ws->m_monitor->m_name : "";

    const SHAState& s = g_state.get(wsId, mon);

    if (ws->m_space->algorithm()) {
        // Workspace already has windows registered in the algorithm.
        // Use updateTiledAlgo to swap just the tiling engine — this keeps
        // all existing ITarget registrations and triggers a proper re-tile.
        auto newTiled = makeTiledAlgorithm(s.mode);
        ws->m_space->algorithm()->updateTiledAlgo(std::move(newTiled));
    } else {
        // Fresh workspace — set full algorithm provider
        ws->m_space->setAlgorithmProvider(makeAlgorithm(s.mode, ws->m_space));
    }

    if (s.mode == HA::MASTER) {
        std::string orient = CLayoutState::variantName(s);
        if (!orient.empty())
            (void)ws->m_space->layoutMsg("orientation" + orient);
    }

    if (recalc)
        ws->m_space->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);

    Log::logger->log(Log::DEBUG, "[hawesome] Applied {} to ws={} mon={}",
                     CLayoutState::modeName(s.mode), wsId, mon);
}

// ---------------------------------------------------------------------------
// Event listeners
// ---------------------------------------------------------------------------

static CHyprSignalListener g_workspaceCreatedHook;

// ---------------------------------------------------------------------------
// Notify wayapps to refresh layout icon
// ---------------------------------------------------------------------------

void notifyWayapps() {
    system("pkill -SIGUSR1 -f wayapps.py 2>/dev/null");
}

// ---------------------------------------------------------------------------
// Get active workspace for focused monitor
// ---------------------------------------------------------------------------

static PHLWORKSPACE activeWorkspace() {
    auto mon = Desktop::focusState()->monitor();
    if (!mon) return nullptr;
    return mon->m_activeWorkspace;
}

// ---------------------------------------------------------------------------
// Dispatchers
// ---------------------------------------------------------------------------

static SDispatchResult dispatchCycleMode(std::string args) {
    auto ws = activeWorkspace();
    if (!ws || ws->m_isSpecialWorkspace)
        return {.success = false, .error = "no active workspace"};

    int         wsId = ws->m_id;
    std::string mon  = ws->m_monitor ? ws->m_monitor->m_name : "";

    Log::logger->log(Log::INFO, "[hawesome] cycle-mode called: ws={} mon={}", wsId, mon);

    eHAMode     mode = g_state.cycleMode(wsId, mon);
    const auto& s    = g_state.get(wsId, mon);

    Log::logger->log(Log::INFO, "[hawesome] cycle-mode new mode={} ({})",
                     static_cast<int>(mode), CLayoutState::modeName(mode));

    auto algo = makeAlgorithm(mode, ws->m_space);
    if (ws->m_space->algorithm()) {
        ws->m_space->algorithm()->updateTiledAlgo(makeTiledAlgorithm(mode));
    } else {
        ws->m_space->setAlgorithmProvider(algo);
    }

    if (mode == eHAMode::MASTER) {
        std::string orient = CLayoutState::variantName(s);
        if (!orient.empty())
            (void)ws->m_space->layoutMsg("orientation" + orient);
    }

    ws->m_space->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);
    g_state.save();
    notifyWayapps();

    Log::logger->log(Log::INFO, "[hawesome] cycle-mode ws={} mon={} → {}",
                     wsId, mon, CLayoutState::modeName(mode));
    return {};
}

static SDispatchResult dispatchCycleVariant(std::string args) {
    auto ws = activeWorkspace();
    if (!ws || ws->m_isSpecialWorkspace)
        return {.success = false, .error = "no active workspace"};

    int         wsId = ws->m_id;
    std::string mon  = ws->m_monitor ? ws->m_monitor->m_name : "";
    std::string desc = g_state.cycleVariant(wsId, mon);
    const auto& s    = g_state.get(wsId, mon);

    if (s.mode == eHAMode::DWINDLE) {
        (void)ws->m_space->layoutMsg("togglesplit");
    } else if (s.mode == eHAMode::MASTER) {
        (void)ws->m_space->layoutMsg("orientation" + CLayoutState::variantName(s));
    }

    g_state.save();
    notifyWayapps();

    Log::logger->log(Log::INFO, "[hawesome] cycle-variant ws={} mon={} → {}",
                     wsId, mon, desc);
    return {};
}

static SDispatchResult dispatchStatus(std::string args) {
    args.erase(0, args.find_first_not_of(" \t"));

    PHLWORKSPACE ws;
    std::string  mon;

    if (!args.empty()) {
        for (auto& m : State::monitorState()->monitors()) {
            if (m->m_name == args) {
                ws  = m->m_activeWorkspace;
                mon = m->m_name;
                break;
            }
        }
    } else {
        auto m = Desktop::focusState()->monitor();
        if (m) {
            ws  = m->m_activeWorkspace;
            mon = m->m_name;
        }
    }

    if (!ws) return {.success = false, .error = "no workspace"};

    std::string json = g_state.toJson(ws->m_id, mon);
    {
        std::ofstream f("/tmp/hawesome-status.json");
        if (f.is_open()) f << json;
    }

    Log::logger->log(Log::DEBUG, "[hawesome] status: {}", json);
    return {};
}

static SDispatchResult dispatchFocusWindow(std::string args) {
    args.erase(0, args.find_first_not_of(" \t"));
    if (args.empty())
        return {.success = false, .error = "no address"};

    // Normalise — Hyprland addresses are hex pointers
    // Event payloads omit "0x", hyprctl includes it; accept both
    std::string addr = args;
    if (addr.size() > 2 && addr.substr(0, 2) != "0x")
        addr = "0x" + addr;

    PHLWINDOW target;
    for (auto& w : Desktop::windowState()->windows()) {
        if (!w->m_isMapped) continue;
        std::ostringstream oss;
        oss << "0x" << std::hex << (uintptr_t)w.get();
        if (oss.str() == addr) { target = w; break; }
    }

    if (!target) return {.success = false, .error = "window not found: " + addr};

    // CMonocleAlgorithm responds to focusTargetUpdate() which is triggered by
    // the standard window focus path — simply focusing the window causes the
    // algorithm to update m_currentVisibleIndex and recalculate.
    Desktop::focusState()->fullWindowFocus(target, Desktop::FOCUS_REASON_KEYBIND);

    return {};
}

// ---------------------------------------------------------------------------
// Plugin init / exit
// ---------------------------------------------------------------------------

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();
    if (HASH != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE,
            "[hawesome] Version mismatch — recompile the plugin",
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hawesome] Version mismatch");
    }

    // Register dispatchers
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:cycle-mode",    ::dispatchCycleMode);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:cycle-variant", ::dispatchCycleVariant);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:status",        ::dispatchStatus);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:focus-window",  ::dispatchFocusWindow);

    // Hook workspace creation to assign saved layout.
    // Do NOT apply to all existing workspaces at init — that would disrupt
    // running windows and fire setAlgorithmProvider on copyq/popup workspaces.
    g_workspaceCreatedHook = Event::bus()->m_events.workspace.created.listen(
        [](PHLWORKSPACEREF wsRef) {
            auto ws = wsRef.lock();
            if (!ws || !isManagedWorkspace(ws)) return;
            // New workspace: apply saved layout if any, else default (dwindle)
            applyStateToWorkspace(ws, false);
        });

    Log::logger->log(Log::INFO, "[hawesome] Plugin initialised");
    HyprlandAPI::addNotification(PHANDLE, "[hawesome] Loaded",
                                 CHyprColor{0.2, 0.8, 0.4, 1.0}, 3000);

    return {"hawesome", "Per-workspace × per-monitor tiling layout",
            "boris", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_state.save();
    Log::logger->log(Log::INFO, "[hawesome] Plugin unloaded");
}
