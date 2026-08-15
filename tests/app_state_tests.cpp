#include "app_state.h"
#include "decoder.h"
#include "open_error.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool nearly_equal(float left, float right, float tolerance = 0.01f) {
    return std::abs(left - right) <= tolerance;
}

struct FakeComicAppPort {
    bool enabled_value = true;
    mv::ComicAppAutoOwner owner_value = mv::ComicAppAutoOwner::None;
    int speed_value = 1;
    bool toggle_result = true;
    bool middle_start_result = true;
    bool capture_result = true;
    bool timer_value = false;
    bool timer_start_result = true;
    bool transient_value = false;
    float advance_result = 0.0f;
    bool stop_owner_on_advance = false;
    std::vector<std::string> calls;

    bool enabled() const { return enabled_value; }
    mv::ComicAppAutoOwner owner() const { return owner_value; }
    int speed_index() const { return speed_value; }
    bool timer_running() const { return timer_value; }
    bool transient_visible() const { return transient_value; }

    bool toggle_cruise() {
        calls.push_back("toggle_cruise");
        if (owner_value == mv::ComicAppAutoOwner::Cruise) {
            owner_value = mv::ComicAppAutoOwner::None;
            return false;
        }
        owner_value = toggle_result
            ? mv::ComicAppAutoOwner::Cruise
            : mv::ComicAppAutoOwner::None;
        return toggle_result;
    }

    void set_speed(int speed) {
        speed_value = std::clamp(speed, 0, 3);
        calls.push_back("set_speed:" + std::to_string(speed_value));
    }

    void show_cruise_status(mv::ComicAppCruiseStatus status) {
        transient_value = true;
        const char* name = status == mv::ComicAppCruiseStatus::Speed
            ? "speed"
            : (status == mv::ComicAppCruiseStatus::Paused
                ? "paused" : "boundary");
        calls.push_back(std::string("status:") + name);
    }

    void begin_tick_clock() { calls.push_back("begin_clock"); }

    bool start_middle(float anchor_x, float anchor_y, float pointer_x, float pointer_y) {
        calls.push_back("start_middle:" + std::to_string(static_cast<int>(anchor_x))
            + "," + std::to_string(static_cast<int>(anchor_y))
            + "," + std::to_string(static_cast<int>(pointer_x))
            + "," + std::to_string(static_cast<int>(pointer_y)));
        if (middle_start_result) owner_value = mv::ComicAppAutoOwner::Middle;
        return middle_start_result;
    }

    bool acquire_middle_capture() {
        calls.push_back("acquire_capture");
        return capture_result;
    }

    void release_middle_capture() { calls.push_back("release_capture"); }

    void set_middle_cursor(bool active) {
        calls.push_back(active ? "cursor:on" : "cursor:off");
    }

    void cancel_auto_scroll(mv::ComicAppCancelTrigger trigger) {
        const char* name = "invalid";
        switch (trigger) {
        case mv::ComicAppCancelTrigger::ManualInput: name = "manual"; break;
        case mv::ComicAppCancelTrigger::Scrollbar: name = "scrollbar"; break;
        case mv::ComicAppCancelTrigger::RepeatedMiddleClick: name = "repeated"; break;
        case mv::ComicAppCancelTrigger::LeftButton: name = "left"; break;
        case mv::ComicAppCancelTrigger::Escape: name = "escape"; break;
        case mv::ComicAppCancelTrigger::KeyboardPage: name = "keyboard"; break;
        case mv::ComicAppCancelTrigger::MouseWheel: name = "wheel"; break;
        case mv::ComicAppCancelTrigger::FocusLost: name = "focus"; break;
        case mv::ComicAppCancelTrigger::ExitMode: name = "exit"; break;
        case mv::ComicAppCancelTrigger::EmptyBook: name = "empty"; break;
        case mv::ComicAppCancelTrigger::ViewportChanged: name = "viewport"; break;
        case mv::ComicAppCancelTrigger::InvalidInput: name = "invalid"; break;
        }
        calls.push_back(std::string("cancel:") + name);
        owner_value = mv::ComicAppAutoOwner::None;
    }

    float advance_cruise(float) {
        calls.push_back("advance_cruise");
        if (stop_owner_on_advance) owner_value = mv::ComicAppAutoOwner::None;
        return advance_result;
    }

    float advance_middle(float) {
        calls.push_back("advance_middle");
        if (stop_owner_on_advance) owner_value = mv::ComicAppAutoOwner::None;
        return advance_result;
    }

    void sync_page() { calls.push_back("sync_page"); }
    void request_pages() { calls.push_back("request_pages"); }

    void clear_status_transient() {
        calls.push_back("clear_status");
        transient_value = false;
    }

    void clear_all_transient() {
        calls.push_back("clear_transient");
        transient_value = false;
    }

    bool start_timer() {
        calls.push_back("start_timer");
        timer_value = timer_start_result;
        return timer_start_result;
    }

    void stop_timer() {
        calls.push_back("stop_timer");
        timer_value = false;
    }

    void invalidate() { calls.push_back("invalidate"); }
};

