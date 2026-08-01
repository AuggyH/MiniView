#pragma once

#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mv {

enum class DeleteMode {
    Recycle,
    Permanent,
};

inline constexpr UINT IDM_DELETE = 1031;
inline constexpr UINT IDM_DELETE_PERM = 1032;

enum class DeleteCommandEntry {
    WindowCommand,
    Toolbar,
    ContextMenu,
};

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

std::optional<DeleteIntent> route_delete_key(
    UINT key, LPARAM key_lparam, const DeleteRouteState& state);
std::optional<DeleteIntent> route_delete_command(
    UINT command, bool grid_mode, bool has_selection);

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

struct DeleteShellRequest {
    UINT operation = FO_DELETE;
    std::vector<std::wstring> targets;
    std::wstring from_multi_string;
    FILEOP_FLAGS flags = 0;
};

struct DeleteShellResult {
    int shell_result = 0;
    bool aborted = false;
    std::vector<std::wstring> missing_targets;
};

struct DeleteOsPorts {
    std::function<int(const PermanentDeletePrompt&)> message_box;
    std::function<DeleteShellResult(const DeleteShellRequest&)> shell_delete;
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

struct DeleteKeyGuards {
    bool shift_down = false;
    bool control_down = false;
    bool main_window_focused = false;
    bool ime_composing = false;
};

struct DeleteCompositionState {
    std::vector<std::wstring> index_paths;
    std::wstring current_path;
    int current_index = -1;
    bool has_image = false;
    bool grid_mode = false;
    int grid_selection = -1;
    std::vector<bool> selected;
    int selection_anchor = -1;
    bool loader_running = false;
};

class DeleteCompositionHost {
public:
    virtual ~DeleteCompositionHost() = default;

    virtual HWND delete_owner_window() const = 0;
    virtual DeleteCompositionState capture_delete_state() const = 0;
    virtual void remove_delete_indices(const std::vector<int>& indices) = 0;
    virtual bool open_delete_successor(const std::wstring& path, int index) = 0;
    virtual void set_delete_current_identity(
        const std::wstring& path, int index, bool has_image) = 0;
    virtual void set_delete_grid_state(
        bool grid_mode, int grid_selection, const std::vector<bool>& selected,
        int selection_anchor) = 0;
    virtual void reset_delete_current_bitmap() = 0;
    virtual void stop_delete_loader() = 0;
    virtual void start_delete_loader() = 0;
    virtual void rebuild_delete_thumbnails() = 0;
    virtual void clear_delete_thumbnails() = 0;
    virtual void reset_delete_grid_cache() = 0;
    virtual void ensure_delete_grid_visible() = 0;
    virtual void update_delete_title() = 0;
    virtual void invalidate_delete_view() = 0;
};

class DeleteComposition {
public:
    DeleteComposition(DeleteCompositionHost& host, DeleteOsPorts ports);

    bool handle_key(UINT key, LPARAM key_lparam, const DeleteKeyGuards& guards);
    bool handle_command(DeleteCommandEntry entry, UINT command);

private:
    void dispatch_intent(const DeleteIntent& intent);
    void delete_current(DeleteMode mode);
    void delete_grid(DeleteMode mode);
    void show_incomplete_warning(const wchar_t* message);

    DeleteCompositionHost& m_host;
    DeleteOsPorts m_ports;
};

std::unique_ptr<DeleteComposition> make_delete_composition(
    DeleteCompositionHost& host, DeleteOsPorts ports);
DeleteOsPorts make_windows_delete_ports(DeleteCompositionHost& host);
std::unique_ptr<DeleteComposition> make_windows_delete_composition(
    DeleteCompositionHost& host);

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
