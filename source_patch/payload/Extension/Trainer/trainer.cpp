#include "trainer.h"
#include "Engine/Core/Hooks/hooks.h"
#include "trainer_classes.h"
#include "trainer_jump.h"
#include "trainer_presets.h"
#include "trainer_session.h"
#include "trainer_landing.h"
#include "trainer_waypoint.h"
#include "rock_to_fakie_live.h"
#include "board_stall_input.h"
#include "board_stall_auto.h"
#include "rock_position.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/physics_tuning.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <limits>
#include <numbers>
#include <optional>

// Game thread only. See trainer.h for how the menu reaches this.
namespace dingosdk::trainer {
namespace {
namespace tuning = dingosdk::physics_tuning;
using Vec3 = std::array<float, 3>;
constexpr float max_coordinate = 1.0e6f;
constexpr std::uint32_t air_states_first = 200, air_states_end = 300;
constexpr std::uint32_t xinput_up = 0x1, xinput_down = 0x2, xinput_left = 0x4, xinput_right = 0x8,
                        xinput_lb = 0x100, xinput_rb = 0x200;

struct Entry {
    std::string id, key, label, group; // key: lower-case id
    Kind kind{};
    std::uint16_t offset{}, size{};
    std::size_t curve{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{};
    bool used{}; // the game's code was found to read it (trainer_used.inc), or it is linked
    std::vector<std::size_t> drives; // graph multipliers this value scales (value_links)
    std::vector<std::size_t> members; // graph: its outputs (Y0.., Min_Y, Max_Y)
    int class_field{-1};              // a field of one of the game's data-defined classes (trainer_classes.h), not of the tuning asset
};
struct MapData {
    std::array<Marker, marker_slots> markers{};
    std::string preset;
};
struct MapFile { // Mods/<mod>/trainer.json
    std::string note, preset_name;
    std::vector<std::pair<std::string, double>> preset;
    std::vector<Spot> spots;
};
struct Saved {
    double value{};
    bool frozen{};
};
struct SelfTest {
    int step{}; // 0: idle
    std::uint64_t until{};
    std::size_t entry{};
    double before{}, target{};
    Vec3 origin{};
    int failures{};
};
struct Motion {
    bool valid{};
    Vec3 position{}, velocity{};
    std::int64_t stamp{};
    bool airborne{};
    Vec3 takeoff{}, takeoff_velocity{};
    std::int64_t takeoff_stamp{};
    float apex{};
    bool axes_valid{};
    Vec3 forward{}, up{};
    float spin{}, spin_rate{}, flip{};
    float board_turn{}; // the board's fastest turn rate in this jump, degrees per second
    std::uint32_t air_state{}, ground_state{};
};
// The trick multipliers and auto push in force: x of the game's own, and for auto push the
// speed a rolling skater is carried up to (m/s, 0: off).
struct Boosts {
    float flip{1}, hippy{1}, nocomply{1}, boneless{1}, offboard{1}, cruise{};
};
struct State {
    bool loaded{}; // the store was read
    std::shared_ptr<const tuning::Model> model;
    std::vector<Entry> entries;
    std::map<std::string, std::size_t, std::less<>> index;
    std::uintptr_t base{}, asset{}, entity{};
    tuning::Values own, want;
    bool write_due{}, view_due{true}, save_due{};
    bool profile_due{}; // the loaded map's preset is still to be applied
    bool refresh_due{}; // values were written while the skater had no cached copy to refresh
    std::uint64_t next_hold{}, next_save{}, next_skater{}, revision{};
    std::string status = "Waiting for the game.", last;
    bool ready{};
    std::map<std::string, Saved, std::less<>> saved;                                    // by key
    std::map<std::string, std::map<std::string, double, std::less<>>, std::less<>> user; // presets
    std::vector<std::string> active;                                                    // presets applied since Stock
    std::map<std::string, MapData, std::less<>> maps;
    std::string map;
    MapFile map_file;
    int slot{};
    bool auto_return{}, pad_shortcuts{}, hud{}, hud_jump{}, logging{};
    // The hippy jump's height is set by the game's trick scripts, not by tuning: the trainer
    // scales the upward velocity when it sees one start.
    float hippy_height{1};
    // An on-foot jump: the same scaling, when a skater standing off the board starts to rise.
    float offboard_height{1};
    // Board flip tricks: a multiplier on the game's flip speed curves (trainer_classes.h).
    float flip_speed{1};
    bool offboard_grounded{}; // off the board and not moving up or down at the last tick
    const char *boost_name{""};
    // The no comply and the boneless are launched by the trick scripts too (trainer_jump.cpp).
    float nocomply_height{1}, boneless_height{1};
    std::uint32_t last_state{};
    float boost_factor{}; // velocity factor of the jump now starting, 0: none
    std::uint64_t boost_until{};
    float return_delay{1.5f};
    std::uint64_t seen_wipeouts{}, return_at{}, open_serial{};
    std::uint64_t class_search_at{}; // when to look for the game's tuning classes (0: not due)
    int class_search_tries{};
    std::uint64_t class_search_seen{};      // the searches that had finished when this level loaded
    bool class_list_wanted{};               // the player has the list of every value open on this level
    std::vector<std::size_t> class_entries; // the entries that are fields of those classes
    // A session whose host sets everyone's physics (session_physics.h): what the host shares
    // beyond its tuning, and what each class field holds for it. The player's own stand down.
    bool enforced{};
    std::uint64_t host_revision{};
    SessionExtras host;
    std::array<float, class_field_count> host_classes{};
    Boosts boosts;         // what is in force this tick
    bool extras_due{true}; // the player's own extras are to be published for a session again
    bool wipeouts_known{}; // the wipeout count the session started with was read
    int open_tab{};
    std::uint32_t pad_previous{};
    // The pause map's waypoint. The registry is read only while the player has a use for the
    // answer (the TELEPORT card is on screen, or `trainer waypoint`), and the waypoint is
    // forgotten on a new level: it belonged to the old one.
    std::optional<Vec3> waypoint;
    std::string waypoint_detail, waypoint_level;
    std::uint64_t waypoint_wanted_until{}, waypoint_next_read{};
    // A ground-snapped arrival being watched (trainer_landing.h).
    landing::Watch landing;
    Motion motion;
    reskate_custom::rock_to_fakie::LiveDetector rock_detector;
    // Board Stall v1.2: independent direct-controller B edges and game-hook diagnostics.
    bool stall_probe_enabled{};
    std::ofstream stall_probe_file;
    std::filesystem::path stall_probe_path;
    std::uint64_t stall_probe_started_ms{};
    std::int64_t stall_probe_last{};
    std::uint64_t stall_probe_rows{}, stall_probe_last_press_count{};
    stall_probe_input::DirectBTracker stall_direct_input{};
    // v1.4.14: bounded, native-selector + B-edge trace; opt-in and no debugger.
    bool native_b_recording{};
    std::filesystem::path native_b_path;
    std::uint64_t native_b_started{};
    stall_probe_input::DirectBTracker native_b_controller{};
    std::uint64_t native_b_edges{};
    // v1.3: opt-in preview or keyboard-action experiment, OFF by default.
    stall_auto::Gate stall_auto_gate{};
    bool stall_auto_enabled{}, stall_auto_live{}, stall_key_held{};
    unsigned stall_auto_key{}; // ASCII A-Z / 0-9 only, never globally mapped by ReSkate.
    std::uint64_t stall_auto_up_at{}, stall_auto_sent{}, stall_auto_failed{};

    // ROCK_POSITION_SCREEN_V07: calibrated, read-only geometric screening.
    bool rock_position_enabled{};
    std::int64_t rock_position_last{};
    reskate_custom::rock_position::Calibration rock_position_site{reskate_custom::rock_position::lower_lip};
    reskate_custom::rock_position::Detector rock_position_detector{reskate_custom::rock_position::lower_lip};
    bool rock_detection_enabled{}; // OFF until trainer rock 1
    // ROCK_SURFACE_PROBE_V03: diagnostic-only surface sampling; no game state writes.
    bool rock_probe_enabled{};
    std::ofstream rock_probe_file;
    std::int64_t rock_probe_start{}, rock_probe_last{};
    std::uint64_t rock_probe_rows{};
    // ROCK_BOARD_PROBE_V05: opt-in, passive native board-body telemetry.
    bool rock_board_enabled{};
    std::ofstream rock_board_file;
    std::int64_t rock_board_start{}, rock_board_last{};
    std::uint64_t rock_board_rows{}, rock_board_valid{};
    Telemetry telemetry;
    std::ofstream log;
    std::int64_t log_start{};
    std::map<std::uint32_t, std::uint64_t> states; // physics state -> ticks seen
    SelfTest test;
};
State &state() {
    static auto *value = new State;
    return *value;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// "MaxPushSpeed" -> "Max Push Speed"
std::string spaced(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool upper = std::isupper(c) != 0;
        if (i && upper) {
            const auto before = static_cast<unsigned char>(text[i - 1]);
            const bool next_lower = i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1]));
            if (std::islower(before) || std::isdigit(before) || (std::isupper(before) && next_lower)) result += ' ';
        }
        result += text[i] == '.' ? ' ' : text[i];
        if (text[i] == '.') result += "/ ";
    }
    return result;
}
std::optional<double> number(std::string_view text) {
    double value{};
    const auto *last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }

// ---- storage ---------------------------------------------------------------------------
std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"trainer";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::filesystem::path game_directory() {
    std::vector<wchar_t> exe(32768);
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    return std::filesystem::path(exe.data()).parent_path();
}
std::optional<Json> read_json(const std::filesystem::path &path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 1024 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}
std::optional<Vec3> read_position(const Json &value) {
    if (!value.is_array() || value.size() != 3) return std::nullopt;
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number()) return std::nullopt;
        result[i] = value.at(i).get<float>();
        if (!std::isfinite(result[i]) || std::abs(result[i]) > max_coordinate) return std::nullopt;
    }
    return result;
}
Json write_position(const Vec3 &p) { return Json::array({Json(p[0]), Json(p[1]), Json(p[2])}); }

void load_store() {
    auto &s = state();
    s.loaded = true;
    const auto json = read_json(data_directory() / L"trainer.json");
    if (!json || !json->is_object()) return;
    try {
        if (json->contains("options") && json->at("options").is_object()) {
            const auto &o = json->at("options");
            s.auto_return = o.value("auto_return", false);
            s.pad_shortcuts = o.value("pad_shortcuts", false);
            s.hud = o.value("hud", false);
            s.hud_jump = o.value("hud_jump", false);
            s.return_delay = std::clamp(o.value("return_delay", 1.5f), 0.0f, 10.0f);
            s.hippy_height = std::clamp(o.value("hippy_height", 1.0f), height_low, height_high);
            s.nocomply_height = std::clamp(o.value("nocomply_height", 1.0f), height_low, height_high);
            s.offboard_height = std::clamp(o.value("offboard_height", 1.0f), height_low, height_high);
            s.flip_speed = std::clamp(o.value("flip_speed", 1.0f), flip_low, flip_high);
            s.boneless_height = std::clamp(o.value("boneless_height", 1.0f), height_low, height_high);
            s.slot = std::clamp(o.value("slot", 0), 0, static_cast<int>(marker_slots) - 1);
        }
        if (json->contains("values") && json->at("values").is_object())
            for (const auto &[key, row] : json->at("values").items())
                if (row.is_object() && row.contains("value") && row.at("value").is_number())
                    s.saved[lower(key)] = {row.at("value").get<double>(), row.value("frozen", false)};
        if (json->contains("presets") && json->at("presets").is_object())
            for (const auto &[name, values] : json->at("presets").items()) {
                if (!values.is_object() || name.empty() || name.size() > 48) continue;
                auto &preset = s.user[name];
                for (const auto &[key, value] : values.items())
                    if (value.is_number()) preset[lower(key)] = value.get<double>();
            }
        if (json->contains("active") && json->at("active").is_array())
            for (const auto &name : json->at("active"))
                if (name.is_string()) s.active.push_back(name.string());
        if (json->contains("maps") && json->at("maps").is_object())
            for (const auto &[level, row] : json->at("maps").items()) {
                if (!row.is_object()) continue;
                auto &map = s.maps[lower(level)];
                map.preset = row.value("preset", "");
                if (row.contains("markers") && row.at("markers").is_array())
                    for (std::size_t i = 0; i < marker_slots && i < row.at("markers").size(); ++i)
                        if (const auto position = read_position(row.at("markers").at(i))) map.markers[i] = {true, *position};
            }
    } catch (...) {
        say(logging::Level::warning, "Trainer: trainer.json is damaged; starting from what could be read.");
    }
}
void save_store() {
    auto &s = state();
    s.save_due = false;
    try {
        Json json = Json::object();
        json["schema"] = 1;
        Json options = Json::object();
        options["auto_return"] = s.auto_return;
        options["pad_shortcuts"] = s.pad_shortcuts;
        options["hud"] = s.hud;
        options["hud_jump"] = s.hud_jump;
        options["return_delay"] = s.return_delay;
        options["hippy_height"] = s.hippy_height;
        options["nocomply_height"] = s.nocomply_height;
        options["offboard_height"] = s.offboard_height;
        options["flip_speed"] = s.flip_speed;
        options["boneless_height"] = s.boneless_height;
        options["slot"] = s.slot;
        json["options"] = std::move(options);
        Json values = Json::object();
        // Values of fields this build's tuning does not have are kept as they were read.
        for (const auto &[key, saved] : s.saved) {
            Json row = Json::object();
            row["value"] = saved.value;
            row["frozen"] = saved.frozen;
            values[key] = std::move(row);
        }
        json["values"] = std::move(values);
        Json presets = Json::object();
        for (const auto &[name, preset] : s.user) {
            Json row = Json::object();
            for (const auto &[key, value] : preset) row[key] = value;
            presets[name] = std::move(row);
        }
        json["presets"] = std::move(presets);
        Json active = Json::array();
        for (const auto &name : s.active) active.push_back(name);
        json["active"] = std::move(active);
        Json maps = Json::object();
        for (const auto &[level, map] : s.maps) {
            if (level.empty()) continue;
            Json row = Json::object();
            Json markers = Json::array();
            for (const auto &marker : map.markers) markers.push_back(marker.set ? write_position(marker.position) : Json(nullptr));
            row["markers"] = std::move(markers);
            row["preset"] = map.preset;
            maps[level] = std::move(row);
        }
        json["maps"] = std::move(maps);
        const auto directory = data_directory();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto target = directory / L"trainer.json", temporary = directory / L"trainer.json.tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << json.dump(2);
            if (!file) return;
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(target, error);
            std::filesystem::rename(temporary, target, error);
        }
    } catch (...) {
        say(logging::Level::warning, "Trainer: could not save trainer.json.");
    }
}
void changed(bool save = true) {
    auto &s = state();
    s.view_due = true;
    s.extras_due = true;
    if (save) {
        s.save_due = true;
        s.next_save = GetTickCount64() + 1500;
    }
}