void test_comic_app_controller() {
    expect(mv::kComicAppTimerIntervalMs == 16
            && mv::kComicAppTransientDurationMs == 1000,
        "production comic timing must use a 16 ms tick and 1000 ms transient");
    const std::vector<std::string> cruise_start = {
        "toggle_cruise", "begin_clock", "status:speed",
        "start_timer", "invalidate"};
    FakeComicAppPort keyboard;
    expect(mv::ComicAppController::dispatch_command(
            keyboard, mv::ComicAppCommand::ToggleCruise)
            && keyboard.calls == cruise_start,
        "P must drive the production cruise transition and timer order");
    FakeComicAppPort menu;
    expect(mv::ComicAppController::dispatch_command(
            menu, mv::ComicAppCommand::ToggleCruise)
            && menu.calls == cruise_start,
        "the View menu must drive the same production cruise transition as P");

    FakeComicAppPort disabled;
    disabled.enabled_value = false;
    expect(!mv::ComicAppController::dispatch_command(
            disabled, mv::ComicAppCommand::ToggleCruise)
            && disabled.calls.empty(),
        "comic commands outside comic mode must fail closed without effects");

    FakeComicAppPort speed;
    expect(mv::ComicAppController::dispatch_command(
            speed, mv::ComicAppCommand::SetSpeed20)
            && speed.speed_value == 3,
        "the 2.0x menu command must select the exact production tier");
    speed.calls.clear();
    expect(mv::ComicAppController::dispatch_command(
            speed, mv::ComicAppCommand::IncreaseSpeed)
            && speed.speed_value == 3
            && speed.calls.front() == "set_speed:3",
        "] must clamp at the highest production speed tier");
    speed.calls.clear();
    expect(mv::ComicAppController::dispatch_command(
            speed, mv::ComicAppCommand::DecreaseSpeed)
            && speed.speed_value == 2
            && speed.calls.front() == "set_speed:2",
        "[ must decrement exactly one production speed tier");

    FakeComicAppPort middle;
    expect(mv::ComicAppController::start_middle(
            middle, 10.0f, 20.0f, 10.0f, 20.0f, true)
            && middle.owner_value == mv::ComicAppAutoOwner::Middle
            && middle.calls == std::vector<std::string>({
                "start_middle:10,20,10,20", "clear_status", "begin_clock",
                "acquire_capture", "cursor:on", "start_timer", "invalidate"}),
        "a valid middle click must start, capture, arm timer, then redraw in order");
    middle.calls.clear();
    expect(mv::ComicAppController::start_middle(
            middle, 10.0f, 20.0f, 10.0f, 20.0f, true)
            && middle.owner_value == mv::ComicAppAutoOwner::None
            && middle.calls == std::vector<std::string>({
                "cancel:repeated", "clear_status", "release_capture",
                "stop_timer", "cursor:off", "invalidate"}),
        "a repeated middle click must cancel capture and timer in production order");

    FakeComicAppPort outside;
    expect(!mv::ComicAppController::start_middle(
            outside, 1.0f, 1.0f, 1.0f, 1.0f, false)
            && outside.calls.empty(),
        "a middle anchor outside the Renderer layout must not acquire resources");

    FakeComicAppPort capture_failure;
    capture_failure.capture_result = false;
    expect(!mv::ComicAppController::start_middle(
            capture_failure, 10.0f, 20.0f, 10.0f, 20.0f, true)
            && capture_failure.owner_value == mv::ComicAppAutoOwner::None
            && capture_failure.calls == std::vector<std::string>({
                "start_middle:10,20,10,20", "clear_status", "begin_clock",
                "acquire_capture", "cancel:invalid", "clear_status",
                "release_capture", "stop_timer", "cursor:off", "invalidate"}),
        "capture failure must roll back the active middle owner before returning");

    FakeComicAppPort cruise_tick;
    cruise_tick.owner_value = mv::ComicAppAutoOwner::Cruise;
    cruise_tick.timer_value = true;
    cruise_tick.advance_result = 12.0f;
    expect(mv::ComicAppController::timer_tick(
            cruise_tick, 0.016f, false)
            && cruise_tick.calls == std::vector<std::string>({
                "advance_cruise", "sync_page", "request_pages", "invalidate"}),
        "a cruise timer tick must advance before synchronizing and requesting pages");

    FakeComicAppPort middle_boundary;
    middle_boundary.owner_value = mv::ComicAppAutoOwner::Middle;
    middle_boundary.timer_value = true;
    middle_boundary.stop_owner_on_advance = true;
    expect(mv::ComicAppController::timer_tick(
            middle_boundary, 0.016f, false)
            && middle_boundary.calls == std::vector<std::string>({
                "advance_middle", "release_capture", "stop_timer",
                "cursor:off", "invalidate"}),
        "a middle boundary tick must release capture before stopping its timer");

    const std::vector<std::pair<mv::ComicAppCancelTrigger, std::string>>
        cancellation_cases = {
            {mv::ComicAppCancelTrigger::LeftButton, "cancel:left"},
            {mv::ComicAppCancelTrigger::Escape, "cancel:escape"},
            {mv::ComicAppCancelTrigger::KeyboardPage, "cancel:keyboard"},
            {mv::ComicAppCancelTrigger::MouseWheel, "cancel:wheel"},
            {mv::ComicAppCancelTrigger::FocusLost, "cancel:focus"},
            {mv::ComicAppCancelTrigger::ExitMode, "cancel:exit"}};
    for (const auto& [trigger, expected_cancel] : cancellation_cases) {
        FakeComicAppPort cancelled;
        cancelled.owner_value = mv::ComicAppAutoOwner::Middle;
        cancelled.timer_value = true;
        expect(mv::ComicAppController::cancel(cancelled, trigger)
                && cancelled.owner_value == mv::ComicAppAutoOwner::None
                && cancelled.calls.size() == 6
                && cancelled.calls[0] == expected_cancel
                && cancelled.calls[1] == "clear_status"
                && cancelled.calls[2] == "release_capture"
                && cancelled.calls[3] == "stop_timer"
                && cancelled.calls[4] == "cursor:off"
                && cancelled.calls[5] == "invalidate",
            "each manual/focus/exit cancellation must share atomic cleanup order");
    }

    struct ViewportRect {
        float left;
        float top;
        float right;
        float bottom;
    };
    const ViewportRect before{0.0f, 0.0f, 1000.0f, 800.0f};
    struct ViewportCase {
        const char* name;
        ViewportRect after;
        bool expect_visible;
        bool old_top_width_gate;
    };
    const std::vector<ViewportCase> viewport_cases = {
        {"same", {0.0f, 0.0f, 1000.0f, 800.0f}, true, false},
        {"top-only", {0.0f, 100.0f, 1000.0f, 800.0f}, true, true},
        {"right-only", {0.0f, 0.0f, 900.0f, 800.0f}, true, true},
        {"bottom-only", {0.0f, 0.0f, 1000.0f, 600.0f}, false, false}};
    constexpr float anchor_x = 500.0f;
    constexpr float anchor_y = 700.0f;
    for (const ViewportCase& item : viewport_cases) {
        const bool old_gate = before.top != item.after.top
            || (before.right - before.left)
                != (item.after.right - item.after.left);
        expect(old_gate == item.old_top_width_gate,
            "the discriminating table must reproduce the old subset gate");
        const bool anchor_visible = anchor_x >= item.after.left
            && anchor_x < item.after.right
            && anchor_y >= item.after.top
            && anchor_y < item.after.bottom;
        expect(anchor_visible == item.expect_visible,
            "the viewport table must classify the post-update anchor");

        FakeComicAppPort viewport;
        viewport.owner_value = mv::ComicAppAutoOwner::Middle;
        viewport.timer_value = true;
        const bool cancelled = mv::ComicAppController::viewport_changed(
            viewport, anchor_visible);
        if (anchor_visible) {
            expect(!cancelled && viewport.calls.empty(),
                "same/top/right updates with a visible anchor must be no-ops");
        } else {
            expect(cancelled
                    && std::string(item.name) == "bottom-only"
                    && !old_gate
                    && viewport.owner_value == mv::ComicAppAutoOwner::None
                    && viewport.calls == std::vector<std::string>({
                        "cancel:viewport", "clear_status", "release_capture",
                        "stop_timer", "cursor:off", "invalidate"}),
                "height-only shrink must bypass the old gate and clean up atomically");
        }
    }

    FakeComicAppPort inactive_viewport;
    inactive_viewport.timer_value = false;
    expect(!mv::ComicAppController::viewport_changed(inactive_viewport, false)
            && inactive_viewport.calls.empty(),
        "every viewport update must remain a cheap no-op without a middle owner");

    FakeComicAppPort timer_failure;
    timer_failure.owner_value = mv::ComicAppAutoOwner::Middle;
    timer_failure.transient_value = true;
    timer_failure.timer_start_result = false;
    expect(!mv::ComicAppController::transient_changed(timer_failure)
            && timer_failure.owner_value == mv::ComicAppAutoOwner::None
            && timer_failure.calls == std::vector<std::string>({
                "start_timer", "clear_transient", "cancel:invalid",
                "release_capture", "stop_timer", "cursor:off", "invalidate"}),
        "timer creation failure must clear transient and middle resources together");

    FakeComicAppPort expired;
    expired.timer_value = true;
    expired.transient_value = true;
    expect(mv::ComicAppController::timer_tick(expired, 0.016f, true)
            && expired.calls == std::vector<std::string>({
                "clear_transient", "stop_timer", "invalidate"}),
        "transient expiry must stop an otherwise idle timer before redraw");
}

