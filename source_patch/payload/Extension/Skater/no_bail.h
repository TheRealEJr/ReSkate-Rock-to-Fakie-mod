#pragma once
#include <cstdint>
#include <vector>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;

// v1.4.24: native B/trick capture + OFF-by-default temporary event-classification experiment.
// kind: 1 = native transition, 2 = physical B rising edge, 3 = hold sample,
// 4 = native trick packet, 5 = native internal-name lookup,
// 6 = native localization GETTER, 7 = verified copy-wrapper parent,
// 8 = DIRECT native virtual localized-key method caller (v1.4.39).
// Neither locale event kind proves ownership of the stock HUD.
// validity bit mask: 1=context, 2=selector target, 4=core context fields,
// 8=selector target fields, 16=optional context input bytes,
// 32=verified manager/vtable plus readable callback and userdata (kind 1/2 only).
struct NativeBTraceEvent {
    std::uint64_t tick_ms{};
    std::uintptr_t selector{}, context{}, target{};
    std::uint32_t kind{}, raw{}, chosen{}, previous{}, buttons{}, device{};
    std::uint32_t c4{}, cc{}, d0{}, dc{}, fa0{}, fb0{}, at1418{}, type{};
    std::uint8_t source19d{}, p48{}, p49{}, valid_mask{};
    // v1.4.20: read-only snapshot of the game's registered input callback.
    std::uintptr_t input_manager{}, input_vtable{}, input_callback{}, input_userdata{};
    std::uint32_t input_flags{};
    // v1.4.22: valid native trick packet. These pointers are read-only metadata.
    std::uintptr_t trick_recorder{}, trick_packet{};
    std::uint32_t trick_id{};
    std::uint8_t trick_valid{};
    std::uint32_t trick_effective_id{};
    std::uint8_t trick_remapped{};
    std::uint8_t trick_remap_phase{}; // 0=unchanged, 1=entry/hold, 2=B-triggered exit
    std::uintptr_t trick_callsite{}; // return address of native producer for HUD analysis
    // v1.4.28: five read-only original producer event slots. The validity mask
    // identifies readable, valid event headers; 0xffffffff means unavailable.
    // No producer packet or HUD state is ever written.
    std::uintptr_t producer{};
    std::uint32_t producer_valid_mask{};
    std::uint32_t source_a50_id{0xffffffffu}, source_ac0_id{0xffffffffu};
    std::uint32_t source_a58_id{0xffffffffu}, source_b90_id{0xffffffffu};
    std::uint32_t source_b28_id{0xffffffffu};
    std::uint32_t producer_ca0{}, producer_a54{}, producer_b20{};
    // v1.4.35: native locale-getter text match, kind=6. UTF-8 ASCII-filtered
    // diagnostic copy; only the returned source key, caller, and text are read.
    std::uint32_t localized_hash{};
    std::uintptr_t localized_caller{};
    char localized_text[96]{};
    // v1.4.40: bounded read-only call-stack snapshot for kind-8 entries.
    // Addresses are not assumed to be Skate.exe or a HUD caller.
    std::uint32_t localized_stack_count{};
    std::uintptr_t localized_stack[12]{};
    // v1.4.41: localized output-buffer trial: kind 9, status 1=applied,
    // 2=stack gate failed, 3=buffer too small. No original keys modified.
    std::uint32_t localized_trial_status{}, localized_buffer_capacity{};
};
struct NativeBTraceStats {
    bool running{}; std::uint32_t count{}; std::uint64_t dropped{};
    bool trick_hook_ready{}, trick_name_hook_ready{}, localization_hook_ready{}, localization_key_hook_ready{};
    std::uint32_t trick_mode{}, trick_remaps{}, localized_trial_applied{};
};
// Modes: 0=off, 1=fakie_rock packet (187), 2=fs (185), 3=bs (186),
// 4=fakie_rock packet+native-name-lookup experiment (187),
// 5=opt-in native localized-output-buffer experiment (also ID 187 recorder copy).
// Nonzero CAPTURE modes require active recording and reset on start/stop.
// Persistent HUD is independent and is controlled by native_rock_hud_set().
bool native_b_trick_mode_set(std::uint32_t mode) noexcept;
// Session-long opt-in native trick-ticker label, independent of telemetry timeout.
// Enabled until explicitly turned off or the DLL/game process exits.
// Returns false only if the necessary hooks are unavailable when enabling.
bool native_rock_hud_set(bool enabled) noexcept;
bool native_rock_hud_enabled() noexcept;
// Opt-in display derived from a verified B-triggered ID 9/12 -> 187 remap.
// Returns UINT32_MAX if hidden; otherwise milliseconds since the event (0..2299).
// This is a separate ReSkate overlay, NOT the native Skate trick popup.
std::uint32_t native_rock_popup_age_ms() noexcept;
bool native_b_trace_start() noexcept;
NativeBTraceStats native_b_trace_stats() noexcept;
void native_b_trace_controller_edge(std::uint32_t state, std::uint32_t buttons, std::uint32_t device) noexcept;
std::vector<NativeBTraceEvent> native_b_trace_stop() noexcept;

// The local skater as No Bail follows it (update_no_bail publishes it), for a feature that reads it
// (Hall of Meat): any thread, its ownership chain resolved again now.
struct NoBailSkater {
    std::uintptr_t base{}, entity{}, core{}, context{}, rig{};
};
bool no_bail_skater(NoBailSkater& skater) noexcept;
// Every physics step of that skater, whether or not No Bail protects it: No Bail's hook on the
// skeleton response runs once per step, right after the step's body contacts, and hands it on after
// its own filtering, so a wipeout it filtered never happened. `seconds` is the step's simulated time.
// One observer, nullptr for none; it runs on the physics thread.
using NoBailStepObserver = void (*)(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept;
}
