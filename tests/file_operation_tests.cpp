#include "file_operation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TempDirectory {
public:
    TempDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / (L"minview-delete-tests-" + std::to_wstring(suffix));
        fs::create_directories(m_path);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void write_test_file(const fs::path& path, char byte) {
    std::ofstream output(path, std::ios::binary);
    output << byte;
}

void expect_intent(
    const std::optional<mv::DeleteIntent>& intent,
    mv::DeleteTarget target, mv::DeleteMode mode, const char* message) {
    expect(intent && intent->target == target && intent->mode == mode, message);
}

struct FakeDeleteAdapter {
    int confirm_response = IDOK;
    bool targets_current = true;
    mv::DeleteMutationResult mutation_result;
    std::vector<std::wstring> reported_missing;
    std::function<void()> after_confirm;
    int confirm_calls = 0;
    int mutation_calls = 0;
    int stop_loader_calls = 0;
    int start_loader_calls = 0;
    std::vector<std::wstring> confirmed_targets;
    std::vector<std::wstring> mutated_targets;
    mv::DeleteMode mutated_mode = mv::DeleteMode::Recycle;

    mv::DeleteAdapterCallbacks callbacks() {
        mv::DeleteAdapterCallbacks result;
        result.confirm = [this](const mv::PermanentDeletePrompt& prompt) {
            ++confirm_calls;
            confirmed_targets.clear();
            for (const auto& target : reported_targets)
                if (prompt.message.find(target) != std::wstring::npos)
                    confirmed_targets.push_back(target);
            if (after_confirm) after_confirm();
            return confirm_response;
        };
        result.targets_still_current = [this](const std::vector<std::wstring>& targets) {
            reported_targets = targets;
            return targets_current;
        };
        result.mutate = [this](
            const std::vector<std::wstring>& targets, mv::DeleteMode mode) {
            ++mutation_calls;
            mutated_targets = targets;
            mutated_mode = mode;
            return mutation_result;
        };
        result.target_is_missing = [this](const std::wstring& target) {
            return std::find(reported_missing.begin(), reported_missing.end(), target)
                != reported_missing.end();
        };
        result.stop_loader = [this] { ++stop_loader_calls; };
        result.start_loader = [this] { ++start_loader_calls; };
        return result;
    }

    std::vector<std::wstring> reported_targets;
};

} // namespace

