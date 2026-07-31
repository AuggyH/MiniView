#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace mv {

enum class DeleteMode {
    Recycle,
    Permanent,
};

struct DeleteKeyState {
    bool shift_down = false;
    bool control_down = false;
    bool main_window_focused = false;
    bool ime_composing = false;
    bool repeated = false;
};

inline std::optional<DeleteMode> route_delete_key(
    UINT key, const DeleteKeyState& state) {
    if (key != VK_DELETE || state.control_down || !state.main_window_focused
        || state.ime_composing || state.repeated) return std::nullopt;
    return state.shift_down ? DeleteMode::Permanent : DeleteMode::Recycle;
}

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