// ---- values ----------------------------------------------------------------------------
double read_field(const std::vector<std::uint8_t> &image, const Entry &e) {
    const auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        float value;
        std::memcpy(&value, at, 4);
        return value;
    }
    if (e.kind == Kind::flag) return *at ? 1 : 0;
    switch (e.size) {
    case 1: { std::int8_t v; std::memcpy(&v, at, 1); return v; }
    case 2: { std::int16_t v; std::memcpy(&v, at, 2); return v; }
    case 4: { std::int32_t v; std::memcpy(&v, at, 4); return v; }
    default: { std::int64_t v; std::memcpy(&v, at, 8); return static_cast<double>(v); }
    }
}
void write_field(std::vector<std::uint8_t> &image, const Entry &e, double value) {
    auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        const auto v = static_cast<float>(value);
        std::memcpy(at, &v, 4);
    } else if (e.kind == Kind::flag) {
        *at = value != 0 ? 1 : 0;
    } else {
        switch (e.size) {
        case 1: { const auto v = static_cast<std::int8_t>(std::clamp(value, -128.0, 127.0)); std::memcpy(at, &v, 1); break; }
        case 2: { const auto v = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0)); std::memcpy(at, &v, 2); break; }
        case 4: { const auto v = static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0)); std::memcpy(at, &v, 4); break; }
        default: { const auto v = static_cast<std::int64_t>(value); std::memcpy(at, &v, 8); break; }
        }
    }
}
// A curve's outputs times `factor`: a point is 0x1c bytes with its output at +0x14.
tuning::Curve scaled_curve(const tuning::Curve &source, double factor) {
    auto result = source;
    for (std::size_t p = 0; p + addr::physics_tuning::curve_point_size <= result.points.size(); p += addr::physics_tuning::curve_point_size) {
        float y;
        std::memcpy(&y, result.points.data() + p + 0x14, 4);
        y = static_cast<float>(y * factor);
        if (std::isfinite(y)) std::memcpy(result.points.data() + p + 0x14, &y, 4);
    }
    return result;
}
double sane(const Entry &e, double value) {
    if (!std::isfinite(value)) return e.stock;
    if (e.kind == Kind::flag) return value != 0 ? 1 : 0;
    if (e.kind == Kind::integer) return std::round(value);
    if (e.kind == Kind::curve || e.kind == Kind::graph) return std::clamp(value, 0.0, 1.0e6);
    return std::clamp(value, -1.0e6, 1.0e6);
}
// What a class field should hold: the player's value, or the host's while a session's host
// sets everyone's physics.
void want_class(const Entry &e) {
    auto &s = state();
    const auto field = static_cast<std::size_t>(e.class_field);
    want_class_value(field, s.enforced ? s.host_classes[field] : static_cast<float>(e.touched ? e.value : e.stock));
}
void apply_entry(Entry &e) {
    auto &s = state();
    if (e.class_field >= 0) {
        want_class(e);
        return;
    }
    if (e.kind == Kind::graph) {
        // A point the player set by hand keeps their value.
        for (const auto index : e.members) {
            const auto &member = s.entries[index];
            if (!member.touched && static_cast<std::size_t>(member.offset) + member.size <= s.want.image.size())
                write_field(s.want.image, member, e.touched ? member.stock * e.value : member.stock);
        }
    } else if (e.kind == Kind::curve) {
        if (e.curve < s.own.curves.size() && s.own.curves[e.curve] && e.curve < s.want.curves.size())
            s.want.curves[e.curve] = e.touched ? scaled_curve(*s.own.curves[e.curve], e.value) : *s.own.curves[e.curve];
    } else if (static_cast<std::size_t>(e.offset) + e.size <= s.want.image.size()) {
        write_field(s.want.image, e, e.touched ? e.value : e.stock);
    }
    s.write_due = true;
}
void remember(const Entry &e) {
    auto &s = state();
    if (e.touched || e.frozen) s.saved[e.key] = {e.value, e.frozen};
    else s.saved.erase(e.key);
}
void set_entry(Entry &e, double value) {
    e.value = sane(e, value);
    e.touched = e.value != e.stock;
    apply_entry(e);
    remember(e);
    // A linked value scales its graphs by the same ratio (the graphs are what the game reads).
    if (!e.drives.empty() && e.stock != 0) {
        const auto ratio = e.value / e.stock;
        const auto targets = e.drives; // set_entry may not touch `e` again, but keep the list stable
        auto &s = state();
        for (const auto index : targets)
            if (index < s.entries.size() && !s.entries[index].frozen) {
                auto &target = s.entries[index];
                const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
                set_entry(target, multiplier ? ratio : target.stock * ratio);
            }
    }
}
// A value a locked linked value drives (its graphs, the push class's speeds) is locked with it:
// resetting it alone would leave the lock showing a number that no longer does anything.
bool is_locked(const Entry &e) {
    if (e.frozen) return true;
    auto &s = state();
    const auto index = static_cast<std::size_t>(&e - s.entries.data());
    return std::ranges::any_of(s.entries, [&](const Entry &other) {
        return other.frozen && std::ranges::find(other.drives, index) != other.drives.end();
    });
}
Entry *find_entry(std::string_view id) {
    auto &s = state();
    const auto found = s.index.find(lower(id));
    return found == s.index.end() ? nullptr : &s.entries[found->second];
}
void build_entries() {
    auto &s = state();
    s.entries.clear();
    s.index.clear();
    s.class_entries.clear();
    const auto &m = *s.model;
    const auto add = [&](std::string name, Kind kind, std::uint16_t offset, std::uint16_t size, std::size_t curve) {
        if (name.empty()) name = std::format("Unnamed.0x{:x}", offset);
        Entry e;
        e.id = kind == Kind::curve || kind == Kind::graph ? name + " x" : name;
        e.key = lower(e.id);
        if (s.index.contains(e.key)) { // arrays of structures repeat names
            e.id += std::format(" @0x{:x}", offset);
            e.key = lower(e.id);
        }
        const auto dot = name.find('.');
        auto group = dot == std::string::npos ? std::string("General") : name.substr(0, dot);
        for (const std::string_view prefix : {"SkatePhysics", "Physics"})
            if (group.size() > prefix.size() && group.starts_with(prefix)) {
                group.erase(0, prefix.size());
                break;
            }
        e.group = spaced(group);
        e.label = spaced(dot == std::string::npos ? name : name.substr(dot + 1));
        if (kind == Kind::curve || kind == Kind::graph) e.label += " (x)";
        e.kind = kind;
        e.offset = offset;
        e.size = size;
        e.curve = curve;
        e.used = value_used(offset);
        s.index.emplace(e.key, s.entries.size());
        s.entries.push_back(std::move(e));
    };
    for (std::size_t i = 0; i < m.fields.size(); ++i) {
        const auto &f = m.fields[i];
        add(i < m.field_names.size() ? m.field_names[i] : std::string{}, f.real ? Kind::real : f.flag ? Kind::flag : Kind::integer,
            f.offset, f.size, 0);
    }
    for (std::size_t i = 0; i < m.curve_slots.size(); ++i)
        add(i < m.curve_names.size() ? m.curve_names[i] : std::string{}, Kind::curve, m.curve_slots[i], 8, i);
    // Graphs stored in the asset: sixteen X and Y values, a point count and bounds. Each
    // becomes one multiplier row; its points stay editable but out of the way.
    const auto plain = s.entries.size();
    for (std::size_t i = 0; i < plain; ++i) {
        if (!s.entries[i].id.ends_with(".Y0") || s.entries[i].kind != Kind::real) continue;
        const auto parent = s.entries[i].id.substr(0, s.entries[i].id.size() - 3);
        const auto offset = s.entries[i].offset;
        std::vector<std::size_t> outputs;
        for (std::size_t j = 0; j < plain; ++j) {
            auto &other = s.entries[j];
            if (other.id.size() <= parent.size() + 1 || !other.id.starts_with(parent) || other.id[parent.size()] != '.') continue;
            const std::string_view leaf(other.id.c_str() + parent.size() + 1);
            if (leaf.find('.') != std::string_view::npos) continue;
            other.detail = true;
            const bool output = (leaf.starts_with("Y") && leaf.size() <= 3) || leaf == "Min_Y" || leaf == "Max_Y";
            if (output && other.kind == Kind::real) outputs.push_back(j);
        }
        add(parent, Kind::graph, offset, 0, 0);
        s.entries.back().members = std::move(outputs);
    }
    // The game's data-defined classes: one row per number field, under the class's name.
    for (std::size_t c = 0; c < class_count; ++c)
        for (std::size_t i = 0; i < class_specs[c].count; ++i) {
            const auto &field = class_fields[class_specs[c].first + i];
            Entry e;
            e.id = std::format("{}.{}", class_specs[c].key, field.name);
            e.key = lower(e.id);
            if (s.index.contains(e.key)) continue;
            e.group = class_specs[c].group;
            e.label = spaced(field.name);
            e.kind = Kind::real;
            e.used = field.seen;
            e.class_field = static_cast<int>(class_specs[c].first + i);
            e.stock = e.value = field.stock;
            s.index.emplace(e.key, s.entries.size());
            s.class_entries.push_back(s.entries.size());
            s.entries.push_back(std::move(e));
        }
    for (const auto &link : value_links()) {
        const auto from = s.index.find(link.value), to = s.index.find(link.drives);
        if (from == s.index.end() || to == s.index.end()) continue;
        s.entries[from->second].drives.push_back(to->second);
        s.entries[from->second].used = true;
    }
    // A host's values may have arrived before there were entries to hold them.
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    s.extras_due = true;
    say(logging::Level::info, std::format("Trainer: {} tuning values and {} curves are editable.", m.fields.size(), m.curve_slots.size()));
}
// A new copy of the asset (first sight, or a level load): its values are the stock ones.
void adopt(const tuning::Values &live) {
    auto &s = state();
    s.own = live;
    s.want = live;
    for (auto &e : s.entries) {
        e.stock = e.class_field >= 0 ? class_fields[e.class_field].stock : e.kind == Kind::curve || e.kind == Kind::graph ? 1.0 : read_field(s.own.image, e);
        const auto found = s.saved.find(e.key);
        if (found != s.saved.end()) {
            e.frozen = found->second.frozen;
            e.value = sane(e, found->second.value);
            e.touched = e.value != e.stock;
        } else if (!e.touched) {
            e.value = e.stock;
        }
        if (e.touched) apply_entry(e);
    }
    // A linked value saved by itself (before links existed, or edited in the file) still
    // drives its graphs.
    for (auto &e : s.entries) {
        if (e.drives.empty() || !e.touched || e.stock == 0) continue;
        for (const auto index : e.drives) {
            auto &target = s.entries[index];
            if (target.touched || target.frozen) continue;
            const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
            target.value = sane(target, multiplier ? e.value / e.stock : target.stock * e.value / e.stock);
            target.touched = target.value != target.stock;
            if (target.touched) {
                apply_entry(target);
                remember(target);
            }
        }
    }
    s.write_due = true;
    changed(false);
}
std::string apply_preset(std::string_view name);
bool editable(std::string *why = nullptr) {
    // A guest's physics are the host's while the session enforces them: the tuning
    // (physics_tuning::enforce), and what the host shares beyond it (sync_session). Writing
    // over either would only fight the session, so the player's own stand down.
    if (session_tuning_enforced()) {
        if (why) *why = "This session's host sets everyone's physics: you skate with the host's.";
        return false;
    }
    return true;
}
// Auto push is the trainer's doing: the game's flag reaches only its animation.
float own_cruise() {
    const auto *auto_push = find_entry("physicsmode.autopushenabled"), *auto_speed = find_entry("physicspush.maxspeedforautopush");
    return auto_push && auto_speed && auto_push->touched && auto_push->value != 0 ? static_cast<float>(auto_speed->value) : 0.0f;
}
// The host's where a session's host sets everyone's physics, whatever the boosts switch says
// (that switch is about a guest's own); otherwise the player's, and the game's own in a
// session that has turned boosts off.
Boosts boosts_in_force() {
    auto &s = state();
    if (s.enforced)
        return {s.host.flip_speed, s.host.hippy_height, s.host.nocomply_height, s.host.boneless_height, s.host.offboard_height, s.host.cruise};
    if (multiplayer_session_active() && !session_boosts_allowed()) return {};
    return {s.flip_speed, s.hippy_height, s.nocomply_height, s.boneless_height, s.offboard_height, own_cruise()};
}
// The player's own, for a session they host to share with its guests.
void publish_extras() {
    auto &s = state();
    s.extras_due = false;
    SessionExtras own;
    own.flip_speed = s.flip_speed;
    own.hippy_height = s.hippy_height;
    own.nocomply_height = s.nocomply_height;
    own.boneless_height = s.boneless_height;
    own.offboard_height = s.offboard_height;
    own.cruise = own_cruise();
    for (const auto index : s.class_entries)
        if (const auto &e = s.entries[index]; e.touched)
            own.classes.emplace_back(static_cast<std::uint16_t>(e.class_field), static_cast<float>(e.value));
    set_local_physics_extras(encode_session_extras(own));
}
// Follows a session into and out of its host's physics, and the host's changes while there.
void sync_session() {
    auto &s = state();
    const bool enforced = session_tuning_enforced();
    std::vector<std::uint8_t> bytes;
    bool fresh = false;
    if (enforced) fresh = host_physics_extras(s.host_revision, bytes);
    else s.host_revision = 0; // read again the next time a host sets the physics
    if (enforced == s.enforced && !fresh) return;
    if (fresh) {
        auto decoded = decode_session_extras(bytes);
        if (!decoded) say(logging::Level::warning, "Trainer: what the host shares beyond its tuning could not be read; the game's own is used for it.");
        s.host = decoded ? std::move(*decoded) : SessionExtras{};
        for (std::size_t i = 0; i < class_field_count; ++i) s.host_classes[i] = class_fields[i].stock;
        for (const auto &[index, value] : s.host.classes) s.host_classes[index] = value;
        if (!s.host.stock())
            say(logging::Level::info, std::format("Trainer: skating with the host's trick multipliers and {} of its class values.", s.host.classes.size()));
    }
    s.enforced = enforced;
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    changed(false);
}
void hold(std::uint64_t now) {
    auto &s = state();
    if (!s.base) return;
    if (!s.model) {
        tuning::prepare();
        std::string error;
        s.model = tuning::game_tuning(&error);
        if (!s.model) {
            const auto next = error.empty() ? std::string("Reading the game's physics tuning...") : "Physics tuning is unavailable: " + error + ".";
            if (next != s.status) { s.status = next; changed(false); }
            return;
        }
        build_entries();
    }
    if (!s.write_due && now < s.next_hold) return;
    s.next_hold = now + 500;
    tuning::Values live;
    std::uintptr_t asset{};
    if (!tuning::read_live(s.base, live, &asset)) {
        if (s.ready) { s.ready = false; s.status = "Waiting for a level."; changed(false); }
        return;
    }
    if (asset != s.asset || !s.ready) {
        s.asset = asset;
        adopt(live);
        s.ready = true;
        s.status = "Ready.";
    }
    if (!editable()) return;
    if (s.profile_due) {
        s.profile_due = false;
        if (const auto found = s.maps.find(s.map); found != s.maps.end() && !found->second.preset.empty())
            say(logging::Level::info, "Trainer: this map's preset - " + apply_preset(found->second.preset));
    }
    const bool pending = s.write_due || std::ranges::any_of(s.entries, [](const Entry &e) { return e.touched; });
    s.write_due = false;
    if (!pending) return;
    const auto written = tuning::write_live(s.base, s.entity, s.want);
    if (written.failed) {
        s.status = "The physics tuning could not be written.";
        changed(false);
    }
    // The skater skates from its own copy of part of the tuning: keep asking until it took it.
    if (written.values || written.curves) {
        s.refresh_due = !written.refreshed;
        say(logging::Level::info, std::format("Trainer: wrote {} values and {} curves; skater copy refreshed: {}.", written.values, written.curves,
                                              written.refreshed ? "yes" : "not yet"));
    } else if (s.refresh_due && tuning::refresh_skater(s.base, s.entity)) {
        s.refresh_due = false;
        say(logging::Level::info, "Trainer: skater copy refreshed.");
    }
}

