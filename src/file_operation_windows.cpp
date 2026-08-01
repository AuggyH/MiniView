#include "file_operation.h"

#include <shellapi.h>
#include <utility>

namespace mv {

DeleteOsPorts make_windows_delete_ports(DeleteCompositionHost& host) {
    DeleteOsPorts ports;
    ports.message_box = [&host](const PermanentDeletePrompt& prompt) {
        return MessageBoxW(host.delete_owner_window(), prompt.message.c_str(),
            prompt.title.c_str(), prompt.flags);
    };
    ports.shell_delete = [](const DeleteShellRequest& request) {
        SHFILEOPSTRUCTW operation = {};
        operation.wFunc = request.operation;
        operation.pFrom = request.from_multi_string.c_str();
        operation.fFlags = request.flags;
        DeleteShellResult result;
        result.shell_result = SHFileOperationW(&operation);
        result.aborted = operation.fAnyOperationsAborted != FALSE;
        for (const auto& target : request.targets) {
            if (path_is_confirmed_missing(target))
                result.missing_targets.push_back(target);
        }
        return result;
    };
    return ports;
}

std::unique_ptr<DeleteComposition> make_windows_delete_composition(
    DeleteCompositionHost& host) {
    return make_delete_composition(host, make_windows_delete_ports(host));
}

} // namespace mv
