#include "file_operation.h"
#include "decoder.h"

#include <algorithm>
#include <array>
#include <cctype>
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

static_assert(mv::IDM_DELETE == 1031);
static_assert(mv::IDM_DELETE_PERM == 1032);

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const fs::path base = fs::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const std::wstring name = L"minview-file-operation-"
                + std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(GetTickCount64()) + L"-"
                + std::to_wstring(attempt);
            const fs::path candidate = base / name;
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                m_path = candidate;
                return;
            }
        }
    }

    ~TemporaryDirectory() {
        if (!is_owned_path()) return;
        std::error_code error;
        static_cast<void>(fs::remove_all(m_path, error));
    }

    const fs::path& path() const { return m_path; }

private:
    bool is_owned_path() const {
        if (m_path.empty()) return false;
        std::error_code error;
        const fs::path temp_root = fs::weakly_canonical(
            fs::temp_directory_path(), error);
        if (error) return false;
        const fs::path candidate = fs::weakly_canonical(m_path, error);
        return !error && candidate.parent_path() == temp_root
            && candidate.filename().wstring().starts_with(
                L"minview-file-operation-");
    }

    fs::path m_path;
};

bool write_test_bmp(const fs::path& path, LONG width = 2, LONG height = 2) {
    BITMAPFILEHEADER file_header = {};
    BITMAPINFOHEADER info_header = {};
    const DWORD row_bytes = static_cast<DWORD>((width * 3 + 3) & ~3);
    std::vector<unsigned char> pixels(
        static_cast<size_t>(row_bytes) * static_cast<size_t>(height), 0x7f);
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits
        + static_cast<DWORD>(pixels.size());
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = width;
    info_header.biHeight = height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = static_cast<DWORD>(pixels.size());

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file_header),
        static_cast<std::streamsize>(sizeof(file_header)));
    output.write(reinterpret_cast<const char*>(&info_header),
        static_cast<std::streamsize>(sizeof(info_header)));
    output.write(reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(output);
}

std::string read_source_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string without_whitespace(std::string source) {
    source.erase(std::remove_if(source.begin(), source.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; }), source.end());
    return source;
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

void test_windows_port_with_scaled_sources(size_t target_count) {
    TemporaryDirectory temp;
    expect(!temp.path().empty(),
        "the Windows delete integration test requires an isolated temp root");
    if (temp.path().empty()) return;

    std::vector<std::wstring> targets;
    for (size_t index = 0; index < target_count; ++index) {
        const fs::path path = temp.path()
            / (L"target-" + std::to_wstring(index + 1) + L".bmp");
        constexpr std::array<std::pair<LONG, LONG>, 3> dimensions = {{
            {2, 2}, {320, 240}, {640, 480},
        }};
        expect(write_test_bmp(
                path, dimensions[index].first, dimensions[index].second),
            "the integration test must create each isolated bitmap target");
        targets.push_back(fs::absolute(path).wstring());
    }
    const fs::path sentinel_path = temp.path() / L"sentinel.bmp";
    expect(write_test_bmp(sentinel_path),
        "the integration test must create an unselected sentinel");
    const std::wstring sentinel = fs::absolute(sentinel_path).wstring();
    const std::string sentinel_bytes = read_source_file(sentinel_path);

    mv::Decoder decoder;
    std::vector<Microsoft::WRL::ComPtr<IWICBitmapSource>> scaled_sources;
    for (const auto& target : targets)
        scaled_sources.push_back(decoder.decode_scaled(target, 160));
    expect(scaled_sources.size() == target_count
            && std::all_of(scaled_sources.begin(), scaled_sources.end(),
                [](const auto& source) { return source != nullptr; }),
        "production scaled bitmap sources must remain alive during deletion");

    FakeDeleteHost host;
    host.state.index_paths = targets;
    host.state.index_paths.push_back(sentinel);
    host.state.current_path = targets.front();
    host.state.current_index = 0;
    host.state.has_image = true;
    host.state.grid_mode = true;
    host.state.grid_selection = 0;
    host.state.selected.assign(host.state.index_paths.size(), false);
    std::fill_n(host.state.selected.begin(), target_count, true);
    host.state.selection_anchor = 0;

    int confirmation_calls = 0;
    int warning_calls = 0;
    int shell_calls = 0;
    mv::DeleteShellRequest captured_request;
    mv::DeleteShellResult captured_result;
    mv::DeleteOsPorts ports = mv::make_windows_delete_ports(host);
    ports.message_box = [&](const mv::PermanentDeletePrompt& prompt) {
        if ((prompt.flags & MB_TYPEMASK) == MB_OKCANCEL)
            ++confirmation_calls;
        else
            ++warning_calls;
        return IDOK;
    };
    auto production_shell_delete = std::move(ports.shell_delete);
    ports.shell_delete = [&](const mv::DeleteShellRequest& request) {
        ++shell_calls;
        captured_request = request;
        captured_result = production_shell_delete(request);
        return captured_result;
    };

    auto composition = mv::make_delete_composition(host, std::move(ports));
    expect(composition->handle_command(
            mv::DeleteCommandEntry::WindowCommand, mv::IDM_DELETE_PERM),
        "the Windows integration path must enter the production composition");

    expect(confirmation_calls == 1 && warning_calls == 0 && shell_calls == 1,
        "confirmed Windows deletion must invoke the real Shell port once without warning");
    expect(captured_request.targets == targets
            && request_has_exact_multi_string(captured_request, targets)
            && request_has_silent_shell_flags(captured_request)
            && (captured_request.flags & FOF_ALLOWUNDO) == 0,
        "the real Windows port must receive the exact permanent-delete multi-string");
    expect(captured_result.shell_result == 0 && !captured_result.aborted
            && captured_result.missing_targets == targets,
        "the real Windows port must report every confirmed target removed");
    expect(std::all_of(targets.begin(), targets.end(),
            [](const auto& target) { return mv::path_is_confirmed_missing(target); }),
        "one, two, and three held scaled bitmap targets must be permanently deleted");
    expect(fs::exists(sentinel_path)
            && read_source_file(sentinel_path) == sentinel_bytes,
        "the unselected sentinel must remain byte-identical");

    std::vector<int> expected_indices;
    for (size_t index = 0; index < target_count; ++index)
        expected_indices.push_back(static_cast<int>(index));
    expect(host.removed_indices == expected_indices
            && host.state.index_paths == std::vector<std::wstring>{sentinel},
        "the App list must remove exactly the confirmed production targets");
}