// ---- presets ---------------------------------------------------------------------------
bool matches(const std::string &key, std::string_view pattern) {
    std::size_t start = 0;
    while (start < pattern.size()) {
        auto end = pattern.find(' ', start);
        if (end == std::string_view::npos) end = pattern.size();
        const auto token = pattern.substr(start, end - start);
        if (!token.empty()) {
            const bool exclude = token.front() == '!';
            const auto word = exclude ? token.substr(1) : token;
            if ((key.find(word) != std::string::npos) == exclude) return false;
        }
        start = end + 1;
    }
    return true;
}
// What each built-in preset's rules reach in the running game: (entry, rule) pairs, worked out
// once per table of entries.
const std::vector<std::vector<std::pair<std::size_t, std::size_t>>> &preset_targets() {
    static std::vector<std::vector<std::pair<std::size_t, std::size_t>>> cache;
    static const Entry *cached_for{};
    static std::size_t cached_count{};
    auto &s = state();
    if (cached_for == s.entries.data() && cached_count == s.entries.size() && !cache.empty()) return cache;
    cached_for = s.entries.data();
    cached_count = s.entries.size();
    const auto &presets = builtin_presets();
    cache.assign(presets.size(), {});
    for (std::size_t p = 0; p < presets.size(); ++p)
        for (std::size_t r = 0; r < presets[p].rules.size(); ++r) {
            const auto &rule = presets[p].rules[r];
            for (std::size_t i = 0; i < s.entries.size(); ++i) {
                const auto &e = s.entries[i];
                const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                if (e.detail || scale != rule.curves || !matches(e.key, rule.pattern)) continue;
                if (e.kind == Kind::flag && rule.multiply) continue;
                cache[p].emplace_back(i, r);
            }
        }
    return cache;
}
// A preset as a dial: `factor` is where its first value goes (x of stock); the others follow in
// proportion, so 1 is the game's own and the first rule's amount is the preset as it ships.
std::string apply_dial(std::string_view name, double factor) {
    auto &s = state();
    const auto &presets = builtin_presets();
    const auto wanted = lower(name);
    std::size_t p = 0;
    while (p < presets.size() && lower(presets[p].name) != wanted) ++p;
    if (p == presets.size()) return "error: no built-in preset is called \"" + std::string(name) + "\".";
    const auto &rules = presets[p].rules;
    if (rules.empty() || !rules[0].multiply || rules[0].amount <= 0 || rules[0].amount == 1)
        return "error: " + std::string(presets[p].name) + " is a switch, not a dial.";
    if (!(factor > 0) || !std::isfinite(factor)) return "error: a dial needs a multiplier above 0; 1 is the game's own.";
    const double t = std::log(factor) / std::log(rules[0].amount);
    std::size_t count{}, locked{};
    for (const auto &[index, r] : preset_targets()[p]) {
        auto &e = s.entries[index];
        const auto &rule = rules[r];
        if (is_locked(e)) {
            ++locked;
            continue;
        }
        if (rule.multiply) set_entry(e, rule.amount > 0 ? e.stock * std::pow(rule.amount, t) : e.stock * (1 + (rule.amount - 1) * t));
        else set_entry(e, t > 0.01 ? rule.amount : e.stock); // a switch that goes with the preset's direction
        ++count;
    }
    std::erase(s.active, std::string(presets[p].name));
    if (std::abs(t - 1) < 1e-3) s.active.emplace_back(presets[p].name);
    changed();
    return std::format("{} x{:.2f}: {} values set{}.", presets[p].name, factor, count, locked ? std::format("; {} locked values left alone", locked) : "");
}
std::string remove_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    std::size_t count{};
    std::string removed;
    const auto put_back = [&](Entry &e) {
        if (is_locked(e)) return;
        if (e.touched) {
            set_entry(e, e.stock);
            ++count;
            return;
        }
        // A linked value at stock whose graphs were set directly (by an older version's preset).
        for (const auto index : e.drives)
            if (auto &target = s.entries[index]; target.touched && !is_locked(target)) {
                set_entry(target, target.stock);
                ++count;
            }
    };
    for (const auto &[user_name, values] : s.user) {
        if (lower(user_name) != wanted) continue;
        for (const auto &saved : values)
            if (auto *e = find_entry(saved.first)) put_back(*e);
        removed = user_name;
        break;
    }
    if (removed.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (e.detail || scale != rule.curves || !matches(e.key, rule.pattern)) continue;
                    put_back(e);
                }
            removed = preset.name;
            break;
        }
    if (removed.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    std::erase(s.active, removed);
    // Presets that are still on may share values with the one switched off: set theirs again.
    const auto still_on = s.active;
    for (const auto &other : still_on) (void)apply_preset(other);
    changed();
    return std::format("{} off: {} values put back.", removed, count);
}
std::size_t reset_all() {
    auto &s = state();
    std::size_t count{};
    for (auto &e : s.entries) {
        if (!e.touched || is_locked(e)) continue;
        set_entry(e, e.stock);
        ++count;
    }
    s.active.clear();
    return count;
}
std::string apply_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    if (wanted == "stock") {
        const auto count = reset_all();
        changed();
        return std::format("Stock: {} values put back (frozen values kept).", count);
    }
    std::size_t count{}, locked{};
    std::string applied;
    if (wanted == "map") {
        if (s.map_file.preset.empty()) return "error: this map ships no trainer preset.";
        for (const auto &[id, value] : s.map_file.preset)
            if (auto *e = find_entry(id); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
        applied = s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    }
    if (applied.empty())
        for (const auto &[user_name, values] : s.user) {
            if (lower(user_name) != wanted) continue;
            for (const auto &[key, value] : values)
                if (auto *e = find_entry(key); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
            applied = user_name;
            break;
        }
    if (applied.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (e.detail || scale != rule.curves || !matches(e.key, rule.pattern)) continue;
                    if (e.kind == Kind::flag && rule.multiply) continue;
                    if (is_locked(e)) {
                        ++locked;
                        continue;
                    }
                    set_entry(e, rule.multiply ? e.stock * rule.amount : rule.amount);
                    ++count;
                }
            applied = preset.name;
            break;
        }
    if (applied.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    if (std::ranges::find(s.active, applied) == s.active.end()) s.active.push_back(applied);
    changed();
    return std::format("{}: {} values set{}.", applied, count,
                       locked ? std::format("; {} locked values left alone (untick their boxes in Tune to let presets change them)", locked) : "");
}

// ---- maps ------------------------------------------------------------------------------
MapFile read_map_file(const std::string &level) {
    MapFile result;
    const auto mods = game_directory() / L"Mods";
    std::error_code error;
    for (std::filesystem::directory_iterator it(mods, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto json = read_json(it->path() / L"trainer.json");
        if (!json || !json->is_object()) continue;
        try {
            // The file names its level, or it stands for every level its mod adds.
            bool mine = false;
            if (json->contains("level") && json->at("level").is_string()) mine = lower(json->at("level").string()) == level;
            else if (const auto levels = read_json(it->path() / L"reskate-levels.json"); levels && levels->contains("levels"))
                for (const auto &row : levels->at("levels"))
                    if (row.is_object() && lower(row.value("asset", "")) == level) mine = true;
            if (!mine) continue;
            result.note = json->value("note", "");
            if (json->contains("preset") && json->at("preset").is_object()) {
                const auto &preset = json->at("preset");
                result.preset_name = preset.value("name", "");
                if (preset.contains("values") && preset.at("values").is_object())
                    for (const auto &[id, value] : preset.at("values").items())
                        if (value.is_number() && result.preset.size() < 512) result.preset.emplace_back(id, value.get<double>());
            }
            if (json->contains("spots") && json->at("spots").is_array())
                for (const auto &row : json->at("spots")) {
                    if (!row.is_object() || !row.contains("position") || result.spots.size() >= 64) continue;
                    if (const auto position = read_position(row.at("position")))
                        result.spots.push_back({row.value("name", "Spot"), *position});
                }
            return result;
        } catch (...) {}
    }
    return result;
}
void enter_map(const std::string &level) {
    auto &s = state();
    if (s.rock_probe_file.is_open()) s.rock_probe_file.close();
    s.rock_probe_enabled = false; // a new map needs a new profile
    s.map = level;
    s.map_file = level.empty() ? MapFile{} : read_map_file(level);
    s.motion = {};
    s.rock_detector.reset();
    s.rock_position_enabled = false; // re-calibrate explicitly on each map
    s.rock_position_detector.reset();
    s.rock_position_last = 0;
    s.telemetry.last = {};
    s.telemetry.best = {};
    s.landing = {};
    s.telemetry.top_speed = 0;
    s.return_at = 0;
    s.profile_due = false;
    // A level brings fresh copies of the game's tuning classes: look for them once it has settled.
    s.class_search_at = level.empty() ? 0 : GetTickCount64() + 10000;
    s.class_search_tries = 0;
    s.class_search_seen = class_searches();
    s.class_list_wanted = false;
    if (!level.empty()) {
        const auto found = s.maps.find(level);
        s.profile_due = found != s.maps.end() && !found->second.preset.empty();
        if (!s.map_file.spots.empty() || !s.map_file.preset.empty())
            say(logging::Level::info, std::format("Trainer: the map ships {} spots and {} preset values.", s.map_file.spots.size(),
                                                    s.map_file.preset.size()));
    }
    changed(false);
}
MapData &current_map() { return state().maps[state().map]; }
bool teleport_allowed(std::string &why) {
    if (multiplayer_session_active() && !session_noclip_allowed()) {
        why = "The host has turned off teleporting in this session.";
        return false;
    }
    return true;
}
std::string go_to(const Vec3 &position, const std::string &what) {
    std::string why;
    if (!teleport_allowed(why)) return "error: " + why;
    if (!teleport_local_skater(position)) return "error: teleporting is unavailable right now.";
    state().motion.valid = false;
    return "Teleporting to " + what + ".";
}

// ---- ground-safe arrivals -------------------------------------------------------------
// The topmost collision surface at x, z (trainer_landing.h for the ray), from the client
// physics world. Empty while collision there is still streaming in.
std::optional<float> topmost_ground(float x, float z, float hint_y) {
    const auto first = landing::first_ray(hint_y);
    if (const auto hit = local_ground_height(x, z, first.top, first.bottom)) return hit;
    if (const auto retry = landing::retry_ray(first)) return local_ground_height(x, z, retry->top, retry->bottom);
    return std::nullopt;
}

// Teleports onto the surface at x, z rather than to a raw height: a map waypoint's height is
// not the ground, and arriving under it drops the skater through the map.
std::string land_at(float x, float z, float hint_y, const std::string &what) {
    auto &s = state();
    const auto ground = topmost_ground(x, z, hint_y);
    // Collision far away may not be loaded yet: go to the hint's height, and the landing
    // watch puts the skater on the surface once it streams in.
    const Vec3 to{x, ground ? *ground + landing::stand_height : hint_y + 2.0f, z};
    const auto answer = go_to(to, what);
    if (answer.starts_with("error")) return answer;
    s.landing = landing::start(x, z, to[1], GetTickCount64());
    say(logging::Level::info, ground ? std::format("Trainer: landing on the surface at {:.1f}, {:.1f}, {:.1f}.", to[0], to[1], to[2])
                                     : std::format("Trainer: no collision at {:.1f}, {:.1f} yet; going to height {:.1f} and waiting for the ground.", x, z, to[1]));
    const auto stem = answer.substr(0, answer.size() - 1);
    return ground ? stem + std::format(" (ground at {:.1f}).", *ground)
                  : stem + " (the ground there is still loading; you'll be put on it when it arrives).";
}

void update_landing(std::uint64_t now) {
    auto &s = state();
    auto &w = s.landing;
    if (!w.active || now < w.next || !s.telemetry.skater) return;
    const auto &p = s.telemetry.position;
    landing::Sample sample{.now = now, .position = p, .vertical = s.telemetry.vertical, .ground = std::nullopt, .may_resend = true};
    const float dx = p[0] - w.x, dz = p[2] - w.z;
    if (dx * dx + dz * dz <= landing::arrived_radius * landing::arrived_radius) sample.ground = topmost_ground(p[0], p[2], p[1]);
    switch (landing::step(w, sample)) {
    case landing::Step::resend: {
        const auto ground = topmost_ground(w.x, w.z, w.y);
        (void)go_to({w.x, ground ? *ground + landing::stand_height : w.y, w.z}, "the same spot");
        say(logging::Level::info, "Trainer: the teleport hadn't arrived; sent it again.");
        break;
    }
    case landing::Step::put_back:
        (void)go_to({p[0], *sample.ground + landing::stand_height, p[2]}, "the surface");
        say(logging::Level::info, std::format("Trainer: sank to {:.1f} under the surface at {:.1f}; put back on it.", p[1], *sample.ground));
        break;
    case landing::Step::landed:
        say(logging::Level::info, std::format("Trainer: landed at {:.1f}, {:.1f}, {:.1f}.", p[0], p[1], p[2]));
        break;
    case landing::Step::ended:
        say(logging::Level::info, std::format("Trainer: landing watch ended at {:.1f}, {:.1f}, {:.1f} after {} correction(s).", p[0], p[1], p[2], w.fixes));
        break;
    case landing::Step::wait: break;
    }
}

void refresh_waypoint() {
    auto &s = state();
    const auto read = read_map_waypoint(s.base);
    s.waypoint_detail = read.detail;
    if (const auto next = landing::remembered(s.waypoint, read.reading); next != s.waypoint) {
        s.waypoint = next;
        s.view_due = true;
    }
}

// ---- telemetry -------------------------------------------------------------------------
std::int64_t clock_now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double clock_seconds(std::int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}
float horizontal(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
// ROCK_SURFACE_PROBE_V03: a profile of terrain heights along the SKATER's forward axis.
// These are world collision heights, NOT deck/truck/wheel contacts. Read-only, opt-in, 5 Hz.
void sample_rock_surface(const std::array<float, 16>& matrix, const Vec3& position,
                         std::int64_t stamp, const Telemetry& telemetry) {
    auto &s = state();
    if (!s.rock_probe_enabled || !s.rock_probe_file) return;
    if (s.rock_probe_last && clock_seconds(stamp - s.rock_probe_last) < 0.05) return;
    s.rock_probe_last = stamp;
    const float fx = matrix[8], fz = matrix[10]; // skater forward, projected onto XZ plane
    const float length = std::hypot(fx, fz);
    if (!std::isfinite(length) || length < 0.01f || !std::isfinite(position[1])) return;
    const float ux = fx / length, uz = fz / length;
    const double elapsed = clock_seconds(stamp - s.rock_probe_start);
    s.rock_probe_file << std::format("{:.4f},{:.4f},{:.4f},{:.4f},{:.1f},{:.3f},{:.3f},{},{},{:.4f},{:.4f}",
                                     elapsed, position[0], position[1], position[2],
                                     telemetry.heading, telemetry.speed, telemetry.vertical,
                                     telemetry.physics_state, telemetry.airborne ? 1 : 0, ux, uz);
    // 7 samples, 30 cm apart, over +/-90 cm of skater facing direction.
    // The skater origin is NOT the board midpoint. Missing collision is an empty field.
    constexpr std::array<float, 7> offsets{-0.90f, -0.60f, -0.30f, 0.0f, 0.30f, 0.60f, 0.90f};
    for (const float distance : offsets) {
        s.rock_probe_file << ',';
        // ROCK_NEAR_SURFACE_PROBE_V04: query only near the skater's current Y.
        // topmost_ground() intentionally starts >1500m up, returning upper roofs
        // or map decks instead of the quarterpipe beneath the skater.
        const float sample_x = position[0] + ux * distance;
        const float sample_z = position[2] + uz * distance;
        const auto height = local_ground_height(sample_x, sample_z,
                                                 position[1] + 1.5f,
                                                 position[1] - 3.5f);
        if (height && std::isfinite(*height)) s.rock_probe_file << std::format("{:.4f}", *height);
    }
    // ROCK_SHARED_TIMESTAMP_V06: QPC counter makes both CSVs exactly joinable.
    s.rock_probe_file << ',' << stamp << '\n';
    if (++s.rock_probe_rows % 5 == 0) s.rock_probe_file.flush();
}
// ROCK_NATIVE_OWNERSHIP_V02: verified board wrapper and distinct shape/body handles.
// Evidence: core+0x430 -> board wrapper, wrapper+0x18 -> board collection.
// FUN_141009EA0 uses wrapper+0x718 -> provider. Records 0..5 are SHAPE
// handles (FUN_1425D7E00); records 6..8 are BODY handles (FUN_1425D7B10).
// This is NOT a contact detector, physics modifier, or full ownership proof.
struct RockNativeBody {
    bool present{}, descriptor_ok{}, generation_ok{}, position_ok{};
    const char* handle_kind{"unknown"};
    std::uintptr_t world{}, descriptor{}, record_ptr{};
    std::uint32_t index{0xffffffffu}, generation{}, pool_count{};
    Vec3 native_position{};
    float pose_gap{};
};

static RockNativeBody inspect_rock_native_body(std::uintptr_t provider,
                                              std::uintptr_t record,
                                              std::size_t slot,
                                              const Vec3& derived_position) {
    RockNativeBody result{};
    result.record_ptr = record;
    result.handle_kind = slot < 6 ? "shape" : "body";
    if (!provider || !record || slot >= 9) return result;
    // Source: FUN_140FC9B80 (slots 0..5) and FUN_141009EA0 (slots 6..8).
    constexpr std::array<std::uintptr_t, 9> handle_offsets{
        0xe0, 0xf0, 0x100, 0x110, 0x90, 0xa0, 0x60, 0x70, 0x80
    };
    const auto address = provider + handle_offsets[slot];
    if (address < provider ||
        !memory::peek(address, result.world) ||
        !memory::peek(address + 8, result.index) ||
        !memory::peek(address + 12, result.generation)) return result;
    result.present = true;
    if (result.world < 0x10000 || (result.world & 7u) || result.index == 0xffffffffu)
        return result;

    // Both handle types use a world pointer and a generation, but they do NOT
    // share the same generation table or index space. Never interpret a shape
    // index as a body index in a motion array.
    std::uintptr_t descriptor{};
    if (!memory::peek(result.world + 0x28, descriptor) ||
        descriptor < 0x10000 || (descriptor & 7u)) return result;
    result.descriptor = descriptor;
    if (slot < 6) {
        // FUN_1425D7E00: shape validity comes from a live-bit bitmap (+F58)
        // and a u8 generation table (+FF8). The descriptor count at +908
        // belongs to BODIES, not shapes.
        std::uintptr_t shape_live_bits{}, shape_generations{};
        std::uint8_t mask{}, current_generation{};
        if (result.index >= 2000000u ||
            !memory::peek(descriptor + 0xf58, shape_live_bits) ||
            !memory::peek(descriptor + 0xff8, shape_generations) ||
            shape_live_bits < 0x10000 || shape_generations < 0x10000) return result;
        result.descriptor_ok = true;
        if (!memory::peek(shape_live_bits + (result.index >> 3), mask) ||
            !memory::peek(shape_generations + result.index, current_generation)) return result;
        result.generation_ok = ((mask >> (result.index & 7)) & 1u) &&
                               static_cast<std::uint32_t>(current_generation) == result.generation;
        return result; // Do not read body-center positions for shape handles.
    }
    // FUN_1425D7B10: body index maps into descriptor +0x900 and world+0x680.
    std::uint32_t count{};
    if (!memory::peek(descriptor + 0x908, count) ||
        count == 0 || count > 1000000u || result.index >= count) return result;
    result.pool_count = count;
    result.descriptor_ok = true;
    std::uintptr_t generations{};
    std::uint32_t current_generation{};
    if (!memory::peek(result.world + 0x680, generations) || generations < 0x10000 ||
        !memory::peek(generations + static_cast<std::uintptr_t>(result.index) * 4u,
                      current_generation) || current_generation != result.generation) return result;
    result.generation_ok = true;

    // Position is only a secondary geometry check. A part's derived pose need not
    // equal the physics body's center of mass. Never reject a valid handle on gap.
    std::uintptr_t position_base{};
    std::uint16_t current_ring{}, previous_ring{};
    if (!memory::peek(descriptor + 0x900, position_base) ||
        !memory::peek(descriptor + 0x90c, current_ring) ||
        !memory::peek(descriptor + 0x90e, previous_ring) || position_base < 0x10000)
        return result;
    const unsigned ring = current_ring >= previous_ring
        ? static_cast<unsigned>(current_ring - previous_ring)
        : static_cast<unsigned>(current_ring);
    if (ring > 8) return result; // unexpected selector: refuse unsafe addressing
    const std::uint64_t stride = (static_cast<std::uint64_t>(count) * 16u + 0x3cu) & ~0x3full;
    const std::uint64_t byte_offset = ring * stride + static_cast<std::uint64_t>(result.index) * 16u;
    if (byte_offset > 0x80000000ull) return result;
    std::array<float, 4> pose{};
    if (!memory::peek(position_base + static_cast<std::uintptr_t>(byte_offset), pose))
        return result;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(pose[i]) || std::abs(pose[i]) > 1000000.0f)
            return result;
    result.native_position = {pose[0], pose[1], pose[2]};
    result.pose_gap = std::hypot(
        std::hypot(pose[0] - derived_position[0], pose[1] - derived_position[1]),
        pose[2] - derived_position[2]);
    result.position_ok = std::isfinite(result.pose_gap);
    return result;
}

// Extends the existing 20 Hz board probe with native handle provenance.
// ROCK_CONTACT_OBSERVER_V01: observer implementation (never forces motion).
// ROCK_CONTACT_OUTPUT_V02: generated 64-byte output snapshots (passive).
// ROCK_CONTACT_IDENTITY_V03: synchronized native body pose/context snapshots (read-only).
// ROCK_SCENE_PATH_SCOUT_V04: read-only alternative geometry-path counters.
// ROCK_ALT_PAIR_OUTPUT_V05: read-only alternate pair and appended-record capture.
// ROCK_ALT_REJECTION_DIAGNOSTICS_V06: first-failure reasons and bounded raw argument snapshots.
#include "rock_contact_observer.inc"

void sample_rock_board(std::uintptr_t base, std::uintptr_t client, const Vec3& position,
                       std::int64_t stamp, const Telemetry& telemetry) {
    auto& s = state();
    if (!s.rock_board_enabled || !s.rock_board_file) return;
    if (s.rock_board_last && clock_seconds(stamp - s.rock_board_last) < 0.05) return;
    s.rock_board_last = stamp;
    bool readable = false, position_plausible = false;
    float board_height = 0.0f;
    std::array<Vec3, 9> positions{}, velocities{}, spins{};
    std::array<RockNativeBody, 9> native{};
    std::uintptr_t board_owner{}, board_wrapper{}, provider{};
    try {
        const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
        if (!bodies.offboard && !bodies.wipeout) {
            bool all_good = true;
            for (std::size_t i = 0; i < positions.size(); ++i) {
                if (!bodies.parts[i] ||
                    !memory::peek(bodies.parts[i] + 0x50, positions[i]) ||
                    !memory::peek(bodies.parts[i] + 0x70, velocities[i]) ||
                    !memory::peek(bodies.parts[i] + 0x90, spins[i])) {
                    all_good = false;
                    break;
                }
                for (float v : positions[i]) if (!std::isfinite(v) || std::abs(v) > 1.0e6f) all_good = false;
                for (float v : velocities[i]) if (!std::isfinite(v) || std::abs(v) > 1.0e5f) all_good = false;
                for (float v : spins[i]) if (!std::isfinite(v) || std::abs(v) > 1.0e5f) all_good = false;
            }
            if (all_good && std::isfinite(bodies.board_height)) {
                board_height = bodies.board_height;
                readable = true;
                const float dx = positions[0][0] - position[0];
                const float dz = positions[0][2] - position[2];
                const float dy = positions[0][1] - board_height;
                position_plausible = std::abs(dy) < 0.02f && std::hypot(dx, dz) < 15.0f;

                // The record's +0x10 is the BOARD COLLECTION, NOT the
                // higher-level wrapper that owns the handle-provider field.
                // Source: client_noclip.cpp + FUN_141009EA0.
                std::uintptr_t linked_board{}, parts_base{}, candidate_provider{};
                if (memory::peek(bodies.parts[0] + 0x10, board_owner) &&
                    board_owner >= 0x10000 &&
                    memory::peek(bodies.core + 0x430, board_wrapper) &&
                    board_wrapper >= 0x10000 &&
                    memory::peek(board_wrapper + 0x18, linked_board) &&
                    linked_board == board_owner &&
                    memory::peek(board_owner + 0x20, parts_base) &&
                    parts_base == bodies.parts[0] &&
                    memory::peek(board_wrapper + 0x718, candidate_provider) &&
                    candidate_provider >= 0x10000 && !(candidate_provider & 7u)) {
                    provider = candidate_provider;
                }
                for (std::size_t i = 0; i < 9; ++i) {
                    native[i].record_ptr = bodies.parts[i];
                    native[i].handle_kind = i < 6 ? "shape" : "body";
                }
                if (provider >= 0x10000) {
                    for (std::size_t i = 0; i < 9; ++i)
                        native[i] = inspect_rock_native_body(provider, bodies.parts[i], i, positions[i]);
                }
            }
        }
    } catch (...) {
        // Invalidations during loading/respawn produce missing handle fields.
    }
    auto &csv = s.rock_board_file;
    csv << std::format("{:.4f},{:.4f},{:.4f},{:.4f},{:.1f},{:.3f},{:.3f},{},{}",
                       clock_seconds(stamp - s.rock_board_start), position[0], position[1], position[2],
                       telemetry.heading, telemetry.speed, telemetry.vertical,
                       telemetry.physics_state, telemetry.airborne ? 1 : 0);
    csv << ',' << (readable ? 1 : 0) << ',' << (position_plausible ? 1 : 0) << ',';
    if (readable) csv << std::format("{:.4f}", board_height);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            csv << ',';
            if (readable) csv << std::format("{:.4f}", positions[i][j]);
        }
        csv << ',';
        if (readable) csv << std::format("{:.4f}", horizontal(velocities[i]));
        csv << ',';
        if (readable) csv << std::format("{:.4f}", std::hypot(spins[i][0], spins[i][1], spins[i][2]));
    }
    // All provenance fields are appended after the original board fields.
    // Keep QPC as the final column for joinability with the surface probe.
    csv << ','; if (board_owner) csv << std::format("0x{:X}", board_owner);
    csv << ','; if (board_wrapper) csv << std::format("0x{:X}", board_wrapper);
    csv << ','; if (provider) csv << std::format("0x{:X}", provider);
    for (std::size_t i = 0; i < native.size(); ++i) {
        const auto& n = native[i];
        csv << ',' << n.handle_kind;
        csv << ','; if (n.record_ptr) csv << std::format("0x{:X}", n.record_ptr);
        csv << ','; if (n.present) csv << std::format("0x{:X}", n.world);
        csv << ','; if (n.present) csv << n.index;
        csv << ','; if (n.present) csv << n.generation;
        csv << ','; if (n.present) csv << (n.descriptor_ok ? 1 : 0);
        csv << ','; if (n.present) csv << (n.generation_ok ? 1 : 0);
        csv << ','; if (n.descriptor_ok) csv << std::format("0x{:X}", n.descriptor);
        csv << ','; if (n.position_ok) csv << std::format("{:.5f}", n.native_position[0]);
        csv << ','; if (n.position_ok) csv << std::format("{:.5f}", n.native_position[1]);
        csv << ','; if (n.position_ok) csv << std::format("{:.5f}", n.native_position[2]);
        csv << ','; if (n.position_ok) csv << std::format("{:.5f}", n.pose_gap);
    }
    csv << ',' << stamp << '\n';
    ++s.rock_board_rows;
    if (readable && position_plausible) ++s.rock_board_valid;
    if (s.rock_board_rows % 5 == 0) csv.flush();
}

