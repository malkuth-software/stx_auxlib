// Tests of stx_auxlib's files and directories
// (include/stx_auxlib/files.hpp), in a fresh directory under the host's
// temporary directory that the test removes again.

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/files.hpp"
#include "stx_auxlib/paths.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib files: %s\n", message);
    std::abort();
}

void require_status(const stx_auxlib::FileStatus actual, const stx_auxlib::FileStatus expected,
                    const char* const message) {
    if (actual == expected) return;
    std::fprintf(stderr, "stx_auxlib files: %s (status %d, expected %d)\n", message,
                 static_cast<int>(actual), static_cast<int>(expected));
    std::abort();
}

stx_auxlib::Path under(const stx_auxlib::Path& base, const std::string_view relative) {
    const stx_auxlib::PathParse name = stx_auxlib::Path::parse(relative);
    require(name.succeeded(), "a test name did not parse");
    const stx_auxlib::PathParse joined = base.join(*name.path());
    require(joined.succeeded(), "a test path did not join");
    stx_auxlib::Path result = *joined.path();
    return result;
}

std::vector<std::byte> bytes_of(const std::string_view text) {
    std::vector<std::byte> result(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
        result[index] = static_cast<std::byte>(text[index]);
    return result;
}

// A scratch directory unique to this run, removed with everything in it.
struct Scratch final {
    stx_auxlib::Path root{};

    Scratch() {
        const stx_auxlib::FileValue<stx_auxlib::Path> temporary = stx_auxlib::temporary_directory();
        require(temporary.succeeded(), "no temporary directory");
        const std::int64_t stamp = stx_auxlib::MonotonicClock::now().time_since_epoch().count();
        root = under(*temporary.value(), "stx-auxlib-files-" + std::to_string(stamp));
        require_status(stx_auxlib::create_directory(root), stx_auxlib::FileStatus::ok,
                       "the scratch directory could not be made");
    }
    ~Scratch() { (void)stx_auxlib::remove_tree(root); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

void whole_files(const Scratch& scratch) {
    const stx_auxlib::Path file = under(scratch.root, "data.bin");
    const std::vector<std::byte> first = bytes_of("first content");
    require_status(stx_auxlib::write_file(file, first), stx_auxlib::FileStatus::ok, "write failed");
    std::vector<std::byte> read_back{};
    require_status(stx_auxlib::read_file(file, 1024, read_back), stx_auxlib::FileStatus::ok,
                   "read failed");
    require(read_back == first, "read content differs");

    // Replacement leaves no temporary file behind.
    const std::vector<std::byte> second = bytes_of("second, longer content");
    require_status(stx_auxlib::write_file(file, second), stx_auxlib::FileStatus::ok,
                   "replacement failed");
    require_status(stx_auxlib::read_file(file, 1024, read_back), stx_auxlib::FileStatus::ok,
                   "re-read failed");
    require(read_back == second, "replaced content differs");
    stx_auxlib::DirectoryListing listing{};
    require_status(stx_auxlib::list_directory(scratch.root, 16, listing),
                   stx_auxlib::FileStatus::ok, "listing failed");
    require(listing.entries.size() == 1 && listing.entries[0].name == "data.bin",
            "write_file left a temporary file");

    // Limits: exactly at the limit succeeds, one byte over is refused.
    require_status(stx_auxlib::read_file(file, second.size(), read_back),
                   stx_auxlib::FileStatus::ok, "a file at the limit was refused");
    require_status(stx_auxlib::read_file(file, second.size() - 1, read_back),
                   stx_auxlib::FileStatus::too_large, "a file over the limit was read");
    require(read_back.empty(), "a refused read left content");

    // An empty file, and a larger one read in several chunks.
    const stx_auxlib::Path empty = under(scratch.root, "empty");
    require_status(stx_auxlib::write_file(empty, std::span<const std::byte>()),
                   stx_auxlib::FileStatus::ok, "an empty write failed");
    require_status(stx_auxlib::read_file(empty, 0, read_back), stx_auxlib::FileStatus::ok,
                   "an empty read failed");
    require(read_back.empty(), "an empty file read content");
    std::vector<std::byte> large(3 * 1024 * 1024 + 17);
    for (std::size_t index = 0; index < large.size(); ++index)
        large[index] = static_cast<std::byte>(index * 131 + 7);
    const stx_auxlib::Path big = under(scratch.root, "big.bin");
    require_status(stx_auxlib::write_file(big, large), stx_auxlib::FileStatus::ok,
                   "a large write failed");
    require_status(stx_auxlib::read_file(big, large.size(), read_back), stx_auxlib::FileStatus::ok,
                   "a large read failed");
    require(read_back == large, "large content differs");

    require_status(stx_auxlib::read_file(under(scratch.root, "missing"), 16, read_back),
                   stx_auxlib::FileStatus::not_found, "a missing file was read");
    require_status(stx_auxlib::read_file(scratch.root, 16, read_back),
                   stx_auxlib::FileStatus::is_a_directory, "a directory was read as a file");
    // A path without a final name cannot be written as a file.
    const stx_auxlib::PathParse root = stx_auxlib::Path::parse("/");
    require_status(stx_auxlib::write_file(*root.path(), first), stx_auxlib::FileStatus::invalid_name,
                   "write_file accepted the root");
    require_status(stx_auxlib::write_file(stx_auxlib::Path(), first),
                   stx_auxlib::FileStatus::invalid_name, "write_file accepted \".\"");
}

void file_handles(const Scratch& scratch) {
    const stx_auxlib::Path path = under(scratch.root, "stream.txt");
    stx_auxlib::FileValue<stx_auxlib::File> created =
        stx_auxlib::File::open(path, stx_auxlib::FileMode::create_new);
    require(created.succeeded(), "create_new failed");
    stx_auxlib::File& file = *created.value();
    require_status(file.write_all(bytes_of("hello ")), stx_auxlib::FileStatus::ok, "write failed");
    require_status(file.write_all(bytes_of("world")), stx_auxlib::FileStatus::ok, "write failed");
    const stx_auxlib::FileValue<std::uint64_t> written = file.size();
    const stx_auxlib::FileValue<std::uint64_t> at = file.position();
    require(written.succeeded() && *written.value() == 11 && at.succeeded() && *at.value() == 11,
            "size or position wrong");
    require_status(file.sync(), stx_auxlib::FileStatus::ok, "sync failed");
    require_status(file.close(), stx_auxlib::FileStatus::ok, "close failed");
    require(!file.is_open(), "a closed file is open");
    require_status(file.close(), stx_auxlib::FileStatus::bad_handle, "a double close succeeded");
    require_status(file.write_all(bytes_of("x")), stx_auxlib::FileStatus::bad_handle,
                   "a closed file accepted a write");

    require_status(stx_auxlib::File::open(path, stx_auxlib::FileMode::create_new).status(),
                   stx_auxlib::FileStatus::already_exists, "create_new reused a name");

    stx_auxlib::FileValue<stx_auxlib::File> appended =
        stx_auxlib::File::open(path, stx_auxlib::FileMode::append);
    require(appended.succeeded(), "append failed");
    require_status((*appended.value()).write_all(bytes_of("!")), stx_auxlib::FileStatus::ok,
                   "append write failed");
    require_status((*appended.value()).close(), stx_auxlib::FileStatus::ok, "append close failed");

    stx_auxlib::FileValue<stx_auxlib::File> reader =
        stx_auxlib::File::open(path, stx_auxlib::FileMode::read);
    require(reader.succeeded(), "open for reading failed");
    stx_auxlib::File& input = *reader.value();
    require_status(input.seek(6), stx_auxlib::FileStatus::ok, "seek failed");
    std::byte buffer[16]{};
    const stx_auxlib::FileValue<std::size_t> got = input.read(buffer);
    require(got.succeeded() && *got.value() == 6, "read after seek is wrong");
    const std::string text(reinterpret_cast<const char*>(buffer), 6);
    require(text == "world!", "read content is wrong");
    const stx_auxlib::FileValue<std::size_t> end = input.read(buffer);
    require(end.succeeded() && *end.value() == 0, "the end of file was not reported");

    // Moving transfers the handle.
    stx_auxlib::File moved(std::move(input));
    require(!input.is_open() && moved.is_open(), "moving a file did not transfer it");

    stx_auxlib::FileValue<stx_auxlib::File> replaced =
        stx_auxlib::File::open(path, stx_auxlib::FileMode::replace);
    require(replaced.succeeded(), "replace failed");
    const stx_auxlib::FileValue<std::uint64_t> emptied = (*replaced.value()).size();
    require(emptied.succeeded() && *emptied.value() == 0, "replace kept old content");
}

void metadata_and_directories(const Scratch& scratch) {
    const stx_auxlib::Path nested = under(scratch.root, "a/b/c");
    require_status(stx_auxlib::create_directories(nested), stx_auxlib::FileStatus::ok,
                   "create_directories failed");
    require_status(stx_auxlib::create_directories(nested), stx_auxlib::FileStatus::ok,
                   "create_directories failed on an existing directory");
    require_status(stx_auxlib::create_directory(nested), stx_auxlib::FileStatus::already_exists,
                   "create_directory accepted an existing directory");

    const stx_auxlib::Path file = under(scratch.root, "a/note.txt");
    const stx_auxlib::WallClock::time_point before =
        stx_auxlib::WallClock::now() - std::chrono::seconds(5);
    require_status(stx_auxlib::write_file(file, bytes_of("12345")), stx_auxlib::FileStatus::ok,
                   "write failed");
    const stx_auxlib::FileValue<stx_auxlib::FileInformation> information =
        stx_auxlib::inspect(file, stx_auxlib::LinkMode::follow);
    require(information.succeeded(), "inspect failed");
    require((*information.value()).kind == stx_auxlib::FileKind::regular, "kind is wrong");
    require((*information.value()).size == 5, "size is wrong");
    require((*information.value()).modified >= before &&
                (*information.value()).modified <= stx_auxlib::WallClock::now() + std::chrono::seconds(5),
            "modification time is not current Unix time");
    const stx_auxlib::FileValue<stx_auxlib::FileInformation> directory =
        stx_auxlib::inspect(nested, stx_auxlib::LinkMode::follow);
    require(directory.succeeded() &&
                (*directory.value()).kind == stx_auxlib::FileKind::directory,
            "a directory is not a directory");
    require_status(stx_auxlib::inspect(under(scratch.root, "nothing"), stx_auxlib::LinkMode::follow)
                       .status(),
                   stx_auxlib::FileStatus::not_found, "a missing file was inspected");

    // Listing is sorted by name bytes, whatever order the host returns.
    const char* const names[] = {"zeta", "Alpha", "beta", "_under", "\xc3\xa9t\xc3\xa9"};
    for (const char* const name : names)
        require_status(stx_auxlib::write_file(under(scratch.root, std::string("list/") + name),
                                              bytes_of(name)),
                       stx_auxlib::FileStatus::not_found, "a write into a missing directory succeeded");
    require_status(stx_auxlib::create_directory(under(scratch.root, "list")),
                   stx_auxlib::FileStatus::ok, "create_directory failed");
    for (const char* const name : names)
        require_status(stx_auxlib::write_file(under(scratch.root, std::string("list/") + name),
                                              bytes_of(name)),
                       stx_auxlib::FileStatus::ok, "a listing file could not be written");
    stx_auxlib::DirectoryListing listing{};
    require_status(stx_auxlib::list_directory(under(scratch.root, "list"), 16, listing),
                   stx_auxlib::FileStatus::ok, "listing failed");
    const char* const sorted[] = {"Alpha", "_under", "beta", "zeta", "\xc3\xa9t\xc3\xa9"};
    require(listing.entries.size() == 5 && listing.unnamed_entries == 0, "listing count is wrong");
    for (std::size_t index = 0; index < 5; ++index)
        require(listing.entries[index].name == sorted[index], "listing is not sorted by bytes");
    require_status(stx_auxlib::list_directory(under(scratch.root, "list"), 4, listing),
                   stx_auxlib::FileStatus::too_large, "an over-limit listing succeeded");
    require(listing.entries.empty(), "a refused listing left entries");

    // Rename replaces; remove and remove_tree.
    const stx_auxlib::Path renamed = under(scratch.root, "list/beta");
    require_status(stx_auxlib::rename(under(scratch.root, "list/zeta"), renamed),
                   stx_auxlib::FileStatus::ok, "rename over a file failed");
    std::vector<std::byte> content{};
    require_status(stx_auxlib::read_file(renamed, 64, content), stx_auxlib::FileStatus::ok,
                   "the renamed file could not be read");
    require(content == bytes_of("zeta"), "rename did not replace the target");
    require_status(stx_auxlib::copy_file(renamed, under(scratch.root, "list/Alpha"),
                                         stx_auxlib::CopyMode::fail_if_exists),
                   stx_auxlib::FileStatus::already_exists, "copy replaced without permission");
    require_status(stx_auxlib::copy_file(renamed, under(scratch.root, "list/copy"),
                                         stx_auxlib::CopyMode::fail_if_exists),
                   stx_auxlib::FileStatus::ok, "copy failed");
    require_status(stx_auxlib::remove(under(scratch.root, "list")),
                   stx_auxlib::FileStatus::directory_not_empty, "a full directory was removed");
    require_status(stx_auxlib::remove(under(scratch.root, "list/copy")), stx_auxlib::FileStatus::ok,
                   "remove failed");
    require_status(stx_auxlib::remove(under(scratch.root, "list/copy")),
                   stx_auxlib::FileStatus::not_found, "a removed file was removed again");
    const stx_auxlib::FileValue<std::uint64_t> removed =
        stx_auxlib::remove_tree(under(scratch.root, "list"));
    require(removed.succeeded() && *removed.value() == 5, "remove_tree count is wrong");
    const stx_auxlib::FileValue<std::uint64_t> nothing =
        stx_auxlib::remove_tree(under(scratch.root, "list"));
    require(nothing.succeeded() && *nothing.value() == 0, "remove_tree of nothing failed");
}

// Creates `link` naming "target.txt". True when created; false when the
// host restricts links (Windows without developer mode or the symbolic link
// privilege reports permission_denied, as files.hpp states).
bool created_link(const stx_auxlib::Path& link) {
    const stx_auxlib::FileStatus status =
        stx_auxlib::create_link(*stx_auxlib::Path::parse("target.txt").path(), link);
#if defined(_WIN32)
    if (status == stx_auxlib::FileStatus::permission_denied) {
        std::puts("links: skipped (this host restricts symbolic links)");
        return false;
    }
#endif
    require_status(status, stx_auxlib::FileStatus::ok, "create_link failed");
    return true;
}

void link_checks(const stx_auxlib::Path& target, const stx_auxlib::Path& link) {
    const stx_auxlib::FileValue<stx_auxlib::Path> named = stx_auxlib::read_link(link);
    require(named.succeeded() && (*named.value()).text() == "target.txt", "read_link is wrong");
    const stx_auxlib::FileValue<stx_auxlib::FileInformation> itself =
        stx_auxlib::inspect(link, stx_auxlib::LinkMode::no_follow);
    require(itself.succeeded() && (*itself.value()).kind == stx_auxlib::FileKind::symbolic_link,
            "a link is not a link");
    const stx_auxlib::FileValue<stx_auxlib::FileInformation> followed =
        stx_auxlib::inspect(link, stx_auxlib::LinkMode::follow);
    require(followed.succeeded() && (*followed.value()).kind == stx_auxlib::FileKind::regular,
            "following a link did not reach its target");
    const stx_auxlib::FileValue<stx_auxlib::Path> resolved = stx_auxlib::resolve(link);
    const stx_auxlib::FileValue<stx_auxlib::Path> resolved_target = stx_auxlib::resolve(target);
    require(resolved.succeeded() && resolved_target.succeeded() &&
                *resolved.value() == *resolved_target.value(),
            "resolve did not follow the link");
}

void links_and_resolution(const Scratch& scratch) {
    const stx_auxlib::Path target = under(scratch.root, "target.txt");
    require_status(stx_auxlib::write_file(target, bytes_of("t")), stx_auxlib::FileStatus::ok,
                   "write failed");
    const stx_auxlib::Path link = under(scratch.root, "link");
    if (created_link(link)) link_checks(target, link);

    const stx_auxlib::FileValue<stx_auxlib::Path> working = stx_auxlib::working_directory();
    require(working.succeeded() && (*working.value()).is_absolute(), "no working directory");
    const stx_auxlib::FileValue<stx_auxlib::Path> made =
        stx_auxlib::absolute(*stx_auxlib::Path::parse("x/../y").path());
    require(made.succeeded() && *made.value() == under(*working.value(), "y"),
            "absolute is not lexical");
    const stx_auxlib::FileValue<stx_auxlib::Path> unchanged = stx_auxlib::absolute(scratch.root);
    require(unchanged.succeeded() && *unchanged.value() == scratch.root,
            "absolute changed an absolute path");
}

void host_path_forms() {
#if !defined(_WIN32)
    const stx_auxlib::Path drive = *stx_auxlib::Path::parse("C:/x").path();
    std::vector<std::byte> content{};
    require_status(stx_auxlib::read_file(drive, 16, content), stx_auxlib::FileStatus::unsupported,
                   "a POSIX host accepted a drive path");
#endif
}

} // namespace

int main() {
    {
        const Scratch scratch{};
        whole_files(scratch);
        file_handles(scratch);
        metadata_and_directories(scratch);
        links_and_resolution(scratch);
    }
    host_path_forms();
    std::puts("stx_auxlib files tests passed");
    return 0;
}
