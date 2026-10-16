#include "no_bail.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include <atomic>
#include <array>
#include <vector>
#include <intrin.h>
#include <cstring>

namespace dingosdk {
namespace {
using namespace addr::no_bail;
// Native cause recorder: RCX=collector, EDX=reason, XMM2=magnitude.
using RecordCause = void (*)(std::uintptr_t, std::int32_t, float);
using ChooseState = std::uint32_t (*)(std::uintptr_t, std::uint32_t);
using SkeletonResponse = void (*)(std::uintptr_t, float, bool);
using PublishAnimation = void (*)(std::uintptr_t);
using ResetCauses = void (*)(std::uintptr_t);
// Skate+0x47803A0 takes (trick-recorder, 104-byte event packet).
// Observe the packet and then call the untouched original implementation.
using RecordTrickEvent = void (*)(std::uintptr_t, std::uintptr_t);
// PE-verified Skate+0x4783030: uint32 ID -> const char* internal trick name.
using LookupTrickName = const char* (*)(std::uint32_t);
// Skate+0x3322FD0: localization hash + optional missing-string flag -> UTF-8.
// Ghidra/native disassembly verifies second argument is read from DL.
using LookupLocalizedText = const char* (*)(std::uint32_t, bool);
// Skate+0x2E1AC10: virtual localized-string method.
// RCX=object, RDX=key, R8=output buffer, R9D=capacity; AL=bool success.
// It has no direct CALL sites; its address occurs in the executable vtable.
using ResolveLocalizedKey = bool (*)(std::uintptr_t, const char*, char*, std::uint32_t);
constexpr std::uintptr_t highest = memory::highest_user_address;
// The hooks re-resolve the local skater's ownership (some 35 fields) on every
// protected physics step: guarded same-process copies, not a system call each.
template<class T> bool read(std::uintptr_t address, T& value) noexcept { return memory::peek(address, value); }
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || object > highest - offset || !read(object + offset, value) ||
        value < 0x10000 || value > highest - 0x1000100) return 0;
    return value;
}
struct Owner {
    std::uintptr_t client{}, entity{}, player{}, handle{}, component{}, core{}, context{}, selector{}, causes{}, rig{};
    bool operator==(const Owner&) const = default;
};
struct Lease {
    Owner owner;
    std::uint64_t manual_until{}, flight_until{};
    bool active(std::uint64_t now) const noexcept {
        return now < manual_until || now < flight_until;
    }
};
struct BoardLock {
    Owner owner;
    std::uint64_t until{};
};
struct Protection {
    std::uintptr_t base{};
    bool mount_test{}; // the selector's mount request test is the known one
    BoardLock board;
    RecordCause cause_original{};
    ChooseState choose_original{};
    SkeletonResponse skeleton_original{};
    PublishAnimation publish_original{};
    ResetCauses reset_causes{};
    RecordTrickEvent trick_original{};
    std::atomic<bool> trick_hook_ready{false};
    LookupTrickName trick_name_original{};
    std::atomic<bool> trick_name_hook_ready{false};
    LookupLocalizedText localized_original{};
    std::atomic<bool> localization_hook_ready{false};
    ResolveLocalizedKey localized_key_original{};
    std::atomic<bool> localization_key_hook_ready{false};
    std::atomic<bool> ready{};
    SRWLOCK lock = SRWLOCK_INIT;
    Lease lease;
    std::atomic<NoBailStepObserver> step_observer{};
};
Protection& protection() { static auto* value = new Protection; return *value; }
struct StateWatch {
    std::atomic<std::uintptr_t> selector{};
    std::atomic<std::uint64_t> until{}, changes{}, wipeouts{};
    std::atomic<std::uint32_t> state{}, previous{};
    std::atomic<std::int64_t> since{}, previous_ticks{}; // performance-counter ticks
};
StateWatch& state_watch() { static auto* value = new StateWatch; return *value; }