// ROCK_POSITION_SCREEN_V07: does not identify collisions or alter physics.
// Only the three previously observed board reference points are accessed.
void sample_rock_position(std::uintptr_t base, std::uintptr_t client, const Vec3& skater,
                          std::int64_t stamp, const Telemetry& t) {
    auto& s=state();
    if (!s.rock_position_enabled) return;
    if (s.rock_position_last && clock_seconds(stamp-s.rock_position_last)<0.05) return;
    s.rock_position_last=stamp;
    using namespace reskate_custom::rock_position;
    Sample sample{};
    sample.seconds=clock_seconds(stamp);
    sample.skater={skater[0],skater[1],skater[2]};
    sample.heading_deg=t.heading;
    sample.skater_speed=t.speed;
    sample.airborne=t.airborne;
    try {
        const auto bodies=client_source::detail::debug_noclip_bodies(base,client,s.entity);
        if (!bodies.offboard && !bodies.wipeout && bodies.parts[6] && bodies.parts[7] && bodies.parts[8]) {
            Vec3 a{},b{},c{};
            if (memory::peek(bodies.parts[6]+0x50,a) &&
                memory::peek(bodies.parts[7]+0x50,b) &&
                memory::peek(bodies.parts[8]+0x50,c)) {
                sample.axle6={a[0],a[1],a[2]};
                sample.axle7={b[0],b[1],b[2]};
                sample.deck8={c[0],c[1],c[2]};
                sample.valid=std::isfinite(c[0])&&std::isfinite(c[1])&&std::isfinite(c[2])&&
                    std::hypot(c[0]-skater[0],c[2]-skater[2])<15.0f;
            }
        }
    } catch (...) {} // Spawning, menu, or unsupported native body layout.
    const auto result=s.rock_position_detector.ingest(sample);
    if (result.event==Event::rock_position_candidate) {
        say(logging::Level::info,std::format("RockPosition v0.7: geometric candidate at {:.2f}s; center-to-lip {:.3f}m; leading-axle {:.3f}m; height over deck {:.3f}m. COPING CONTACT UNVERIFIED.",
            result.seconds,result.center_to_lip,result.axle_lead_to_lip,result.deck_above_plane));
        overlay::notify(overlay::NoticeLevel::info,"ROCK POSITION?","Board crossed calibrated lip; contact unverified");
    } else if (result.event==Event::fakie_release_candidate) {
        say(logging::Level::info,std::format("RockPosition v0.7: fakie-release SEQUENCE candidate at {:.2f}s (board retreats {:.3f}m from estimated lip). Contact still unverified.",
            result.seconds,-result.center_to_lip));
        overlay::notify(overlay::NoticeLevel::info,"ROCK-TO-FAKIE?","Rock position then fakie return (experimental)");
    }
}

