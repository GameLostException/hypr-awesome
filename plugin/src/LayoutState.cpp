#include "LayoutState.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external deps)
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    size_t b = s.find_last_not_of(" \t\r\n\"");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// CLayoutState
// ---------------------------------------------------------------------------

CLayoutState::CLayoutState() {
    // load() is called explicitly from PLUGIN_INIT after the plugin handle is
    // set up, so we don't call it here.
}

SHAState& CLayoutState::get(int wsId, const std::string& monitor) {
    auto key = std::make_pair(wsId, monitor);
    if (m_state.find(key) == m_state.end()) {
        // Try to restore from saved slot-keyed state
        auto saved = _savedFor(wsId, monitor);
        m_state[key] = saved ? *saved : SHAState{};
    }
    return m_state[key];
}

const SHAState* CLayoutState::peek(int wsId, const std::string& monitor) const {
    auto it = m_state.find({wsId, monitor});
    return (it != m_state.end()) ? &it->second : nullptr;
}

eHAMode CLayoutState::cycleMode(int wsId, const std::string& monitor) {
    auto& s = get(wsId, monitor);
    s.mode  = static_cast<eHAMode>((static_cast<int>(s.mode) + 1) % 3);
    return s.mode;
}

std::string CLayoutState::cycleVariant(int wsId, const std::string& monitor) {
    auto& s = get(wsId, monitor);
    switch (s.mode) {
        case eHAMode::DWINDLE:
            s.dwindleVertical = !s.dwindleVertical;
            return s.dwindleVertical ? "vertical" : "horizontal";
        case eHAMode::MASTER:
            s.masterOrientation = static_cast<eHAMasterVariant>(
                (static_cast<int>(s.masterOrientation) + 1) % 4);
            return variantName(s);
        default:
            return "none";
    }
}

std::string CLayoutState::modeName(eHAMode m) {
    switch (m) {
        case eHAMode::DWINDLE: return "dwindle";
        case eHAMode::MONOCLE: return "monocle";
        case eHAMode::MASTER:  return "master";
    }
    return "dwindle";
}

std::string CLayoutState::variantName(const SHAState& s) {
    switch (s.mode) {
        case eHAMode::DWINDLE:
            return s.dwindleVertical ? "v" : "h";
        case eHAMode::MONOCLE:
            return "";
        case eHAMode::MASTER:
            return masterOrientName(s.masterOrientation);
    }
    return "";
}

std::string CLayoutState::masterOrientName(eHAMasterVariant v) {
    switch (v) {
        case eHAMasterVariant::TOP:    return "top";
        case eHAMasterVariant::RIGHT:  return "right";
        case eHAMasterVariant::BOTTOM: return "bottom";
        case eHAMasterVariant::LEFT:
        default:                       return "left";
    }
}

std::string CLayoutState::toJson(int wsId, const std::string& monitor) const {
    const SHAState* s = peek(wsId, monitor);
    SHAState        def;
    if (!s) s = &def;

    std::string variant = variantName(*s);
    std::string variantJson = variant.empty() ? "null" : ("\"" + variant + "\"");

    std::ostringstream o;
    o << "{"
      << "\"ws\":" << wsId << ","
      << "\"mon\":\"" << monitor << "\","
      << "\"mode\":\"" << modeName(s->mode) << "\","
      << "\"variant\":" << variantJson
      << "}";
    return o.str();
}

// ---------------------------------------------------------------------------
// Slot resolution helpers
//
// split-monitor-workspaces assigns ws IDs as:
//   monitor index i → ws (i*WS_PER_MON + 1) … (i*WS_PER_MON + WS_PER_MON)
//
// The "slot" (1–WS_PER_MON) is the workspace's position within its monitor.
// We save state as "slot@monitor" so state survives monitor index changes:
// if DP-5 moves from index 1 (ws 9) to index 0 (ws 1) after eDP-1 is removed,
// the key "1@DP-5" still resolves correctly.
// ---------------------------------------------------------------------------

