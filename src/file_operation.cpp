#include "file_operation.h"
#include "app_state.h"

#include <algorithm>
#include <utility>

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

bool current_delete_state_is_valid(const DeleteCompositionState& state) {
    return can_delete_current_image(state.has_image, state.current_path)
        && state.current_index >= 0
        && state.current_index < static_cast<int>(state.index_paths.size())
        && state.index_paths[static_cast<size_t>(state.current_index)]
            == state.current_path;
}

std::vector<std::wstring> selected_paths_from_state(
    const DeleteCompositionState& state) {
    std::vector<std::wstring> paths;
    if (state.selected.size() != state.index_paths.size()) return paths;
    for (size_t i = 0; i < state.selected.size(); ++i) {
        if (state.selected[i]) paths.push_back(state.index_paths[i]);
    }
    return paths;
}

DeleteShellRequest make_shell_request(
    const std::vector<std::wstring>& targets, DeleteMode mode) {
    DeleteShellRequest request;
    request.targets = targets;
    request.flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (mode == DeleteMode::Recycle) request.flags |= FOF_ALLOWUNDO;
    for (const auto& target : targets) {
        request.from_multi_string += target;
        request.from_multi_string.push_back(L'\0');
    }
    request.from_multi_string.push_back(L'\0');
    return request;
}

std::vector<std::wstring> paths_after_removal(
    const std::vector<std::wstring>& paths,
    const std::vector<int>& removed_indices) {
    std::vector<std::wstring> remaining;
    remaining.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        if (std::find(removed_indices.begin(), removed_indices.end(),
                static_cast<int>(i)) == removed_indices.end())
            remaining.push_back(paths[i]);
    }
    return remaining;
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

DeleteComposition::DeleteComposition(
    DeleteCompositionHost& host, DeleteOsPorts ports)
    : m_host(host), m_ports(std::move(ports)) {}

bool DeleteComposition::handle_key(
    UINT key, LPARAM key_lparam, const DeleteKeyGuards& guards) {
    if (key != VK_DELETE) return false;

    const DeleteCompositionState state = m_host.capture_delete_state();
    DeleteRouteState route_state;
    route_state.grid_mode = state.grid_mode;
    route_state.has_selection = !selected_paths_from_state(state).empty();
    route_state.shift_down = guards.shift_down;
    route_state.control_down = guards.control_down;
    route_state.main_window_focused = guards.main_window_focused;
    route_state.ime_composing = guards.ime_composing;
    const auto intent = route_delete_key(key, key_lparam, route_state);
    if (intent) dispatch_intent(*intent);
    return true;
}

bool DeleteComposition::handle_window_command(UINT command) {
    return handle_command(command);
}

bool DeleteComposition::handle_toolbar_command(UINT command) {
    return handle_command(command);
}

bool DeleteComposition::handle_context_command(UINT command) {
    return handle_command(command);
}

bool DeleteComposition::handle_command(UINT command) {
    if (command != kDeleteCommandRecycle
        && command != kDeleteCommandPermanent)
        return false;

    const DeleteCompositionState state = m_host.capture_delete_state();
    const auto intent = route_delete_command(
        command, state.grid_mode, !selected_paths_from_state(state).empty());
    if (intent) dispatch_intent(*intent);
    return true;
}

void DeleteComposition::dispatch_intent(const DeleteIntent& intent) {
    if (intent.target == DeleteTarget::GridSelection)
        delete_grid(intent.mode);
    else
        delete_current(intent.mode);
}