void test_windows_port_invalid_set_fails_closed() {
    TemporaryDirectory temp;
    expect(!temp.path().empty(),
        "the fail-closed integration test requires an isolated temp root");
    if (temp.path().empty()) return;

    const fs::path existing_path = temp.path() / L"existing.bmp";
    const fs::path sentinel_path = temp.path() / L"sentinel.bmp";
    expect(write_test_bmp(existing_path) && write_test_bmp(sentinel_path),
        "the fail-closed integration test must create isolated files");
    const std::string existing_bytes = read_source_file(existing_path);
    const std::string sentinel_bytes = read_source_file(sentinel_path);
    const std::wstring existing = fs::absolute(existing_path).wstring();
    const std::wstring missing =
        fs::absolute(temp.path() / L"missing.bmp").wstring();
    const std::wstring sentinel = fs::absolute(sentinel_path).wstring();

    FakeDeleteHost host;
    host.state.index_paths = {existing, missing, sentinel};
    host.state.current_path = existing;
    host.state.current_index = 0;
    host.state.has_image = true;
    host.state.grid_mode = true;
    host.state.grid_selection = 0;
    host.state.selected = {true, true, false};
    host.state.selection_anchor = 0;

    int dialog_calls = 0;
    int shell_calls = 0;
    mv::DeleteOsPorts ports = mv::make_windows_delete_ports(host);
    ports.message_box = [&](const auto&) {
        ++dialog_calls;
        return IDOK;
    };
    auto production_shell_delete = std::move(ports.shell_delete);
    ports.shell_delete = [&](const mv::DeleteShellRequest& request) {
        ++shell_calls;
        return production_shell_delete(request);
    };
    auto composition = mv::make_delete_composition(host, std::move(ports));
    composition->handle_command(
        mv::DeleteCommandEntry::WindowCommand, mv::IDM_DELETE_PERM);

    expect(dialog_calls == 0 && shell_calls == 0 && host.remove_calls == 0,
        "an invalid multi-target set must fail closed before confirmation or Shell mutation");
    expect(read_source_file(existing_path) == existing_bytes
            && read_source_file(sentinel_path) == sentinel_bytes,
        "fail-closed validation must preserve every existing file byte-for-byte");
}

