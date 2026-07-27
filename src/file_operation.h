#pragma once

#include <Windows.h>
#include <cstddef>
#include <string>

namespace mv {

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
