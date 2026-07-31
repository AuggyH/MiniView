#include "file_operation.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
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

std::string read_source_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool request_has_exact_multi_string(
    const mv::DeleteShellRequest& request,
    const std::vector<std::wstring>& expected) {
    size_t position = 0;
    for (const auto& path : expected) {
        if (position + path.size() >= request.from_multi_string.size()) return false;
        if (request.from_multi_string.compare(position, path.size(), path) != 0)
            return false;
        position += path.size();
        if (request.from_multi_string[position] != L'\0') return false;
        ++position;
    }
    return position + 1 == request.from_multi_string.size()
        && request.from_multi_string[position] == L'\0';
}

bool request_has_silent_shell_flags(const mv::DeleteShellRequest& request) {
    const FILEOP_FLAGS required =
        FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    return (request.flags & required) == required;
}

struct FakeDeleteHost final : mv::DeleteCompositionHost {
    mv::DeleteCompositionState state;
    bool open_successor_result = true;
    int remove_calls = 0;
    int open_successor_calls = 0;
    int set_current_calls = 0;
    int set_grid_calls = 0;
    int reset_bitmap_calls = 0;
    int stop_loader_calls = 0;
    int start_loader_calls = 0;
    int rebuild_thumbnails_calls = 0;
    int clear_thumbnails_calls = 0;
    int reset_grid_cache_calls = 0;
    int ensure_grid_visible_calls = 0;
    int update_title_calls = 0;
    int invalidate_calls = 0;
    std::vector<int> removed_indices;
    std::wstring opened_path;
    int opened_index = -1;

    HWND delete_owner_window() const override { return nullptr; }

    mv::DeleteCompositionState capture_delete_state() const override {
        return state;
    }

    void remove_delete_indices(const std::vector<int>& indices) override {
        ++remove_calls;
        removed_indices = indices;
        std::vector<int> descending = indices;
        std::sort(descending.begin(), descending.end(), std::greater<int>());
        for (int index : descending) {
            if (index >= 0 && index < static_cast<int>(state.index_paths.size()))
                state.index_paths.erase(state.index_paths.begin() + index);
        }
    }

    bool open_delete_successor(const std::wstring& path, int index) override {
        ++open_successor_calls;
        opened_path = path;
        opened_index = index;
        return open_successor_result;
    }

    void set_delete_current_identity(
        const std::wstring& path, int index, bool has_image) override {
        ++set_current_calls;
        state.current_path = path;
        state.current_index = index;
        state.has_image = has_image;
    }

    void set_delete_grid_state(
        bool grid_mode, int grid_selection, const std::vector<bool>& selected,
        int selection_anchor) override {
        ++set_grid_calls;
        state.grid_mode = grid_mode;
        state.grid_selection = grid_selection;
        state.selected = selected;
        state.selection_anchor = selection_anchor;
    }

    void reset_delete_current_bitmap() override { ++reset_bitmap_calls; }
    void stop_delete_loader() override { ++stop_loader_calls; }
    void start_delete_loader() override { ++start_loader_calls; }
    void rebuild_delete_thumbnails() override { ++rebuild_thumbnails_calls; }
    void clear_delete_thumbnails() override { ++clear_thumbnails_calls; }
    void reset_delete_grid_cache() override { ++reset_grid_cache_calls; }
    void ensure_delete_grid_visible() override { ++ensure_grid_visible_calls; }
    void update_delete_title() override { ++update_title_calls; }
    void invalidate_delete_view() override { ++invalidate_calls; }
};

struct FakeDeletePorts {
    int confirmation_response = IDOK;
    mv::DeleteShellResult shell_result;
    std::function<void(const mv::PermanentDeletePrompt&)> after_confirmation;
    std::vector<mv::PermanentDeletePrompt> dialogs;
    std::vector<mv::DeleteShellRequest> shell_requests;

