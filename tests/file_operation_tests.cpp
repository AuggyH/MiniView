#include "file_operation.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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

} // namespace

int main() {
    TempDirectory temp;
    const fs::path existing = temp.path() / L"existing.png";
    {
        std::ofstream output(existing, std::ios::binary);
        output << 'x';
    }

    expect(!mv::path_is_confirmed_missing(existing.wstring()),
        "an existing file must remain in the index");
    expect(mv::path_is_confirmed_missing((temp.path() / L"missing.png").wstring()),
        "a file-not-found postcondition should remove the index entry");
    expect(mv::path_is_existing_file(existing.wstring()),
        "an existing file should be a valid delete target");
    expect(!mv::path_is_existing_file(temp.path().wstring()),
        "a directory must not be accepted as a file delete target");

    mv::DeleteKeyState key_state;
    key_state.main_window_focused = true;
    auto routed_mode = mv::route_delete_key(VK_DELETE, key_state);
    expect(routed_mode && *routed_mode == mv::DeleteMode::Recycle,
        "Del should route to recycle-bin deletion");
    key_state.shift_down = true;
    routed_mode = mv::route_delete_key(VK_DELETE, key_state);
    expect(routed_mode && *routed_mode == mv::DeleteMode::Permanent,
        "Shift+Del should route to permanent deletion");
    key_state.control_down = true;
    expect(!mv::route_delete_key(VK_DELETE, key_state),
        "Ctrl+Shift+Del must not route to deletion");
    key_state.control_down = false;
    key_state.main_window_focused = false;
    expect(!mv::route_delete_key(VK_DELETE, key_state),
        "delete input outside the focused main window must fail closed");
    key_state.main_window_focused = true;
    key_state.ime_composing = true;
    expect(!mv::route_delete_key(VK_DELETE, key_state),
        "delete input during IME composition must fail closed");
    key_state.ime_composing = false;
    key_state.repeated = true;
    expect(!mv::route_delete_key(VK_DELETE, key_state),
        "repeated delete keydown must fail closed");
    key_state.repeated = false;
    expect(!mv::route_delete_key(VK_RETURN, key_state),
        "unrelated keys must not route to deletion");

    const fs::path second = temp.path() / L"second.png";
    {
        std::ofstream output(second, std::ios::binary);
        output << 'y';
    }
    const std::vector<std::wstring> targets = {
        existing.wstring(), second.wstring()};
    const mv::PermanentDeletePrompt prompt =
        mv::make_permanent_delete_prompt(targets);
    expect(prompt.title.find(L"永久删除") != std::wstring::npos,
        "the prompt title should identify permanent deletion in Chinese");
    expect(prompt.message.find(L"永久删除") != std::wstring::npos
            && prompt.message.find(L"无法恢复") != std::wstring::npos,
        "the prompt should state permanent and unrecoverable in Chinese");
    expect(prompt.message.find(targets[0]) != std::wstring::npos
            && prompt.message.find(targets[1]) != std::wstring::npos,
        "the prompt should identify every exact target");
    expect((prompt.flags & MB_TYPEMASK) == MB_OKCANCEL,
        "the prompt should provide explicit OK and Cancel choices");
    expect((prompt.flags & MB_DEFMASK) == MB_DEFBUTTON2,
        "Cancel should be the safe default button");

    int confirm_calls = 0;
    int mutation_calls = 0;
    std::vector<std::wstring> mutated_targets;
    mv::DeleteMode mutated_mode = mv::DeleteMode::Recycle;
    auto result = mv::run_guarded_delete(
        mv::DeleteMode::Permanent, targets,
        [&confirm_calls](const mv::PermanentDeletePrompt&) {
            ++confirm_calls;
            return IDOK;
        },
        [&targets](const std::vector<std::wstring>& current) {
            return current == targets;
        },
        [&mutation_calls, &mutated_targets, &mutated_mode](
            const std::vector<std::wstring>& approved, mv::DeleteMode mode) {
            ++mutation_calls;
            mutated_targets = approved;
            mutated_mode = mode;
        });
    expect(result == mv::DeleteRequestResult::MutationInvoked
            && confirm_calls == 1 && mutation_calls == 1,
        "explicit confirmation should invoke one permanent-delete mutation");
    expect(mutated_targets == targets && mutated_mode == mv::DeleteMode::Permanent,
        "the grid deletion path should mutate only the confirmed target snapshot");

    const std::vector<std::wstring> single_target = {existing.wstring()};
    mutation_calls = 0;
    result = mv::run_guarded_delete(
        mv::DeleteMode::Permanent, single_target,
        [](const mv::PermanentDeletePrompt&) { return IDOK; },
        [&single_target](const std::vector<std::wstring>& current) {
            return current == single_target;
        },
        [&mutation_calls, &single_target](
            const std::vector<std::wstring>& approved, mv::DeleteMode mode) {
            if (approved == single_target && mode == mv::DeleteMode::Permanent)
                ++mutation_calls;
        });
    expect(result == mv::DeleteRequestResult::MutationInvoked && mutation_calls == 1,
        "the current-image deletion path should use the same confirmation gate");

    const auto expect_zero_mutation = [&targets](int response, const char* message) {
        int mutations = 0;
        const auto rejected = mv::run_guarded_delete(
            mv::DeleteMode::Permanent, targets,
            [response](const mv::PermanentDeletePrompt&) { return response; },
            [](const std::vector<std::wstring>&) { return true; },
            [&mutations](const std::vector<std::wstring>&, mv::DeleteMode) {
                ++mutations;
            });
        expect(rejected == mv::DeleteRequestResult::Cancelled && mutations == 0, message);
    };
    expect_zero_mutation(IDCANCEL, "Cancel or Esc should produce zero mutation");
    expect_zero_mutation(IDCLOSE, "closing the prompt should produce zero mutation");
    expect_zero_mutation(0, "an unknown prompt result should produce zero mutation");

    confirm_calls = 0;
    mutation_calls = 0;
    result = mv::run_guarded_delete(
        mv::DeleteMode::Recycle, single_target,
        [&confirm_calls](const mv::PermanentDeletePrompt&) {
            ++confirm_calls;
            return IDCANCEL;
        },
        [](const std::vector<std::wstring>&) { return true; },
        [&mutation_calls, &mutated_mode](
            const std::vector<std::wstring>&, mv::DeleteMode mode) {
            ++mutation_calls;
            mutated_mode = mode;
        });
    expect(result == mv::DeleteRequestResult::MutationInvoked
            && confirm_calls == 0 && mutation_calls == 1
            && mutated_mode == mv::DeleteMode::Recycle,
        "ordinary Del should keep recycle-bin behavior without a permanent-delete prompt");

    mutation_calls = 0;
    result = mv::run_guarded_delete(
        mv::DeleteMode::Permanent, {},
        [](const mv::PermanentDeletePrompt&) { return IDOK; },
        [](const std::vector<std::wstring>&) { return true; },
        [&mutation_calls](const std::vector<std::wstring>&, mv::DeleteMode) {
            ++mutation_calls;
        });
    expect(result == mv::DeleteRequestResult::InvalidTargets && mutation_calls == 0,
        "an empty target set must fail closed");

    const std::vector<std::wstring> missing_target = {
        (temp.path() / L"missing.png").wstring()};
    result = mv::run_guarded_delete(
        mv::DeleteMode::Permanent, missing_target,
        [](const mv::PermanentDeletePrompt&) { return IDOK; },
        [](const std::vector<std::wstring>&) { return true; },
        [&mutation_calls](const std::vector<std::wstring>&, mv::DeleteMode) {
            ++mutation_calls;
        });
    expect(result == mv::DeleteRequestResult::InvalidTargets && mutation_calls == 0,
        "a missing target must fail closed before confirmation and mutation");

    result = mv::run_guarded_delete(
        mv::DeleteMode::Permanent, single_target,
        [](const mv::PermanentDeletePrompt&) { return IDOK; },
        [](const std::vector<std::wstring>&) { return false; },
        [&mutation_calls](const std::vector<std::wstring>&, mv::DeleteMode) {
            ++mutation_calls;
        });
    expect(result == mv::DeleteRequestResult::StaleTargets && mutation_calls == 0,
        "a stale selection after confirmation must fail closed");

    const fs::path vanishing = temp.path() / L"vanishing.png";
    {
        std::ofstream output(vanishing, std::ios::binary);
        output << 'z';
    }
    const std::vector<std::wstring> vanishing_target = {vanishing.wstring()};
    result = mv::run_guarded_delete(
        mv::DeleteMode::Permanent, vanishing_target,
        [&vanishing](const mv::PermanentDeletePrompt&) {
            fs::remove(vanishing);
            return IDOK;
        },
        [](const std::vector<std::wstring>&) { return true; },
        [&mutation_calls](const std::vector<std::wstring>&, mv::DeleteMode) {
            ++mutation_calls;
        });
    expect(result == mv::DeleteRequestResult::StaleTargets && mutation_calls == 0,
        "a target that disappears while confirming must fail closed");

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
