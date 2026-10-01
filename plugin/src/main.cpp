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
#include <thread>
#include <atomic>
#include <set>
#include <mutex>
#include <cstring>

// POSIX socket for control IPC
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>

// User headers
#include "LayoutState.hpp"
#include "Dispatchers.hpp"

// ---------------------------------------------------------------------------
// Plugin globals
// ---------------------------------------------------------------------------

inline HANDLE       PHANDLE = nullptr;
inline CLayoutState g_state;

// Control socket (replaces the old Python daemon's socket so wayapps can
// send monocle-swap-begin commands and query status).
static int          g_ctl_sockfd   = -1;
static std::thread  g_ctl_thread;
static std::atomic<bool> g_ctl_running{false};

// Monocle swap suppression: ws_ids currently in a swap, immune to auto-stash.
static std::mutex       g_monocle_swap_mtx;
static std::set<int>    g_monocle_swap_pending;

// ---------------------------------------------------------------------------
// Control socket path
// ---------------------------------------------------------------------------

static std::string ctlSockPath() {
    uid_t uid = getuid();
    return "/tmp/hawesome-" + std::to_string(uid) + ".sock";
}

// ---------------------------------------------------------------------------
// Non-blocking wayapps SIGUSR1 notify — B3 fix: no blocking system() call
// ---------------------------------------------------------------------------

static void notifyWayapps() {
    // Use a detached thread to avoid blocking the compositor main thread.
    // pkill is a process scan but it's isolated from the render loop.
    std::thread([]() {
        ::system("pkill -SIGUSR1 -f wayapps.py 2>/dev/null");
    }).detach();
}

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

static bool isManagedWorkspace(PHLWORKSPACE ws) {
    if (!ws || !ws->m_space)      return false;
    if (ws->m_isSpecialWorkspace) return false;
    if (ws->inert())              return false;
    if (ws->m_id <= 0)            return false;
    if (ws->m_id >= 800)          return false;
    return true;
}