int CLayoutState::wsPerMonitor() {
    // Read from workspaces.conf at runtime so it stays in sync with config.
    static int cached = 0;
    if (cached > 0) return cached;
    const char* home = std::getenv("HOME");
    if (!home) return 8;
    std::string path = std::string(home) + "/.config/hypr/workspaces.conf";
    std::ifstream f(path);
    if (!f.is_open()) return 8;
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find("count");
        if (p == std::string::npos) continue;
        size_t eq = line.find('=', p);
        if (eq == std::string::npos) continue;
        try {
            cached = std::stoi(line.substr(eq + 1));
            return cached;
        } catch (...) {}
    }
    return 8;
}

// Convert a ws_id to its slot number (1-based within monitor).
int CLayoutState::wsIdToSlot(int wsId) {
    int n = wsPerMonitor();
    return ((wsId - 1) % n) + 1;
}

// Build the save key: "slot@monitor"
std::string CLayoutState::saveKey(int wsId, const std::string& monitor) {
    return std::to_string(wsIdToSlot(wsId)) + "@" + monitor;
}

// Look up saved state for a ws_id/monitor pair using slot-keyed lookup.
// Returns nullptr if not found in m_saved.
const SHAState* CLayoutState::_savedFor(int wsId, const std::string& monitor) const {
    std::string key = saveKey(wsId, monitor);
    auto it = m_saved.find(key);
    return (it != m_saved.end()) ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

std::string CLayoutState::statePath() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/hypr-awesome/state.json";
}

static eHAMode modeFromString(const std::string& s) {
    if (s == "monocle") return eHAMode::MONOCLE;
    if (s == "master")  return eHAMode::MASTER;
    return eHAMode::DWINDLE;
}

static eHAMasterVariant orientFromString(const std::string& s) {
    if (s == "top")    return eHAMasterVariant::TOP;
    if (s == "right")  return eHAMasterVariant::RIGHT;
    if (s == "bottom") return eHAMasterVariant::BOTTOM;
    return eHAMasterVariant::LEFT;
}

// Tiny hand-rolled JSON parser sufficient for our format.
static std::map<std::string, std::map<std::string, std::string>>
parseJson(const std::string& text) {
    std::map<std::string, std::map<std::string, std::string>> result;
    size_t start = text.find('{');
    size_t end   = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos) return result;

    size_t pos = start + 1;
    while (pos < end) {
        size_t ks = text.find('"', pos);
        if (ks == std::string::npos || ks >= end) break;
        size_t ke = text.find('"', ks + 1);
        if (ke == std::string::npos) break;
        std::string key = text.substr(ks + 1, ke - ks - 1);
        pos = ke + 1;

        size_t os = text.find('{', pos);
        if (os == std::string::npos || os >= end) break;
        size_t oe = text.find('}', os + 1);
        if (oe == std::string::npos) break;

        std::string inner = text.substr(os + 1, oe - os - 1);
        pos = oe + 1;

        std::map<std::string, std::string> fields;
        size_t ip = 0;
        while (ip < inner.size()) {
            size_t fks = inner.find('"', ip);
            if (fks == std::string::npos) break;
            size_t fke = inner.find('"', fks + 1);
            if (fke == std::string::npos) break;
            std::string fkey = inner.substr(fks + 1, fke - fks - 1);
            ip = fke + 1;
            size_t col = inner.find(':', ip);
            if (col == std::string::npos) break;
            ip = col + 1;
            while (ip < inner.size() && (inner[ip] == ' ' || inner[ip] == '\t')) ip++;
            std::string fval;
            if (ip < inner.size() && inner[ip] == '"') {
                size_t vs = ip + 1;
                size_t ve = inner.find('"', vs);
                if (ve == std::string::npos) break;
                fval = inner.substr(vs, ve - vs);
                ip = ve + 1;
            } else {
                size_t ve = inner.find_first_of(",}", ip);
                fval = trim(inner.substr(ip, ve - ip));
                ip = ve;
            }
            fields[fkey] = fval;
        }
        result[key] = fields;
    }
    return result;
}