    mv::DeleteOsPorts make_ports() {
        mv::DeleteOsPorts ports;
        ports.message_box = [this](const mv::PermanentDeletePrompt& prompt) {
            dialogs.push_back(prompt);
            if ((prompt.flags & MB_TYPEMASK) == MB_OKCANCEL) {
                if (after_confirmation) {
                    auto callback = std::move(after_confirmation);
                    callback(prompt);
                }
                return confirmation_response;
            }
            return IDOK;
        };
        ports.shell_delete = [this](const mv::DeleteShellRequest& request) {
            shell_requests.push_back(request);
            return shell_result;
        };
        return ports;
    }

    int confirmation_calls() const {
        return static_cast<int>(std::count_if(
            dialogs.begin(), dialogs.end(), [](const auto& dialog) {
                return (dialog.flags & MB_TYPEMASK) == MB_OKCANCEL;
            }));
    }

    int warning_calls() const {
        return static_cast<int>(dialogs.size()) - confirmation_calls();
    }
};

mv::DeleteCompositionState make_current_state(
    const std::wstring& first, const std::wstring& second,
    const std::wstring& third) {
    mv::DeleteCompositionState state;
    state.index_paths = {first, second, third};
    state.current_path = first;
    state.current_index = 0;
    state.has_image = true;
    state.loader_running = true;
    return state;
}

mv::DeleteCompositionState make_grid_state(
    const std::wstring& first, const std::wstring& second,
    const std::wstring& third) {
    mv::DeleteCompositionState state = make_current_state(first, second, third);
    state.grid_mode = true;
    state.grid_selection = 0;
    state.selected = {true, true, false};
    state.selection_anchor = 0;
    return state;
}

mv::DeleteKeyGuards focused_guards(bool shift = false) {
    mv::DeleteKeyGuards guards;
    guards.shift_down = shift;
    guards.main_window_focused = true;
    return guards;
}

void test_app_delegation_contract(const fs::path& source_root) {
    const std::string app_header = read_source_file(source_root / "src/app.h");
    const std::string app_source = read_source_file(source_root / "src/app.cpp");
    const std::string cmake = read_source_file(source_root / "CMakeLists.txt");

    expect(app_header.find("class App : private DeleteCompositionHost")
            != std::string::npos,
        "App must be the production DeleteComposition host");
    expect(app_header.find(
            "std::unique_ptr<DeleteComposition> m_delete_composition")
            != std::string::npos,
        "App must own the production DeleteComposition object");
    expect(app_source.find("make_windows_delete_composition(*this)")
            != std::string::npos,
        "App must instantiate the production Windows composition factory");
    expect(app_source.find("m_delete_composition->handle_key")
            != std::string::npos,
        "App WM_KEYDOWN must delegate to DeleteComposition");
    expect(app_source.find("m_delete_composition->handle_window_command")
            != std::string::npos,
        "App WM_COMMAND must delegate to DeleteComposition");
    expect(app_source.find("m_delete_composition->handle_toolbar_command")
            != std::string::npos,
        "the toolbar must delegate delete commands to DeleteComposition");
    expect(app_source.find("m_delete_composition->handle_context_command")
            != std::string::npos,
        "the context menu must delegate delete commands to DeleteComposition");
    expect(app_source.find("DeleteAdapterCallbacks") == std::string::npos
            && app_source.find("SHFileOperationW") == std::string::npos
            && app_source.find("delete_current_file(") == std::string::npos
            && app_source.find("delete_selected(") == std::string::npos,
        "app.cpp must not retain a parallel delete composition or Shell mutation");

    const size_t test_target = cmake.find("add_executable(file_operation_tests");
    const size_t test_target_end = cmake.find(
        "target_include_directories(file_operation_tests", test_target);
    expect(test_target != std::string::npos
            && test_target_end != std::string::npos
            && cmake.substr(test_target, test_target_end - test_target)
                .find("file_operation_windows.cpp") == std::string::npos,
        "the unit target must not link the real Windows delete ports");
}

