#include "albumstate.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using mv::AlbumFolderView;
    using mv::AlbumStore;

    // ── 相册 CRUD ──
    {
        AlbumStore store;
        expect(mv::add_album(store, L"发布素材"), "add album");
        expect(mv::add_album(store, L"工作集"), "add second album");
        expect(!mv::add_album(store, L"发布素材"), "duplicate name rejected");
        expect(!mv::add_album(store, L"发布素材"), "duplicate case-insensitive");
        expect(!mv::add_album(store, L"   "), "blank name rejected");
        expect(store.albums.size() == 2, "two albums");

        expect(mv::rename_album(store, 0, L"素材库"), "rename album");
        expect(store.albums[0].name == L"素材库", "renamed value");
        expect(!mv::rename_album(store, 1, L"素材库"), "rename to duplicate rejected");
        expect(!mv::rename_album(store, 9, L"x"), "rename bad index");

        expect(mv::remove_album(store, 1), "remove album");
        expect(store.albums.size() == 1, "one album left");
        expect(!mv::remove_album(store, 5), "remove bad index");
    }

    // ── 文件夹操作 ──
    {
        AlbumStore store;
        mv::add_album(store, L"测试");
        expect(mv::add_folder(store, 0, L"D:\\AIGC\\Assets", false), "add folder");
        expect(mv::add_folder(store, 0, L"E:\\4K 图库", true), "add recursive folder");
        expect(!mv::add_folder(store, 0, L"d:\\aigc\\assets", true),
            "duplicate path case-insensitive rejected");
        expect(!mv::add_folder(store, 0, L"D:\\AIGC\\Assets\\", false),
            "trailing slash deduped");
        expect(!mv::add_folder(store, 0, L"", false), "empty path rejected");
        expect(!mv::add_folder(store, 3, L"x", false), "bad album index");
        expect(store.albums[0].folders.size() == 2, "two folders");
        expect(store.albums[0].folders[1].recursive, "recursive flag stored");

        expect(mv::set_folder_recursive(store, 0, 0, true), "toggle recursive");
        expect(store.albums[0].folders[0].recursive, "recursive toggled");
        expect(!mv::set_folder_recursive(store, 0, 9, true), "bad folder index");

        expect(mv::move_folder(store, 0, 1, 0), "move folder up");
        expect(store.albums[0].folders[0].path == L"E:\\4K 图库", "moved order");
        expect(!mv::move_folder(store, 0, 0, 0), "move to self rejected");
        expect(!mv::move_folder(store, 0, 0, 9), "move bad index");

        expect(mv::remove_folder(store, 0, 0), "remove folder");
        expect(store.albums[0].folders.size() == 1, "one folder left");
        expect(!mv::remove_folder(store, 0, 5), "remove bad folder index");
    }

    // ── 收藏(固定相册) ──
    {
        AlbumStore store;
        expect(mv::add_favourite(store, L"D:\\AIGC\\cover.png"), "add favourite");
        expect(!mv::add_favourite(store, L"d:\\aigc\\COVER.PNG"),
            "favourite dedupe case-insensitive");
        expect(mv::add_favourite(store, L"E:\\shot.png"), "add second favourite");
        expect(store.favourites.size() == 2, "two favourites");
        expect(mv::is_favourite(store, L"D:\\AIGC\\cover.png"), "is favourite");
        expect(!mv::is_favourite(store, L"D:\\other.png"), "not favourite");
        expect(mv::remove_favourite(store, L"D:\\AIGC\\COVER.PNG"),
            "remove favourite case-insensitive");
        expect(store.favourites.size() == 1, "one favourite left");
        expect(!mv::remove_favourite(store, L"D:\\missing.png"),
            "remove missing favourite");
        expect(!mv::add_favourite(store, L""), "empty favourite rejected");
    }

    // ── 序列化 → 解析 往返 ──
    {
        AlbumStore store;
        store.folder_view = AlbumFolderView::Icons2;
        mv::add_album(store, L"发布素材");
        mv::add_album(store, L"4K 图库");
        mv::add_folder(store, 0, L"D:\\AIGC\\Assets", false);
        mv::add_folder(store, 0, L"E:\\图片 \"测试\"", true);
        mv::add_folder(store, 1, L"F:\\shots\\final", true);
        mv::add_favourite(store, L"D:\\AIGC\\封面.png");

        const std::string json = mv::serialize_album_store(store);
        const auto parsed = mv::parse_album_store(json);
        expect(parsed.has_value(), "round-trip parses");
        if (parsed) {
            expect(parsed->folder_view == AlbumFolderView::Icons2,
                "view mode preserved");
            expect(parsed->albums.size() == 2, "albums preserved");
            expect(parsed->albums[0].name == L"发布素材", "album name utf8");
            expect(parsed->albums[0].folders.size() == 2, "folders preserved");
            expect(parsed->albums[0].folders[1].path == L"E:\\图片 \"测试\"",
                "path with quote and spaces preserved");
            expect(parsed->albums[0].folders[1].recursive, "recursive preserved");
            expect(parsed->albums[1].folders[0].path == L"F:\\shots\\final",
                "second album folder preserved");
            expect(parsed->favourites.size() == 1, "favourite preserved");
            expect(parsed->favourites[0] == L"D:\\AIGC\\封面.png",
                "favourite utf8 path preserved");
        }

        // 解析结果再序列化 = 幂等
        if (parsed) {
            const std::string json2 = mv::serialize_album_store(*parsed);
            expect(json2 == json, "serialize idempotent");
        }

        // 三态视图模式 + 旧版 "icons" 兼容
        {
            AlbumStore s2;
            s2.folder_view = AlbumFolderView::Icons3;
            const auto p3 = mv::parse_album_store(mv::serialize_album_store(s2));
            expect(p3.has_value(), "icons3 round-trip parses");
            if (p3)
                expect(p3->folder_view == AlbumFolderView::Icons3,
                    "icons3 preserved");
            const auto legacy = mv::parse_album_store(
                "{\"folder_view\": \"icons\"}");
            expect(legacy.has_value(), "legacy icons parses");
            if (legacy)
                expect(legacy->folder_view == AlbumFolderView::Icons2,
                    "legacy icons = 2x2");
        }
    }

    // ── 容错解析 ──
    {
        expect(!mv::parse_album_store("").has_value(), "empty json");
        expect(!mv::parse_album_store("{").has_value(), "truncated json");
        expect(!mv::parse_album_store("[]").has_value(), "wrong root");
        expect(!mv::parse_album_store("{ garbage").has_value(), "garbage");
        expect(!mv::parse_album_store("{\"version\": \"1\"}").has_value(),
            "wrong version type");
        expect(!mv::parse_album_store("{\"version\": 2}").has_value(),
            "unsupported version");
        expect(!mv::parse_album_store("{\"unknown\": 1}").has_value(),
            "unknown key refused");
        expect(!mv::parse_album_store(
            "{\"favourites\": [\"a\\q\"]}").has_value(),
            "bad escape");
        expect(!mv::parse_album_store(
            "{\"favourites\": [1]}").has_value(),
            "wrong element type");
        expect(!mv::parse_album_store(
            "{\"albums\": [{\"folders\": [{\"recursive\": \"yes\"}]}]}"
            ).has_value(),
            "wrong recursive type");
        expect(!mv::parse_album_store("{} trailing").has_value(),
            "trailing garbage");
    }

    // ── 宽容:缺省可选字段 ──
    {
        const auto parsed = mv::parse_album_store(
            "{\"albums\": [{\"name\": \"x\"}, {\"name\": \"y\","
            " \"folders\": [{\"path\": \"D:\\\\a\"}]}]}");
        expect(parsed.has_value(), "optional fields tolerated");
        if (parsed) {
            expect(parsed->folder_view == AlbumFolderView::Tree,
                "default view mode");
            expect(parsed->albums.size() == 2, "two albums parsed");
            expect(parsed->albums[0].folders.empty(), "missing folders = empty");
            expect(parsed->albums[1].folders.size() == 1, "folder parsed");
            expect(!parsed->albums[1].folders[0].recursive,
                "missing recursive = false");
        }
        // 无 version 字段 → 默认接受
        expect(mv::parse_album_store("{\"albums\": []}").has_value(),
            "missing version tolerated");
    }

    if (failures == 0) {
        std::cout << "albumstate: all tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