void CLayoutState::load() {
    std::ifstream f(statePath());
    if (!f.is_open()) return;

    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();

    auto parsed = parseJson(text);
    m_saved.clear();

    for (auto& [key, fields] : parsed) {
        // Keys are "slot@monitor" (e.g. "1@DP-5") — slot is always 1-based
        // within the monitor, independent of monitor index.
        size_t at = key.rfind('@');
        if (at == std::string::npos) continue;
        std::string slotStr = key.substr(0, at);
        std::string mon     = key.substr(at + 1);

        // Accept both slot-style ("1@DP-5") and old ws_id-style ("9@DP-5").
        // Both are stored in m_saved keyed as-is; _savedFor() normalises via
        // saveKey() which produces slot-style, so old-format keys starting at
        // 9+ will not be found — they'll fall back to defaults. This is fine:
        // one-time migration cost of losing preferences for the first session.
        // New saves always write slot-style so this path disappears quickly.
        int slotOrId = 0;
        try { slotOrId = std::stoi(slotStr); } catch (...) { continue; }
        if (slotOrId <= 0) continue;

        SHAState s;
        auto it = fields.find("mode");
        if (it != fields.end()) s.mode = modeFromString(it->second);

        it = fields.find("dwindleVertical");
        if (it != fields.end()) s.dwindleVertical = (it->second == "true");

        it = fields.find("masterOrientation");
        if (it != fields.end()) s.masterOrientation = orientFromString(it->second);

        m_saved[key] = s;
    }

    Log::logger->log(Log::INFO, "[hawesome] Loaded {} state entries from {}",
                     m_saved.size(), statePath());
}

void CLayoutState::save() const {
    // Create directory if needed — use std::filesystem in C++17 or fallback
    std::string path = statePath();
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        // mkdir -p via system() is acceptable here (called infrequently, not on hot path)
        ::system(("mkdir -p " + dir).c_str());
    }

    std::ostringstream o;
    o << "{\n";
    bool first = true;

    // Save using slot@monitor keys — stable across monitor index changes.
    for (auto& [key, s] : m_state) {
        if (!first) o << ",\n";
        first = false;
        // key.first = ws_id, key.second = monitor name
        // Convert ws_id to slot for stable key
        int slot = wsIdToSlot(key.first);
        o << "  \"" << slot << "@" << key.second << "\": {\n"
          << "    \"mode\": \"" << CLayoutState::modeName(s.mode) << "\",\n"
          << "    \"dwindleVertical\": " << (s.dwindleVertical ? "true" : "false") << ",\n"
          << "    \"masterOrientation\": \"" << CLayoutState::masterOrientName(s.masterOrientation) << "\"\n"
          << "  }";
    }
    o << "\n}\n";

    std::ofstream f(path);
    if (f.is_open()) {
        f << o.str();
        Log::logger->log(Log::DEBUG, "[hawesome] Saved {} state entries (slot-keyed)",
                         m_state.size());
    }
}

// ---------------------------------------------------------------------------
// Monitor change handling
// ---------------------------------------------------------------------------

void CLayoutState::onMonitorAdded(const std::string& monitorName) {
    // Nothing to do in state — new workspaces will trigger workspace.created
    // events and get() will lazily restore from m_saved using the slot key.
    Log::logger->log(Log::INFO,
        "[hawesome] monitor added: {} — state will be restored lazily on ws creation",
        monitorName);
}

void CLayoutState::onMonitorRemoved(const std::string& monitorName) {
    // Remove all runtime state entries for this monitor. Their workspaces are
    // going inert. We do NOT remove m_saved entries — the user's preferences
    // for that monitor's slots should survive an unplug/replug cycle.
    std::vector<std::pair<int,std::string>> toRemove;
    for (auto& [key, _] : m_state) {
        if (key.second == monitorName)
            toRemove.push_back(key);
    }
    for (auto& k : toRemove)
        m_state.erase(k);

    Log::logger->log(Log::INFO,
        "[hawesome] monitor removed: {} — cleared {} runtime state entries (saved preserved)",
        monitorName, toRemove.size());
}