void test_keyboard_guards(
    const std::wstring& first, const std::wstring& second,
    const std::wstring& third) {
    FakeDeleteHost host;
    host.state = make_current_state(first, second, third);
    FakeDeletePorts fake;
    auto composition = mv::make_delete_composition(host, fake.make_ports());

    expect(!composition->handle_key(VK_RETURN, 0, focused_guards()),
        "unrelated keys must not be consumed by the delete composition");
    expect(composition->handle_key(VK_DELETE, 0, mv::DeleteKeyGuards{}),
        "an unfocused Delete key must be consumed but fail closed");

    mv::DeleteKeyGuards guards = focused_guards(true);
    guards.control_down = true;
    expect(composition->handle_key(VK_DELETE, 0, guards),
        "Ctrl+Shift+Del must be consumed but fail closed");
    guards.control_down = false;
    guards.ime_composing = true;
    expect(composition->handle_key(VK_DELETE, 0, guards),
        "Delete during IME composition must be consumed but fail closed");
    guards.ime_composing = false;
    const LPARAM repeated = static_cast<LPARAM>(ULONG_PTR{1} << 30);
    expect(composition->handle_key(VK_DELETE, repeated, guards),
        "a repeated Delete key must be consumed but fail closed");

    host.state.grid_mode = true;
    host.state.selected = {false, false, false};
    expect(composition->handle_key(VK_DELETE, 0, guards),
        "grid Delete without selection must be consumed but fail closed");
    expect(fake.confirmation_calls() == 0 && fake.shell_requests.empty()
            && host.remove_calls == 0 && host.stop_loader_calls == 0,
        "focus, Ctrl, IME, repeat, and empty-selection guards must prevent mutation");
}

void test_current_keyboard_paths(
    const std::wstring& first, const std::wstring& second,
    const std::wstring& third) {
    {
        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        FakeDeletePorts fake;
        fake.shell_result.missing_targets = {first};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        expect(composition->handle_key(VK_DELETE, 0, focused_guards()),
            "ordinary Del must enter the production composition");
        expect(fake.confirmation_calls() == 0 && fake.shell_requests.size() == 1,
            "ordinary Del must skip confirmation and invoke the Shell port once");
        const auto& request = fake.shell_requests.front();
        expect(request.operation == FO_DELETE && request.targets == std::vector{first},
            "ordinary Del must bind Shell deletion to the exact current snapshot");
        expect((request.flags & FOF_ALLOWUNDO) != 0
                && request_has_silent_shell_flags(request)
                && request_has_exact_multi_string(request, {first}),
            "ordinary Del must use recycle semantics and a double-NUL Shell list");
        expect(host.removed_indices == std::vector<int>{0}
                && host.state.index_paths == std::vector{second, third},
            "current deletion must remove only the reported missing index entry");
        expect(host.open_successor_calls == 1 && host.opened_path == second
                && host.opened_index == 0 && host.state.current_path == second
                && host.state.current_index == 0 && host.state.has_image,
            "current deletion must recover the exact successor through the host");
        expect(host.stop_loader_calls == 1 && host.start_loader_calls == 1
                && host.rebuild_thumbnails_calls == 1
                && host.reset_bitmap_calls == 1,
            "current deletion must preserve loader and thumbnail recovery semantics");
    }

    {
        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        host.state.loader_running = false;
        FakeDeletePorts fake;
        fake.shell_result.missing_targets = {first};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        composition->handle_key(VK_DELETE, 0, focused_guards(true));
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.size() == 1,
            "Shift+Del plus IDOK must confirm and invoke Shell exactly once");
        const auto& prompt = fake.dialogs.front();
        expect(prompt.title.find(L"永久删除") != std::wstring::npos
                && prompt.message.find(L"永久删除") != std::wstring::npos
                && prompt.message.find(L"无法恢复") != std::wstring::npos
                && prompt.message.find(first) != std::wstring::npos,
            "the production prompt must identify permanent, unrecoverable exact targets in Chinese");
        expect((prompt.flags & MB_TYPEMASK) == MB_OKCANCEL
                && (prompt.flags & MB_DEFMASK) == MB_DEFBUTTON2,
            "the production prompt must expose OK/Cancel with Cancel as default");
        const auto& request = fake.shell_requests.front();
        expect((request.flags & FOF_ALLOWUNDO) == 0
                && request_has_silent_shell_flags(request)
                && request_has_exact_multi_string(request, {first}),
            "permanent deletion must omit FOF_ALLOWUNDO and retain double-NUL framing");
        expect(host.stop_loader_calls == 0 && host.start_loader_calls == 0,
            "a previously stopped loader must remain stopped");
    }

    for (int response : {IDCANCEL, IDCLOSE, 0, 7777}) {
        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        FakeDeletePorts fake;
        fake.confirmation_response = response;
        fake.shell_result.missing_targets = {first};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        composition->handle_key(VK_DELETE, 0, focused_guards(true));
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.empty()
                && host.remove_calls == 0 && host.stop_loader_calls == 0
                && host.start_loader_calls == 0,
            "Cancel, close, Esc, and unknown results must produce zero mutation");
        expect(host.state.current_path == first && host.state.index_paths.size() == 3,
            "a rejected permanent delete must preserve the exact App state");
    }

    {
        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        FakeDeletePorts fake;
        fake.after_confirmation = [&host, &second](const auto&) {
            host.state.current_path = second;
            host.state.current_index = 1;
        };
        fake.shell_result.missing_targets = {first};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        composition->handle_key(VK_DELETE, 0, focused_guards(true));
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.empty()
                && host.remove_calls == 0 && host.stop_loader_calls == 0,
            "a current-image identity drift after confirmation must fail closed");
    }
}