void test_native_owner_menu_state() {
    HMENU menu = CreatePopupMenu();
    expect(menu != nullptr, "native owner-menu test must create a popup menu");
    if (!menu) return;

    constexpr UINT disabled_id = 7001;
    constexpr UINT enabled_id = 7002;
    auto insert_owner_item = [&](UINT id, bool disabled) {
        MENUITEMINFOW item = {sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE;
        item.fType = MFT_OWNERDRAW;
        item.fState = disabled ? MFS_DISABLED : MFS_ENABLED;
        item.wID = id;
        return InsertMenuItemW(
            menu, GetMenuItemCount(menu), TRUE, &item) != FALSE;
    };

    expect(insert_owner_item(disabled_id, true),
        "native menu must accept a disabled owner-draw item");
    expect(insert_owner_item(enabled_id, false),
        "native menu must accept an enabled owner-draw item");

    MENUITEMINFOW disabled_state = {sizeof(disabled_state)};
    disabled_state.fMask = MIIM_STATE;
    expect(GetMenuItemInfoW(menu, disabled_id, FALSE, &disabled_state) != FALSE
            && (disabled_state.fState & MFS_DISABLED) == MFS_DISABLED,
        "disabled owner-draw item must retain MFS_DISABLED in native state");
    const UINT disabled_flags = GetMenuState(menu, disabled_id, MF_BYCOMMAND);
    expect(disabled_flags != static_cast<UINT>(-1)
            && (disabled_flags & (MF_DISABLED | MF_GRAYED))
                == (MF_DISABLED | MF_GRAYED),
        "native menu behavior must report the owner-draw item as unavailable");

    MENUITEMINFOW enabled_state = {sizeof(enabled_state)};
    enabled_state.fMask = MIIM_STATE;
    expect(GetMenuItemInfoW(menu, enabled_id, FALSE, &enabled_state) != FALSE
            && (enabled_state.fState & MFS_DISABLED) == 0,
        "enabled owner-draw item must remain selectable in native state");
    const UINT enabled_flags = GetMenuState(menu, enabled_id, MF_BYCOMMAND);
    expect(enabled_flags != static_cast<UINT>(-1)
            && (enabled_flags & (MF_DISABLED | MF_GRAYED)) == 0,
        "native menu behavior must report the enabled owner-draw item as available");

    DestroyMenu(menu);
}

void test_transition_interrupt_planning() {
    using D = mv::TransitionDirection;
    using T = mv::TransitionTrigger;
    using A = mv::TransitionInterruptAction;
    expect(mv::plan_transition_interrupt({D::None, T::Space}) == A::None,
        "no animation -> no action");
    expect(mv::plan_transition_interrupt({D::ToImage, T::Space}) == A::Reverse,
        "space during entry reverses");
    expect(mv::plan_transition_interrupt({D::ToGrid, T::Space}) == A::Reverse,
        "space during exit reverses");
    expect(mv::plan_transition_interrupt({D::ToImage, T::DoubleClick})
            == A::Reverse,
        "double-click during entry reverses");
    expect(mv::plan_transition_interrupt({D::ToGrid, T::DoubleClick})
            == A::Reverse,
        "double-click during exit reverses");
    expect(mv::plan_transition_interrupt({D::ToImage, T::Escape}) == A::Reverse,
        "escape during entry reverses toward grid");
    expect(mv::plan_transition_interrupt({D::ToGrid, T::Escape})
            == A::FastForward,
        "escape during exit fast-forwards");
    expect(mv::plan_transition_interrupt({D::ToImage, T::ArrowLeft})
            == A::FastForwardAndNavigate,
        "arrow during entry completes then navigates");
    expect(mv::plan_transition_interrupt({D::ToGrid, T::ArrowRight})
            == A::FastForwardAndNavigate,
        "arrow during exit completes then navigates");
}

} // namespace