// This is NOT another hook: choose_state below already intercepts the selector.
// One small, fixed-size buffer; nothing is printed or allocated in the physics hook.
// Try-lock prevents waiting on a game thread when the reader stops recording.
struct NativeBCapture {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> last_hold_ms{0};
    std::atomic<std::uint64_t> last_stall_exit_ms{0};
    // Capture-only mode; defaults OFF at recording start/stop.
    // 1=fakie_rock, 2=fs_rock_n_roll, 3=bs_rock_n_roll.
    std::atomic<std::uint32_t> trick_mode{0};
    // v1.4.42: Long-lived opt-in. Independent of 45-second CSV recording.
    // Resets when the DLL/game process exits, or on explicit console OFF.
    std::atomic<bool> hud_persistent{false};
    std::atomic<std::uint64_t> last_stall_b_ms{0};
    std::atomic<std::uintptr_t> last_stall_b_selector{0};
    std::atomic<std::uint32_t> trick_remaps{0};
    std::atomic<std::uint32_t> localized_trial_applied{0};
    // v1.4.34: ReSkate-only optional display; NEVER modifies Skate's stock HUD.
    std::atomic<std::uint64_t> rock_label_since_ms{0};
    std::array<NativeBTraceEvent, 1024> events{};
    std::size_t count{};
};
NativeBCapture& native_b_capture() { static auto* value = new NativeBCapture; return *value; }
void native_b_append(NativeBTraceEvent event) noexcept {
    auto& cap = native_b_capture();
    if (!cap.running.load(std::memory_order_acquire)) return;
    if (!TryAcquireSRWLockExclusive(&cap.lock)) {
        cap.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (cap.running.load(std::memory_order_relaxed)) {
        // Only the first match per (locale hash, native caller, state) is needed.
        // This prevents UI updates every frame from exhausting the 1,024 rows.
        if (event.kind == 6 || event.kind == 7 || event.kind == 8 || event.kind == 9) {
            for (std::size_t i = 0; i < cap.count; ++i) {
                const auto& prior = cap.events[i];
                if (prior.kind == event.kind && prior.localized_hash == event.localized_hash &&
                    prior.localized_caller == event.localized_caller && prior.raw == event.raw &&
                    // Kind 8 intentionally lacks a native hash; dedupe on text too.
                    ((event.kind != 8 && event.kind != 9) ||
                     (std::strcmp(prior.localized_text, event.localized_text) == 0 &&
                      (event.kind != 9 || prior.localized_trial_status == event.localized_trial_status)))) {
                    ReleaseSRWLockExclusive(&cap.lock);
                    return;
                }
            }
        }
        if (cap.count < cap.events.size()) cap.events[cap.count++] = event;
        else cap.dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&cap.lock);
}
NativeBTraceEvent native_b_snapshot(std::uint32_t kind, std::uintptr_t selector,
                                   std::uint32_t raw, std::uint32_t selected,
                                   std::uint32_t previous, std::uint32_t buttons,
                                   std::uint32_t device) noexcept {
    NativeBTraceEvent e{};
    e.tick_ms = GetTickCount64();
    e.kind = kind; e.selector = selector; e.raw = raw; e.chosen = selected;
    e.previous = previous; e.buttons = buttons; e.device = device;
    // Guarded, read-only snapshots only; failure is recorded as validity bits.
    std::uintptr_t context{};
    if (memory::peek(selector + 8, context) && context >= 0x10000 &&
        context < highest - 0x1500) {
        e.context = context;
        e.valid_mask |= 1;
        if (memory::peek(context + 0x13c4, e.c4) &&
            memory::peek(context + 0x13cc, e.cc) &&
            memory::peek(context + 0x13d0, e.d0) &&
            memory::peek(context + 0x13dc, e.dc) &&
            memory::peek(context + 0xfa0, e.fa0) &&
            memory::peek(context + 0xfb0, e.fb0) &&
            memory::peek(context + 0x1418, e.at1418)) e.valid_mask |= 4;
        std::uintptr_t input{};
        if (memory::peek(context + 0x1480, input) && input >= 0x10000 &&
            input < highest - 0x50 && memory::peek(input + 0x48, e.p48) &&
            memory::peek(input + 0x49, e.p49)) e.valid_mask |= 16;
    }
    std::uintptr_t vtable{}, target{};
    if (memory::peek(selector, vtable) && vtable >= 0x10000 && vtable < highest - 0x28 &&
        memory::peek(vtable + 0x20, target) && target >= 0x10000 &&
        target < highest - 0x1a0) {
        e.target = target; e.valid_mask |= 2;
        if (memory::peek(target + 0x19d, e.source19d) &&
            memory::peek(target + 0xbc, e.type)) e.valid_mask |= 8;
    }
    // Native input event receiver inspection. The game constructor publishes the
    // SAME manager to global Skate+0x7603940 (verified in Skate+0x18AAA00).
    // Its vtable is Skate+0x642A858. All addresses use the supported image base.
    // Do not call, overwrite, or install any callback. Only inspect transitions
    // and physical B edges; unchanged hold samples do not touch these fields.
    if ((kind == 1 || kind == 2) && protection().base) {
        const std::uintptr_t base = protection().base;
        constexpr std::uintptr_t manager_global_rva = 0x7603940;
        constexpr std::uintptr_t manager_vtable_rva = 0x642A858;
        if (base <= highest - manager_global_rva - sizeof(std::uintptr_t)) {
            std::uintptr_t manager{};
            if (memory::peek(base + manager_global_rva, manager) &&
                manager >= 0x10000 && manager <= highest - 0x80) {
                e.input_manager = manager;
                std::uintptr_t input_vtable{};
                if (memory::peek(manager, input_vtable)) {
                    e.input_vtable = input_vtable;
                    if (input_vtable == base + manager_vtable_rva &&
                        memory::peek(manager + 0x68, e.input_callback) &&
                        memory::peek(manager + 0x70, e.input_userdata) &&
                        memory::peek(manager + 0x78, e.input_flags)) {
                        e.valid_mask |= 32;
                    }
                }
            }
        }
    }
    return e;
}


// Recheck live local ownership at use time. A retained physics address alone
// must never protect another skater after a respawn or level change.
bool resolve(std::uintptr_t client, std::uintptr_t entity, Owner& o) noexcept {
    const auto base = protection().base;
    unsigned state{}, manager_offset{};
    if (pointer(client) != base + addr::engine::client_vtable || !read(client + 0xc4, state) ||
        (state != 13 && state != 21) || pointer(entity) != base + addr::engine::skater_entity_vtable ||
        !read(base + addr::engine::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000) return false;
    const auto context = pointer(client, 8);
    const auto manager = pointer(context, manager_offset);
    const auto begin = pointer(manager, 0x4c8), end = pointer(manager, 0x4d0);
    if (!context || pointer(entity, 0x20) != context || pointer(manager) != base + addr::engine::local_player_manager_vtable ||
        !begin || end != begin + 8) return false;
    o.player = pointer(begin);
    std::uint8_t local{}, remote{}, teleport{};
    if (pointer(o.player) != base + addr::engine::local_player_vtable || pointer(o.player, 0x78) != context ||
        !read(o.player + 0x45, local) || local != 1 || !read(o.player + 0x44, remote) || remote ||
        pointer(o.player, 0xb8) != entity || pointer(entity, 0xf8) != o.player ||
        !read(entity + 0x7e0, teleport) || teleport) return false;
    o.handle = pointer(o.player, 0xb0);
    const auto collection = pointer(entity, 0x70);
    o.component = pointer(entity, 0x628);
    o.core = pointer(o.component, 0x70);
    o.context = pointer(o.core, 0x3c0);
    o.selector = pointer(o.core, 0x440);
    o.causes = pointer(o.core, 0x428);
    o.rig = pointer(o.core, 0x438);
    if (pointer(o.handle) != entity + 8 || pointer(collection) != entity ||
        pointer(o.component) != base + addr::engine::skater_component_vtable || pointer(o.component, 0x18) != collection ||
        pointer(o.core) != base + bail_core_vtable || !o.context || !o.selector ||
        pointer(o.selector, 8) != o.context || pointer(o.causes, 0x20) != o.context || pointer(o.rig) != o.context || pointer(o.rig, 0x4630) != o.core)
        return false;
    o.client = client;
    o.entity = entity;
    return true;
}
bool protected_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    // The lock only copies SDK data, and is never held across native code or
    // memory reads. Hooks do not contend for the debug/camera action lock.
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    if (!lease.active(GetTickCount64()) || object != lease.owner.*member) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) return false;
    if (owner) *owner = current;
    return true;
}
// The published owner, its chain resolved again now; when `rig` is given, only if it is that owner's.
bool published_skater(std::uintptr_t rig, NoBailSkater& skater) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&p.lock);
    const auto published = p.lease.owner;
    ReleaseSRWLockShared(&p.lock);
    Owner current;
    if (!published.entity || (rig && rig != published.rig) || !resolve(published.client, published.entity, current) ||
        current != published) return false;
    skater = {p.base, current.entity, current.core, current.context, current.rig};
    return true;
}
bool cancel_request(std::uintptr_t context, std::uintptr_t offset, LONG mask) noexcept {
    const auto address = context + offset;
    if (context < 0x10000 || context > highest - offset - sizeof(LONG) ||
        (address & (alignof(LONG) - 1)) != 0) return false;
    // Consume only the identified request bit. Preserve unrelated native flags
    // even when another producer updates the same word.
    __try {
        _InterlockedAnd(reinterpret_cast<volatile LONG*>(address), ~mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool cancel_impact_request(std::uintptr_t context) noexcept {
    return cancel_request(context, impact_request_offset, impact_request_mask);
}
bool cancel_wipeout_requests(std::uintptr_t context) noexcept {
    return cancel_impact_request(context) &&
        cancel_request(context, animation_request_offset, animation_request_mask);
}
bool reset_pending_causes(std::uintptr_t causes) noexcept {
    const auto reset = protection().reset_causes;
    if (!reset) return false;
    __try {
        reset(causes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool filter_requests(std::uintptr_t object, std::uintptr_t Owner::* member) noexcept {
    LastError error;
    Owner owner;
    // Native collision/landing checks also write the collector inline. Reset
    // it at each consumer, after those writes, before either animation's bail
    // or recover/runout query. Filtering record_cause alone misses this path.
    return protected_owner(object, member, &owner) && cancel_wipeout_requests(owner.context) &&
        reset_pending_causes(owner.causes);
}
bool suppress_cause(std::uintptr_t causes, std::int32_t reason, std::uintptr_t caller) noexcept {
    LastError error;
    Owner owner;
    if (!protected_owner(causes, &Owner::causes, &owner)) return false;
    for (const auto& impact : impact_bail_calls) {
        if (caller == protection().base + impact.return_rva && reason == impact.reason)
            return cancel_impact_request(owner.context);
    }
    return true;
}
// v1.4.30: opt-in early trick-name consumer trial + existing recorder remap.
// This changes ONLY a temporary copy passed to the native trick recorder; the
// caller's packet and original game physics/input fields are never written.
// The game copies the 104-byte packet internally at Skate+0x47803A0.
// This does NOT press Xbox B, produce stall release, or guarantee score/UI labels.
// Unlike the 104-byte recorder packet, this code returns the game catalogue's
// internal name pointer. It MAY be used by the native HUD; we measure calls.
constexpr std::uintptr_t trick_name_rva = 0x4783030;
constexpr std::array<unsigned char, 24> trick_name_prologue = {
    0x81,0xf9,0x50,0x01,0x00,0x00,0x77,0x13,
    0x48,0x63,0xc1,0x48,0x8d,0x0d,0x06,0x7f,
    0xe5,0x01,0x48,0xc1,0xe0,0x05,0x48,0x8b
};
// Only the native trick NAME lookup is redirected. No writes to the PE tables,
// event packets, game physics or controller state.
const char* lookup_trick_name(std::uint32_t id) {
    auto& cap = native_b_capture();
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto now = GetTickCount64();
    const auto& w = state_watch();
    const auto selector = w.selector.load(std::memory_order_acquire);
    const auto exit_at = cap.last_stall_exit_ms.load(std::memory_order_acquire);
    const auto b_at = cap.last_stall_b_ms.load(std::memory_order_acquire);
    const auto state = w.state.load(std::memory_order_acquire);
    const bool watched = cap.running.load(std::memory_order_acquire) &&
        selector != 0 && now < w.until.load(std::memory_order_acquire);
    // v1.4.29: Observing ONLY the exit was a blind spot: the stock popup may
    // request its name on stall ENTRY or during HOLD, before the B press.
    // No extra hooks. This reuses the already installed native name resolver.
    const bool stall_phase = state == 603 || state == 400 || state == 401;
    const bool after_exit = exit_at != 0 && now >= exit_at && now - exit_at <= 5000;
    const bool observe_name = watched && (stall_phase || after_exit) &&
        (id == 9u || id == 12u || id == 187u);
    // v1.4.30: PE-verified caller Skate+0x553EE0 is in a distinct live
    // name-consumer: Skate+0x553DF0 resolves a native trick name and copies
    // that string to a UI/state object's +0x20 field. It was observed in
    // actual state 400 *before* B. The previous mode 4 could not affect it.
    // Gate the trial to EXACTLY this caller, one active local stall and
    // an explicit opt-in mode. Never change the original trick packet.
    constexpr std::uintptr_t early_name_caller_rva = 0x553EE0;
    const auto base = protection().base;
    const auto mode = cap.trick_mode.load(std::memory_order_acquire);
    const bool candidate_name = id == 9u || id == 12u;
    const bool early_name_consumer = mode == 4 && watched &&
        (state == 400 || state == 401) && candidate_name &&
        base != 0 && base <= highest - early_name_caller_rva &&
        caller == base + early_name_caller_rva;
    // Retain the v1.4.25 exit-stage name mode as a fallback. This stage was
    // not observed to query source IDs 9/12 in the recent recordings.
    const bool remap_window = watched &&
        exit_at != 0 && b_at != 0 && now >= exit_at && now >= b_at &&
        now - exit_at <= 2000 && now - b_at <= 2150 &&
        cap.last_stall_b_selector.load(std::memory_order_acquire) == selector;
    const bool exit_name_consumer = mode == 4 && remap_window && candidate_name;
    const std::uint32_t mapped_id =
        (early_name_consumer || exit_name_consumer) ? 187u : id;
    // Hook runs on the game's name-resolution path. No memory writes, and
    // only selected names in the fixed 1,024-row capture are recorded.
    if (observe_name) {
        NativeBTraceEvent event{};
        event.tick_ms = now;
        event.kind = 5;
        event.selector = selector;
        event.raw = state;
        event.chosen = state;
        event.trick_id = id;
        event.trick_effective_id = mapped_id;
        event.trick_remapped = id != mapped_id ? 1 : 0;
        event.trick_remap_phase = early_name_consumer ? 4u : 3u;
        // 4=exact early string-consumer candidate; 3=other name lookup.
        event.trick_callsite = caller;
        native_b_append(event);
    }
    return protection().trick_name_original(mapped_id);
}
// v1.4.35: Skate's native localization getter. We only record its returned
// text; we DO NOT override hashes, return pointers, string tables, or HUD state.
// This is a different code path from the internal trick-name catalogue.
constexpr std::uintptr_t localization_get_rva = 0x3322FD0;
constexpr std::array<unsigned char, 16> localization_get_prologue = {
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,
    0xec,0x20,0x48,0x8b,0x05,0xd7,0x84,0x2f
};
__declspec(noinline) const char* lookup_localized_text(std::uint32_t hash, bool allow_missing) {
    auto& p = protection();
    const auto* result = p.localized_original(hash, allow_missing);
    auto& cap = native_b_capture();
    if (!cap.running.load(std::memory_order_acquire) || !result) return result;
    const auto& w = state_watch();
    const auto state = w.state.load(std::memory_order_acquire);
    if (state != 603 && state != 400 && state != 401 && state != 604 && state != 100)
        return result;
    LastError last_error;
    const auto now = GetTickCount64();
    const auto selector = w.selector.load(std::memory_order_acquire);
    if (selector < 0x10000 || now >= w.until.load(std::memory_order_acquire)) return result;
    const auto exit = cap.last_stall_exit_ms.load(std::memory_order_acquire);
    if (state == 100 && (exit == 0 || now < exit || now - exit > 3000)) return result;
    char text[96]{};
    const auto length = memory::peek_cstring(reinterpret_cast<std::uintptr_t>(result), text, sizeof(text));
    if (length <= 0) return result;
    char lowered[96]{};
    for (std::ptrdiff_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20u && c != '\t') return result;
        lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : text[i];
        // Keep CSV safe without affecting what the game actually returns.
        if (text[i] == ',' || text[i] == '"' || text[i] == '\t') text[i] = ' ';
    }
    // Bounded text filter. Component labels can be separate, hence both
    // single tokens (BOARD / LIP / STALL) and combined names are observed.
    if (!std::strstr(lowered, "board") && !std::strstr(lowered, "lip") &&
        !std::strstr(lowered, "stall") && !std::strstr(lowered, "rock") &&
        !std::strstr(lowered, "slide") && !std::strstr(lowered, "grind")) return result;
    NativeBTraceEvent e{};
    e.tick_ms = now;
    e.kind = 6;
    e.selector = selector;
    e.raw = state;
    e.chosen = state;
    e.localized_hash = hash;
    e.localized_caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::memcpy(e.localized_text, text, static_cast<std::size_t>(length) + 1u);
    native_b_append(e);

    // v1.4.37: The five PE-verified direct callers of Skate+0x33230B0
    // may fail to enter our optional copy-wrapper detour. When this getter
    // is called from Skate+0x33230C6, recover the wrapper's return address
    // WITHOUT another hook. From the verified wrapper prologue:
    //   push rdi; sub rsp, 0x20; call Skate+0x3322FD0
    // the wrapper's own caller return lies +0x30 bytes above the getter's
    // return-address slot. Only emit exact, independently verified callsites.
    if (e.localized_caller == p.base + 0x33230CBu) {
        constexpr std::uintptr_t outer_offset = 0x30;
        const auto return_slot = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
        std::uintptr_t outer_caller{};
        if (return_slot >= 0x10000 && return_slot < highest - outer_offset &&
            memory::peek(return_slot + outer_offset, outer_caller)) {
            const auto rva = outer_caller >= p.base ? outer_caller - p.base : 0;
            switch (rva) {
            case 0x2E1A3CDu: // CALL at Skate+0x2E1A3C8
            case 0x2E1ABF7u: // CALL at Skate+0x2E1ABF2
            case 0x2E1AC87u: // CALL at Skate+0x2E1AC82
            case 0x2E1ACD7u: // CALL at Skate+0x2E1ACD2
            case 0x2E1AE32u: // CALL at Skate+0x2E1AE2D
                e.kind = 7;
                e.localized_caller = outer_caller;
                native_b_append(e);

                // v1.4.39: The parent is a VIRTUAL method. Do not infer its
                // caller from a guessed stack slot; a dedicated, typed hook
                // below captures the callsite directly in kind-8 rows.
                break;
            default:
                break; // unknown stack layout; do not publish speculative caller
            }
        }
    }
    return result;
}

// v1.4.39/40: Observe virtual localization and its unwound call chain.
// Native code at Skate+0x2E1AC10 returns success in AL and has four parameters:
//   bool method(void* object, const char* key, char* buffer, uint32_t capacity).
// The game's original method writes the caller-supplied buffer. Observe it
// only AFTER calling the original; do not modify text, object or return value.
constexpr std::uintptr_t localization_key_rva = 0x2E1AC10;
constexpr std::array<unsigned char, 11> localization_key_prologue = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20
};
__declspec(noinline) bool resolve_localized_key(std::uintptr_t object, const char* key,
                                                char* output, std::uint32_t capacity) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto& p = protection();
    const bool success = p.localized_key_original(object, key, output, capacity);
    auto& cap = native_b_capture();
    // A running recorder is optional: keep the proven output-buffer rule active
    // across the entire session when the user explicitly enables persistent HUD.
    if (!success || !output || capacity == 0 ||
        (!cap.running.load(std::memory_order_acquire) &&
         !cap.hud_persistent.load(std::memory_order_acquire))) return success;
    const auto& w = state_watch();
    const auto state = w.state.load(std::memory_order_acquire);
    if (state != 603 && state != 400 && state != 401 && state != 604 && state != 100)
        return success;
    LastError last_error;
    const auto now = GetTickCount64();
    const auto selector = w.selector.load(std::memory_order_acquire);
    if (selector < 0x10000 || now >= w.until.load(std::memory_order_acquire)) return success;
    const auto exit = cap.last_stall_exit_ms.load(std::memory_order_acquire);
    if (state == 100 && (exit == 0 || now < exit || now - exit > 3000)) return success;
    char label[96]{};
    const auto len = memory::peek_cstring(reinterpret_cast<std::uintptr_t>(output), label,
                                          sizeof(label));
    if (len <= 0 || static_cast<std::uint32_t>(len) >= capacity) return success;
    char lower[96]{};
    for (std::ptrdiff_t i = 0; i < len; ++i) {
        const auto c = static_cast<unsigned char>(label[i]);
        if (c < 0x20u && c != '\t') return success;
        lower[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : label[i];
        if (c == ',' || c == '"' || c == '\t') label[i] = ' ';
    }
    if (!std::strstr(lower, "board") && !std::strstr(lower, "lip") &&
        !std::strstr(lower, "stall") && !std::strstr(lower, "slide") &&
        !std::strstr(lower, "rock") && !std::strstr(lower, "grind")) return success;
    NativeBTraceEvent e{};
    e.tick_ms = now;
    e.kind = 8; // v1.4.39: DIRECT virtual-method caller, not guessed stack frame.
    e.selector = selector;
    e.raw = state;
    e.chosen = state;
    e.localized_hash = 0; // Unknown at this boundary; do not invent a key hash.
    e.localized_caller = caller; // The direct caller, including ReSkate-owned frames.
    std::memcpy(e.localized_text, label, static_cast<std::size_t>(len) + 1u);
    // v1.4.40: The v1.4.39 direct caller was proven to belong to ReSkate.dll.
    // Capture UNWOUND return addresses, not guessed stack offsets. This runs
    // only after an accepted stall-related localized label, and never changes
    // any of the input arguments, game buffer, or returned result.
    std::array<void*, 12> unwound{};
    const USHORT frame_count = RtlCaptureStackBackTrace(
        0, static_cast<ULONG>(unwound.size()), unwound.data(), nullptr);
    e.localized_stack_count = static_cast<std::uint32_t>(frame_count);
    for (USHORT frame_index = 0; frame_index < frame_count; ++frame_index)
        e.localized_stack[frame_index] =
            reinterpret_cast<std::uintptr_t>(unwound[frame_index]);
    native_b_append(e);

    // v1.4.41/42: opt-in localized output buffer, session-long HUD OR capture trial.
    // Matching these generic labels alone does not establish stock HUD ownership.
    // Restrict the trial to the same three contiguous Skate.exe call stack frames
    // observed for Board Stall/Lip Stall in the verified v1.4.40 capture.
    // The source locale database, source key, and other game state are untouched.
    if ((cap.hud_persistent.load(std::memory_order_acquire) ||
         cap.trick_mode.load(std::memory_order_acquire) == 5) &&
        (state == 400 || state == 401) &&
        (std::strcmp(label, "Board Stall") == 0 ||
         std::strcmp(label, "Lip Stall") == 0)) {
        NativeBTraceEvent trial{};
        trial.tick_ms = now;
        trial.kind = 9;
        trial.selector = selector;
        trial.raw = state;
        trial.chosen = state;
        trial.localized_caller = caller;
        trial.localized_buffer_capacity = capacity;
        std::memcpy(trial.localized_text, label, static_cast<std::size_t>(len) + 1u);
        constexpr char replacement[] = "Rock to Fakie";
        const auto base = p.base;
        const bool verified_route = frame_count >= 5 && base >= 0x10000 &&
            base <= highest - 0x59C1241u &&
            reinterpret_cast<std::uintptr_t>(unwound[2]) == base + 0x758078u &&
            reinterpret_cast<std::uintptr_t>(unwound[3]) == base + 0x59C1241u &&
            reinterpret_cast<std::uintptr_t>(unwound[4]) == base + 0x16D2178u;
        if (!verified_route) {
            trial.localized_trial_status = 2; // unexpected native call route
        } else if (capacity < sizeof(replacement)) {
            trial.localized_trial_status = 3; // never truncate the native label
        } else {
            // The original method already wrote to this caller-owned writable
            // buffer; no global asset, native packet, or game physics is patched.
            std::memcpy(output, replacement, sizeof(replacement));
            trial.localized_trial_status = 1;
            cap.localized_trial_applied.fetch_add(1, std::memory_order_relaxed);
        }
        native_b_append(trial);
    }
    return success;
}

constexpr std::uintptr_t trick_record_rva = 0x47803A0;
constexpr std::array<unsigned char, 32> trick_record_prologue = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xec, 0x20, 0x80, 0x7a, 0x1c, 0x00, 0x48, 0x8b,
    0xda, 0x48, 0x8b, 0xf9, 0x74, 0x75, 0x81, 0x7a,
    0x0c, 0x50, 0x01, 0x00, 0x00, 0x77, 0x6c, 0x48
};
void record_trick_event(std::uintptr_t recorder, std::uintptr_t packet) {
    auto& p = protection();
    auto& cap = native_b_capture();
    const auto producer_callsite = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // Stack storage persists until the original function has returned. The
    // verified native implementation copies the whole packet into its history.
    std::array<unsigned char, 0x68> trial_packet{};
    bool remap = false;
    std::uint8_t remap_phase = 0; // 1=stall entry/hold, 2=verified B-triggered exit
    if (cap.running.load(std::memory_order_acquire) ||
        cap.hud_persistent.load(std::memory_order_acquire)) {
        {
            LastError previous_error;
            const auto now = GetTickCount64();
            const auto& w = state_watch();
            const auto current = w.state.load(std::memory_order_acquire);
            const auto previous = w.previous.load(std::memory_order_relaxed);
            const auto selector = w.selector.load(std::memory_order_acquire);
            const auto exit_at = cap.last_stall_exit_ms.load(std::memory_order_acquire);
            const bool in_stall = current == 603 || current == 400 || current == 401 || current == 604;
            const bool after_stall = current == 100 && exit_at != 0 &&
                now >= exit_at && now - exit_at <= 5000;
            if ((in_stall || after_stall) && now < w.until.load(std::memory_order_acquire)) {
                std::uint8_t valid{};
                std::uint32_t id{};
                if (packet >= 0x10000 && packet < highest - 0x68 &&
                    memory::peek(packet + 0x1c, valid) && valid != 0 &&
                    memory::peek(packet + 0x0c, id) && id <= 0x150) {
                    std::uint32_t effective = id;
                    const auto mode = cap.hud_persistent.load(std::memory_order_acquire)
                        ? 5u : cap.trick_mode.load(std::memory_order_acquire);
                    const auto b_at = cap.last_stall_b_ms.load(std::memory_order_acquire);
                    // ID 9=bs_board, ID 12=bs_lip. Both may be classified
                    // during Board Stall entry/hold BEFORE the physical B edge.
                    // Therefore allow them only while our watched local selector
                    // reports 603/400/401, OR at the tightly verified B exit.
                    // We have NOT established that the native HUD consumes this
                    // recorder copy; do not claim the onscreen label changed.
                    const bool candidate = id == 9u || id == 12u;
                    const bool stall_entry_or_hold =
                        current == 603 || current == 400 || current == 401;
                    const bool verified_b_exit =
                        current == 604 && (previous == 400 || previous == 401) &&
                        cap.last_stall_b_selector.load(std::memory_order_acquire) == selector &&
                        b_at != 0 && now >= b_at && now - b_at <= 125 &&
                        exit_at != 0 && now >= exit_at && now - exit_at <= 100;
                    if (candidate && mode >= 1 && mode <= 5 && selector != 0 &&
                        ((mode == 4 || mode == 5) ? verified_b_exit :
                         (stall_entry_or_hold || verified_b_exit)) &&
                        memory::peek(packet, trial_packet)) {
                        effective = (mode == 1 || mode == 4 || mode == 5) ? 187u :
                                    (mode == 2 ? 185u : 186u);
                        std::memcpy(trial_packet.data() + 0x0c, &effective, sizeof(effective));
                        remap = true;
                        remap_phase = stall_entry_or_hold ? 1u : 2u;
                    }
                    NativeBTraceEvent e{};
                    e.tick_ms = now;
                    e.kind = 4;
                    e.selector = selector;
                    e.raw = current; e.chosen = current;
                    e.previous = previous;
                    e.trick_recorder = recorder;
                    e.trick_packet = packet;
                    e.trick_id = id;
                    e.trick_valid = valid;
                    e.trick_effective_id = effective;
                    e.trick_remapped = remap ? 1 : 0;
                    e.trick_remap_phase = remap_phase;
                    e.trick_callsite = producer_callsite;
                    // Proven original call: Skate+0x4781B73 invokes this
                    // recorder with RDX = producer+0xA50; return RIP is
                    // Skate+0x4781B78. The same producer also holds other
                    // distinct event packets. Capture header IDs, never edit.
                    constexpr std::uintptr_t original_caller_rva = 0x4781B78;
                    constexpr std::uintptr_t source_offset = 0xA50;
                    const auto base = p.base;
                    if (base != 0 && base <= highest - original_caller_rva &&
                        producer_callsite == base + original_caller_rva &&
                        packet >= 0x10000 + source_offset &&
                        packet < highest - 0xD00) {
                        const auto producer = packet - source_offset;
                        e.producer = producer;
                        // Only inspect the event headers at the five producer
                        // offsets evidenced by native disassembly. Each is
                        // validated independently to avoid treating unrelated
                        // fields or stale events as an ID.
                        const auto capture_id = [producer](std::uintptr_t off,
                            std::uint32_t& id_out) noexcept -> bool {
                            std::uint8_t is_valid{};
                            std::uint32_t id_value{};
                            if (!memory::peek(producer + off + 0x1C, is_valid) ||
                                !is_valid ||
                                !memory::peek(producer + off + 0x0C, id_value) ||
                                id_value > 0x150) return false;
                            id_out = id_value;
                            return true;
                        };
                        if (capture_id(0xA50, e.source_a50_id)) e.producer_valid_mask |= 1u;
                        if (capture_id(0xAC0, e.source_ac0_id)) e.producer_valid_mask |= 2u;
                        if (capture_id(0xA58, e.source_a58_id)) e.producer_valid_mask |= 4u;
                        if (capture_id(0xB90, e.source_b90_id)) e.producer_valid_mask |= 8u;
                        if (capture_id(0xB28, e.source_b28_id)) e.producer_valid_mask |= 16u;
                        (void)memory::peek(producer + 0xCA0, e.producer_ca0);
                        (void)memory::peek(producer + 0xA54, e.producer_a54);
                        (void)memory::peek(producer + 0xB20, e.producer_b20);
                    }
                    native_b_append(e);
                }
            }
        }
    }
    if (remap) {
        p.trick_original(recorder, reinterpret_cast<std::uintptr_t>(trial_packet.data()));
        cap.trick_remaps.fetch_add(1, std::memory_order_relaxed);
        // Mode 4 already requires a verified local B-triggered exit into 604
        // for recorder remaps. Show a ReSkate label for this exact event only.
        if (remap_phase == 2 && cap.trick_mode.load(std::memory_order_acquire) == 4)
            cap.rock_label_since_ms.store(GetTickCount64(), std::memory_order_release);
    } else {
        p.trick_original(recorder, packet);
    }
}

void record_cause(std::uintptr_t causes, std::int32_t reason, float magnitude) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool protect = suppress_cause(causes, reason, caller);
    // Do not force the native recovery/stumble predicate (recovery_predicate). Its
    // result is also exported to animation at +0x9e, even without a collision.
    // Stop new causes before they reach either the wipeout or runout decision;
    // the native per-step reset still owns clearing the collector's history.
    if (!protect) protection().cause_original(causes, reason, magnitude);
}
// The local skater waiting on foot for its S.K.A.T.E. turn: its mount request goes.
void hold_off_board(std::uintptr_t selector, std::uint32_t current) noexcept {
    auto& p = protection();
    if (current != offboard_physics_state || !p.mount_test || !p.ready.load(std::memory_order_acquire)) return;
    AcquireSRWLockShared(&p.lock);
    const auto board = p.board;
    ReleaseSRWLockShared(&p.lock);
    if (GetTickCount64() >= board.until || selector != board.owner.selector) return;
    Owner current_owner;
    if (!resolve(board.owner.client, board.owner.entity, current_owner) || current_owner != board.owner) return;
    (void)cancel_request(current_owner.context, animation_request_offset, mount_request_mask);
}
std::uint32_t choose_state(std::uintptr_t selector, std::uint32_t current) {
    {
        LastError error;
        hold_off_board(selector, current);
    }
    // Clear shared requests before selection so native ground/air/walking
    // transitions can still run. Some contact tests return Wipeout directly;
    // retain the current state only for that result, never ordinary Offboard.
    const bool filtered = filter_requests(selector, &Owner::selector);
    const auto next = protection().choose_original(selector, current);
    LastError error;
    const auto chosen = filtered && next == wipeout_physics_state && protected_owner(selector, &Owner::selector) ? current : next;
    auto& w = state_watch();
    const auto tick = GetTickCount64();
    if (selector == w.selector.load(std::memory_order_acquire) && tick < w.until.load(std::memory_order_acquire)) {
        const auto before = w.state.exchange(chosen, std::memory_order_acq_rel);
        if (before != chosen) {
            if (chosen == 604 && (before == 400 || before == 401))
                native_b_capture().last_stall_exit_ms.store(tick, std::memory_order_relaxed);
            if (native_b_capture().running.load(std::memory_order_relaxed) &&
                (chosen == 603 || chosen == 400 || chosen == 401 || chosen == 604 ||
                 (chosen == 100 && (before == 604 || before == 400 || before == 401))))
                native_b_append(native_b_snapshot(1, selector, current, chosen, before, 0, 0));
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            w.previous.store(before, std::memory_order_relaxed);
            w.previous_ticks.store(now.QuadPart - w.since.exchange(now.QuadPart, std::memory_order_relaxed), std::memory_order_relaxed);
            w.changes.fetch_add(1, std::memory_order_relaxed);
            if (chosen == wipeout_physics_state) w.wipeouts.fetch_add(1, std::memory_order_relaxed);
        } else if ((chosen == 400 || chosen == 401) &&
                   native_b_capture().running.load(std::memory_order_relaxed)) {
            // v1.4.15: downsample unchanged hold frames to ~40 Hz. No debugger,
            // new hooks, blocking waits, I/O, or writes to game state.
            // A CAS ensures one sample per interval across physics threads.
            auto& cap = native_b_capture();
            auto prior = cap.last_hold_ms.load(std::memory_order_relaxed);
            if (tick - prior >= 25 &&
                cap.last_hold_ms.compare_exchange_strong(prior, tick,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                native_b_append(native_b_snapshot(3, selector, current, chosen, before, 0, 0));
        }
    }
    return chosen;
}
void skeleton_response(std::uintptr_t rig, float seconds, bool wipeout) {
    // The state post-update can raise another request after the selector ran.
    // Filter at this consumer, then let native constraints and recovery run.
    if (filter_requests(rig, &Owner::rig)) wipeout = false;
    if (const auto observer = protection().step_observer.load(std::memory_order_acquire)) {
        LastError error;
        NoBailSkater skater;
        if (published_skater(rig, skater)) observer(skater, seconds, wipeout);
    }
    protection().skeleton_original(rig, seconds, wipeout);
}
bool clear_contact_output(std::uintptr_t contacts) noexcept {
    if (contacts < 0x10000 || contacts > highest - body_contact_output_offset) return false;
    __try {
        _InterlockedExchange8(reinterpret_cast<volatile char*>(contacts + body_contact_output_offset), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void filter_contact_animation(std::uintptr_t core) noexcept {
    LastError error;
    Owner owner;
    std::uint32_t state{};
    if (!protected_owner(core, &Owner::core, &owner) || !read(owner.context + 0x1414, state)) return;
    // Ground/air and grind state families. Offboard, mounting, handplants and
    // existing ragdolls retain their native contact reporting and recovery.
    if (!((state >= 100 && state < 300) || (state >= 400 && state < 500))) return;
    const auto contacts = pointer(pointer(core, 0x3b8), 0x30);
    std::uint8_t contact{};
    if (!contacts || !read(contacts + body_contact_output_offset, contact) || contact > 1) return;
    // Animation gets this sensitive-body hit independently of the ordinary
    // wipeout output and cause collector. Filter it after native publication,
    // before the animation contact context copies it. Keep the general contact
    // latch, collision timers, per-bone records, impulses and physics state.
    if (clear_contact_output(contacts))
        (void)cancel_request(owner.context, body_contact_context_offset, body_contact_context_mask);
}
void publish_animation(std::uintptr_t core) {
    // Animation also reads requests without consulting the cause collector.
    // Clear flags and pending causes before native publication, including
    // its early recovery query and cause export. Leave the native query result
    // unchanged: forcing it true used to put the skater into a stumbling state.
    (void)filter_requests(core, &Owner::core);
    protection().publish_original(core);
    filter_contact_animation(core);
}
bool compatible(std::uintptr_t base) noexcept {
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.SizeOfImage != supported_build::game_image_size)
        return false;
    std::array<unsigned char, reset_bail_causes_code.size()> reset_code{};
    if (!read(base + reset_bail_causes_rva, reset_code) || reset_code != reset_bail_causes_code) return false;
    for (const auto& contract : {record_bail_cause_contract, choose_physics_state_contract,
            bail_animation_caller_contract, bail_state_caller_contract, bail_skeleton_contract, bail_publish_contract}) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : bail_consumer_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : body_contact_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& impact : impact_bail_calls) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + impact.caller.rva, actual) || actual != impact.caller.bytes) return false;
    }
    return true;
}
}
bool start_no_bail(std::uintptr_t base) noexcept {
    LastError error;
    try {
        auto& p = protection();
        if (p.ready.load()) return p.base == base;
        if (!compatible(base)) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "No Bail is unavailable: native bail contract did not match.");
            return false;
        }
        p.base = base;
        p.reset_causes = reinterpret_cast<ResetCauses>(base + reset_bail_causes_rva);
        std::array<unsigned char, 32> mount_test{};
        p.mount_test = read(base + mount_request_test_contract.rva, mount_test) &&
                       mount_test == mount_request_test_contract.bytes;
        const std::array targets{
            reinterpret_cast<void*>(base + record_bail_cause_contract.rva),
            reinterpret_cast<void*>(base + choose_physics_state_contract.rva),
            reinterpret_cast<void*>(base + bail_skeleton_contract.rva),
            reinterpret_cast<void*>(base + bail_publish_contract.rva)};
        const std::array replacements{
            reinterpret_cast<void*>(&record_cause), reinterpret_cast<void*>(&choose_state),
            reinterpret_cast<void*>(&skeleton_response), reinterpret_cast<void*>(&publish_animation)};
        std::array<void*, 4> originals{};
        auto status = HookOk;
        std::size_t prepared{};
        for (; prepared < targets.size(); ++prepared) {
            status = hook_prepare(targets[prepared], replacements[prepared], &originals[prepared]);
            if (status != HookOk) break;
            if (!originals[prepared]) { ++prepared; status = HookUnsupportedFunction; break; }
        }
        if (status == HookOk) {
            // Publish every relay before enabling any target.
            p.cause_original = reinterpret_cast<RecordCause>(originals[0]);
            p.choose_original = reinterpret_cast<ChooseState>(originals[1]);
            p.skeleton_original = reinterpret_cast<SkeletonResponse>(originals[2]);
            p.publish_original = reinterpret_cast<PublishAnimation>(originals[3]);
            for (auto target : targets) {
                status = hook_enable(target);
                if (status != HookOk) break;
            }
            if (status == HookOk) {
                // Optional diagnostic detour: only this exact executable prologue.
                // Failure does not disable the unrelated No Bail hooks.
                std::array<unsigned char, trick_record_prologue.size()> trick_actual{};
                if (read(base + trick_record_rva, trick_actual) &&
                    trick_actual == trick_record_prologue) {
                    void* native_trick_original{};
                    void* trick_target = reinterpret_cast<void*>(base + trick_record_rva);
                    const auto hook_status = hook_prepare(
                        trick_target, reinterpret_cast<void*>(&record_trick_event),
                        &native_trick_original);
                    if (hook_status == HookOk && native_trick_original) {
                        p.trick_original = reinterpret_cast<RecordTrickEvent>(native_trick_original);
                        if (hook_enable(trick_target) == HookOk)
                            p.trick_hook_ready.store(true, std::memory_order_release);
                        else (void)hook_remove(trick_target);
                    } else if (hook_status == HookOk) {
                        (void)hook_remove(trick_target);
                    }
                }
                // Optional small lookup hook, independent of recording path.
                // If the hook engine cannot relocate it, retain stock behavior.
                std::array<unsigned char, trick_name_prologue.size()> name_actual{};
                if (read(base + trick_name_rva, name_actual) &&
                    name_actual == trick_name_prologue) {
                    void* native_name_original{};
                    void* name_target = reinterpret_cast<void*>(base + trick_name_rva);
                    const auto name_status = hook_prepare(
                        name_target, reinterpret_cast<void*>(&lookup_trick_name),
                        &native_name_original);
                    if (name_status == HookOk && native_name_original) {
                        p.trick_name_original = reinterpret_cast<LookupTrickName>(native_name_original);
                        if (hook_enable(name_target) == HookOk)
                            p.trick_name_hook_ready.store(true, std::memory_order_release);
                        else (void)hook_remove(name_target);
                    } else if (name_status == HookOk) {
                        (void)hook_remove(name_target);
                    }
                }
                // Optional localization observation. Disabled outside native
                // recording, and best-effort: if unsupported, game is unchanged.
                std::array<unsigned char, localization_get_prologue.size()> localized_actual{};
                if (read(base + localization_get_rva, localized_actual) &&
                    localized_actual == localization_get_prologue) {
                    void* native_localized_original{};
                    void* localized_target = reinterpret_cast<void*>(base + localization_get_rva);
                    const auto localized_status = hook_prepare(
                        localized_target, reinterpret_cast<void*>(&lookup_localized_text),
                        &native_localized_original);
                    if (localized_status == HookOk && native_localized_original) {
                        p.localized_original = reinterpret_cast<LookupLocalizedText>(native_localized_original);
                        if (hook_enable(localized_target) == HookOk)
                            p.localization_hook_ready.store(true, std::memory_order_release);
                        else (void)hook_remove(localized_target);
                    } else if (localized_status == HookOk) {
                        (void)hook_remove(localized_target);
                    }
                }
                // Observe the actual virtual-method caller, without stack-slot
                // guesses. Unlike Skate+0x33230B0 (which returns VOID), this
                // native function returns a success flag in AL.
                std::array<unsigned char, localization_key_prologue.size()> key_actual{};
                if (read(base + localization_key_rva, key_actual) &&
                    key_actual == localization_key_prologue) {
                    void* original_key{};
                    void* key_target = reinterpret_cast<void*>(base + localization_key_rva);
                    const auto key_status = hook_prepare(
                        key_target, reinterpret_cast<void*>(&resolve_localized_key),
                        &original_key);
                    if (key_status == HookOk && original_key) {
                        p.localized_key_original = reinterpret_cast<ResolveLocalizedKey>(original_key);
                        if (hook_enable(key_target) == HookOk)
                            p.localization_key_hook_ready.store(true, std::memory_order_release);
                        else (void)hook_remove(key_target);
                    } else if (key_status == HookOk) {
                        (void)hook_remove(key_target);
                    }
                }
                p.ready.store(true, std::memory_order_release);
                logging::write(logging::Level::info, logging::Channel::skater,
                    "No Bail ready: collision and landing causes filtered before physics/animation consume them; noclip is protected.");
                return true;
            }
        }
        // Published relays remain callable even if Detours reports an uncertain
        // attach result. With ready=false any remaining hook simply forwards.
        logging::log(logging::Level::warning, logging::Channel::skater,
            "No Bail hook setup failed (status {}); protected noclip is unavailable.", static_cast<LONG>(status));
        while (prepared) (void)hook_remove(targets[--prepared]);
    } catch (...) {}
    return false;
}
bool no_bail_available() noexcept { return protection().ready.load(std::memory_order_acquire); }