void test_command_sources_and_recovery(
    const std::wstring& first, const std::wstring& second,
    const std::wstring& third) {
    {
        FakeDeleteHost host;
        host.state = make_grid_state(first, second, third);
        FakeDeletePorts fake;
        fake.shell_result.missing_targets = {first, second};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        expect(composition->handle_key(VK_DELETE, 0, focused_guards(true)),
            "grid Shift+Del must enter the production keyboard composition");
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.size() == 1
                && fake.shell_requests.front().targets == std::vector{first, second},
            "grid Shift+Del must bind the exact selected snapshot");
        expect(host.removed_indices == std::vector<int>({0, 1})
                && host.state.index_paths == std::vector{third}
                && host.state.current_path == third
                && host.state.current_index == 0 && host.state.has_image
                && host.state.grid_mode && host.state.grid_selection == 0
                && host.state.selected == std::vector<bool>({true}),
            "grid Shift+Del must run the production grid handler and recovery");
    }

    {
        FakeDeleteHost host;
        host.state = make_grid_state(first, second, third);
        FakeDeletePorts fake;
        fake.shell_result.shell_result = 5;
        fake.shell_result.aborted = true;
        fake.shell_result.missing_targets = {first};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        expect(composition->handle_window_command(mv::kDeleteCommandPermanent),
            "WM_COMMAND must enter the production composition");
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.size() == 1,
            "grid permanent WM_COMMAND must confirm and invoke Shell once");
        const auto& request = fake.shell_requests.front();
        expect(request.targets == std::vector{first, second}
                && (request.flags & FOF_ALLOWUNDO) == 0
                && request_has_silent_shell_flags(request)
                && request_has_exact_multi_string(request, {first, second}),
            "grid permanent deletion must bind the exact multi-select double-NUL snapshot");
        expect(host.removed_indices == std::vector<int>{0}
                && host.state.index_paths == std::vector{second, third},
            "an aborted partial Shell operation must remove only confirmed missing targets");
        expect(host.state.current_path == second && host.state.current_index == 0
                && host.state.grid_mode && host.state.grid_selection == 0
                && host.state.selected == std::vector<bool>({true, false})
                && host.state.selection_anchor == 0,
            "partial grid failure must recover current identity, focus, and selection");
        expect(host.stop_loader_calls == 1 && host.start_loader_calls == 1
                && host.rebuild_thumbnails_calls == 1
                && host.reset_bitmap_calls == 1
                && host.reset_grid_cache_calls == 1
                && host.ensure_grid_visible_calls == 1
                && fake.warning_calls() == 1,
            "an aborted partial grid mutation must run the production recovery and warning path");
    }

    {
        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        FakeDeletePorts fake;
        fake.shell_result.shell_result = 5;
        fake.shell_result.aborted = true;
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        expect(composition->handle_toolbar_command(mv::kDeleteCommandRecycle),
            "toolbar delete forwarding must enter the production composition");
        expect(fake.confirmation_calls() == 0 && fake.shell_requests.size() == 1
                && (fake.shell_requests.front().flags & FOF_ALLOWUNDO) != 0,
            "toolbar ordinary delete must preserve recycle semantics without confirmation");
        expect(host.remove_calls == 0 && host.stop_loader_calls == 1
                && host.start_loader_calls == 1 && fake.warning_calls() == 1,
            "a failed toolbar Shell operation must restore the loader and retain the list");
    }

    {
        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        host.state.loader_running = false;
        FakeDeletePorts fake;
        fake.shell_result.missing_targets = {first};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        expect(composition->handle_context_command(mv::kDeleteCommandPermanent),
            "context delete forwarding must enter the production composition");
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.size() == 1
                && (fake.shell_requests.front().flags & FOF_ALLOWUNDO) == 0,
            "context permanent delete must use the same IDOK-only permanent path");
        expect(!composition->handle_context_command(9999),
            "unrelated context commands must not be claimed by the composition");
    }

    {
        FakeDeleteHost host;
        host.state = make_grid_state(first, second, third);
        FakeDeletePorts fake;
        fake.after_confirmation = [&host](const auto&) {
            host.state.selected = {false, true, false};
            host.state.grid_selection = 1;
        };
        fake.shell_result.missing_targets = {first, second};
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        composition->handle_window_command(mv::kDeleteCommandPermanent);
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.empty()
                && host.remove_calls == 0 && host.stop_loader_calls == 0,
            "grid selection drift after confirmation must produce zero mutation");
    }

    {
        FakeDeleteHost host;
        host.state = make_grid_state(first, second, third);
        host.state.selected = {false, false, false};
        FakeDeletePorts fake;
        auto composition = mv::make_delete_composition(host, fake.make_ports());

        expect(composition->handle_window_command(mv::kDeleteCommandPermanent)
                && fake.dialogs.empty() && fake.shell_requests.empty(),
            "a recognized grid delete command without a selection must fail closed");
    }
}

} // namespace

int main() {
    const fs::path source_root = fs::path(MINVIEW_SOURCE_DIR);
    const std::wstring first =
        (source_root / "tests/file_operation_tests.cpp").wstring();
    const std::wstring second =
        (source_root / "src/file_operation.cpp").wstring();
    const std::wstring third = (source_root / "src/app.cpp").wstring();

    expect(mv::path_is_existing_file(first)
            && mv::path_is_existing_file(second)
            && mv::path_is_existing_file(third),
        "composition tests require only existing read-only repository files");
    expect(!mv::path_is_existing_file(
            (source_root / "tests/does-not-exist.png").wstring()),
        "a missing target must remain invalid without touching the filesystem");

    test_app_delegation_contract(source_root);
    test_keyboard_guards(first, second, third);
    test_current_keyboard_paths(first, second, third);
    test_command_sources_and_recovery(first, second, third);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "file operation composition tests passed\n";
    return 0;
}
