#include "file_operation.h"
#include "app_state.h"

#include <algorithm>

namespace mv {

namespace {

std::optional<DeleteIntent> make_delete_intent(
    DeleteMode mode, bool grid_mode, bool has_selection) {
    if (grid_mode) {
        if (!has_selection) return std::nullopt;
        return DeleteIntent{DeleteTarget::GridSelection, mode};
    }
    return DeleteIntent{DeleteTarget::CurrentImage, mode};
}

void dispatch_delete_intent(
    const DeleteIntent& intent, const DeleteIntentHandlers& handlers) {
    if (intent.target == DeleteTarget::GridSelection) {
        handlers.grid_selection(intent.mode);
    } else {
        handlers.current_image(intent.mode);
    }
}

DeleteAdapterResult execute_delete(
    DeleteMode mode, const std::vector<std::wstring>& targets,
    bool stop_loader_before_mutation, bool restart_loader_if_unchanged,
    const DeleteAdapterCallbacks& callbacks) {
    DeleteMutationResult mutation;
    DeleteAdapterResult result;
    result.request_result = run_guarded_delete(
        mode, targets, callbacks.confirm, callbacks.targets_still_current,
        [&](const std::vector<std::wstring>& approved_paths, DeleteMode approved_mode) {
            if (stop_loader_before_mutation) callbacks.stop_loader();
            mutation = callbacks.mutate(approved_paths, approved_mode);
        });
    if (result.request_result != DeleteRequestResult::MutationInvoked)
        return result;

    for (size_t i = 0; i < targets.size(); ++i) {
        if (callbacks.target_is_missing(targets[i]))
            result.removed_positions.push_back(i);
        else
            result.remaining_targets.push_back(targets[i]);
    }
    result.complete = delete_fully_completed(
        mutation.shell_result, mutation.aborted,
        targets.size(), result.removed_positions.size());
    if (result.removed_positions.empty() && restart_loader_if_unchanged)
        callbacks.start_loader();
    return result;
}

int index_of_path(
    const std::vector<std::wstring>& paths, const std::wstring& path) {
    const auto found = std::find(paths.begin(), paths.end(), path);
    return found == paths.end()
        ? -1 : static_cast<int>(std::distance(paths.begin(), found));
}

} // namespace

std::optional<DeleteIntent> route_delete_key(
    UINT key, LPARAM key_lparam, const DeleteRouteState& state) {
    if (key != VK_DELETE || state.control_down || !state.main_window_focused
        || state.ime_composing
        || (static_cast<ULONG_PTR>(key_lparam) & (ULONG_PTR{1} << 30)) != 0)
        return std::nullopt;
    return make_delete_intent(
        state.shift_down ? DeleteMode::Permanent : DeleteMode::Recycle,
        state.grid_mode, state.has_selection);
}

std::optional<DeleteIntent> route_delete_command(
    UINT command, bool grid_mode, bool has_selection) {
    if (command == kDeleteCommandRecycle)
        return make_delete_intent(DeleteMode::Recycle, grid_mode, has_selection);
    if (command == kDeleteCommandPermanent)
        return make_delete_intent(DeleteMode::Permanent, grid_mode, has_selection);
    return std::nullopt;
}

bool dispatch_delete_key(
    UINT key, LPARAM key_lparam, const DeleteRouteState& state,
    const DeleteIntentHandlers& handlers) {
    if (key != VK_DELETE) return false;
    const auto intent = route_delete_key(key, key_lparam, state);
    if (intent) dispatch_delete_intent(*intent, handlers);
    return true;
}

bool dispatch_delete_command(
    UINT command, bool grid_mode, bool has_selection,
    const DeleteIntentHandlers& handlers) {
    const auto intent = route_delete_command(command, grid_mode, has_selection);
    if (!intent) return false;
    dispatch_delete_intent(*intent, handlers);
    return true;
}

DeleteAdapterResult execute_current_delete(
    DeleteMode mode, const std::vector<std::wstring>& targets,
    bool loader_was_running, const DeleteAdapterCallbacks& callbacks) {
    return execute_delete(
        mode, targets, loader_was_running, loader_was_running, callbacks);
}

DeleteAdapterResult execute_grid_delete(
    DeleteMode mode, const std::vector<std::wstring>& targets,
    const DeleteAdapterCallbacks& callbacks) {
    return execute_delete(mode, targets, true, true, callbacks);
}

CurrentDeleteRecoveryResult recover_current_delete(
    const std::wstring& deleted_path, int deleted_index,
    const std::vector<std::wstring>& remaining_paths,
    bool loader_was_running,
    const std::function<bool(const std::wstring&, int)>& open_successor,
    std::wstring& current_path, int& current_index, bool& has_image) {
    const PostDeleteTransitionResult transition = run_post_delete_transition(
        deleted_path, deleted_index, static_cast<int>(remaining_paths.size()),
        [&remaining_paths](int index) {
            return remaining_paths[static_cast<size_t>(index)];
        },
        open_successor, current_path, current_index, has_image);

    CurrentDeleteRecoveryResult result;
    result.successor_path = transition.successor_path;
    result.successor_index = transition.successor_index;
    result.successor_attempted = transition.successor_attempted;
    result.successor_opened = transition.successor_opened;
    result.restart_loader = loader_was_running && result.successor_attempted;
    return result;
}

GridDeleteRecoveryResult recover_grid_delete(
    const std::vector<std::wstring>& remaining_paths,
    int previous_grid_selection, const std::wstring& focused_path,
    const std::vector<std::wstring>& remaining_selected_paths,
    std::wstring& current_path, int& current_index, bool& has_image,
    bool& grid_mode, int& grid_selection, std::vector<bool>& selected,
    int& selection_anchor) {
    GridDeleteRecoveryResult result;
    result.index_empty = remaining_paths.empty();
    if (result.index_empty) {
        result.current_identity_changed = has_image || !current_path.empty()
            || current_index != -1;
        current_path.clear();
        current_index = -1;
        has_image = false;
        grid_mode = false;
        grid_selection = -1;
        selected.clear();
        selection_anchor = -1;
        return result;
    }

    grid_selection = focused_path.empty()
        ? -1 : index_of_path(remaining_paths, focused_path);
    if (grid_selection < 0 && !remaining_selected_paths.empty())
        grid_selection = index_of_path(remaining_paths, remaining_selected_paths.front());
    if (grid_selection < 0)
        grid_selection = std::min(
            previous_grid_selection, static_cast<int>(remaining_paths.size()) - 1);

    current_index = current_path.empty()
        ? -1 : index_of_path(remaining_paths, current_path);
    if (current_index < 0 && grid_selection >= 0) {
        current_index = grid_selection;
        current_path = remaining_paths[static_cast<size_t>(grid_selection)];
        has_image = true;
        result.current_identity_changed = true;
    }

    selected.assign(remaining_paths.size(), false);
    for (const auto& path : remaining_selected_paths) {
        const int index = index_of_path(remaining_paths, path);
        if (index >= 0) selected[static_cast<size_t>(index)] = true;
    }
    if (std::none_of(selected.begin(), selected.end(), [](bool value) { return value; })
        && grid_selection >= 0)
        selected[static_cast<size_t>(grid_selection)] = true;
    selection_anchor = grid_selection;
    result.restart_loader = true;
    return result;
}

} // namespace mv
