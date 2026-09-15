#include "LayoutState.hpp"

#include <hyprland/src/debug/log/Logger.hpp>

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external deps)
// ---------------------------------------------------------------------------

static std::string jsonStr(const std::string& s) {
    return "\"" + s + "\"";
}

// Very small JSON parser for our state file.
// Format: { "slot@monitor": { "mode": "dwindle", "dwindleVertical": false,
//                             "masterOrientation": "left" }, ... }
// We keep it simple — no nested objects beyond one level.

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    size_t b = s.find_last_not_of(" \t\r\n\"");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// CLayoutState
// ---------------------------------------------------------------------------

CLayoutState::CLayoutState() {
    load();
}

SHAState& CLayoutState::get(int wsId, const std::string& monitor) {
    auto key = std::make_pair(wsId, monitor);
    if (m_state.find(key) == m_state.end())
        m_state[key] = SHAState{};
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
            switch (s.masterOrientation) {
                case eHAMasterVariant::LEFT:   return "left";
                case eHAMasterVariant::TOP:    return "top";
                case eHAMasterVariant::RIGHT:  return "right";
                case eHAMasterVariant::BOTTOM: return "bottom";
            }
    }
    return "";
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
// Extracts top-level key→object pairs.
static std::map<std::string, std::map<std::string, std::string>>
parseJson(const std::string& text) {
    std::map<std::string, std::map<std::string, std::string>> result;
    // strip outer {}
    size_t start = text.find('{');
    size_t end   = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos) return result;

    size_t pos = start + 1;
    while (pos < end) {
        // find key
        size_t ks = text.find('"', pos);
        if (ks == std::string::npos || ks >= end) break;
        size_t ke = text.find('"', ks + 1);
        if (ke == std::string::npos) break;
        std::string key = text.substr(ks + 1, ke - ks - 1);
        pos = ke + 1;

        // find inner object
        size_t os = text.find('{', pos);
        if (os == std::string::npos || os >= end) break;
        size_t oe = text.find('}', os + 1);
        if (oe == std::string::npos) break;

        std::string inner = text.substr(os + 1, oe - os - 1);
        pos = oe + 1;

        // parse inner key:value pairs
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
            // value: could be "string" or true/false
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

    // State file keys are "slot@monitor" (e.g. "3@DP-5").
    // We store by (ws_id, monitor) at runtime but load by slot name.
    // For now, accept both numeric ws_id keys and slot@monitor keys.
    // Slot→ws_id mapping happens lazily when first accessed.
    // Store in a pending map; apply when workspace is first used.
    // For simplicity, load numeric ws_id keys directly.
    auto parsed = parseJson(text);
    for (auto& [key, fields] : parsed) {
        // key format: "slot@monitor" or "ws_id@monitor"
        size_t at = key.rfind('@');
        if (at == std::string::npos) continue;
        std::string slotStr = key.substr(0, at);
        std::string mon     = key.substr(at + 1);

        int wsId = 0;
        try { wsId = std::stoi(slotStr); } catch (...) { continue; }
        if (wsId <= 0) continue;

        SHAState s;
        auto it = fields.find("mode");
        if (it != fields.end()) s.mode = modeFromString(it->second);

        it = fields.find("dwindleVertical");
        if (it != fields.end()) s.dwindleVertical = (it->second == "true");

        it = fields.find("masterOrientation");
        if (it != fields.end()) s.masterOrientation = orientFromString(it->second);

        m_state[{wsId, mon}] = s;
    }

    Log::logger->log(Log::INFO, "[hawesome] Loaded {} state entries from {}",
                     m_state.size(), statePath());
}

void CLayoutState::save() const {
    // Create directory if needed
    std::string path = statePath();
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        system(("mkdir -p " + dir).c_str());
    }

    std::ostringstream o;
    o << "{\n";
    bool first = true;
    for (auto& [key, s] : m_state) {
        if (!first) o << ",\n";
        first = false;
        // Use ws_id@monitor as key — matches Python daemon format for slots
        // (slot == ws_id for split-monitor-workspaces where names == IDs)
        o << "  \"" << key.first << "@" << key.second << "\": {\n"
          << "    \"mode\": \"" << CLayoutState::modeName(s.mode) << "\",\n"
          << "    \"dwindleVertical\": " << (s.dwindleVertical ? "true" : "false") << ",\n"
          << "    \"masterOrientation\": \"" << CLayoutState::variantName(s) << "\"\n"
          << "  }";
    }
    o << "\n}\n";

    std::ofstream f(path);
    if (f.is_open()) {
        f << o.str();
        Log::logger->log(Log::DEBUG, "[hawesome] Saved {} state entries", m_state.size());
    }
}