// Capture APIs are called by Trainer on its game thread. No additional Detours.
bool native_b_trace_start() noexcept {
    if (!protection().ready.load(std::memory_order_acquire) ||
        !protection().trick_hook_ready.load(std::memory_order_acquire)) return false;
    auto& cap = native_b_capture();
    if (cap.running.load(std::memory_order_acquire)) return false;
    AcquireSRWLockExclusive(&cap.lock);
    cap.count = 0;
    cap.dropped.store(0, std::memory_order_relaxed);
    cap.last_hold_ms.store(0, std::memory_order_relaxed);
    cap.last_stall_exit_ms.store(0, std::memory_order_relaxed);
    cap.last_stall_b_ms.store(0, std::memory_order_relaxed);
    cap.last_stall_b_selector.store(0, std::memory_order_relaxed);
    cap.trick_remaps.store(0, std::memory_order_relaxed);
    cap.localized_trial_applied.store(0, std::memory_order_relaxed);
    cap.rock_label_since_ms.store(0, std::memory_order_release);
    cap.trick_mode.store(0, std::memory_order_release);
    cap.running.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&cap.lock);
    return true;
}
NativeBTraceStats native_b_trace_stats() noexcept {
    auto& cap = native_b_capture();
    AcquireSRWLockShared(&cap.lock);
    NativeBTraceStats result{cap.running.load(std::memory_order_relaxed),
        static_cast<std::uint32_t>(cap.count), cap.dropped.load(std::memory_order_relaxed),
        protection().trick_hook_ready.load(std::memory_order_relaxed),
        protection().trick_name_hook_ready.load(std::memory_order_relaxed),
        protection().localization_hook_ready.load(std::memory_order_relaxed),
        protection().localization_key_hook_ready.load(std::memory_order_relaxed),
        cap.trick_mode.load(std::memory_order_relaxed),
        cap.trick_remaps.load(std::memory_order_relaxed),
        cap.localized_trial_applied.load(std::memory_order_relaxed)};
    ReleaseSRWLockShared(&cap.lock);
    return result;
}
void native_b_trace_controller_edge(std::uint32_t state, std::uint32_t buttons,
                                    std::uint32_t device) noexcept {
    if (!native_b_capture().running.load(std::memory_order_acquire) &&
        !native_b_capture().hud_persistent.load(std::memory_order_acquire)) return;
    const auto& w = state_watch();
    const auto selector = w.selector.load(std::memory_order_acquire);
    const auto tick = GetTickCount64();
    if (selector < 0x10000 || tick >= w.until.load(std::memory_order_acquire)) return;
    if ((state == 400 || state == 401) && (buttons & 0x2000u) != 0 &&
        w.state.load(std::memory_order_acquire) == state) {
        auto& cap = native_b_capture();
        cap.last_stall_b_selector.store(selector, std::memory_order_release);
        cap.last_stall_b_ms.store(tick, std::memory_order_release);
    }
    // No snapshot/CSV collection outside an explicit recording.
    if (native_b_capture().running.load(std::memory_order_acquire))
        native_b_append(native_b_snapshot(2, selector, state, state, state, buttons, device));
}
// UINT32_MAX means hidden; age 0..2299 ms means show the ReSkate label.
// Only reads atomics: safe to call on the DX12 render thread.
std::uint32_t native_rock_popup_age_ms() noexcept {
    const auto& cap = native_b_capture();
    if (!cap.running.load(std::memory_order_acquire) ||
        cap.trick_mode.load(std::memory_order_acquire) != 4) return 0xffffffffu;
    const auto since = cap.rock_label_since_ms.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    if (since == 0 || now < since || now - since >= 2300) return 0xffffffffu;
    return static_cast<std::uint32_t>(now - since);
}