void DeleteComposition::delete_current(DeleteMode mode) {
    const DeleteCompositionState initial = m_host.capture_delete_state();
    if (!current_delete_state_is_valid(initial)) return;

    const std::wstring deleted_path = initial.current_path;
    const std::vector<std::wstring> requested_paths = {deleted_path};
    std::vector<std::wstring> reported_missing;
    DeleteAdapterCallbacks callbacks;
    callbacks.confirm = [this](const PermanentDeletePrompt& prompt) {
        return m_ports.message_box ? m_ports.message_box(prompt) : 0;
    };
    callbacks.targets_still_current = [this](
        const std::vector<std::wstring>& approved_paths) {
        const DeleteCompositionState current = m_host.capture_delete_state();
        return approved_paths.size() == 1
            && current_delete_state_is_valid(current)
            && current.current_path == approved_paths.front();
    };
    callbacks.mutate = [this, &reported_missing](
        const std::vector<std::wstring>& approved_paths, DeleteMode approved_mode) {
        const DeleteShellRequest request =
            make_shell_request(approved_paths, approved_mode);
        DeleteShellResult shell_result;
        if (m_ports.shell_delete) shell_result = m_ports.shell_delete(request);
        else shell_result.shell_result = ERROR_CALL_NOT_IMPLEMENTED;
        reported_missing = std::move(shell_result.missing_targets);
        return DeleteMutationResult{shell_result.shell_result, shell_result.aborted};
    };
    callbacks.target_is_missing = [&reported_missing](const std::wstring& path) {
        return std::find(reported_missing.begin(), reported_missing.end(), path)
            != reported_missing.end();
    };
    callbacks.stop_loader = [this] { m_host.stop_delete_loader(); };
    callbacks.start_loader = [this] { m_host.start_delete_loader(); };

    const DeleteAdapterResult delete_result = execute_current_delete(
        mode, requested_paths, initial.loader_running, callbacks);
    if (delete_result.request_result != DeleteRequestResult::MutationInvoked) return;
    if (delete_result.removed_positions.empty()) {
        if (!delete_result.complete)
            show_incomplete_warning(L"删除未完成，文件仍保留在列表中。");
        return;
    }

    const int removed_index = initial.current_index;
    m_host.remove_delete_indices({removed_index});
    const std::vector<std::wstring> remaining_paths =
        paths_after_removal(initial.index_paths, {removed_index});
    m_host.rebuild_delete_thumbnails();
    m_host.reset_delete_current_bitmap();

    std::wstring current_path = initial.current_path;
    int current_index = initial.current_index;
    bool has_image = initial.has_image;
    const CurrentDeleteRecoveryResult recovery = recover_current_delete(
        deleted_path, removed_index, remaining_paths, initial.loader_running,
        [this](const std::wstring& path, int index) {
            return m_host.open_delete_successor(path, index);
        },
        current_path, current_index, has_image);
    m_host.set_delete_current_identity(current_path, current_index, has_image);

    if (!recovery.successor_attempted) {
        m_host.update_delete_title();
        m_host.invalidate_delete_view();
        if (!delete_result.complete)
            show_incomplete_warning(
                L"删除操作报告未完全完成，列表已按磁盘实际状态更新。");
        return;
    }
    if (!recovery.successor_opened) {
        m_host.update_delete_title();
        m_host.invalidate_delete_view();
    }
    if (recovery.restart_loader) m_host.start_delete_loader();
    if (!delete_result.complete)
        show_incomplete_warning(
            L"删除操作报告未完全完成，列表已按磁盘实际状态更新。");
}

