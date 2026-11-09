#define WLR_USE_UNSTABLE

// Hyprland headers must come before any user headers that use Hyprland types
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
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
#include <optional>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <cstring>

// POSIX socket for control IPC
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <wayland-server-core.h>

// User headers
#include "LayoutState.hpp"

// ---------------------------------------------------------------------------
// Plugin globals
// ---------------------------------------------------------------------------

inline HANDLE       PHANDLE = nullptr;
inline CLayoutState g_state;

// Control socket. Its fds are watched by the compositor's own event loop, so
// every command runs on the main thread, like the rest of the plugin.
static int              g_ctl_sockfd = -1;
static wl_event_source* g_ctl_source = nullptr;

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

// Numbered, non-special workspace with a layout space and an ID below the
// 800+ range Hyprland reserves for internal use. Returns its numeric ID.
static std::optional<int> managedWsId(const PHLWORKSPACE& ws) {
    if (!ws || !ws->space())                                return std::nullopt;
    if (ws->type() != Workspace::eWorkspaceType::NORMAL)   return std::nullopt;
    auto id = ws->numberedID();
    if (!id || *id == 0 || *id >= 800)                     return std::nullopt;
    return static_cast<int>(*id);
}

static std::string monitorName(const PHLWORKSPACE& ws) {
    auto mon = ws->m_monitor.lock();
    return mon ? mon->m_name : "";
}

static void applyStateToWorkspace(const PHLWORKSPACE& ws) {
    auto wsId = managedWsId(ws);
    if (!wsId) return;

    std::string     mon   = monitorName(ws);
    const SHAState& s     = g_state.get(*wsId, mon);
    auto            space = ws->space();

    if (space->algorithm())
        space->algorithm()->updateTiledAlgo(makeTiledAlgorithm(s.mode));
    else
        space->setAlgorithmProvider(makeAlgorithm(s.mode, space));

    if (s.mode == HA::MASTER) {
        std::string orient = CLayoutState::variantName(s);
        if (!orient.empty())
            (void)space->layoutMsg("orientation" + orient);
    }

    space->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);

    Log::logger->log(Log::DEBUG, "hawesome", "Applied {} to ws={} mon={}",
                     CLayoutState::modeName(s.mode), *wsId, mon);
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
    if (!win || !win->mapped()) return;
    if (win->isFloating())      return;

    auto ws   = win->m_workspace;
    auto wsId = managedWsId(ws);
    if (!wsId) return;

    const SHAState* s = g_state.peek(*wsId, monitorName(ws));
    if (!s || s->mode != HA::MONOCLE) return;

    Desktop::focusState()->fullWindowFocus(win, Desktop::FOCUS_REASON_NEW_WINDOW);
    Log::logger->log(Log::INFO, "hawesome", "monocle: focused new window {} on ws={}",
                     win->metadata().appID(), *wsId);
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
    auto ws   = activeWorkspace();
    auto wsId = managedWsId(ws);
    if (!wsId)
        return {.success = false, .error = "active workspace is not a managed numbered workspace"};

    std::string mon  = monitorName(ws);
    eHAMode     mode = g_state.cycleMode(*wsId, mon);

    applyStateToWorkspace(ws);
    g_state.save();
    notifyWayapps();

    Log::logger->log(Log::INFO, "hawesome", "cycle-mode ws={} mon={} → {}",
                     *wsId, mon, CLayoutState::modeName(mode));
    return {};
}

static SDispatchResult dispatchCycleVariant(std::string args) {
    auto ws   = activeWorkspace();
    auto wsId = managedWsId(ws);
    if (!wsId)
        return {.success = false, .error = "active workspace is not a managed numbered workspace"};

    std::string mon  = monitorName(ws);
    std::string desc = g_state.cycleVariant(*wsId, mon);
    const auto& s    = g_state.get(*wsId, mon);

    if (s.mode == eHAMode::DWINDLE) {
        (void)ws->space()->layoutMsg("togglesplit");
    } else if (s.mode == eHAMode::MASTER) {
        (void)ws->space()->layoutMsg("orientation" + CLayoutState::variantName(s));
    }

    g_state.save();
    notifyWayapps();

    Log::logger->log(Log::INFO, "hawesome", "cycle-variant ws={} mon={} → {}",
                     *wsId, mon, desc);
    return {};
}

// Monitor by name, or the focused monitor when the name is empty.
static PHLMONITOR findMonitor(const std::string& name) {
    if (name.empty())
        return Desktop::focusState()->monitor();
    for (auto& m : State::monitorState()->monitors())
        if (m->m_name == name) return m;
    return nullptr;
}

// Layout state of the monitor's active workspace as JSON, or nullopt with
// the reason in `error`.
static std::optional<std::string> statusJson(const PHLMONITOR& m, std::string& error) {
    if (!m) { error = "monitor not found"; return std::nullopt; }
    auto wsId = managedWsId(m->m_activeWorkspace);
    if (!wsId) { error = "active workspace is not a managed numbered workspace"; return std::nullopt; }
    return g_state.toJson(*wsId, m->m_name);
}