void finish_jump(const Vec3 &landing, float landing_speed, std::int64_t landed_at) {
    auto &s = state();
    auto &m = s.motion;
    const auto air = static_cast<float>(clock_seconds(landed_at - m.takeoff_stamp));
    m.airborne = false;
    s.telemetry.airborne = false;
    if (air < 0.25f) return;
    Jump jump;
    jump.serial = s.telemetry.last.serial + 1;
    jump.air_time = air;
    jump.takeoff = m.takeoff;
    jump.landing = landing;
    jump.takeoff_speed = std::sqrt(m.takeoff_velocity[0] * m.takeoff_velocity[0] + m.takeoff_velocity[1] * m.takeoff_velocity[1] +
                                   m.takeoff_velocity[2] * m.takeoff_velocity[2]);
    jump.takeoff_angle = std::atan2(m.takeoff_velocity[1], std::max(horizontal(m.takeoff_velocity), 0.01f)) * 180.0f / std::numbers::pi_v<float>;
    jump.height = m.apex - m.takeoff[1];
    jump.distance = horizontal({landing[0] - m.takeoff[0], 0, landing[2] - m.takeoff[2]});
    jump.drop = m.takeoff[1] - landing[1];
    jump.landing_speed = landing_speed;
    jump.spin = std::abs(m.spin);
    jump.spin_rate = m.spin_rate;
    jump.flip = m.flip;
    jump.board_turn = m.board_turn;
    jump.state = m.air_state;
    s.telemetry.last = jump;
    if (jump.distance > s.telemetry.best.distance) s.telemetry.best = jump;
    // In the log for a player who is measuring: the read-out is on, or telemetry is recorded.
    if (!s.hud_jump && !s.logging) return;
    say(logging::Level::info,
        std::format("Trainer jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, "
                    "landing {:.1f} km/h at ({:.1f}, {:.1f}, {:.1f}); spin {:.0f} deg (peak {:.0f} deg/s), flip {:.0f} deg, "
                    "board {:.0f} deg/s, states {}>{}.",
                    jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle, jump.air_time, jump.height, jump.distance, jump.drop,
                    jump.landing_speed * 3.6f, landing[0], landing[1], landing[2], jump.spin, jump.spin_rate, jump.flip, jump.board_turn, m.ground_state,
                    m.air_state));
}
void observe(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.telemetry;
    auto &m = s.motion;
    overlay::DebugModel skater;
    std::array<float, 16> matrix{};
    bool found = false;
    if (now >= s.next_skater) {
        try {
            matrix = client_source::detail::debug_skater(base, client, skater);
            found = skater.skater_position_valid;
        } catch (...) {
            s.next_skater = now + 250; // no skater: menus, loading, a respawn
        }
    }
    if (!found) {
        if (t.skater) { t.skater = false; m.valid = false; s.entity = 0; s.rock_detector.reset(); }
        return;
    }
    s.entity = skater.skater_identity;
    watch_physics_state(client, s.entity);
    const auto watch = watched_physics_state();
    const auto stamp = clock_now();
    const Vec3 position = skater.skater_position;
    t.skater = true;
    t.position = position;
    // Rows: right, up, forward, position.
    t.heading = std::atan2(matrix[8], matrix[10]) * 180.0f / std::numbers::pi_v<float>;
    if (t.heading < 0) t.heading += 360.0f;
    t.physics_state = watch.valid ? watch.state : 0;
    if (watch.valid) ++s.states[watch.state];
    // A hippy jump leaves the board (504) straight from riding (100): measured. A plain
    // dismount does the same but is not rising, so it is left alone.
    if (watch.valid && watch.state != s.last_state) {
        if (watch.state == addr::no_bail::offboard_physics_state && s.last_state == 100 && s.boosts.hippy != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.hippy); // height goes with the square of the speed
            s.boost_until = now + 250;
            s.boost_name = "hippy jump";
        }
        s.last_state = watch.state;
    }
    if (s.boost_factor > 0) {
        const auto result = take_jump_scale_result();
        if (result.outcome > 0) {
            say(logging::Level::info, std::format("Trainer: {} boosted: up speed {:.2f} -> {:.2f} m/s.", s.boost_name, result.up_speed,
                                                  result.up_speed * s.boost_factor));
            s.boost_factor = 0;
        } else if (now >= s.boost_until || result.outcome == -2) {
            s.boost_factor = 0;
        } else {
            // The factor comes from the multipliers in force, so a session's rules are already in it.
            (void)queue_jump_scale(client, s.entity, s.boost_factor); // again each tick until the skater is rising
        }
    }
    const auto seconds = m.valid ? clock_seconds(stamp - m.stamp) : 0.0;
    const Vec3 step{position[0] - m.position[0], position[1] - m.position[1], position[2] - m.position[2]};
    const bool jumped = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]) > 25.0f;
    if (!m.valid || seconds <= 0.0005 || seconds > 0.25 || jumped) {
        // First sample, a hitch, or a teleport: nothing can be measured across it.
        m = {};
        s.rock_detector.reset();
        m.valid = true;
        m.position = position;
        m.stamp = stamp;
        t.airborne = false;
        t.air_time = t.height = 0;
        return;
    }
    const auto dt = static_cast<float>(seconds);
    const Vec3 raw{step[0] / dt, step[1] / dt, step[2] / dt};
    const Vec3 velocity{m.velocity[0] * 0.5f + raw[0] * 0.5f, m.velocity[1] * 0.5f + raw[1] * 0.5f, m.velocity[2] * 0.5f + raw[2] * 0.5f};
    // An on-foot jump: off the board, level at the last tick and now rising faster than stairs
    // or a slope can lift a runner.
    if (watch.valid && watch.state == addr::no_bail::offboard_physics_state) {
        if (s.offboard_grounded && raw[1] > 2.2f && s.boost_factor == 0 && s.boosts.offboard != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.offboard);
            s.boost_until = now + 250;
            s.boost_name = "off-board jump";
        }
        s.offboard_grounded = std::abs(raw[1]) < 0.6f;
    } else {
        s.offboard_grounded = false;
    }
    // The skater's physics says when it is in the air: its states 200-299 are the pop and
    // the flight (100s ride the ground, 300 is a wipeout, 500s are off the board).
    const bool flying = watch.valid && watch.state >= air_states_first && watch.state < air_states_end;
    // Rotation in the air: about the vertical (spins) and of the up axis (flips and rolls).
    const Vec3 forward{matrix[8], matrix[9], matrix[10]}, up{matrix[4], matrix[5], matrix[6]};
    if (m.airborne && m.axes_valid) {
        const float yaw = std::atan2(m.forward[2] * forward[0] - m.forward[0] * forward[2], m.forward[0] * forward[0] + m.forward[2] * forward[2]);
        const float degrees = yaw * 180.0f / std::numbers::pi_v<float>;
        // A ragdoll or a pose snap turns further in one tick than any trick does: leave those out.
        if (std::abs(degrees) < 45.0f) {
            m.spin += degrees;
            m.spin_rate = std::max(m.spin_rate, std::abs(degrees) / dt);
        }
        const float cosine = std::clamp(m.up[0] * up[0] + m.up[1] * up[1] + m.up[2] * up[2], -1.0f, 1.0f);
        if (const float tumble = std::acos(cosine) * 180.0f / std::numbers::pi_v<float>; tumble < 45.0f) m.flip += tumble;
    }
    m.forward = forward;
    m.up = up;
    m.axes_valid = true;
    if (!flying && watch.valid) m.ground_state = watch.state;
    if (flying && !m.airborne) {
        m.spin = m.spin_rate = m.flip = m.board_turn = 0;
        m.air_state = watch.state;
        m.airborne = true;
        m.takeoff = m.position;
        m.takeoff_velocity = velocity;
        m.takeoff_stamp = m.stamp;
        m.apex = std::max(m.position[1], position[1]);
    }
    if (m.airborne) {
        m.apex = std::max(m.apex, position[1]);
        t.air_time = static_cast<float>(clock_seconds(stamp - m.takeoff_stamp));
        t.height = m.apex - m.takeoff[1];
        if (!flying)
            finish_jump(position, std::sqrt(m.velocity[0] * m.velocity[0] + m.velocity[1] * m.velocity[1] + m.velocity[2] * m.velocity[2]),
                        stamp);
    }
    t.airborne = m.airborne;
    t.speed = horizontal(velocity);
    t.vertical = velocity[1];
    t.top_speed = std::max(t.top_speed, t.speed);
    m.velocity = velocity;
    m.position = position;
    m.stamp = stamp;
    // How fast the board turns (flip tricks, shuvits): body 0's angular velocity (+0x90), read while a jump
    // is measured or telemetry is recorded.
    float board_turn = 0;
    bool turning = m.airborne || (s.logging && s.log);
    if (turning) {
        try {
            const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
            std::array<float, 3> spin{};
            if (!bodies.parts.empty() && memory::peek(bodies.parts[0] + 0x90, spin)) {
                board_turn = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]) * 180.0f / std::numbers::pi_v<float>;
                if (!std::isfinite(board_turn) || board_turn > 1.0e5f) board_turn = 0;
            }
        } catch (...) {}
        if (m.airborne) m.board_turn = std::max(m.board_turn, board_turn);
    }
    if (s.logging && s.log) {
        s.log << std::format("{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.1f},{},{},{:.0f}\n", clock_seconds(stamp - s.log_start), position[0],
                             position[1], position[2], t.speed, t.vertical, t.heading, t.airborne ? 1 : 0, t.physics_state, board_turn);
    }
    // Rock to Fakie experiment: passive telemetry only; NEVER changes game physics.
    // Runs on the same game thread that owns Trainer state.
    if (s.rock_detection_enabled && watch.valid) {
        const reskate_custom::rock_to_fakie::Sample sample{
            clock_seconds(stamp), position[0], position[1], position[2],
            t.speed, t.vertical, t.heading, t.airborne,
            static_cast<int>(t.physics_state), board_turn
        };
        if (const auto hit = s.rock_detector.ingest(sample)) {
            say(logging::Level::info,
                std::format("RockToFakie: candidate at {:.2f}s, return {:.2f}m, facing {:.1f} deg. Coping NOT verified.",
                            hit->crest_seconds, hit->reverse_distance_m, hit->approach_heading_deg));
            overlay::notify(overlay::NoticeLevel::info, "ROCK TO FAKIE", "Fakie return candidate (coping unverified)");
        }
    }
    sample_rock_surface(matrix, position, stamp, t);
    sample_rock_board(base, client, position, stamp, t);
    sample_rock_position(base, client, position, stamp, t);
    // A bail: back to the selected marker once the skater has had time to fall.
    if (watch.valid && (!s.wipeouts_known || watch.wipeouts != s.seen_wipeouts)) {
        if (s.wipeouts_known && s.auto_return) {
            const bool marked = current_map().markers[static_cast<std::size_t>(s.slot)].set;
            if (marked) s.return_at = now + static_cast<std::uint64_t>(s.return_delay * 1000.0f);
            say(logging::Level::info, marked ? std::format("Trainer: bail seen; back to marker {} in {:.1f} s.", s.slot + 1, s.return_delay)
                                             : std::format("Trainer: bail seen, but marker {} is empty.", s.slot + 1));
        }
        s.wipeouts_known = true;
        s.seen_wipeouts = watch.wipeouts;
    }
}
void set_logging(bool on) {
    auto &s = state();
    if (s.log.is_open()) s.log.close();
    s.logging = false;
    if (!on) return;
    const auto directory = data_directory() / L"telemetry";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::string name = s.map.empty() ? "unknown" : s.map.substr(s.map.find_last_of('/') + 1);
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    SYSTEMTIME time;
    GetLocalTime(&time);
    const auto file = directory / std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", name, time.wYear, time.wMonth, time.wDay, time.wHour,
                                              time.wMinute, time.wSecond);
    s.log.open(file, std::ios::binary | std::ios::trunc);
    if (!s.log) return;
    s.log << "seconds,x,y,z,speed_mps,vertical_mps,heading_deg,airborne,physics_state,board_turn_dps\n";
    s.log_start = clock_now();
    s.logging = true;
}
void shortcuts() {
    auto &s = state();
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input);
    const auto buttons = input.available ? input.buttons : 0;
    const auto pressed = buttons & ~s.pad_previous;
    s.pad_previous = buttons;
    if (!s.pad_shortcuts || (buttons & (xinput_lb | xinput_rb)) != (xinput_lb | xinput_rb)) return;
    std::string result;
    if (pressed & xinput_up) result = command("marker", {"save"});
    else if (pressed & xinput_down) result = command("marker", {"go"});
    else if (pressed & xinput_left) result = command("slot", {std::to_string((s.slot + static_cast<int>(marker_slots) - 1) % static_cast<int>(marker_slots) + 1)});
    else if (pressed & xinput_right) result = command("slot", {std::to_string((s.slot + 1) % static_cast<int>(marker_slots) + 1)});
    if (!result.empty()) overlay::notify(overlay::NoticeLevel::info, "TRAINER", result);
}

// ---- self test -------------------------------------------------------------------------
void test_line(bool ok, const std::string &text) {
    auto &s = state();
    if (!ok) ++s.test.failures;
    say(ok ? logging::Level::info : logging::Level::error, std::format("trainer selftest: {} {}", ok ? "OK" : "FAIL", text));
}
void run_selftest(std::uint64_t now) {
    auto &s = state();
    auto &t = s.test;
    if (!t.step || now < t.until) return;
    tuning::Values live;
    switch (t.step) {
    case 1: {
        test_line(s.model != nullptr, std::format("game tuning read ({} values, {} curves)", s.model ? s.model->fields.size() : 0,
                                                  s.model ? s.model->curve_slots.size() : 0));
        std::size_t named{};
        if (s.model) named = static_cast<std::size_t>(std::ranges::count_if(s.model->field_names, [](const std::string &n) { return !n.empty(); }));
        test_line(s.model && named == s.model->fields.size(), std::format("field names ({} of {})", named, s.model ? s.model->fields.size() : 0));
        test_line(s.ready && s.asset, "running tuning asset found");
        test_line(s.telemetry.skater, "local skater found");
        // The first plain real that is not zero and that the player has not touched.
        t.entry = s.entries.size();
        for (std::size_t i = 0; i < s.entries.size(); ++i)
            if (s.entries[i].kind == Kind::real && !s.entries[i].detail && !s.entries[i].touched && std::abs(s.entries[i].stock) > 0.01) {
                t.entry = i;
                break;
            }
        if (!s.ready || t.entry == s.entries.size() || !editable()) {
            test_line(false, "no value to write (tuning not ready or locked by a session)");
            t.step = 4;
            break;
        }
        auto &e = s.entries[t.entry];
        t.before = e.stock;
        t.target = e.stock * 1.01;
        set_entry(e, t.target);
        t.step = 2;
        t.until = now + 1200;
        break;
    }
    case 2: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && std::abs(read_field(live.image, e) - static_cast<float>(t.target)) < 1e-4 * std::max(1.0, std::abs(t.target)),
                  std::format("write and hold {} = {:.5g}", e.id, t.target));
        set_entry(e, t.before);
        t.step = 3;
        t.until = now + 1200;
        break;
    }
    case 3: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && read_field(live.image, e) == static_cast<float>(t.before) && !e.touched, std::format("restore {} = {:.5g}", e.id, t.before));
        t.step = 4;
        break;
    }
    case 4: {
        if (!s.telemetry.skater) {
            test_line(false, "teleport (no skater)");
            t.step = 6;
            break;
        }
        t.origin = s.telemetry.position;
        const Vec3 target{t.origin[0] + 3.0f, t.origin[1] + 1.0f, t.origin[2]};
        const auto result = go_to(target, "a test point");
        test_line(!result.starts_with("error"), "teleport request: " + result);
        t.origin = target;
        t.step = result.starts_with("error") ? 6 : 5;
        t.until = now + 700;
        break;
    }
    case 5: {
        const auto &p = s.telemetry.position;
        // The skater may already be rolling away from where it was put.
        const float away = horizontal({p[0] - t.origin[0], 0, p[2] - t.origin[2]});
        test_line(s.telemetry.skater && away < 25.0f, std::format("teleport put the skater {:.2f} m from the target", away));
        t.step = 6;
        t.until = now + 1500;
        break;
    }
    default:
        say(t.failures ? logging::Level::error : logging::Level::info,
            std::format("trainer selftest: DONE {} failures", t.failures));
        t = {};
        changed(false);
        break;
    }
}