void applyStateToWorkspace(PHLWORKSPACE ws, bool recalc = true) {
    if (!isManagedWorkspace(ws)) return;

    int         wsId = ws->m_id;
    std::string mon  = ws->m_monitor ? ws->m_monitor->m_name : "";

    const SHAState& s = g_state.get(wsId, mon);

    if (ws->m_space->algorithm()) {
        auto newTiled = makeTiledAlgorithm(s.mode);
        ws->m_space->algorithm()->updateTiledAlgo(std::move(newTiled));
    } else {
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
static CHyprSignalListener g_windowOpenHook;
static CHyprSignalListener g_monitorAddedHook;
static CHyprSignalListener g_monitorRemovedHook;

// New window on a monocle workspace → focus it so CMonocleAlgorithm brings it to front
static void onWindowOpen(PHLWINDOW win) {
    if (!win || !win->m_isMapped) return;
    if (win->m_isFloating)        return;

    auto ws = win->m_workspace;
    if (!isManagedWorkspace(ws)) return;

    int         wsId = ws->m_id;
    std::string mon  = ws->m_monitor ? ws->m_monitor->m_name : "";

    const SHAState* s = g_state.peek(wsId, mon);
    if (!s || s->mode != HA::MONOCLE) return;

    Desktop::focusState()->fullWindowFocus(win, Desktop::FOCUS_REASON_NEW_WINDOW);
    Log::logger->log(Log::INFO, "[hawesome] monocle: focused new window {} on ws={}",
                     win->m_class, wsId);
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

    if (ws->m_space->algorithm()) {
        ws->m_space->algorithm()->updateTiledAlgo(makeTiledAlgorithm(mode));
    } else {
        ws->m_space->setAlgorithmProvider(makeAlgorithm(mode, ws->m_space));
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

// B4 fix: dispatchStatus returns JSON via /tmp/hawesome-status.json AND
// prints to log. No change to external behaviour but file write is kept
// for compat; it was always the mechanism since plugins can't return strings
// to hyprctl callers directly.
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

    Log::logger->log(Log::INFO, "[hawesome] status: {}", json);
    return {};
}

static SDispatchResult dispatchFocusWindow(std::string args) {
    args.erase(0, args.find_first_not_of(" \t"));
    if (args.empty())
        return {.success = false, .error = "no address"};

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

    Desktop::focusState()->fullWindowFocus(target, Desktop::FOCUS_REASON_KEYBIND);
    return {};
}

// ---------------------------------------------------------------------------
// Control socket — B5 fix: provide Unix socket at /tmp/hawesome-<uid>.sock
// so wayapps can send monocle-swap-begin commands and query status/dump.
// Runs on a background thread; handles one connection at a time.
// ---------------------------------------------------------------------------

static std::string handleCtlCommand(const std::string& cmd) {
    if (cmd.rfind("monocle-swap-begin:", 0) == 0) {
        try {
            int wsId = std::stoi(cmd.substr(std::string("monocle-swap-begin:").size()));
            {
                std::lock_guard<std::mutex> lk(g_monocle_swap_mtx);
                g_monocle_swap_pending.insert(wsId);
            }
            // Remove suppression after 500ms on a detached thread
            std::thread([wsId]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::lock_guard<std::mutex> lk(g_monocle_swap_mtx);
                g_monocle_swap_pending.erase(wsId);
            }).detach();
            return "{\"ok\":true}";
        } catch (...) {
            return "{\"error\":\"invalid ws_id\"}";
        }
    }

    if (cmd == "status") {
        auto m = Desktop::focusState()->monitor();
        if (!m) return "{\"error\":\"no focused monitor\"}";
        auto ws = m->m_activeWorkspace;
        if (!ws) return "{\"error\":\"no active workspace\"}";
        return g_state.toJson(ws->m_id, m->m_name);
    }

    if (cmd.rfind("status:", 0) == 0) {
        std::string monName = cmd.substr(7);
        for (auto& m : State::monitorState()->monitors()) {
            if (m->m_name == monName) {
                auto ws = m->m_activeWorkspace;
                if (!ws) return "{\"error\":\"no active workspace\"}";
                return g_state.toJson(ws->m_id, m->m_name);
            }
        }
        return "{\"error\":\"monitor not found\"}";
    }

    if (cmd == "dump") {
        // Return full state as JSON object
        std::ostringstream o;
        o << "{";
        bool first = true;
        // Access via save path — re-read file for simplicity (avoids lock contention)
        // Alternatively iterate m_state but that needs a lock; for a debug command
        // reading the file is fine.
        std::ifstream f([]() {
            const char* h = std::getenv("HOME");
            return std::string(h ? h : "/tmp") + "/.config/hypr-awesome/state.json";
        }());
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            return content;
        }
        return "{}";
    }

    return "{\"error\":\"unknown command\"}";
}

static void ctlSocketThread() {
    std::string path = ctlSockPath();
    ::unlink(path.c_str());

    g_ctl_sockfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctl_sockfd < 0) {
        Log::logger->log(Log::ERR, "[hawesome] ctl socket create failed: {}",
                         std::strerror(errno));
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(g_ctl_sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Log::logger->log(Log::ERR, "[hawesome] ctl socket bind failed: {}",
                         std::strerror(errno));
        ::close(g_ctl_sockfd);
        g_ctl_sockfd = -1;
        return;
    }

    ::listen(g_ctl_sockfd, 8);
    Log::logger->log(Log::INFO, "[hawesome] control socket: {}", path);

    while (g_ctl_running) {
        // Non-blocking accept with timeout so we can check g_ctl_running
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_ctl_sockfd, &fds);
        struct timeval tv{.tv_sec = 1, .tv_usec = 0};
        int r = ::select(g_ctl_sockfd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        int client = ::accept(g_ctl_sockfd, nullptr, nullptr);
        if (client < 0) continue;

        // Read command (newline terminated, max 256 bytes)
        char buf[256]{};
        ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string cmd(buf, n);
            // Trim whitespace/newline
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'
                                    || cmd.back() == ' '))
                cmd.pop_back();

            std::string reply = handleCtlCommand(cmd);
            reply += "\n";
            ::send(client, reply.c_str(), reply.size(), MSG_NOSIGNAL);
        }
        ::close(client);
    }

    ::close(g_ctl_sockfd);
    ::unlink(path.c_str());
    g_ctl_sockfd = -1;
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

    // Load persisted state
    g_state.load();

    // Register dispatchers
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:cycle-mode",    ::dispatchCycleMode);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:cycle-variant", ::dispatchCycleVariant);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:status",        ::dispatchStatus);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hawesome:focus-window",  ::dispatchFocusWindow);

    // Hook workspace creation → apply saved layout
    g_workspaceCreatedHook = Event::bus()->m_events.workspace.created.listen(
        [](PHLWORKSPACEREF wsRef) {
            auto ws = wsRef.lock();
            if (!ws || !isManagedWorkspace(ws)) return;
            applyStateToWorkspace(ws, false);
        });

    // Hook window open → focus new arrivals on monocle workspaces
    g_windowOpenHook = Event::bus()->m_events.window.open.listen(
        [](PHLWINDOW win) {
            onWindowOpen(win);
        });

    // B2: Hook monitor added/removed → update state
    g_monitorAddedHook = Event::bus()->m_events.monitor.added.listen(
        [](PHLMONITOR mon) {
            if (!mon) return;
            g_state.onMonitorAdded(mon->m_name);
        });

    g_monitorRemovedHook = Event::bus()->m_events.monitor.removed.listen(
        [](PHLMONITOR mon) {
            if (!mon) return;
            g_state.onMonitorRemoved(mon->m_name);
        });

    // B5: Start control socket thread
    g_ctl_running = true;
    g_ctl_thread  = std::thread(ctlSocketThread);

    Log::logger->log(Log::INFO, "[hawesome] Plugin initialised (slot-keyed state)");
    HyprlandAPI::addNotification(PHANDLE, "[hawesome] Loaded",
                                 CHyprColor{0.2, 0.8, 0.4, 1.0}, 3000);

    return {"hawesome", "Per-workspace × per-monitor tiling layout",
            "boris", "0.3.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_state.save();

    // Stop control socket thread
    g_ctl_running = false;
    if (g_ctl_thread.joinable())
        g_ctl_thread.join();

    Log::logger->log(Log::INFO, "[hawesome] Plugin unloaded");
}