// Writes the status JSON to /tmp/hawesome-status.json and the log: a
// dispatcher cannot return a string to its hyprctl caller.
static SDispatchResult dispatchStatus(std::string args) {
    args.erase(0, args.find_first_not_of(" \t"));

    std::string error;
    auto        json = statusJson(findMonitor(args), error);
    if (!json) return {.success = false, .error = error};
    {
        std::ofstream f("/tmp/hawesome-status.json");
        if (f.is_open()) f << *json;
    }

    Log::logger->log(Log::INFO, "hawesome", "status: {}", *json);
    return {};
}

// ---------------------------------------------------------------------------
// Control socket at /tmp/hawesome-<uid>.sock — status queries for scripts and
// status bars. Accept and read happen in event-loop callbacks on the main
// thread, so Hyprland state and g_state are never touched concurrently.
// ---------------------------------------------------------------------------

static std::string handleCtlCommand(const std::string& cmd) {
    if (cmd == "status" || cmd.rfind("status:", 0) == 0) {
        std::string monName = cmd == "status" ? "" : cmd.substr(7);
        std::string error;
        auto        json = statusJson(findMonitor(monName), error);
        return json ? *json : "{\"error\":\"" + error + "\"}";
    }

    if (cmd == "dump") {
        const char* h = std::getenv("HOME");
        std::ifstream f(std::string(h ? h : "/tmp") + "/.config/hypr-awesome/state.json");
        if (f.is_open())
            return std::string((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        return "{}";
    }

    return "{\"error\":\"unknown command\"}";
}

struct SCtlClient {
    int              fd     = -1;
    wl_event_source* source = nullptr;
};

static void closeCtlClient(SCtlClient* c) {
    wl_event_source_remove(c->source);
    ::close(c->fd);
    delete c;
}

// Client fd is readable: one command per connection, then close.
static int onCtlClientReadable(int fd, uint32_t mask, void* data) {
    auto* c = static_cast<SCtlClient*>(data);

    char    buf[256]{};
    ssize_t n = (mask & WL_EVENT_READABLE) ? ::recv(fd, buf, sizeof(buf) - 1, 0) : 0;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;   // spurious wakeup — wait for data

    if (n > 0) {
        std::string cmd(buf, n);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
            cmd.pop_back();

        std::string reply = handleCtlCommand(cmd) + "\n";
        ::send(fd, reply.c_str(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    }

    closeCtlClient(c);
    return 0;
}

static int onCtlListenReadable(int fd, uint32_t, void*) {
    int client = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) return 0;

    auto* c   = new SCtlClient{.fd = client};
    c->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, client,
                                     WL_EVENT_READABLE, onCtlClientReadable, c);
    if (!c->source) {
        ::close(client);
        delete c;
    }
    return 0;
}

static void startCtlSocket() {
    std::string path = ctlSockPath();
    ::unlink(path.c_str());

    g_ctl_sockfd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_ctl_sockfd < 0) {
        Log::logger->log(Log::ERR, "hawesome", "ctl socket create failed: {}", std::strerror(errno));
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(g_ctl_sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        ::listen(g_ctl_sockfd, 8) < 0) {
        Log::logger->log(Log::ERR, "hawesome", "ctl socket bind/listen failed: {}", std::strerror(errno));
        ::close(g_ctl_sockfd);
        g_ctl_sockfd = -1;
        return;
    }

    g_ctl_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_ctl_sockfd,
                                        WL_EVENT_READABLE, onCtlListenReadable, nullptr);
    Log::logger->log(Log::INFO, "hawesome", "control socket: {}", path);
}

static void stopCtlSocket() {
    if (g_ctl_source) wl_event_source_remove(g_ctl_source);
    g_ctl_source = nullptr;
    if (g_ctl_sockfd >= 0) ::close(g_ctl_sockfd);
    g_ctl_sockfd = -1;
    ::unlink(ctlSockPath().c_str());
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

    // Hook workspace creation → apply saved layout
    // createdLate fires once the workspace is fully set up (monitor, space).
    g_workspaceCreatedHook = Event::bus()->m_events.workspace.createdLate.listen(
        [](PHLWORKSPACEREF wsRef) {
            applyStateToWorkspace(wsRef.lock());
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

    startCtlSocket();

    Log::logger->log(Log::INFO, "hawesome", "Plugin initialised (slot-keyed state)");
    HyprlandAPI::addNotification(PHANDLE, "[hawesome] Loaded",
                                 CHyprColor{0.2, 0.8, 0.4, 1.0}, 3000);

    return {"hawesome", "Per-workspace × per-monitor tiling layout",
            "boris", "0.3.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_state.save();

    stopCtlSocket();

    Log::logger->log(Log::INFO, "hawesome", "Plugin unloaded");
}
