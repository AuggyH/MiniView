#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mv {

enum class DeleteMode {
    Recycle,
    Permanent,
};

inline constexpr UINT kDeleteCommandRecycle = 1031;
inline constexpr UINT kDeleteCommandPermanent = 1032;

enum class DeleteTarget {
    CurrentImage,
    GridSelection,
};

struct DeleteIntent {
    DeleteTarget target = DeleteTarget::CurrentImage;
    DeleteMode mode = DeleteMode::Recycle;
};

struct DeleteRouteState {
    bool grid_mode = false;
    bool has_selection = false;
    bool shift_down = false;
    bool control_down = false;
    bool main_window_focused = false;
    bool ime_composing = false;
};

struct DeleteIntentHandlers {
    std::function<void(DeleteMode)> current_image;
    std::function<void(DeleteMode)> grid_selection;
};

std::optional<DeleteIntent> route_delete_key(
    UINT key, LPARAM key_lparam, const DeleteRouteState& state);
std::optional<DeleteIntent> route_delete_command(
    UINT command, bool grid_mode, bool has_selection);
bool dispatch_delete_key(
    UINT key, LPARAM key_lparam, const DeleteRouteState& state,
    const DeleteIntentHandlers& handlers);
bool dispatch_delete_command(
    UINT command, bool grid_mode, bool has_selection,
    const DeleteIntentHandlers& handlers);

struct PermanentDeletePrompt {
    std::wstring title;
    std::wstring message;
    UINT flags = 0;
};

inline PermanentDeletePrompt make_permanent_delete_prompt(
    const std::vector<std::wstring>& targets) {
    PermanentDeletePrompt prompt;
    prompt.title = L"确认永久删除";
    prompt.message = L"将永久删除以下文件：\r\n\r\n";
    for (const auto& target : targets) {
        prompt.message += target;
        prompt.message += L"\r\n";
    }
    prompt.message += L"\r\n此操作无法恢复。";
    prompt.flags = MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2;
    return prompt;
}

inline bool path_is_existing_file(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool delete_targets_are_valid(const std::vector<std::wstring>& targets) {
    return !targets.empty() && std::all_of(targets.begin(), targets.end(),
        [](const std::wstring& target) { return path_is_existing_file(target); });
}

enum class DeleteRequestResult {
    InvalidTargets,
    Cancelled,
    StaleTargets,
    MutationInvoked,
};

struct DeleteMutationResult {
    int shell_result = 0;
    bool aborted = false;
};

struct DeleteAdapterCallbacks {
    std::function<int(const PermanentDeletePrompt&)> confirm;
    std::function<bool(const std::vector<std::wstring>&)> targets_still_current;
    std::function<DeleteMutationResult(
        const std::vector<std::wstring>&, DeleteMode)> mutate;
    std::function<bool(const std::wstring&)> target_is_missing;
    std::function<void()> stop_loader;
    std::function<void()> start_loader;
};

struct DeleteAdapterResult {
    DeleteRequestResult request_result = DeleteRequestResult::InvalidTargets;
    std::vector<size_t> removed_positions;
    std::vector<std::wstring> remaining_targets;
    bool complete = false;
};

DeleteAdapterResult execute_current_delete(
    DeleteMode mode, const std::vector<std::wstring>& targets,
    bool loader_was_running, const DeleteAdapterCallbacks& callbacks);
DeleteAdapterResult execute_grid_delete(
    DeleteMode mode, const std::vector<std::wstring>& targets,
    const DeleteAdapterCallbacks& callbacks);

struct CurrentDeleteRecoveryResult {
    std::wstring successor_path;
    int successor_index = -1;
    bool successor_attempted = false;
    bool successor_opened = false;
    bool restart_loader = false;
};

CurrentDeleteRecoveryResult recover_current_delete(
    const std::wstring& deleted_path, int deleted_index,
    const std::vector<std::wstring>& remaining_paths,
    bool loader_was_running,
    const std::function<bool(const std::wstring&, int)>& open_successor,
    std::wstring& current_path, int& current_index, bool& has_image);

struct GridDeleteRecoveryResult {
    bool index_empty = false;
    bool current_identity_changed = false;
    bool restart_loader = false;
};

GridDeleteRecoveryResult recover_grid_delete(
    const std::vector<std::wstring>& remaining_paths,
    int previous_grid_selection, const std::wstring& focused_path,
    const std::vector<std::wstring>& remaining_selected_paths,
    std::wstring& current_path, int& current_index, bool& has_image,
    bool& grid_mode, int& grid_selection, std::vector<bool>& selected,
    int& selection_anchor);

template <typename Confirm, typename TargetsStillCurrent, typename Mutate>
inline DeleteRequestResult run_guarded_delete(
    DeleteMode mode, const std::vector<std::wstring>& targets,
    Confirm confirm, TargetsStillCurrent targets_still_current, Mutate mutate) {
    if (!delete_targets_are_valid(targets)) return DeleteRequestResult::InvalidTargets;
    if (mode == DeleteMode::Permanent) {
        const PermanentDeletePrompt prompt = make_permanent_delete_prompt(targets);
        if (confirm(prompt) != IDOK) return DeleteRequestResult::Cancelled;
    }
    if (!targets_still_current(targets) || !delete_targets_are_valid(targets))
        return DeleteRequestResult::StaleTargets;
    mutate(targets, mode);
    return DeleteRequestResult::MutationInvoked;
}

inline bool path_is_confirmed_missing(const std::wstring& path) {
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

inline bool delete_fully_completed(
    int shell_result, bool aborted, size_t requested_count, size_t removed_count) {
    return shell_result == 0 && !aborted && removed_count == requested_count;
}

} // namespace mv