void build_view() {
    auto &s = state();
    s.view_due = false;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->ready = s.ready;
    next->status = s.status;
    next->last = s.last;
    next->editable = editable(&next->blocked);
    next->rows.reserve(s.entries.size());
    for (const auto &e : s.entries) {
        int rank{};
        std::uint8_t modes{};
        const auto friendly = essential_name(e.key, &rank, &modes);
        next->rows.push_back({e.id, e.label, e.group, e.kind, e.value, e.stock, e.touched, e.frozen, e.detail, std::string(friendly), rank, modes,
                              e.used});
        if (e.touched) ++next->touched;
        if (std::ranges::find(next->groups, e.group) == next->groups.end()) next->groups.push_back(e.group);
    }
    const auto is_active = [&](std::string_view name) { return std::ranges::find(s.active, name) != s.active.end(); };
    next->presets.push_back({"Stock", "Everything back to how this map loaded (frozen values stay).", true, s.active.empty() && !next->touched});
    {
        // A built-in preset is on when the values say so, whatever switched them: a slider, a
        // dial or another preset.
        const auto &presets = builtin_presets();
        const auto &targets = preset_targets();
        for (std::size_t p = 0; p < presets.size(); ++p) {
            const auto &preset = presets[p];
            PresetRow row{std::string(preset.name), std::string(preset.note), true, false};
            bool any = false, on = true;
            const Entry *headline{};
            for (const auto &[index, r] : targets[p]) {
                const auto &e = s.entries[index];
                const auto &rule = preset.rules[r];
                if (!headline && r == 0) headline = &e;
                const double expected = rule.multiply ? e.stock * rule.amount : rule.amount;
                any = true;
                // To the two decimals a dial shows: dragging one to the preset's number switches it on.
                if (std::abs(e.value - sane(e, expected)) > 4e-3 * std::max(1e-3, std::abs(expected))) on = false;
            }
            row.active = any && on;
            const auto dial = preset_dial(preset.name);
            row.title = std::string(dial.title);
            row.modes = dial.modes;
            if (!preset.rules.empty() && preset.rules[0].multiply && headline && headline->stock != 0 && !dial.title.empty()) {
                row.dial = true;
                row.amount = preset.rules[0].amount;
                row.factor = headline->value / headline->stock;
            }
            next->presets.push_back(std::move(row));
        }
    }
    for (const auto &[name, values] : s.user) next->presets.push_back({name, std::format("{} values", values.size()), false, is_active(name)});
    next->slot = s.slot;
    next->auto_return = s.auto_return;
    next->hippy_height = s.hippy_height;
    next->nocomply_height = s.nocomply_height;
    next->offboard_height = s.offboard_height;
    next->flip_speed = s.flip_speed;
    next->boneless_height = s.boneless_height;
    next->return_delay = s.return_delay;
    next->pad_shortcuts = s.pad_shortcuts;
    next->hud = s.hud;
    next->hud_jump = s.hud_jump;
    next->logging = s.logging;
    next->map = s.map;
    next->map_note = s.map_file.note;
    next->map_preset = s.map_file.preset.empty() ? std::string{} : s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    next->spots = s.map_file.spots;
    next->waypoint_set = s.waypoint.has_value();
    if (s.waypoint) next->waypoint = *s.waypoint;
    next->open_serial = s.open_serial;
    next->open_tab = s.open_tab;
    if (const auto found = s.maps.find(s.map); found != s.maps.end()) {
        next->markers = found->second.markers;
        next->profile_preset = found->second.preset;
    }
    publish(std::move(next));
}
bool flag(const std::string &text, bool &out) {
    const auto value = lower(text);
    if (value == "1" || value == "on" || value == "true") { out = true; return true; }
    if (value == "0" || value == "off" || value == "false") { out = false; return true; }
    return false;
}
std::optional<int> slot_argument(const std::vector<std::string> &arguments, std::size_t index) {
    auto &s = state();
    if (arguments.size() <= index) return s.slot;
    const auto value = number(arguments[index]);
    if (!value || *value < 1 || *value > static_cast<double>(marker_slots) || *value != std::floor(*value)) return std::nullopt;
    return static_cast<int>(*value) - 1;
}
// Buffered CSV recording on Trainer's game thread only. No input or game writes.
void sample_board_stall_probe(std::uint64_t now_ms) {
    auto& s = state();
    if (!s.stall_probe_enabled) return;
    // Bounded recording, even if the user forgets to stop it manually.
    if (now_ms - s.stall_probe_started_ms >= 45000u) {
        stall_probe_input::stop();
        s.stall_probe_enabled = false;
        if (s.stall_probe_file.is_open()) s.stall_probe_file.close();
        say(logging::Level::info, std::format(
            "Board Stall v1.2: auto-saved {} rows, {} direct B edges, {} XInput edges to {}",
            s.stall_probe_rows, s.stall_direct_input.presses,
            stall_probe_input::b_press_count.load(), path_utf8(s.stall_probe_path)));
        return;
    }
    const auto stamp = clock_now();
    if (s.stall_probe_last && clock_seconds(stamp - s.stall_probe_last) < 0.025) return;
    s.stall_probe_last = stamp;
    // Uses the already-supported XInput / PlayStation controller reader, which
    // obtains controller state independently of our game-facing XInput hooks.
    // When the ReSkate console/menu is open this reader returns neutral input.
    ControllerInput direct{};
    DingoSDKOverlayReadControllerInput(&direct);
    const bool direct_edge = s.stall_direct_input.sample(
        direct.available, direct.device, direct.buttons, now_ms);
    const auto& t = s.telemetry;
    const auto count = stall_probe_input::b_press_count.load(std::memory_order_acquire);
    const auto events = count >= s.stall_probe_last_press_count ? count - s.stall_probe_last_press_count : 0;
    s.stall_probe_last_press_count = count;
    auto& csv = s.stall_probe_file;
    csv << std::format(
        "{:.4f},{},{},{},{:.4f},{:.4f},{:.4f},{:.2f},{:.3f},{:.3f},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
        (now_ms - s.stall_probe_started_ms) / 1000.0,
        stamp, t.skater ? 1 : 0, t.physics_state,
        t.position[0], t.position[1], t.position[2],
        t.heading, t.speed, t.vertical, t.airborne ? 1 : 0,
        count, events, stall_probe_input::last_b_tick_ms.load(std::memory_order_relaxed),
        stall_probe_input::last_b_user.load(std::memory_order_relaxed),
        stall_probe_input::polls.load(std::memory_order_relaxed),
        stall_probe_input::users_seen_mask.load(std::memory_order_relaxed),
        stall_probe_input::held(0) ? 1 : 0,
        stall_probe_input::held(1) ? 1 : 0,
        s.stall_direct_input.available ? 1 : 0,
        s.stall_direct_input.device,
        s.stall_direct_input.buttons,
        s.stall_direct_input.b_down() ? 1 : 0,
        direct_edge ? 1 : 0,
        s.stall_direct_input.presses,
        s.stall_direct_input.valid_reads,
        s.stall_direct_input.last_press_ms);
    if ((++s.stall_probe_rows % 120u) == 0) csv.flush();
}

// These records are created in ReSkate's EXISTING local selector hook (only when
// native_b_recording is on), not by a debugger breakpoint or extra detour.
std::string finish_native_b_capture() {
    auto& s = state();
    s.native_b_recording = false;
    const auto stats = native_b_trace_stats();
    auto events = native_b_trace_stop();
    const auto dropped = stats.dropped;
    if (s.native_b_path.empty()) return "Native B capture stopped; no output path.";
    std::ofstream csv(s.native_b_path, std::ios::binary | std::ios::trunc);
    if (!csv) return "error: unable to write native B CSV";
    csv << "tick_ms,kind,selector,context,target,raw,selected,previous,buttons,device,"
           "c4,cc,d0,dc,fa0,fb0,at1418,type,source19d,p48,p49,valid_mask,"
           "input_manager,input_vtable,input_callback,input_userdata,input_flags,"
           "trick_recorder,trick_packet,trick_id,trick_valid,trick_effective_id,trick_remapped,"
           "trick_remap_phase,trick_callsite,producer,producer_valid_mask,"
           "source_a50_id,source_ac0_id,source_a58_id,source_b90_id,source_b28_id,"
           "producer_ca0,producer_a54,producer_b20,localized_hash,localized_caller,localized_text,"
           "localized_trial_status,localized_buffer_capacity,"
           "localized_stack_count,localized_frame_00,localized_frame_01,localized_frame_02,"
           "localized_frame_03,localized_frame_04,localized_frame_05,localized_frame_06,"
           "localized_frame_07,localized_frame_08,localized_frame_09,localized_frame_10,"
           "localized_frame_11\n";
    for (const auto& e : events) {
        csv << std::format("{},{},{:x},{:x},{:x},{},{},{},{:x},{},{:08x},{:08x},{:08x},{:08x},"
                           "{:08x},{:08x},{:08x},{:x},{:02x},{:02x},{:02x},{},"
                           "{:x},{:x},{:x},{:x},{:08x},"
                           "{:x},{:x},{},{},{},{},{},{:x}",
            e.tick_ms, e.kind, e.selector, e.context, e.target,
            e.raw, e.chosen, e.previous, e.buttons, e.device,
            e.c4, e.cc, e.d0, e.dc, e.fa0, e.fb0, e.at1418,
            e.type, e.source19d, e.p48, e.p49, e.valid_mask,
            e.input_manager, e.input_vtable, e.input_callback,
            e.input_userdata, e.input_flags,
            e.trick_recorder, e.trick_packet, e.trick_id, e.trick_valid,
            e.trick_effective_id, e.trick_remapped, e.trick_remap_phase,
            e.trick_callsite);
        csv << ',' << std::format("{:x}", e.producer)
            << ',' << e.producer_valid_mask
            << ',' << e.source_a50_id << ',' << e.source_ac0_id
            << ',' << e.source_a58_id << ',' << e.source_b90_id
            << ',' << e.source_b28_id
            << ',' << e.producer_ca0 << ',' << e.producer_a54
            << ',' << e.producer_b20
            << ',' << std::format("{:08x}", e.localized_hash)
            << ',' << std::format("{:x}", e.localized_caller)
            << ',' << e.localized_text
            << ',' << e.localized_trial_status << ',' << e.localized_buffer_capacity
            << ',' << e.localized_stack_count;
        for (const auto frame_address : e.localized_stack)
            csv << ',' << std::format("{:x}", frame_address);
        csv << '\n';
    }
    csv.close();
    return std::format("Native B capture saved {} events, {} B presses, {} dropped. {}",
        events.size(), s.native_b_edges, dropped, path_utf8(s.native_b_path));
}
void sample_native_b_capture(std::uint64_t now, bool playing) {
    auto& s = state();
    // Trace recording is bounded to 45s, but session-long HUD replacement
    // needs physical B rising edges for the verified 9/12 -> 187 exit packet.
    // Continue this controller observer WITHOUT keeping a CSV capture running.
    if (s.native_b_recording && (!playing || now - s.native_b_started > 45000))
        say(logging::Level::info, finish_native_b_capture());
    if (!playing || (!s.native_b_recording && !native_rock_hud_enabled())) return;
    ControllerInput direct{};
    DingoSDKOverlayReadControllerInput(&direct);
    if (s.native_b_controller.sample(direct.available, direct.device, direct.buttons, now)) {
        if (s.native_b_recording) ++s.native_b_edges;
        native_b_trace_controller_edge(s.telemetry.physics_state, direct.buttons, direct.device);
    }
}