bool native_rock_hud_set(bool enabled) noexcept {
    auto& cap = native_b_capture();
    if (enabled && (!protection().ready.load(std::memory_order_acquire) ||
                    !protection().trick_hook_ready.load(std::memory_order_acquire) ||
                    !protection().localization_key_hook_ready.load(std::memory_order_acquire)))
        return false;
    cap.hud_persistent.store(enabled, std::memory_order_release);
    // A previous B edge must never arm a fresh session of the persistent mode.
    cap.last_stall_b_ms.store(0, std::memory_order_release);
    cap.last_stall_b_selector.store(0, std::memory_order_release);
    return true;
}
bool native_rock_hud_enabled() noexcept {
    return native_b_capture().hud_persistent.load(std::memory_order_acquire);
}

bool native_b_trick_mode_set(std::uint32_t mode) noexcept {
    auto& cap = native_b_capture();
    if (mode > 5 || (mode != 0 &&
        (!cap.running.load(std::memory_order_acquire) ||
         !protection().trick_hook_ready.load(std::memory_order_acquire) ||
         (mode == 4 && !protection().trick_name_hook_ready.load(std::memory_order_acquire)) ||
         (mode == 5 && !protection().localization_key_hook_ready.load(std::memory_order_acquire)))))
        return false;
    cap.trick_mode.store(mode, std::memory_order_release);
    if (mode != 4) cap.rock_label_since_ms.store(0, std::memory_order_release);
    return true;
}
std::vector<NativeBTraceEvent> native_b_trace_stop() noexcept {
    auto& cap = native_b_capture();
    cap.trick_mode.store(0, std::memory_order_release);
    cap.rock_label_since_ms.store(0, std::memory_order_release);
    cap.running.store(false, std::memory_order_release);
    std::vector<NativeBTraceEvent> rows;
    AcquireSRWLockExclusive(&cap.lock);
    try { rows.assign(cap.events.begin(), cap.events.begin() + cap.count); }
    catch (...) { rows.clear(); }
    cap.count = 0;
    ReleaseSRWLockExclusive(&cap.lock);
    return rows;
}

bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept {
    LastError error;
    auto& p = protection();
    Lease next;
    const bool available = p.ready.load(std::memory_order_acquire) && resolve(client, entity, next.owner);
    if (available) {
        const auto now = GetTickCount64();
        next.manual_until = manual ? now + 500 : 0;
        next.flight_until = flying ? flight_expires : 0;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.lease = available ? next : Lease{};
    ReleaseSRWLockExclusive(&p.lock);
    return available;
}
bool set_teleport_on_board(std::uintptr_t component) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(component + 0xc0) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept {
    LastError error;
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return;
    static bool rearm{}; // client tick only
    if (locked) {
        // The owner does not resolve while a teleport is under way: the last one stands
        // until it expires.
        BoardLock next;
        if (resolve(client, entity, next.owner)) {
            next.until = GetTickCount64() + 500;
            AcquireSRWLockExclusive(&p.lock);
            p.board = next;
            ReleaseSRWLockExclusive(&p.lock);
        }
        rearm = true;
        return;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.board = {};
    ReleaseSRWLockExclusive(&p.lock);
    // Released: teleports that keep the skater's own choice (the SDK's /tp) put it on the
    // board again. Once the skater resolves, outside a teleport.
    Owner owner;
    if (rearm && resolve(client, entity, owner) && set_teleport_on_board(owner.component)) rearm = false;
}
void clear_no_bail() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease = {};
    ReleaseSRWLockExclusive(&p.lock);
}
bool no_bail_skater(NoBailSkater& skater) noexcept { return published_skater(0, skater); }
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept {
    protection().step_observer.store(observer, std::memory_order_release);
}
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept {
    auto& w = state_watch();
    Owner owner;
    if (!protection().ready.load(std::memory_order_acquire) || !resolve(client, entity, owner)) return;
    if (w.selector.exchange(owner.selector, std::memory_order_acq_rel) != owner.selector)
        w.state.store(0, std::memory_order_release);
    w.until.store(GetTickCount64() + 500, std::memory_order_release);
}
PhysicsStateWatch watched_physics_state() noexcept {
    auto& w = state_watch();
    PhysicsStateWatch result;
    result.valid = protection().ready.load(std::memory_order_acquire) && w.selector.load(std::memory_order_acquire) &&
        GetTickCount64() < w.until.load(std::memory_order_acquire);
    result.state = w.state.load(std::memory_order_acquire);
    result.previous = w.previous.load(std::memory_order_relaxed);
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    result.previous_seconds = static_cast<float>(static_cast<double>(w.previous_ticks.load(std::memory_order_relaxed)) / frequency);
    result.changes = w.changes.load(std::memory_order_relaxed);
    result.wipeouts = w.wipeouts.load(std::memory_order_relaxed);
    return result;
}
void clear_no_bail_flight() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease.flight_until = 0;
    ReleaseSRWLockExclusive(&p.lock);
}
}