int main() {
    test_transition_interrupt_planning();
    using mv::OpenInputRoute;
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    expect(SUCCEEDED(com_result), "COM must initialize for the corrupt PNG decoder fixture");

    const auto missing_file = mv::classify_open_input(
        false, false, ERROR_FILE_NOT_FOUND, L".png");
    const auto missing_parent = mv::classify_open_input(
        false, false, ERROR_PATH_NOT_FOUND, L".png");
    const auto unreadable_file = mv::classify_open_input(
        false, false, ERROR_ACCESS_DENIED, L".png");
    const auto corrupt_png = mv::classify_open_input(
        true, false, ERROR_SUCCESS, L".PnG");
    const auto unsupported_file = mv::classify_open_input(
        true, false, ERROR_SUCCESS, L".avif");
    expect(missing_file == OpenInputRoute::MissingPath
            && missing_parent == OpenInputRoute::MissingPath,
        "missing files and parent paths must share the missing-path route");
    expect(unreadable_file == OpenInputRoute::ReadOrDecodeFailed,
        "permission and attribute failures must use the read/decode route");
    expect(corrupt_png == OpenInputRoute::DecodeImage,
        "a supported extension must reach deterministic decoder validation");
    expect(mv::classify_open_input(
            true, false, ERROR_SUCCESS, L".ico") == OpenInputRoute::DecodeImage,
        "existing WIC-associated formats must keep their decoder route");
    expect(unsupported_file == OpenInputRoute::UnsupportedFormat,
        "declared deferred formats must fail before decoder or upload work");
    expect(mv::classify_open_input(
            true, true, ERROR_SUCCESS, L"") == OpenInputRoute::OpenDirectory,
        "an existing directory must keep the directory route");

    mv::ImageLoadResult corrupt_load_result = mv::ImageLoadResult::DecodeFailed;
    if (SUCCEEDED(com_result)) {
        try {
            mv::Decoder decoder;
            const std::wstring corrupt_path =
                std::wstring(MINVIEW_SOURCE_DIR) + L"\\tests\\corrupt_open_input.png";
            corrupt_load_result = mv::run_image_load_stages(
                [&]() { return decoder.decode(corrupt_path); },
                [&](const mv::ComPtr<IWICBitmapSource>& decoded) {
                    return decoder.materialize(decoded.Get());
                },
                [](const mv::ComPtr<IWICBitmapSource>&) { return true; });
        } catch (...) {
            expect(false, "the WIC decoder fixture setup must not throw");
        }
    }
    expect(corrupt_load_result == mv::ImageLoadResult::DecodeFailed,
        "the corrupt PNG fixture must fail the real WIC decode stage");

    auto expect_rejected_open = [&](OpenInputRoute initial_route,
            mv::ImageLoadResult load_result, const wchar_t* expected_feedback,
            int expected_load_calls) {
        bool grid_mode = true;
        bool from_grid = false;
        bool animation_started = false;
        int grid_selection = 0;
        std::vector<bool> selected = {true, false};
        int selection_anchor = 0;
        std::wstring current_path = L"current.png";
        std::wstring directory = L"C:\\existing";
        bool has_image = true;
        std::wstring feedback;
        int load_calls = 0;

        const bool entered = mv::run_grid_entry(
            mv::GridEntryRequest{mv::GridEntryTrigger::DoubleClick, 1},
            mv::GridEntryTransactionState{
                grid_mode, from_grid, animation_started,
                grid_selection, selected, selection_anchor},
            [&](int index) {
                grid_selection = index;
                selected = {false, true};
                selection_anchor = index;
            },
            [&](int) {
                const auto resolved_route = mv::resolve_open_input_route(
                    initial_route, [&]() {
                        ++load_calls;
                        return load_result;
                    });
                if (resolved_route != OpenInputRoute::DecodeImage) {
                    feedback = mv::open_input_error_message(resolved_route);
                    return false;
                }
                current_path = L"changed.png";
                directory = L"C:\\changed";
                has_image = false;
                grid_mode = false;
                from_grid = true;
                return true;
            },
            [&]() { animation_started = true; });

        expect(!entered && grid_mode && !from_grid && !animation_started,
            "a rejected open must preserve browsing and animation state");
        expect(grid_selection == 0
                && selected == std::vector<bool>({true, false})
                && selection_anchor == 0,
            "a rejected open must preserve selection state");
        expect(current_path == L"current.png"
                && directory == L"C:\\existing" && has_image,
            "a rejected open must preserve current image and directory state");
        expect(feedback == expected_feedback,
            "a rejected open must map to its exact Chinese feedback");
        expect(load_calls == expected_load_calls,
            "only supported inputs may invoke the decoder seam");
    };
    expect_rejected_open(
        missing_file, mv::ImageLoadResult::Success,
        L"无法打开：文件或路径不存在。", 0);
    expect_rejected_open(
        unsupported_file, mv::ImageLoadResult::Success,
        L"无法打开：暂不支持此文件格式。", 0);
    expect_rejected_open(
        corrupt_png, corrupt_load_result,
        L"无法打开：文件无法读取或图片解码失败。", 1);

    std::wstring directory_error = mv::open_input_error_message(missing_file);
    expect(!mv::complete_directory_open(-1, directory_error)
            && directory_error == L"无法打开：文件或路径不存在。",
        "a failed directory scan must preserve the current open error");
    expect(mv::complete_directory_open(0, directory_error)
            && directory_error.empty(),
        "a successful empty directory open must clear a previous open error");
    test_native_owner_menu_state();
    test_comic_app_controller();
    using mv::RecursiveScanAction;
    expect(!mv::can_toggle_recursive(false, false, L"")
            && mv::can_toggle_recursive(false, false, L"C:\\空根目录")
            && mv::can_toggle_recursive(true, false, L"C:\\空根目录")
            && !mv::can_toggle_recursive(false, true, L"C:\\图片目录"),
        "recursive commands must be available for a bound empty root and grid only");
    expect(mv::classify_recursive_scan_action(false, false, 2)
            == RecursiveScanAction::EnterUnselectedGrid,
        "recursive descendants from an empty root must enter an unselected grid");
    expect(mv::classify_recursive_scan_action(false, false, 0)
            == RecursiveScanAction::KeepView,
        "an empty recursive result must retain the explicit empty-root view");
    expect(mv::classify_recursive_scan_action(true, true, 0)
            == RecursiveScanAction::ShowEmptyRoot,
        "turning recursion off with no direct images must leave the blank grid");

    mv::GridEntryRouteState entry_state;
    entry_state.grid_mode = true;
    entry_state.selected_index = 1;
    entry_state.hit_index = 1;
    entry_state.item_count = 3;
    const auto space_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::Space, entry_state);
    const auto double_click_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::DoubleClick, entry_state);
    expect(space_entry && double_click_entry
            && space_entry->index == 1 && double_click_entry->index == 1,
        "Space and thumbnail double-click must route to the same grid item");

    entry_state.selected_index = -1;
    entry_state.hit_index = 2;
    const auto no_selection_double_click = mv::route_grid_entry(
        mv::GridEntryTrigger::DoubleClick, entry_state);
    expect(!mv::route_grid_entry(mv::GridEntryTrigger::Space, entry_state),
        "Space without a valid grid selection must fail closed");
    expect(no_selection_double_click && no_selection_double_click->index == 2,
        "double-click must bind its hit when recursive grid has no default selection");

    entry_state.selected_index = 0;
    entry_state.hit_index = 2;
    const auto different_space_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::Space, entry_state);
    const auto different_double_click_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::DoubleClick, entry_state);
    expect(different_space_entry && different_space_entry->index == 0
            && different_double_click_entry
            && different_double_click_entry->index == 2,
        "each trigger must bind its exact request index when hit differs from selection");

    entry_state.hit_index = -1;
    expect(!mv::route_grid_entry(
            mv::GridEntryTrigger::DoubleClick, entry_state),
        "double-click outside a thumbnail must fail closed");
    entry_state.hit_index = 1;
    entry_state.animating = true;
    expect(!mv::route_grid_entry(mv::GridEntryTrigger::Space, entry_state)
            && !mv::route_grid_entry(
                mv::GridEntryTrigger::DoubleClick, entry_state),
        "mode-switch inputs during the transition must remain ignored");

    mv::GridExitRouteState exit_state;
    exit_state.from_grid = true;
    exit_state.has_image = true;
    expect(mv::route_grid_exit(mv::GridExitTrigger::Space, exit_state)
            && mv::route_grid_exit(mv::GridExitTrigger::Escape, exit_state)
            && mv::route_grid_exit(mv::GridExitTrigger::DoubleClick, exit_state),
        "Space, Escape, and big-image double-click must route back to grid");
    exit_state.from_grid = false;
    expect(!mv::route_grid_exit(mv::GridExitTrigger::Space, exit_state)
            && !mv::route_grid_exit(mv::GridExitTrigger::Escape, exit_state)
            && mv::route_grid_exit(mv::GridExitTrigger::DoubleClick, exit_state),
        "only big-image double-click may return without saved grid context");
    exit_state.animating = true;
    expect(!mv::route_grid_exit(mv::GridExitTrigger::Space, exit_state)
            && !mv::route_grid_exit(mv::GridExitTrigger::Escape, exit_state)
            && !mv::route_grid_exit(mv::GridExitTrigger::DoubleClick, exit_state),
        "all mode-switch inputs must remain ignored during animation");

    mv::GridScrollPause scroll_pause;
    std::vector<std::uintptr_t> cancelled_timers;
    const auto cancel_timer = [&cancelled_timers](std::uintptr_t timer) {
        cancelled_timers.push_back(timer);
    };
    scroll_pause.begin(cancel_timer, [] { return std::uintptr_t{17}; });
    expect(scroll_pause.active() && scroll_pause.timer() == 17,
        "a created scroll timer must pause visible thumbnail requests");
    std::vector<int> requested_thumbnails;
    scroll_pause.request_visible(
        true, 40, 44, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails.empty(),
        "active scrolling must continue suppressing visible thumbnail requests");

    scroll_pause.begin(cancel_timer, [] { return std::uintptr_t{0}; });
    expect(!scroll_pause.active() && scroll_pause.timer() == 0
            && cancelled_timers == std::vector<std::uintptr_t>{17},
        "SetTimer failure must cancel the prior timer and immediately resume scheduling");
    scroll_pause.request_visible(
        true, 40, 44, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails == std::vector<int>({40, 41, 42, 43}),
        "SetTimer failure must make the exact visible range requestable");

    requested_thumbnails.clear();
    scroll_pause.begin(cancel_timer, [] { return std::uintptr_t{23}; });
    scroll_pause.finish(cancel_timer);
    expect(!scroll_pause.active() && scroll_pause.timer() == 0
            && cancelled_timers == std::vector<std::uintptr_t>({17, 23}),
        "grid exit must cancel its timer and end the scroll pause");
    scroll_pause.request_visible(
        false, 50, 53, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails.empty(),
        "a stopped loader must not accumulate visible requests");
    scroll_pause.request_visible(
        true, 50, 53, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails == std::vector<int>({50, 51, 52}),
        "a restarted loader must receive the exact visible range after grid exit");

    mv::GridTransitionGeometry geometry;
    geometry.request_index = 2;
    geometry.item_count = 3;
    geometry.row_start_index = 1;
    geometry.row_end_index = 3;
    geometry.row_y = 600;
    geometry.row_height = 200;
    geometry.item_x = 300.0f;
    geometry.item_width = 150.0f;
    geometry.image_width = 300;
    geometry.image_height = 200;
    geometry.thumb_padding = 8;
    geometry.toolbar_height = 56;
    geometry.scroll_y = 400;
    const auto source_rect = mv::calculate_grid_transition_rect(geometry);
    expect(source_rect
            && nearly_equal((source_rect->left + source_rect->right) * 0.5f, 383.0f)
            && nearly_equal((source_rect->top + source_rect->bottom) * 0.5f, 356.0f)
            && nearly_equal((source_rect->right - source_rect->left)
                / (source_rect->bottom - source_rect->top), 1.5f),
        "transition source must use the exact item geometry and scrolled viewport coordinates");
    geometry.scroll_y = 450;
    const auto scrolled_source_rect = mv::calculate_grid_transition_rect(geometry);
    expect(source_rect && scrolled_source_rect
            && nearly_equal(scrolled_source_rect->top, source_rect->top - 50.0f)
            && nearly_equal(scrolled_source_rect->bottom, source_rect->bottom - 50.0f),
        "transition source must move by the exact grid scroll delta");
    geometry.request_index = 0;
    expect(!mv::calculate_grid_transition_rect(geometry),
        "an index outside the captured layout row must not reuse an old source rect");
    geometry.request_index = 2;
    geometry.item_width = 0.0f;
    expect(!mv::calculate_grid_transition_rect(geometry),
        "invalid item geometry must fail closed instead of returning a stale rect");

    auto expect_successful_entry = [&](
            const mv::GridEntryRequest& request, bool capture_throws) {
        bool grid_mode = true;
        bool from_grid = false;
        bool animation_started = false;
        int transaction_selection = 1;
        std::vector<bool> transaction_selected = {false, true, false};
        int transaction_anchor = 1;
        std::vector<std::string> stages;
        const bool entered = mv::run_grid_entry(
            request,
            mv::GridEntryTransactionState{
                grid_mode, from_grid, animation_started,
                transaction_selection, transaction_selected, transaction_anchor},
            [&](int index) {
                expect(index == 1, "entry transition must bind the requested index");
                stages.push_back("capture");
                if (capture_throws)
                    throw std::runtime_error("injected transition capture failure");
            },
            [&](int index) {
                expect(index == 1, "image load must bind the requested index");
                const auto load_result = mv::run_image_load_stages(
                    [&]() {
                        stages.push_back("decode");
                        return 1;
                    },
                    [&](int decoded) {
                        stages.push_back("materialize");
                        return decoded + 1;
                    },
                    [&](int materialized) {
                        stages.push_back("upload");
                        return materialized == 2;
                    });
                const bool loaded = load_result == mv::ImageLoadResult::Success;
                if (loaded) {
                    stages.push_back("commit");
                    grid_mode = false;
                    from_grid = true;
                }
                return loaded;
            },
            [&]() {
                stages.push_back("animation");
                animation_started = true;
            });
        expect(entered && !grid_mode && from_grid && animation_started,
            "a successful load must commit big-image mode");
        expect(stages == std::vector<std::string>(
                {"capture", "decode", "materialize", "upload", "commit", "animation"}),
            capture_throws
                ? "capture failure must not block formal loading and entry"
                : "grid entry must commit before the visual animation begins");
    };
    expect_successful_entry(*space_entry, false);
    expect_successful_entry(*double_click_entry, false);
    expect_successful_entry(*space_entry, true);

    enum class LoadFault { Decode, Materialize, Upload };
    auto expect_failed_load = [&](LoadFault fault) {
        bool grid_mode = true;
        bool from_grid = false;
        bool animation_started = false;
        int transaction_selection = 0;
        std::vector<bool> transaction_selected = {true, false, false};
        int transaction_anchor = 0;
        int decode_calls = 0;
        int materialize_calls = 0;
        int upload_calls = 0;
        mv::ImageLoadResult observed_result = mv::ImageLoadResult::Success;
        const bool entered = mv::run_grid_entry(
            *space_entry,
            mv::GridEntryTransactionState{
                grid_mode, from_grid, animation_started,
                transaction_selection, transaction_selected, transaction_anchor},
            [&](int index) {
                transaction_selection = index;
                transaction_selected = {false, true, false};
                transaction_anchor = index;
            },
            [&](int) {
                observed_result = mv::run_image_load_stages(
                    [&]() {
                        ++decode_calls;
                        if (fault == LoadFault::Decode)
                            throw std::runtime_error("injected decode failure");
                        return 1;
                    },
                    [&](int decoded) {
                        ++materialize_calls;
                        if (fault == LoadFault::Materialize)
                            throw std::runtime_error("injected materialize failure");
                        return decoded + 1;
                    },
                    [&](int) {
                        ++upload_calls;
                        return fault != LoadFault::Upload;
                    });
                const bool loaded = observed_result == mv::ImageLoadResult::Success;
                if (loaded) {
                    grid_mode = false;
                    from_grid = true;
                }
                return loaded;
            },
            [&]() { animation_started = true; });
        expect(!entered && grid_mode && !from_grid && !animation_started,
            "each formal load-stage failure must preserve real grid transaction state");
        expect(transaction_selection == 0
                && transaction_selected == std::vector<bool>({true, false, false})
                && transaction_anchor == 0,
            "a failed entry must restore selection and its anchor");
        expect(decode_calls == 1
                && materialize_calls == (fault == LoadFault::Decode ? 0 : 1)
                && upload_calls == (fault == LoadFault::Upload ? 1 : 0),
            "fault injection must stop at the exact failing production load stage");
        const auto expected_result = fault == LoadFault::Decode
            ? mv::ImageLoadResult::DecodeFailed
            : (fault == LoadFault::Materialize
                ? mv::ImageLoadResult::MaterializeFailed
                : mv::ImageLoadResult::UploadFailed);
        expect(observed_result == expected_result,
            "load failures must retain their exact stage without retrying");
    };
    expect_failed_load(LoadFault::Decode);
    expect_failed_load(LoadFault::Materialize);
    expect_failed_load(LoadFault::Upload);

    bool restored_grid_mode = true;
    bool restored_from_grid = false;
    bool restored_animation = false;
    int restored_selection = 0;
    std::vector<bool> restored_selected = {true, false, false};
    int restored_anchor = 0;
    const bool partial_commit = mv::run_grid_entry(
        *space_entry,
        mv::GridEntryTransactionState{
            restored_grid_mode, restored_from_grid, restored_animation,
            restored_selection, restored_selected, restored_anchor},
        [](int) {},
        [&](int) {
            restored_grid_mode = false;
            restored_from_grid = true;
            return false;
        },
        [&]() { restored_animation = true; });
    expect(!partial_commit && restored_grid_mode && !restored_from_grid
            && !restored_animation,
        "a failed production transaction must restore partially changed mode state");

    expect(mv::is_image_zoomed(2.0f, 1.0f),
        "ordinary wheel input should preserve an existing zoomed state");
    expect(!mv::is_image_zoomed(1.01f, 1.0f),
        "fit-scale tolerance should remain eligible for navigation");

    expect(mv::should_preserve_selection_for_drag(true, false, false),
        "dragging an already selected item should preserve the selection");
    expect(!mv::should_preserve_selection_for_drag(false, false, false),
        "dragging an unselected item should establish a new selection");
    expect(!mv::should_preserve_selection_for_drag(true, true, false),
        "shift click should keep range-selection semantics");

    std::vector<bool> selected(3, false);
    int grid_selection = -1;
    int selection_anchor = -1;
    expect(mv::apply_grid_item_selection(
            0, 3, false, false, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, false, false})
            && grid_selection == 0 && selection_anchor == 0,
        "plain click must establish the exact internal selection");
    expect(mv::apply_grid_item_selection(
            1, 3, false, true, selected, grid_selection, selection_anchor)
            && mv::apply_grid_item_selection(
                2, 3, false, true, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, true, true}),
        "Ctrl click must add each exact item to the internal selection");
    expect(mv::apply_grid_item_selection(
            2, 3, false, true, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, true, false})
            && grid_selection == 2 && selection_anchor == 2,
        "Ctrl toggle off must retain focus without retaining selection");
    expect(mv::grid_item_has_selection_border(0, grid_selection, selected)
            && mv::grid_item_has_selection_border(1, grid_selection, selected)
            && !mv::grid_item_has_selection_border(2, grid_selection, selected),
        "only exact internal selection members may render selection borders");

    selected.assign(3, false);
    grid_selection = -1;
    selection_anchor = -1;
    expect(mv::apply_grid_item_selection(
            0, 3, false, false, selected, grid_selection, selection_anchor)
            && mv::apply_grid_item_selection(
                2, 3, true, false, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, true, true})
            && grid_selection == 2,
        "Shift range must select the inclusive anchor-to-focus interval");

    expect(mv::clamp_grid_scroll_position(900, 600, 400) == 200,
        "shorter layouts should clamp scroll position to the new bottom");
    expect(mv::clamp_grid_scroll_position(-10, 600, 400) == 0,
        "scroll position should never be negative");
    expect(mv::clamp_grid_scroll_position(100, 300, 400) == 0,
        "content shorter than the viewport should reset scrolling");

    expect(mv::ensure_grid_row_visible(400, 450, 550, 1200, 500) == 400,
        "a selected row that remains visible should preserve scroll");
    expect(mv::ensure_grid_row_visible(400, 250, 350, 1200, 500) == 250,
        "a resize that moves the selected row above the viewport should reveal it");
    expect(mv::ensure_grid_row_visible(100, 680, 820, 1400, 500) == 320,
        "column or panel width changes should reveal the selected row below the viewport");
    expect(mv::ensure_grid_row_visible(200, 650, 900, 1500, 500) == 400,
        "zoom, labels, and square/justified row-height changes should share the contract");
    expect(mv::ensure_grid_row_visible(900, 150, 250, 450, 500) == 0,
        "a shortened list should clamp before restoring selected-row visibility");

    using mv::GridRebuildReason;
    expect(mv::classify_grid_rebuild_reason(false, false, false, false)
            == GridRebuildReason::None,
        "an unchanged layout should not rebuild");
    expect(mv::classify_grid_rebuild_reason(true, false, false, true)
            == GridRebuildReason::Structural,
        "a user structural reflow should take precedence over dimensions arriving");
    expect(mv::classify_grid_rebuild_reason(false, true, false, false)
            == GridRebuildReason::Structural,
        "a resize or panel/full-screen width change should be structural");
    expect(mv::classify_grid_rebuild_reason(false, false, true, false)
            == GridRebuildReason::Structural,
        "a first layout or changed index size should be structural");
    expect(mv::classify_grid_rebuild_reason(false, false, false, true)
            == GridRebuildReason::BackgroundDimensions,
        "dimension generation alone should be a background rebuild");

    bool show_labels = true;
    bool label_layout_dirty = false;
    expect(mv::apply_grid_label_toggle(
            true, show_labels, label_layout_dirty)
            && !show_labels && label_layout_dirty,
        "grid label toggle must hide labels and request a structural rebuild");
    label_layout_dirty = false;
    expect(mv::apply_grid_label_toggle(
            true, show_labels, label_layout_dirty)
            && show_labels && label_layout_dirty,
        "a second grid label toggle must restore labels and rebuild layout");
    label_layout_dirty = false;
    expect(!mv::apply_grid_label_toggle(
            false, show_labels, label_layout_dirty)
            && show_labels && !label_layout_dirty,
        "big-image mode must ignore the label toggle without changing layout state");

    int background_scroll = mv::reconcile_grid_scroll_after_rebuild(
        GridRebuildReason::BackgroundDimensions, 900, true,
        100, 220, 2000, 500);
    expect(background_scroll == 900,
        "a background dimension rebuild must not jump to an off-screen selection");
    background_scroll = mv::reconcile_grid_scroll_after_rebuild(
        GridRebuildReason::BackgroundDimensions, background_scroll, true,
        1300, 1420, 2100, 500);
    expect(background_scroll == 900,
        "consecutive background dimension rebuilds must preserve user scroll");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::BackgroundDimensions, 1900, true,
            100, 220, 2000, 500) == 1500,
        "a background rebuild should still clamp scroll to the new content height");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 900, true,
            100, 220, 2000, 500) == 100,
        "a structural rebuild should reveal a selected row above the viewport");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 0, true,
            100, 800, 1200, 500) == 300,
        "a row taller than the viewport should keep the established bottom-alignment rule");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 900, false,
            0, 0, 2000, 500) == 900,
        "a first layout without selection should preserve a valid scroll position");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 100, true,
            680, 820, 1400, 500) == 320,
        "square and justified structural reflows should retain selection visibility");

    std::vector<std::wstring> indexed_paths = {
        L"A-valid.png", L"B-damaged.png"};
    std::wstring current_path = indexed_paths.front();
    int current_index = 0;
    bool has_image = true;
    expect(mv::can_delete_current_image(has_image, current_path),
        "a successfully committed image should be deletable");

    const std::wstring deleted_path = current_path;
    const int deleted_index = current_index;
    indexed_paths.erase(indexed_paths.begin());
    int open_attempts = 0;
    const auto failed_transition = mv::run_post_delete_transition(
        deleted_path, deleted_index, static_cast<int>(indexed_paths.size()),
        [&indexed_paths](int index) { return indexed_paths[static_cast<size_t>(index)]; },
        [&open_attempts](const std::wstring&, int) {
            ++open_attempts;
            return false;
        },
        current_path, current_index, has_image);
    expect(failed_transition.deleted_path == L"A-valid.png"
            && failed_transition.deleted_index == 0,
        "the production transition should stay bound to deleted A's identity");
    expect(failed_transition.successor_attempted && open_attempts == 1,
        "deleting A should attempt to open exactly one successor");
    expect(failed_transition.successor_path == L"B-damaged.png"
            && failed_transition.successor_index == 0,
        "the failed successor attempt should remain bound to indexed B");
    expect(!failed_transition.successor_opened,
        "the injected damaged B open should be reported as failed");
    expect(current_path.empty() && current_index == -1 && !has_image,
        "a damaged successor must leave current image identity cleared");
    if (mv::can_delete_current_image(has_image, current_path))
        indexed_paths.erase(indexed_paths.begin());
    expect(indexed_paths.size() == 1 && indexed_paths.front() == L"B-damaged.png",
        "a consecutive delete must leave damaged B indexed");

    current_path = L"A-valid.png";
    current_index = 0;
    has_image = true;
    const auto successful_transition = mv::run_post_delete_transition(
        L"A-valid.png", 0, 1,
        [](int) { return std::wstring(L"B-valid.png"); },
        [](const std::wstring& path, int index) {
            return path == L"B-valid.png" && index == 0;
        },
        current_path, current_index, has_image);
    expect(successful_transition.successor_opened
            && current_path == L"B-valid.png"
            && current_index == 0 && has_image,
        "a successful successor open should atomically commit B's identity");

    current_path = L"only.png";
    current_index = 0;
    has_image = true;
    bool unexpected_open = false;
    const auto final_transition = mv::run_post_delete_transition(
        L"only.png", 0, 0,
        [](int) { return std::wstring(); },
        [&unexpected_open](const std::wstring&, int) {
            unexpected_open = true;
            return true;
        },
        current_path, current_index, has_image);
    expect(!final_transition.successor_attempted && !unexpected_open,
        "deleting the final item should not attempt a successor open");
    expect(current_path.empty() && current_index == -1 && !has_image,
        "deleting the final item should clear current identity");

    if (failures != 0) {
        if (SUCCEEDED(com_result)) CoUninitialize();
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    if (SUCCEEDED(com_result)) CoUninitialize();
    std::cout << "app state tests passed\n";
    return 0;
}