void DeleteComposition::delete_grid(DeleteMode mode) {
    const DeleteCompositionState initial = m_host.capture_delete_state();
    const std::vector<std::wstring> requested_paths =
        selected_paths_from_state(initial);
    if (!initial.grid_mode || requested_paths.empty()) return;

    std::vector<int> requested_indices;
    requested_indices.reserve(requested_paths.size());
    for (size_t i = 0; i < initial.selected.size(); ++i) {
        if (initial.selected[i]) requested_indices.push_back(static_cast<int>(i));
    }

    std::wstring focused_path;
    if (initial.grid_selection >= 0
        && initial.grid_selection < static_cast<int>(initial.index_paths.size()))
        focused_path = initial.index_paths[static_cast<size_t>(initial.grid_selection)];

    std::vector<std::wstring> reported_missing;
    DeleteAdapterCallbacks callbacks;
    callbacks.confirm = [this](const PermanentDeletePrompt& prompt) {
        return m_ports.message_box ? m_ports.message_box(prompt) : 0;
    };
    callbacks.targets_still_current = [this](
        const std::vector<std::wstring>& approved_paths) {
        const DeleteCompositionState current = m_host.capture_delete_state();
        return current.grid_mode
            && approved_paths == selected_paths_from_state(current);
    };
    callbacks.mutate = [this, &reported_missing](
        const std::vector<std::wstring>& approved_paths, DeleteMode approved_mode) {
        const DeleteShellRequest request =
            make_shell_request(approved_paths, approved_mode);
        DeleteShellResult shell_result;
        if (m_ports.shell_delete) shell_result = m_ports.shell_delete(request);
        else shell_result.shell_result = ERROR_CALL_NOT_IMPLEMENTED;
        reported_missing = std::move(shell_result.missing_targets);
        return DeleteMutationResult{shell_result.shell_result, shell_result.aborted};
    };
    callbacks.target_is_missing = [&reported_missing](const std::wstring& path) {
        return std::find(reported_missing.begin(), reported_missing.end(), path)
            != reported_missing.end();
    };
    callbacks.stop_loader = [this] { m_host.stop_delete_loader(); };
    callbacks.start_loader = [this] { m_host.start_delete_loader(); };

    const DeleteAdapterResult delete_result =
        execute_grid_delete(mode, requested_paths, callbacks);
    if (delete_result.request_result != DeleteRequestResult::MutationInvoked) return;

    std::vector<int> removed_indices;
    removed_indices.reserve(delete_result.removed_positions.size());
    for (size_t position : delete_result.removed_positions) {
        if (position < requested_indices.size())
            removed_indices.push_back(requested_indices[position]);
    }
    if (removed_indices.empty()) {
        if (!delete_result.complete)
            show_incomplete_warning(L"删除未完成，文件仍保留在列表中。");
        return;
    }

    m_host.remove_delete_indices(removed_indices);
    const std::vector<std::wstring> remaining_paths =
        paths_after_removal(initial.index_paths, removed_indices);
    std::wstring current_path = initial.current_path;
    int current_index = initial.current_index;
    bool has_image = initial.has_image;
    bool grid_mode = initial.grid_mode;
    int grid_selection = initial.grid_selection;
    std::vector<bool> selected = initial.selected;
    int selection_anchor = initial.selection_anchor;
    const GridDeleteRecoveryResult recovery = recover_grid_delete(
        remaining_paths, initial.grid_selection, focused_path,
        delete_result.remaining_targets,
        current_path, current_index, has_image,
        grid_mode, grid_selection, selected, selection_anchor);
    m_host.set_delete_current_identity(current_path, current_index, has_image);
    m_host.set_delete_grid_state(
        grid_mode, grid_selection, selected, selection_anchor);
    if (recovery.current_identity_changed)
        m_host.reset_delete_current_bitmap();

    if (recovery.index_empty) {
        m_host.stop_delete_loader();
        m_host.clear_delete_thumbnails();
        m_host.update_delete_title();
        m_host.invalidate_delete_view();
        if (!delete_result.complete)
            show_incomplete_warning(
                L"删除操作报告未完全完成，列表已按磁盘实际状态更新。");
        return;
    }

    m_host.rebuild_delete_thumbnails();
    m_host.reset_delete_grid_cache();
    if (recovery.restart_loader) m_host.start_delete_loader();
    m_host.ensure_delete_grid_visible();
    m_host.invalidate_delete_view();
    if (!delete_result.complete)
        show_incomplete_warning(
            L"删除操作未完全完成，列表已按磁盘实际状态更新。");
}

void DeleteComposition::show_incomplete_warning(const wchar_t* message) {
    if (!m_ports.message_box) return;
    PermanentDeletePrompt warning;
    warning.title = L"MinView";
    warning.message = message;
    warning.flags = MB_OK | MB_ICONWARNING;
    m_ports.message_box(warning);
}

std::unique_ptr<DeleteComposition> make_delete_composition(
    DeleteCompositionHost& host, DeleteOsPorts ports) {
    return std::make_unique<DeleteComposition>(host, std::move(ports));
}

} // namespace mv