// No game-memory writes. Windows keyboard synthesis is experimental: some games
// use input paths that intentionally ignore SendInput. Never claim controller B injection.
void stall_auto_release_key() noexcept {
    auto& s = state();
    if (!s.stall_key_held) return;
    INPUT up{};
    up.type = INPUT_KEYBOARD;
    up.ki.wVk = static_cast<WORD>(s.stall_auto_key);
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &up, sizeof(INPUT)) != 1) ++s.stall_auto_failed;
    s.stall_key_held = false;
}
void update_stall_auto(std::uint64_t now, bool playing) {
    auto& s = state();
    if (s.stall_key_held && (now >= s.stall_auto_up_at || !playing || !s.stall_auto_enabled))
        stall_auto_release_key();
    if (!s.stall_auto_enabled) { s.stall_auto_gate.reset(); return; }
    // ReSkate's direct reader returns unavailable when the console/menu is active
    // or the game isn't foreground. This gates both detection and injection.
    ControllerInput pad{};
    if (playing) DingoSDKOverlayReadControllerInput(&pad);
    const auto& t = s.telemetry;
    const bool valid = playing && t.skater && pad.available && pad.device != 0;
    const bool physical_b = valid && ((pad.buttons & 0x2000u) != 0);
    const bool fire = s.stall_auto_gate.sample(now, valid, t.physics_state, t.speed, t.airborne, physical_b);
    if (!fire || !s.stall_auto_live) return;
    if (!s.stall_auto_key || s.stall_key_held || (GetAsyncKeyState(static_cast<int>(s.stall_auto_key)) & 0x8000)) {
        ++s.stall_auto_failed;
        return;
    }
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = static_cast<WORD>(s.stall_auto_key);
    if (SendInput(1, &down, sizeof(INPUT)) == 1) {
        s.stall_key_held = true;
        s.stall_auto_up_at = now + 85;
        ++s.stall_auto_sent;
    } else ++s.stall_auto_failed;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept {
    try {
        auto &s = state();
        const auto now = GetTickCount64();
        if (!s.loaded) load_store();
        s.base = base;
        sync_session();
        s.boosts = boosts_in_force();
        if (const auto key = lower(playing ? level : std::string{}); key != s.map) enter_map(key);
        if (const auto key = lower(level); !key.empty() && key != s.waypoint_level) {
            s.waypoint_level = key;
            s.waypoint.reset();
            s.view_due = true;
        }
        if (take_teleport_card_shown()) s.waypoint_wanted_until = now + 1000;
        if (now < s.waypoint_wanted_until && now >= s.waypoint_next_read) {
            s.waypoint_next_read = now + 250;
            refresh_waypoint();
        }
        if (playing) {
            observe(base, client, now);
            shortcuts();
            update_landing(now);
            if (s.return_at && now >= s.return_at) {
                s.return_at = 0;
                const auto &marker = current_map().markers[static_cast<std::size_t>(s.slot)];
                if (marker.set) (void)go_to(marker.position, "your marker");
            }
        } else if (s.telemetry.skater) {
            s.telemetry.skater = false;
            s.motion.valid = false;
            s.entity = 0;
        }
        update_stall_auto(now, playing);
        sample_board_stall_probe(now);
        sample_native_b_capture(now, playing);
        rock_contact_observer::refresh_targets(base, client, s.entity, playing);
        (void)start_trick_heights(base);
        if (take_class_list_shown()) s.class_list_wanted = true;
        // The search reads all of the game's writable memory, so it runs only for a player with
        // a use for it: one of the classes' values or the flip speed is not the game's own
        // (theirs, or a host's they skate with), or they have the list of every value open.
        if (s.class_search_at && now >= s.class_search_at && playing && s.telemetry.skater && !finding_classes() &&
            (s.class_list_wanted || classes_wanted())) {
            // A level brings fresh copies, so only a search finished since it loaded counts. The
            // skater's own classes arrive a little after the skater does: a search that came up
            // empty is repeated, twice.
            const bool have = class_searches() > s.class_search_seen && class_copies(0) != 0;
            if (have || s.class_search_tries >= 3) {
                s.class_search_at = 0;
            } else {
                ++s.class_search_tries;
                s.class_search_at = now + 15000;
                find_classes();
            }
        }
        want_flip_speed(s.boosts.flip);
        (void)apply_classes();
        set_trick_heights(s.boosts.nocomply, s.boosts.boneless);
        // The speeds pushes aim for are the push class's (value_links ties them to the tuning's top
        // pushing speed, which only gates whether a push may start).
        const auto *top_speed = find_entry("physicspush.maxpushablespeed");
        set_push_speed(client, s.entity, 1.0f, top_speed ? static_cast<float>(top_speed->stock) : 0.0f, s.boosts.cruise);
        for (TrickLaunch launch; take_trick_launch(launch);)
            if (launch.factor != 1.0f)
                say(logging::Level::info, std::format("Trainer trick: {} launched at {:.2f} m/s up, x{:.2f}.",
                                                      launch.trick == Trick::boneless ? "boneless" : "no comply", launch.up_speed, launch.factor));
        hold(now);
        if (s.extras_due) publish_extras();
        run_selftest(now);
        publish(s.telemetry);
        if (s.view_due) build_view();
        if (s.save_due && now >= s.next_save) save_store();
    } catch (...) {}
}

namespace {
std::string run(std::string_view verb, const std::vector<std::string> &a) {
    auto &s = state();
    const auto v = lower(verb);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    if (v == "rock") {
        if (lower(arg(0)) == "trick") {
            const auto action = lower(arg(1));
            const auto st = native_b_trace_stats();
            if (action.empty() || action == "status")
                return std::format("Rock to Fakie native HUD {} (persistent until explicit OFF or game exit); "
                    "45s CSV recording {}, capture trick mode {}, {} packet remaps, {} label substitutions; "
                    "trick hook {}, virtual localization hook {}. "
                    "HUD scope: watched local skater state 400/401 + exact Board/Lip Stall text + verified native stack. "
                    "IDs 9/12 -> 187 on verified physical B exit. No keyboard/controller injection.",
                    native_rock_hud_enabled() ? "ON" : "OFF",
                    st.running ? "ON" : "OFF", st.trick_mode, st.trick_remaps,
                    st.localized_trial_applied,
                    st.trick_hook_ready ? "READY" : "UNAVAILABLE",
                    st.localization_key_hook_ready ? "READY" : "UNAVAILABLE");
            // v1.4.42: native ticker mode is a long-lived feature, not a trace.
            // No need to start 'trainer rock native 1'; recording may still be
            // started/stopped separately for debugging, with a 45s CSV limit.
            if (action == "hud" || action == "hudon" || action == "hudoff") {
                const auto value = lower(arg(2));
                if (action == "hud" && (value == "status"))
                    return native_rock_hud_enabled() ? "Persistent native HUD ON." : "Persistent native HUD OFF.";
                const bool enabling = action != "hudoff" && value != "off" && value != "0";
                if (!native_rock_hud_set(enabling))
                    return "error: native trick or virtual localization hook unavailable";
                if (enabling && st.running) (void)native_b_trick_mode_set(5);
                if (!enabling && st.trick_mode == 5) (void)native_b_trick_mode_set(0);
                return enabling ? "Native Rock to Fakie HUD ON for the game session (no 45-second limit). "
                    "Only watched Board/Lip Stall; press physical B to exit. 'trainer rock trick off' disables." :
                    "Persistent Rock to Fakie native HUD OFF.";
            }
            std::uint32_t mode{};
            if (action == "off" || action == "0") mode = 0;
            else if (action == "fakie") mode = 1;
            else if (action == "fs") mode = 2;
            else if (action == "bs") mode = 3;
            else if (action == "name") mode = 4;
            else return "error: trainer rock trick off|fakie|fs|bs|name|hud [off]|status";
            if (!native_b_trick_mode_set(mode))
                return "error: start trainer rock native 1 first for capture-only modes; trick hook must be READY";
            // Only successful commands change the independent persistent HUD.
            (void)native_rock_hud_set(false);
            return mode == 0 ? "Native trick remapping OFF." :
                std::format("EXPERIMENTAL native trick packet classification ON (mode {}). "
                    "Permitted offline/private tests only. Physically press B to exit Board Stall. "
                    "IDs 9 and 12 may be remapped (mode 4 on verified B exits). "
                    "Mode 4 tests the native name resolver and ReSkate overlay. "
                    "For a session-long stock ticker replacement use 'trainer rock trick hud' "
                    "without starting recording. "
                    "'trainer rock trick off' disables it.", mode);
        }
        if (lower(arg(0)) == "auto") {
            const auto action = lower(arg(1));
            if (action.empty() || action == "status")
                return std::format("Rock v1.3 auto: {} mode {} key {} delay {} ms; candidates {}, decisions {}, sent {}, failed {}, manual suppressions {}. "
                    "Uses heuristic states 603->400/401. 'preview' is safe; 'on' requests keyboard SendInput, not native Xbox B.",
                    s.stall_auto_enabled ? "ON" : "OFF", s.stall_auto_live ? "KEYBOARD" : "PREVIEW",
                    s.stall_auto_key ? std::string(1, static_cast<char>(s.stall_auto_key)) : "unconfigured",
                    s.stall_auto_gate.delay_ms, s.stall_auto_gate.candidates, s.stall_auto_gate.decisions,
                    s.stall_auto_sent, s.stall_auto_failed, s.stall_auto_gate.suppressed);
            if (action == "off" || action == "0") {
                s.stall_auto_enabled = s.stall_auto_live = false;
                stall_auto_release_key();
                s.stall_auto_gate.reset();
                return "Rock auto OFF. Keyboard released. Native Board Stall remains unchanged.";
            }
            if (action == "preview") {
                s.stall_auto_enabled = true;
                s.stall_auto_live = false;
                stall_auto_release_key();
                s.stall_auto_gate.reset();
                return "Rock auto PREVIEW. Tracks likely Board Stall exits but sends NO input.";
            }
            if (action == "key") {
                const auto key = arg(2);
                if (key.size() != 1) return "error: trainer rock auto key <A-Z or 0-9>; map that keyboard key to the stall-exit action first.";
                const auto k = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(key[0])));
                if (!((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')))
                    return "error: only single-letter or digit keyboard keys are supported";
                if (s.stall_key_held) stall_auto_release_key();
                s.stall_auto_key = k;
                return std::format("Experimental rock exit keyboard key set to {}. This must already be mapped in Skate to the action performed by Xbox B.", static_cast<char>(k));
            }
            if (action == "delay") {
                const auto seconds = number(arg(2));
                if (!seconds || *seconds < 0.08 || *seconds > 0.80)
                    return "error: trainer rock auto delay <0.08 to 0.80 seconds>";
                s.stall_auto_gate.delay_ms = static_cast<unsigned>(*seconds * 1000.0 + 0.5);
                return std::format("Rock auto release delay set to {} ms after 603->400/401.", s.stall_auto_gate.delay_ms);
            }
            if (action == "on" || action == "1") {
                if (!s.stall_auto_key)
                    return "error: no keyboard key set. First test a keyboard key that manually exits Board Stall, then use trainer rock auto key <letter>.";
                s.stall_auto_enabled = s.stall_auto_live = true;
                s.stall_auto_gate.reset();
                return "Experimental rock AUTO KEYBOARD ON. Only after state 603->400/401. Does not modify native game input, tricks or naming. trainer rock auto off disables it.";
            }
            return "error: trainer rock auto status|preview|off|key <A-Z or 0-9>|delay <seconds>|on";
        }

        if (lower(arg(0)) == "native") {
            const auto action = lower(arg(1));
            if (action.empty() || action == "status") {
                const auto st = native_b_trace_stats();
                return std::format("Native trick+B trace {}: {} events, {} dropped; direct B presses {}; file {}. "
                    "Trick packet observer {}. Remap mode {}, trial submissions {}. No WinDbg.",
                    st.running ? "ON" : "OFF", st.count, st.dropped,
                    s.native_b_edges, s.native_b_path.empty() ? "none" : path_utf8(s.native_b_path),
                    st.trick_hook_ready ? "READY" : "UNAVAILABLE",
                    st.trick_mode, st.trick_remaps);
            }
            bool on{};
            if (!flag(action, on)) return "error: trainer rock native 1|0|status";
            if (!on) {
                if (!s.native_b_recording) return "Native B trace is already OFF.";
                return finish_native_b_capture();
            }
            if (s.native_b_recording) return "Native B trace is already ON.";
            // Require the already-installed native selector hook; never attach a new hook.
            if (!no_bail_available()) return "error: native selector hook unavailable on this build";
            const auto dir = data_directory() / L"telemetry";
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            if (error) return "error: cannot create native B telemetry folder";
            SYSTEMTIME tm{};
            GetLocalTime(&tm);
            s.native_b_path = dir / std::format("native-b-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.csv",
                tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond, tm.wMilliseconds);
            s.native_b_controller = {};
            s.native_b_edges = 0;
            if (!native_b_trace_start()) return "error: native trick recorder unavailable (hook/build mismatch)";
            s.native_b_started = GetTickCount64();
            s.native_b_recording = true;
            return "Native trick+B CSV trace ON, max 45 seconds / 1024 events (HUD has no limit). "
                   "Use 'trainer rock trick hud' for session-long native Rock to Fakie ticker, "
                   "or capture-only modes fakie|fs|bs. Press physical B to exit. "
                   "Stop CSV with 'trainer rock native 0'.";
        }
        if (lower(arg(0)) == "stall") {
            const auto action = lower(arg(1));
            if (action.empty() || action == "status")
                return std::format("Board Stall v1.2 recording {}, rows {}, direct B edges {}, XInput B edges {}, direct reads {}, game XInput polls {}, file {}."
                    " Record with trainer rock stall 1; stop/export with trainer rock stall 0.",
                    s.stall_probe_enabled ? "ON" : "OFF", s.stall_probe_rows,
                    s.stall_direct_input.presses, stall_probe_input::b_press_count.load(),
                    s.stall_direct_input.valid_reads, stall_probe_input::polls.load(),
                    s.stall_probe_path.empty() ? "none" : path_utf8(s.stall_probe_path));
            bool enabled{};
            if (!flag(action, enabled)) return "error: trainer rock stall 1|0|status";
            if (!enabled) {
                stall_probe_input::stop();
                s.stall_probe_enabled = false;
                if (s.stall_probe_file.is_open()) s.stall_probe_file.close();
                return std::format("Board Stall v1.2 saved: {} rows, {} direct B edges, {} XInput B edges, {} direct valid reads, {} game XInput polls. {}",
                    s.stall_probe_rows, s.stall_direct_input.presses,
                    stall_probe_input::b_press_count.load(),
                    s.stall_direct_input.valid_reads, stall_probe_input::polls.load(),
                    s.stall_probe_path.empty() ? "No file" : path_utf8(s.stall_probe_path));
            }
            if (s.stall_probe_enabled) return "Board Stall v1.2 recording already ON.";
            const auto dir = data_directory() / L"telemetry";
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            if (error) return "error: cannot create Board Stall telemetry folder";
            SYSTEMTIME tm{};
            GetLocalTime(&tm);
            const auto path = dir / std::format("board_stall-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.csv",
                tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond, tm.wMilliseconds);
            s.stall_probe_file.open(path, std::ios::binary | std::ios::trunc);
            if (!s.stall_probe_file) return "error: unable to create Board Stall CSV";
            s.stall_probe_file << "seconds,qpc_ticks,skater,physics_state,x,y,z,heading_deg,speed_mps,vertical_mps,airborne,"
                                   "xinput_b_presses,b_press_delta,last_b_tick_ms,last_b_user,xinput_polls,xinput_users_mask,"
                                   "user0_b_down,user1_b_down,"
                                   "direct_available,direct_device,direct_buttons,direct_b_down,"
                                   "direct_b_press_delta,direct_b_presses,direct_valid_reads,direct_last_b_tick_ms\n";
            s.stall_probe_path = path;
            s.stall_probe_started_ms = GetTickCount64();
            s.stall_probe_rows = s.stall_probe_last_press_count = 0;
            s.stall_probe_last = 0;
            s.stall_direct_input = {};
            stall_probe_input::begin();
            s.stall_probe_enabled = true;
            return "Board Stall v1.2 dual-input recorder ON (45s maximum). Close the ReSkate console, enter Board Stall, then press B once. " + path_utf8(path);
        }
        // ROCK_POSITION_SCREEN_V07: opt-in, *site-specific* geometry.
        if (lower(arg(0)) == "lip") {
            if (arg(1).empty())
                return std::format("v0.7 lip Z={:.3f}, deck Y={:.3f}, side={}, X={:.3f}. Use trainer rock lip lower|upper|custom <Z> <Y> <-1|1> <X>.",
                    s.rock_position_site.lip_z,s.rock_position_site.deck_y,s.rock_position_site.deck_side,s.rock_position_site.center_x);
            auto next=s.rock_position_site;
            if (lower(arg(1))=="lower") next=reskate_custom::rock_position::lower_lip;
            else if (lower(arg(1))=="upper") next=reskate_custom::rock_position::upper_lip;
            else if (lower(arg(1))=="custom") {
                const auto z=number(arg(2)), y=number(arg(3)),side=number(arg(4)),x=number(arg(5));
                if (!z||!y||!side||!x||(*side!=1 && *side!=-1) ||
                    std::abs(*z)>100000 || std::abs(*y)>100000 || std::abs(*x)>100000)
                    return "error: usage: trainer rock lip custom <Z> <deckY> <-1|1> <centerX>";
                next={*z,*y,*side,*x,1.7};
            } else return "error: use trainer rock lip lower|upper|custom <Z> <Y> <-1|1> <X>";
            s.rock_position_site=next;
            s.rock_position_detector.configure(next);
            s.rock_position_last=0;
            return std::format("Calibrated local ramp: lip Z={:.3f}, deck Y={:.3f}, deck side={}, X={:.3f}. Estimates ONLY; no contact/physics writes.",
                next.lip_z,next.deck_y,next.deck_side,next.center_x);
        }
        if (lower(arg(0)) == "position") {
            // ROCK_GATE_DEBUG_V071: compare LIVE samples to the offline replay.
            if (lower(arg(1)) == "debug") {
                const auto& d=s.rock_position_detector.diagnostics();
                return std::format(
                    "Rock position LIVE diagnostic: samples={} invalid={} airborne={} timestamp-resets={} longest-gap={:.3f}s.\n"
                    "Gate funnel (site/facing/armed/center/axle+ /axle- /height/slow/full): {}/{}/{}/{}/{}/{}/{}/{}/{}.\n"
                    "Longest live hold={:.3f}s; hold resets={}; awaiting-release-frames={}; cooldown-frames={}; positions={} releases={}.",
                    d.samples,d.invalid,d.airborne,d.timestamp_resets,d.largest_gap_seconds,
                    d.near_site,d.facing,d.armed,d.center,d.axle_across,d.axle_behind,
                    d.deck_height,d.slow,d.full_gate,d.longest_hold_seconds,d.hold_resets,
                    d.awaiting_release,d.in_cooldown,
                    s.rock_position_detector.positions(),s.rock_position_detector.releases());
            }
            if (arg(1).empty())
                return std::format("Rock-position screening {} | geometry candidates={} | fakie releases={}. Lip Z={:.3f}, deck Y={:.3f}. No collision validation.",
                    s.rock_position_enabled?"ON":"OFF",s.rock_position_detector.positions(),
                    s.rock_position_detector.releases(),s.rock_position_site.lip_z,s.rock_position_site.deck_y);
            bool enabled{};
            if (!flag(arg(1),enabled))return "error: usage: trainer rock position 0|1";
            s.rock_position_enabled=enabled;
            s.rock_position_last=0;
            s.rock_position_detector.reset();
            return enabled?"Rock-position screening ON for calibrated ramp (read-only; contact UNVERIFIED).":
                "Rock-position screening OFF.";
        }
        // Native solver pair observation, opt-in, read-only.
        if (lower(arg(0)) == "contact")
            return rock_contact_observer::command(lower(arg(1)), s.base);
        // ROCK_BOARD_PROBE_V05: independent read-only board-body sampler.
        if (lower(arg(0)) == "board") {
            if (arg(1).empty())
                return std::format("Rock board probe {} ({} rows; {} plausible). Use trainer rock board 0|1.",
                    s.rock_board_enabled ? "ON" : "OFF", s.rock_board_rows, s.rock_board_valid);
            bool enabled{};
            if (!flag(arg(1), enabled)) return "error: usage: trainer rock board 0|1";
            if (s.rock_board_file.is_open()) s.rock_board_file.close();
            s.rock_board_enabled = false;
            if (!enabled)
                return std::format("Rock board probe stopped ({} rows, {} plausible).", s.rock_board_rows, s.rock_board_valid);
            const auto dir = data_directory() / L"telemetry";
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            if (error) return "error: cannot create board telemetry folder.";
            SYSTEMTIME time{};
            GetLocalTime(&time);
            const auto path = dir / std::format("rock_board-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}.csv",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                time.wMilliseconds, GetTickCount64());
            s.rock_board_file.open(path, std::ios::binary | std::ios::trunc);
            if (!s.rock_board_file) return "error: cannot open board telemetry CSV.";
            s.rock_board_file << "seconds,skater_x,skater_y,skater_z,heading_deg,speed_mps,vertical_mps,physics_state,airborne,readable,layout_plausible,board_root_y";
            for (unsigned i = 0; i < 9; ++i)
                s.rock_board_file << std::format(",body{}_x,body{}_y,body{}_z,body{}_speed_xz,body{}_spin_radps", i, i, i, i, i);
            s.rock_board_file << ",board_owner_ptr,board_wrapper_ptr,board_handle_provider_ptr";
            for (unsigned i = 0; i < 9; ++i) {
                s.rock_board_file << std::format(",body{}_handle_kind", i);
                s.rock_board_file << std::format(
                    ",body{}_record_ptr,body{}_handle_world,body{}_handle_index,body{}_handle_generation,"
                    "body{}_descriptor_ok,body{}_generation_ok,body{}_descriptor,"
                    "body{}_native_x,body{}_native_y,body{}_native_z,body{}_native_position_gap_m",
                    i,i,i,i,i,i,i,i,i,i,i);
            }
            s.rock_board_file << ",qpc_ticks\n";
            s.rock_board_start = clock_now();
            s.rock_board_last = s.rock_probe_enabled ? s.rock_probe_last : 0;
            s.rock_board_rows = 0;
            s.rock_board_valid = 0;
            s.rock_board_enabled = true;
            return "Rock board probe ON (up to 20 Hz, synchronized QPC timestamps; no physics writes): " + path_utf8(path);
        }
        // ROCK_SURFACE_PROBE_V03: read-only terrain profiler, separate from candidate detector.
        if (lower(arg(0)) == "probe") {
            if (arg(1).empty())
                return std::format("Rock surface probe {} ({} rows). Use trainer rock probe 1|0.",
                                   s.rock_probe_enabled ? "ON" : "OFF", s.rock_probe_rows);
            bool enabled{};
            if (!flag(arg(1), enabled)) return "error: usage: trainer rock probe 0|1";
            if (s.rock_probe_file.is_open()) s.rock_probe_file.close();
            s.rock_probe_enabled = false;
            if (!enabled) return std::format("Rock surface probe stopped ({} rows).", s.rock_probe_rows);
            const auto dir = data_directory() / L"telemetry";
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            if (error) return "error: could not create telemetry folder for rock surface probe.";
            SYSTEMTIME time{};
            GetLocalTime(&time);
            const auto path = dir / std::format("rock_near_surface-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}.csv",
                                                time.wYear, time.wMonth, time.wDay,
                                                time.wHour, time.wMinute, time.wSecond,
                                                time.wMilliseconds, GetTickCount64());
            s.rock_probe_file.open(path, std::ios::binary | std::ios::trunc);
            if (!s.rock_probe_file) return "error: cannot open the rock surface CSV.";
            s.rock_probe_file << "seconds,x,y,z,heading_deg,speed_mps,vertical_mps,physics_state,airborne,forward_x,forward_z,"
                                 "surface_m90cm,surface_m60cm,surface_m30cm,surface_0cm,surface_p30cm,surface_p60cm,surface_p90cm,qpc_ticks\n";
            s.rock_probe_start = clock_now();
            s.rock_probe_last = s.rock_board_enabled ? s.rock_board_last : 0;
            s.rock_probe_rows = 0;
            s.rock_probe_enabled = true;
            return "Near-skater surface probe ON; 7 locally bounded raycasts up to 20 Hz to " + path_utf8(path) +
                   ". No board contact or physics changes.";
        }
        if (arg(0).empty())
            return s.rock_detection_enabled ? "RockToFakie detector ON (passive; coping unverified)."
                                            : "RockToFakie detector OFF; use trainer rock 1 to enable.";
        bool enabled{};
        if (!flag(arg(0), enabled)) return "error: usage: trainer rock 0|1";
        s.rock_detector.reset();
        s.rock_detection_enabled = enabled;
        return enabled ? "RockToFakie candidate detector ON (no physics changes)."
                       : "RockToFakie candidate detector OFF.";
    }
    if (v == "status") {
        std::size_t touched{}, frozen{};
        for (const auto &e : s.entries) { touched += e.touched; frozen += e.frozen; }
        return std::format("{} {} values ({} changed, {} frozen). Map: {}. Presets: {}.", s.status, s.entries.size(), touched, frozen,
                           s.map.empty() ? "none" : s.map, s.active.empty() ? "stock" : std::to_string(s.active.size()) + " applied");
    }
    if (v == "set" || v == "freeze" || v == "reset") {
        if (v == "reset" && lower(arg(0)) == "all") {
            const auto count = reset_all();
            std::size_t locked{};
            for (const auto &e : s.entries) locked += e.touched;
            changed();
            return locked ? std::format("{} values put back; {} locked values kept (Reset everything clears those too).", count, locked)
                          : std::format("{} values put back.", count);
        }
        if (v == "reset" && lower(arg(0)) == "tricks") {
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = 1.0f;
            changed();
            return "Trick heights and flip trick speed are the game's own again.";
        }
        if (v == "reset" && lower(arg(0)) == "presets") {
            const auto on = s.active;
            for (const auto &name : on) (void)remove_preset(name);
            s.active.clear();
            changed();
            return std::format("{} presets switched off.", on.size());
        }
        if (v == "reset" && lower(arg(0)) == "everything") {
            // The game as it shipped: no lock survives, no preset stays on, no trick multiplier.
            std::size_t count{};
            for (auto &e : s.entries) e.frozen = false;
            for (auto &e : s.entries) {
                if (e.touched) {
                    set_entry(e, e.stock);
                    ++count;
                } else {
                    remember(e);
                }
            }
            s.saved.clear();
            s.active.clear();
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = 1.0f;
            changed();
            return std::format("Everything is the game's own again: {} values put back, locks cleared, presets off, trick sliders at 1.", count);
        }
        auto *e = find_entry(arg(0));
        if (!e) return "error: no tuning value is called \"" + arg(0) + "\". Try: trainer find <text>";
        std::string why;
        if (v != "freeze" && !editable(&why)) return "error: " + why;
        if (v == "set") {
            const auto value = number(arg(1));
            if (!value) return "error: " + e->id + " needs a number.";
            set_entry(*e, *value);
        } else if (v == "freeze") {
            if (!flag(arg(1), e->frozen)) return "error: freeze needs 0 or 1.";
            remember(*e);
        } else {
            set_entry(*e, e->stock);
        }
        changed();
        return std::format("{} = {:.6g}{}{}", e->id, e->value, e->touched ? std::format(" (stock {:.6g})", e->stock) : "", e->frozen ? ", frozen" : "");
    }
    if (v == "find") {
        const auto pattern = lower(arg(0));
        std::string result;
        std::size_t count{};
        for (const auto &e : s.entries)
            if (matches(e.key, pattern) && ++count <= 12) result += std::format("{}{} = {:.6g}", result.empty() ? "" : "; ", e.id, e.value);
        return count ? std::format("{} match: {}{}", count, result, count > 12 ? "; ..." : "") : "Nothing matches.";
    }
    if (v == "dial") {
        // trainer dial <multiplier> <built-in preset>
        const auto factor = number(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (!factor || name.empty()) return "error: usage: trainer dial <multiplier> <preset name>";
        std::string why;
        if (!editable(&why)) return "error: " + why;
        return apply_dial(name, *factor);
    }
    if (v == "preset") {
        const auto action = lower(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (name.empty()) return "error: usage: trainer preset apply|remove|save|delete <name>";
        if (action == "remove") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return remove_preset(name);
        }
        if (action == "apply") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return apply_preset(name);
        }
        if (action == "save") {
            if (name.size() > 48) return "error: that name is too long.";
            for (const auto &preset : builtin_presets())
                if (lower(preset.name) == lower(name)) return "error: that name belongs to a built-in preset.";
            if (lower(name) == "stock" || lower(name) == "map") return "error: that name is reserved.";
            std::map<std::string, double, std::less<>> values;
            for (const auto &e : s.entries)
                if (e.touched) values[e.key] = e.value;
            if (values.empty()) return "error: nothing is changed, so there is nothing to save.";
            const auto count = values.size();
            s.user[name] = std::move(values);
            changed();
            return std::format("Saved \"{}\" ({} values).", name, count);
        }
        if (action == "delete") {
            if (!s.user.erase(name)) return "error: no preset of yours is called \"" + name + "\".";
            std::erase(s.active, name);
            changed();
            return "Deleted \"" + name + "\".";
        }
        return "error: usage: trainer preset apply|save|delete <name>";
    }
    if (v == "slot") {
        const auto slot = slot_argument(a, 0);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        s.slot = *slot;
        changed();
        return std::format("Marker slot {}{}.", s.slot + 1, current_map().markers[static_cast<std::size_t>(s.slot)].set ? "" : " (empty)");
    }
    if (v == "marker") {
        const auto action = lower(arg(0));
        const auto slot = slot_argument(a, 1);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        if (s.map.empty()) return "error: load a level first.";
        auto &marker = current_map().markers[static_cast<std::size_t>(*slot)];
        if (action == "save") {
            if (!s.telemetry.skater) return "error: no skater to mark.";
            marker = {true, s.telemetry.position};
            changed();
            return std::format("Marker {} saved at ({:.1f}, {:.1f}, {:.1f}).", *slot + 1, marker.position[0], marker.position[1], marker.position[2]);
        }
        if (action == "go") {
            if (!marker.set) return std::format("error: marker {} is empty.", *slot + 1);
            return go_to(marker.position, std::format("marker {}", *slot + 1));
        }
        if (action == "clear") {
            marker = {};
            changed();
            return std::format("Marker {} cleared.", *slot + 1);
        }
        return "error: usage: trainer marker save|go|clear [slot]";
    }
    if (v == "ground") {
        const auto x = number(arg(0)), z = number(arg(1));
        if (!x || !z) return "error: usage: trainer ground <x> <z>  (teleport to the topmost surface there)";
        return land_at(static_cast<float>(*x), static_cast<float>(*z), s.telemetry.position[1], "the ground there");
    }
    if (v == "waypoint") {
        refresh_waypoint();
        if (lower(arg(0)) == "info")
            return "Map waypoint: " + (s.waypoint ? std::format("{:.1f}, {:.1f}, {:.1f}", (*s.waypoint)[0], (*s.waypoint)[1], (*s.waypoint)[2]) : std::string("none")) +
                   " (" + s.waypoint_detail + ").";
        if (!s.waypoint) return "error: no waypoint on the map. Open the pause map and place one first.";
        return land_at((*s.waypoint)[0], (*s.waypoint)[2], (*s.waypoint)[1], "your map waypoint");
    }
    if (v == "tp") {
        const auto x = number(arg(0)), y = number(arg(1)), z = number(arg(2));
        if (!x || !y || !z) return "error: usage: trainer tp <x> <y> <z>";
        return go_to({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)}, "that point");
    }
    if (v == "spot") {
        const auto index = number(arg(0));
        if (!index || *index < 1 || *index > static_cast<double>(s.map_file.spots.size())) return "error: this map has no such spot.";
        const auto &spot = s.map_file.spots[static_cast<std::size_t>(*index) - 1];
        return go_to(spot.position, spot.name);
    }
    if (v == "option") {
        const auto name = lower(arg(0));
        bool on{};
        std::string why;
        if (name == "flip_speed") {
            const auto value = number(arg(1));
            if (!value) return "error: flip_speed needs a multiplier, 1 = the game's own speed.";
            if (!editable(&why)) return "error: " + why;
            s.flip_speed = std::clamp(static_cast<float>(*value), flip_low, flip_high);
        } else if (name == "hippy_height" || name == "nocomply_height" || name == "boneless_height" || name == "offboard_height") {
            const auto value = number(arg(1));
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own height.";
            if (!editable(&why)) return "error: " + why;
            (name == "hippy_height" ? s.hippy_height : name == "nocomply_height" ? s.nocomply_height : name == "offboard_height" ? s.offboard_height : s.boneless_height) =
                std::clamp(static_cast<float>(*value), height_low, height_high);
        } else if (name == "return_delay") {
            const auto value = number(arg(1));
            if (!value) return "error: return_delay needs seconds.";
            s.return_delay = std::clamp(static_cast<float>(*value), 0.0f, 10.0f);
        } else if (!flag(arg(1), on)) {
            return "error: usage: trainer option hud|hud_jump|auto_return|pad|log 0|1";
        } else if (name == "hud") s.hud = on;
        else if (name == "hud_jump") s.hud_jump = on;
        else if (name == "auto_return") s.auto_return = on;
        else if (name == "pad") s.pad_shortcuts = on;
        else if (name == "log") {
            set_logging(on);
            if (on && !s.logging) return "error: the telemetry file could not be created.";
        } else return "error: no option is called \"" + name + "\".";
        changed();
        return name + " = " + arg(1);
    }
    if (v == "profile") {
        const auto action = lower(arg(0));
        if (s.map.empty()) return "error: load a level first.";
        if (action == "clear") {
            current_map().preset.clear();
            changed();
            return "This map no longer applies a preset on load.";
        }
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action != "set" || name.empty()) return "error: usage: trainer profile set <preset> | clear";
        current_map().preset = name;
        changed();
        return "\"" + name + "\" will be applied whenever this map loads.";
    }
    if (v == "open") {
        const auto tab = lower(arg(0));
        // The last three are the Tune tab on one of its lists.
        const std::array<std::string_view, 7> tabs{"tune", "presets", "practice", "map", "realistic", "fun", "everything"};
        const auto found = std::ranges::find(tabs, tab);
        if (!tab.empty() && found == tabs.end()) return "error: usage: trainer open [tune|presets|practice|map|realistic|fun|everything]";
        s.open_tab = tab.empty() ? 1 : static_cast<int>(found - tabs.begin());
        ++s.open_serial;
        s.view_due = true;
        return "Opening the trainer.";
    }
    if (v == "jumps") {
        const auto &j = s.telemetry.last;
        if (!j.serial) return "No jump measured yet.";
        return std::format("Jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, landing {:.1f} km/h.",
                           j.serial, j.takeoff_speed * 3.6f, j.takeoff_angle, j.air_time, j.height, j.distance, j.drop, j.landing_speed * 3.6f);
    }
    if (v == "where") {
        if (!s.telemetry.skater) return "error: no skater.";
        const auto &p = s.telemetry.position;
        return std::format("({:.2f}, {:.2f}, {:.2f}) heading {:.0f}, {:.1f} km/h. Blender: ({:.2f}, {:.2f}, {:.2f})", p[0], p[1], p[2],
                           s.telemetry.heading, s.telemetry.speed * 3.6f, p[0], -p[2], p[1]);
    }
    if (v == "classes") {
        if (lower(arg(0)) == "find") {
            find_classes();
            return "Looking for the game's tuning classes.";
        }
        return "Copies found: " + classes_summary();
    }
    if (v == "refresh") return tuning::refresh_skater(s.base, s.entity) ? "Skater copy refreshed." : "error: the skater has no cached copy right now.";
    if (v == "states") {
        std::string result;
        for (const auto &[id, ticks] : s.states) result += std::format("{}{}: {}", result.empty() ? "" : ", ", id, ticks);
        return result.empty() ? "No physics state seen yet." : "Physics states seen (id: ticks): " + result;
    }
    if (v == "dump") {
        const auto directory = data_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / L"tuning-values.txt", std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the list.";
        for (const auto &e : s.entries)
            file << std::format("{}\t0x{:x}\t{}\t{:.6g}\t{:.6g}\n", e.id, e.offset,
                                e.kind == Kind::real ? "real" : e.kind == Kind::flag ? "flag" : e.kind == Kind::curve ? "curve" :
                                e.kind == Kind::graph ? "graph" : "int", e.stock, e.value);
        return std::format("Wrote {} values to {}.", s.entries.size(), path_utf8(directory / L"tuning-values.txt"));
    }
    if (v == "selftest") {
        if (s.test.step) return "The self test is already running.";
        s.test = {};
        s.test.step = 1;
        s.test.until = GetTickCount64() + 500;
        return "Self test started; see the log for \"trainer selftest:\" lines.";
    }
    return "error: trainer status|rock|open|set|freeze|reset|find|preset|slot|marker|tp|ground|waypoint|spot|option|profile|jumps|where|states|dump|selftest";
}
} // namespace

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto result = run(verb, arguments);
    // The menu shows the last answer, except for the ones a dragged slider sends every frame.
    if (verb != "set" || result.starts_with("error")) {
        auto &s = state();
        s.last = result;
        s.view_due = true;
    }
    return result;
}
} // namespace dingosdk::trainer