void test_windows_port_partial_failure_recovers_exactly() {
    TemporaryDirectory temp;
    expect(!temp.path().empty(),
        "the partial-failure integration test requires an isolated temp root");
    if (temp.path().empty()) return;

    const fs::path first_path = temp.path() / L"first.bmp";
    const fs::path locked_path = temp.path() / L"locked.bmp";
    const fs::path sentinel_path = temp.path() / L"sentinel.bmp";
    expect(write_test_bmp(first_path) && write_test_bmp(locked_path)
            && write_test_bmp(sentinel_path),
        "the partial-failure integration test must create isolated files");
    const std::string locked_bytes = read_source_file(locked_path);
    const std::string sentinel_bytes = read_source_file(sentinel_path);
    const std::wstring first = fs::absolute(first_path).wstring();
    const std::wstring locked = fs::absolute(locked_path).wstring();
    const std::wstring sentinel = fs::absolute(sentinel_path).wstring();

    HANDLE held = CreateFileW(locked.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(held != INVALID_HANDLE_VALUE,
        "the integration test must hold one target without delete sharing");
    if (held == INVALID_HANDLE_VALUE) return;

    FakeDeleteHost host;
    host.state.index_paths = {first, locked, sentinel};
    host.state.current_path = first;
    host.state.current_index = 0;
    host.state.has_image = true;
    host.state.grid_mode = true;
    host.state.grid_selection = 0;
    host.state.selected = {true, true, false};
    host.state.selection_anchor = 0;

    int warning_calls = 0;
    mv::DeleteShellResult captured_result;
    mv::DeleteOsPorts ports = mv::make_windows_delete_ports(host);
    ports.message_box = [&](const mv::PermanentDeletePrompt& prompt) {
        if ((prompt.flags & MB_TYPEMASK) != MB_OKCANCEL) ++warning_calls;
        return IDOK;
    };
    auto production_shell_delete = std::move(ports.shell_delete);
    ports.shell_delete = [&](const mv::DeleteShellRequest& request) {
        captured_result = production_shell_delete(request);
        return captured_result;
    };
    auto composition = mv::make_delete_composition(host, std::move(ports));
    composition->handle_command(
        mv::DeleteCommandEntry::WindowCommand, mv::IDM_DELETE_PERM);
    CloseHandle(held);

    expect(captured_result.shell_result == ERROR_SHARING_VIOLATION
            && !captured_result.aborted
            && captured_result.missing_targets == std::vector{first},
        "the real Shell port must expose return, aborted, and actual partial completion");
    expect(mv::path_is_confirmed_missing(first) && fs::exists(locked_path)
            && read_source_file(locked_path) == locked_bytes
            && fs::exists(sentinel_path)
            && read_source_file(sentinel_path) == sentinel_bytes,
        "partial failure must remove only the completed target and preserve the rest");
    expect(host.removed_indices == std::vector<int>{0}
            && host.state.index_paths == std::vector{locked, sentinel}
            && host.state.selected == std::vector<bool>({true, false})
            && warning_calls == 1,
        "partial failure must restore a coherent list and retained selection with warning");
}

void test_app_raw_delete_forwarding_contract(const fs::path& source_root) {
    const std::string app_source = without_whitespace(
        read_source_file(source_root / "src/app.cpp"));

    expect(app_source.find("IDM_DELETE=") == std::string::npos
            && app_source.find("IDM_DELETE_PERM=") == std::string::npos
            && app_source.find("DeleteMode::") == std::string::npos,
        "App must not retain a private delete command-to-mode mapping");
    expect(app_source.find(
            "m_delete_composition->handle_key(static_cast<UINT>(wp),lp,delete_guards)")
            != std::string::npos,
        "App must forward the raw key action to the shared delete seam");
    expect(app_source.find(
            "m_delete_composition->handle_command(DeleteCommandEntry::WindowCommand,LOWORD(wp))")
            != std::string::npos,
        "App must forward the raw WM_COMMAND ID to the shared delete seam");
    expect(app_source.find(
            "m_delete_composition->handle_command(DeleteCommandEntry::Toolbar,static_cast<UINT>(cmd))")
            != std::string::npos,
        "App must forward the raw toolbar ID to the shared delete seam");
    expect(app_source.find(
            "m_delete_composition->handle_command(DeleteCommandEntry::ContextMenu,static_cast<UINT>(cmd))")
            != std::string::npos,
        "App must forward the raw context-menu ID to the shared delete seam");
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

void test_command_id_entry_matrix(
    const std::wstring& first, const std::wstring& second,
    const std::wstring& third) {
    const std::array entries = {
        mv::DeleteCommandEntry::WindowCommand,
        mv::DeleteCommandEntry::Toolbar,
        mv::DeleteCommandEntry::ContextMenu,
    };
    const std::array commands = {
        std::pair{mv::IDM_DELETE, false},
        std::pair{mv::IDM_DELETE_PERM, true},
    };

    for (const auto entry : entries) {
        for (const bool grid_mode : {false, true}) {
            for (const auto& [command, permanent] : commands) {
                FakeDeleteHost host;
                host.state = grid_mode
                    ? make_grid_state(first, second, third)
                    : make_current_state(first, second, third);
                FakeDeletePorts fake;
                const std::vector<std::wstring> expected_targets = grid_mode
                    ? std::vector{first, second} : std::vector{first};
                fake.shell_result.missing_targets = expected_targets;
                auto composition = mv::make_delete_composition(
                    host, fake.make_ports());

                expect(composition->handle_command(entry, command),
                    "every App delete command entry must accept both exact IDs");
                expect(fake.confirmation_calls() == (permanent ? 1 : 0)
                        && fake.shell_requests.size() == 1,
                    "ordinary must skip confirmation and permanent must require IDOK");
                const auto& request = fake.shell_requests.front();
                expect(request.operation == FO_DELETE
                        && request.targets == expected_targets
                        && request_has_exact_multi_string(request, expected_targets),
                    "each command entry must bind the exact current or grid snapshot");
                expect(((request.flags & FOF_ALLOWUNDO) == 0) == permanent
                        && request_has_silent_shell_flags(request),
                    "IDM_DELETE must recycle and IDM_DELETE_PERM must be permanent");
                expect(host.remove_calls == 1
                        && host.removed_indices == (grid_mode
                            ? std::vector<int>{0, 1} : std::vector<int>{0}),
                    "IDOK command routing must invoke the expected mutation once");
                expect(host.state.index_paths == (grid_mode
                            ? std::vector{third} : std::vector{second, third}),
                    "command mutation must remove only the confirmed snapshot");
            }
        }

        FakeDeleteHost host;
        host.state = make_current_state(first, second, third);
        FakeDeletePorts fake;
        auto composition = mv::make_delete_composition(host, fake.make_ports());
        expect(!composition->handle_command(entry, 9999)
                && fake.dialogs.empty() && fake.shell_requests.empty()
                && host.remove_calls == 0,
            "unknown raw commands must fail closed at every entry");
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

        expect(composition->handle_command(
                mv::DeleteCommandEntry::WindowCommand, mv::IDM_DELETE_PERM),
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

        expect(composition->handle_command(
                mv::DeleteCommandEntry::Toolbar, mv::IDM_DELETE),
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

        expect(composition->handle_command(
                mv::DeleteCommandEntry::ContextMenu, mv::IDM_DELETE_PERM),
            "context delete forwarding must enter the production composition");
        expect(fake.confirmation_calls() == 1 && fake.shell_requests.size() == 1
                && (fake.shell_requests.front().flags & FOF_ALLOWUNDO) == 0,
            "context permanent delete must use the same IDOK-only permanent path");
        expect(!composition->handle_command(
                mv::DeleteCommandEntry::ContextMenu, 9999),
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

        composition->handle_command(
            mv::DeleteCommandEntry::WindowCommand, mv::IDM_DELETE_PERM);
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

        expect(composition->handle_command(
                    mv::DeleteCommandEntry::WindowCommand, mv::IDM_DELETE_PERM)
                && fake.dialogs.empty() && fake.shell_requests.empty(),
            "a recognized grid delete command without a selection must fail closed");
    }
}

} // namespace

int main() {
    const HRESULT com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    expect(SUCCEEDED(com_result),
        "file operation tests require an initialized COM apartment");
    if (FAILED(com_result)) return 1;

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

    test_app_raw_delete_forwarding_contract(source_root);
    test_keyboard_guards(first, second, third);
    test_current_keyboard_paths(first, second, third);
    test_command_id_entry_matrix(first, second, third);
    test_command_sources_and_recovery(first, second, third);
    test_windows_port_with_scaled_sources(1);
    test_windows_port_with_scaled_sources(2);
    test_windows_port_with_scaled_sources(3);
    test_windows_port_invalid_set_fails_closed();
    test_windows_port_partial_failure_recovers_exactly();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        CoUninitialize();
        return 1;
    }
    std::cout << "file operation composition tests passed\n";
    CoUninitialize();
    return 0;
}