int main() {
    TempDirectory temp;
    const fs::path existing = temp.path() / L"existing.png";
    const fs::path second = temp.path() / L"second.png";
    const fs::path third = temp.path() / L"third.png";
    write_test_file(existing, 'x');
    write_test_file(second, 'y');
    write_test_file(third, 'z');

    expect(!mv::path_is_confirmed_missing(existing.wstring()),
        "an existing file must remain in the index");
    expect(mv::path_is_confirmed_missing((temp.path() / L"missing.png").wstring()),
        "a file-not-found postcondition should remove the index entry");
    expect(mv::path_is_existing_file(existing.wstring()),
        "an existing file should be a valid delete target");
    expect(!mv::path_is_existing_file(temp.path().wstring()),
        "a directory must not be accepted as a file delete target");

    mv::DeleteRouteState route_state;
    route_state.main_window_focused = true;
    expect_intent(mv::route_delete_key(VK_DELETE, 0, route_state),
        mv::DeleteTarget::CurrentImage, mv::DeleteMode::Recycle,
        "raw Del should route to current-image recycle deletion");
    route_state.shift_down = true;
    expect_intent(mv::route_delete_key(VK_DELETE, 0, route_state),
        mv::DeleteTarget::CurrentImage, mv::DeleteMode::Permanent,
        "raw Shift+Del should route to current-image permanent deletion");
    route_state.grid_mode = true;
    route_state.has_selection = true;
    expect_intent(mv::route_delete_key(VK_DELETE, 0, route_state),
        mv::DeleteTarget::GridSelection, mv::DeleteMode::Permanent,
        "raw Shift+Del should route a grid selection to permanent deletion");
    route_state.has_selection = false;
    expect(!mv::route_delete_key(VK_DELETE, 0, route_state),
        "grid deletion without a selection must fail closed");
    route_state.grid_mode = false;
    route_state.has_selection = false;
    route_state.control_down = true;
    expect(!mv::route_delete_key(VK_DELETE, 0, route_state),
        "Ctrl+Shift+Del must not route to deletion");
    route_state.control_down = false;
    route_state.main_window_focused = false;
    expect(!mv::route_delete_key(VK_DELETE, 0, route_state),
        "delete input outside the focused main window must fail closed");
    route_state.main_window_focused = true;
    route_state.ime_composing = true;
    expect(!mv::route_delete_key(VK_DELETE, 0, route_state),
        "delete input during IME composition must fail closed");
    route_state.ime_composing = false;
    const LPARAM repeated_lparam = static_cast<LPARAM>(ULONG_PTR{1} << 30);
    expect(!mv::route_delete_key(VK_DELETE, repeated_lparam, route_state),
        "bit 30 in the raw key lParam must reject repeated deletion");
    expect(!mv::route_delete_key(VK_RETURN, 0, route_state),
        "unrelated raw keys must not route to deletion");

    expect_intent(mv::route_delete_command(
            mv::kDeleteCommandRecycle, false, false),
        mv::DeleteTarget::CurrentImage, mv::DeleteMode::Recycle,
        "the common toolbar and WM_COMMAND recycle id should route current deletion");
    expect_intent(mv::route_delete_command(
            mv::kDeleteCommandPermanent, true, true),
        mv::DeleteTarget::GridSelection, mv::DeleteMode::Permanent,
        "the common toolbar, WM_COMMAND, and context id should route grid permanent deletion");
    expect(!mv::route_delete_command(mv::kDeleteCommandPermanent, true, false),
        "a delete command with no grid selection must fail closed");
    expect(!mv::route_delete_command(9999, false, false),
        "an unrelated command must not produce a delete intent");

    const std::vector<std::wstring> grid_targets = {
        existing.wstring(), second.wstring()};
    const mv::PermanentDeletePrompt prompt =
        mv::make_permanent_delete_prompt(grid_targets);
    expect(prompt.title.find(L"永久删除") != std::wstring::npos,
        "the prompt title should identify permanent deletion in Chinese");
    expect(prompt.message.find(L"永久删除") != std::wstring::npos
            && prompt.message.find(L"无法恢复") != std::wstring::npos,
        "the prompt should state permanent and unrecoverable in Chinese");
    expect(prompt.message.find(grid_targets[0]) != std::wstring::npos
            && prompt.message.find(grid_targets[1]) != std::wstring::npos,
        "the prompt should identify every exact target");
    expect((prompt.flags & MB_TYPEMASK) == MB_OKCANCEL,
        "the prompt should provide explicit OK and Cancel choices");
    expect((prompt.flags & MB_DEFMASK) == MB_DEFBUTTON2,
        "Cancel should be the safe default button");

    const std::vector<std::wstring> single_target = {existing.wstring()};
    for (int response : {IDCANCEL, IDCLOSE, 0}) {
        FakeDeleteAdapter cancelled;
        cancelled.confirm_response = response;
        cancelled.reported_targets = single_target;
        auto callbacks = cancelled.callbacks();
        const auto result = mv::execute_current_delete(
            mv::DeleteMode::Permanent, single_target, true, callbacks);
        expect(result.request_result == mv::DeleteRequestResult::Cancelled,
            "Cancel, close, Esc, and unknown confirmation results must cancel");
        expect(cancelled.confirm_calls == 1 && cancelled.mutation_calls == 0
                && cancelled.stop_loader_calls == 0 && cancelled.start_loader_calls == 0,
            "a cancelled current-image adapter must have zero mutation and loader changes");
        expect(fs::exists(existing),
            "a cancelled adapter must leave the isolated target untouched");
    }

    FakeDeleteAdapter current_environment;
    current_environment.reported_targets = single_target;
    current_environment.reported_missing = single_target;
    mv::DeleteAdapterResult current_delete_result;
    mv::CurrentDeleteRecoveryResult current_recovery;
    std::wstring current_path = existing.wstring();
    int current_index = 0;
    bool has_image = true;
    int current_dispatches = 0;
    int grid_dispatches = 0;
    mv::DeleteMode current_dispatched_mode = mv::DeleteMode::Permanent;
    const std::vector<std::wstring> current_remaining_paths = {
        second.wstring(), third.wstring()};
    const mv::DeleteIntentHandlers current_handlers = {
        [&](mv::DeleteMode mode) {
            ++current_dispatches;
            current_dispatched_mode = mode;
            auto callbacks = current_environment.callbacks();
            current_delete_result = mv::execute_current_delete(
                mode, single_target, true, callbacks);
            if (!current_delete_result.removed_positions.empty()) {
                current_recovery = mv::recover_current_delete(
                    existing.wstring(), 0, current_remaining_paths, true,
                    [&second](const std::wstring& path, int index) {
                        return path == second.wstring() && index == 0;
                    },
                    current_path, current_index, has_image);
                if (current_recovery.restart_loader) callbacks.start_loader();
            }
        },
        [&](mv::DeleteMode) { ++grid_dispatches; },
    };
    route_state.shift_down = false;
    expect(mv::dispatch_delete_key(VK_DELETE, 0, route_state, current_handlers),
        "the production key dispatcher should consume raw Del");
    expect(current_dispatches == 1 && grid_dispatches == 0
            && current_dispatched_mode == mv::DeleteMode::Recycle,
        "raw Del must invoke only the current-image production adapter in recycle mode");
    expect(current_environment.confirm_calls == 0
            && current_environment.mutation_calls == 1
            && current_environment.mutated_targets == single_target
            && current_environment.mutated_mode == mv::DeleteMode::Recycle,
        "ordinary Del must skip permanent confirmation and mutate the exact snapshot");
    expect(current_environment.stop_loader_calls == 1
            && current_environment.start_loader_calls == 1,
        "the current-image path must stop and restore a running loader around recovery");
    expect(current_recovery.successor_opened
            && current_path == second.wstring() && current_index == 0 && has_image,
        "the current-image production recovery must commit the exact successor");

    FakeDeleteAdapter permanent_current;
    permanent_current.reported_targets = single_target;
    permanent_current.reported_missing = single_target;
    mv::DeleteAdapterResult permanent_result;
    int permanent_current_dispatches = 0;
    const mv::DeleteIntentHandlers permanent_current_handlers = {
        [&](mv::DeleteMode mode) {
            ++permanent_current_dispatches;
            auto callbacks = permanent_current.callbacks();
            permanent_result = mv::execute_current_delete(
                mode, single_target, false, callbacks);
        },
        [&](mv::DeleteMode) { ++grid_dispatches; },
    };
    route_state.shift_down = true;
    expect(mv::dispatch_delete_key(
            VK_DELETE, 0, route_state, permanent_current_handlers),
        "the production key dispatcher should consume raw Shift+Del");
    expect(permanent_result.request_result == mv::DeleteRequestResult::MutationInvoked
            && permanent_current_dispatches == 1
            && permanent_current.confirm_calls == 1
            && permanent_current.mutation_calls == 1,
        "raw Shift+Del plus IDOK must pass the current production gate once");
    expect(permanent_current.confirmed_targets == single_target
            && permanent_current.mutated_targets == single_target
            && permanent_current.mutated_mode == mv::DeleteMode::Permanent,
        "IDOK must permanently mutate only the confirmed current-image snapshot");

    FakeDeleteAdapter grid_environment;
    grid_environment.reported_targets = grid_targets;
    grid_environment.reported_missing = {existing.wstring()};
    mv::DeleteAdapterResult grid_delete_result;
    mv::GridDeleteRecoveryResult grid_recovery;
    std::wstring grid_current_path = existing.wstring();
    int grid_current_index = 0;
    bool grid_has_image = true;
    bool grid_mode = true;
    int grid_selection = 0;
    std::vector<bool> selected = {true, true, false};
    int selection_anchor = 0;
    mv::DeleteMode grid_dispatched_mode = mv::DeleteMode::Recycle;
    const std::vector<std::wstring> grid_remaining_paths = {
        second.wstring(), third.wstring()};
    const mv::DeleteIntentHandlers grid_handlers = {
        [&](mv::DeleteMode) { ++current_dispatches; },
        [&](mv::DeleteMode mode) {
            ++grid_dispatches;
            grid_dispatched_mode = mode;
            auto callbacks = grid_environment.callbacks();
            grid_delete_result = mv::execute_grid_delete(
                mode, grid_targets, callbacks);
            if (!grid_delete_result.removed_positions.empty()) {
                grid_recovery = mv::recover_grid_delete(
                    grid_remaining_paths, 0, existing.wstring(),
                    grid_delete_result.remaining_targets,
                    grid_current_path, grid_current_index, grid_has_image,
                    grid_mode, grid_selection, selected, selection_anchor);
                if (grid_recovery.restart_loader) callbacks.start_loader();
            }
        },
    };
    const int current_dispatches_before_grid = current_dispatches;
    const int grid_dispatches_before_command = grid_dispatches;
    expect(mv::dispatch_delete_command(
            mv::kDeleteCommandPermanent, true, true, grid_handlers),
        "the shared production command dispatcher should accept permanent deletion");
    expect(current_dispatches == current_dispatches_before_grid
            && grid_dispatches == grid_dispatches_before_command + 1
            && grid_dispatched_mode == mv::DeleteMode::Permanent,
        "the common toolbar, WM_COMMAND, and context id must invoke only the grid adapter");
    expect(grid_environment.confirm_calls == 1
            && grid_environment.mutation_calls == 1
            && grid_environment.confirmed_targets == grid_targets
            && grid_environment.mutated_targets == grid_targets
            && grid_environment.mutated_mode == mv::DeleteMode::Permanent,
        "the grid adapter must confirm and mutate the exact multi-select snapshot");
    expect(grid_environment.stop_loader_calls == 1
            && grid_environment.start_loader_calls == 1,
        "the grid path must stop mutation and restart only after selection recovery");
    expect(!grid_recovery.index_empty && grid_recovery.current_identity_changed
            && grid_current_path == second.wstring() && grid_current_index == 0
            && grid_has_image && grid_mode,
        "partial grid deletion must recover current identity to a surviving selection");
    expect(grid_selection == 0 && selection_anchor == 0
            && selected.size() == 2 && selected[0] && !selected[1],
        "partial grid deletion must preserve the surviving selected path and focus");

    FakeDeleteAdapter no_removal;
    no_removal.reported_targets = single_target;
    auto no_removal_callbacks = no_removal.callbacks();
    const auto unchanged = mv::execute_current_delete(
        mv::DeleteMode::Recycle, single_target, true, no_removal_callbacks);
    expect(unchanged.removed_positions.empty() && !unchanged.complete
            && no_removal.stop_loader_calls == 1 && no_removal.start_loader_calls == 1,
        "a failed current mutation must restore the previously running loader");

    FakeDeleteAdapter no_grid_removal;
    no_grid_removal.reported_targets = grid_targets;
    auto no_grid_callbacks = no_grid_removal.callbacks();
    const auto unchanged_grid = mv::execute_grid_delete(
        mv::DeleteMode::Recycle, grid_targets, no_grid_callbacks);
    expect(unchanged_grid.removed_positions.empty() && !unchanged_grid.complete
            && no_grid_removal.confirm_calls == 0
            && no_grid_removal.mutated_targets == grid_targets
            && no_grid_removal.mutated_mode == mv::DeleteMode::Recycle
            && no_grid_removal.stop_loader_calls == 1
            && no_grid_removal.start_loader_calls == 1,
        "grid Del must skip permanent confirmation and restore its loader on failure");

    FakeDeleteAdapter empty_targets;
    auto empty_callbacks = empty_targets.callbacks();
    const auto empty_result = mv::execute_grid_delete(
        mv::DeleteMode::Permanent, {}, empty_callbacks);
    expect(empty_result.request_result == mv::DeleteRequestResult::InvalidTargets
            && empty_targets.confirm_calls == 0 && empty_targets.mutation_calls == 0,
        "an empty production target snapshot must fail closed");

    FakeDeleteAdapter missing_target;
    const std::vector<std::wstring> missing_paths = {
        (temp.path() / L"missing.png").wstring()};
    missing_target.reported_targets = missing_paths;
    auto missing_callbacks = missing_target.callbacks();
    const auto missing_result = mv::execute_current_delete(
        mv::DeleteMode::Permanent, missing_paths, true, missing_callbacks);
    expect(missing_result.request_result == mv::DeleteRequestResult::InvalidTargets
            && missing_target.confirm_calls == 0 && missing_target.mutation_calls == 0,
        "a missing production target must fail closed before confirmation");

    FakeDeleteAdapter stale_target;
    stale_target.reported_targets = single_target;
    stale_target.targets_current = false;
    auto stale_callbacks = stale_target.callbacks();
    const auto stale_result = mv::execute_current_delete(
        mv::DeleteMode::Permanent, single_target, true, stale_callbacks);
    expect(stale_result.request_result == mv::DeleteRequestResult::StaleTargets
            && stale_target.confirm_calls == 1 && stale_target.mutation_calls == 0
            && stale_target.stop_loader_calls == 0,
        "a changed selection after confirmation must fail closed before loader mutation");

    const fs::path vanishing = temp.path() / L"vanishing.png";
    write_test_file(vanishing, 'v');
    const std::vector<std::wstring> vanishing_target = {vanishing.wstring()};
    FakeDeleteAdapter vanished;
    vanished.reported_targets = vanishing_target;
    vanished.after_confirm = [&vanishing] { fs::remove(vanishing); };
    auto vanished_callbacks = vanished.callbacks();
    const auto vanished_result = mv::execute_current_delete(
        mv::DeleteMode::Permanent, vanishing_target, true, vanished_callbacks);
    expect(vanished_result.request_result == mv::DeleteRequestResult::StaleTargets
            && vanished.confirm_calls == 1 && vanished.mutation_calls == 0,
        "a target disappearing during confirmation must fail closed");

    std::wstring final_current_path = third.wstring();
    int final_current_index = 0;
    bool final_has_image = true;
    bool unexpected_successor = false;
    const auto final_recovery = mv::recover_current_delete(
        third.wstring(), 0, {}, true,
        [&unexpected_successor](const std::wstring&, int) {
            unexpected_successor = true;
            return true;
        },
        final_current_path, final_current_index, final_has_image);
    expect(!final_recovery.successor_attempted && !final_recovery.restart_loader
            && !unexpected_successor && final_current_path.empty()
            && final_current_index == -1 && !final_has_image,
        "deleting the final current image must clear identity without restarting a loader");

    std::wstring empty_grid_current = third.wstring();
    int empty_grid_current_index = 0;
    bool empty_grid_has_image = true;
    bool empty_grid_mode = true;
    int empty_grid_selection = 0;
    std::vector<bool> empty_grid_selected = {true};
    int empty_grid_anchor = 0;
    const auto empty_grid_recovery = mv::recover_grid_delete(
        {}, 0, third.wstring(), {},
        empty_grid_current, empty_grid_current_index, empty_grid_has_image,
        empty_grid_mode, empty_grid_selection, empty_grid_selected,
        empty_grid_anchor);
    expect(empty_grid_recovery.index_empty && !empty_grid_recovery.restart_loader
            && empty_grid_current.empty() && empty_grid_current_index == -1
            && !empty_grid_has_image && !empty_grid_mode
            && empty_grid_selection == -1 && empty_grid_selected.empty()
            && empty_grid_anchor == -1,
        "deleting the final grid item must clear current and selection state");

    expect(mv::delete_fully_completed(0, false, 3, 3),
        "all requested paths plus a successful Shell result is complete");
    expect(!mv::delete_fully_completed(0, true, 3, 3),
        "an aborted operation must remain visibly incomplete");
    expect(!mv::delete_fully_completed(1, false, 3, 1),
        "partial completion must be reported even when one path was removed");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "file operation tests passed\n";
    return 0;
}
