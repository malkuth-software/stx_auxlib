// Tests of stx_auxlib::Path (include/stx_auxlib/paths.hpp).

#include "stx_auxlib/paths.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const char* const message, const std::string_view detail) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib paths: %s: \"%.*s\"\n", message,
                 static_cast<int>(detail.size()), detail.data());
    std::abort();
}

stx_auxlib::Path parse_or_fail(const std::string_view text) {
    const stx_auxlib::PathParse parsed = stx_auxlib::Path::parse(text);
    require(parsed.succeeded(), "a valid path was refused", text);
    stx_auxlib::Path result = *parsed.path();
    return result;
}

struct Normalization final {
    std::string_view input;
    std::string_view normalized;
};

constexpr Normalization normalizations[] = {
    {"a", "a"},
    {".", "."},
    {"./", "."},
    {"a/./b", "a/b"},
    {"a//b///c", "a/b/c"},
    {"a/b/", "a/b"},
    {"a/b/..", "a"},
    {"a/..", "."},
    {"a/../..", ".."},
    {"../a", "../a"},
    {"../../a/../b", "../../b"},
    {"..", ".."},
    {"/", "/"},
    {"//", "/"},
    {"/a/b/../c/.", "/a/c"},
    {"/a/..", "/"},
    {"c:", "C:/"},
    {"c:/", "C:/"},
    {"C:/Users//me/./x/..", "C:/Users/me"},
    {"d:/a/..", "D:/"},
    {"caf\xc3\xa9/\xe6\x96\x87\xe4\xbb\xb6", "caf\xc3\xa9/\xe6\x96\x87\xe4\xbb\xb6"},
    {"\xf0\x9f\x98\x80.png", "\xf0\x9f\x98\x80.png"},
};

void normalization_table() {
    for (const Normalization& row : normalizations) {
        const stx_auxlib::Path path = parse_or_fail(row.input);
        require(path.text() == row.normalized, "normalized wrongly", row.input);
        // Normalization is idempotent.
        const stx_auxlib::Path again = parse_or_fail(path.text());
        require(again == path, "normalization is not idempotent", row.input);
    }
}

struct Refusal final {
    std::string_view input;
    stx_auxlib::PathFailure failure;
};

void refusals() {
    const Refusal rows[] = {
        {"", stx_auxlib::PathFailure::empty},
        {std::string_view("a\0b", 3), stx_auxlib::PathFailure::embedded_nul},
        {"a\\b", stx_auxlib::PathFailure::reserved_character},
        {"a:b", stx_auxlib::PathFailure::reserved_character},
        {"C:x", stx_auxlib::PathFailure::reserved_character},
        {"dir/C:/x", stx_auxlib::PathFailure::reserved_character},
        {"/..", stx_auxlib::PathFailure::escapes_root},
        {"/a/../..", stx_auxlib::PathFailure::escapes_root},
        {"C:/..", stx_auxlib::PathFailure::escapes_root},
        {"\xc0\xaf", stx_auxlib::PathFailure::invalid_utf8},           // overlong '/'
        {"\xe0\x80\xaf", stx_auxlib::PathFailure::invalid_utf8},       // overlong
        {"\xed\xa0\x80", stx_auxlib::PathFailure::invalid_utf8},       // surrogate
        {"\xf4\x90\x80\x80", stx_auxlib::PathFailure::invalid_utf8},   // above U+10FFFF
        {"\xc3", stx_auxlib::PathFailure::invalid_utf8},               // truncated
        {"\x80", stx_auxlib::PathFailure::invalid_utf8},               // lone continuation
        {"\xff", stx_auxlib::PathFailure::invalid_utf8},
    };
    for (const Refusal& row : rows) {
        const stx_auxlib::PathParse parsed = stx_auxlib::Path::parse(row.input);
        require(!parsed.succeeded(), "an invalid path was accepted", row.input);
        require(*parsed.failure() == row.failure, "refused for the wrong reason", row.input);
    }
    const std::string longest(stx_auxlib::Path::maximum_bytes, 'a');
    require(stx_auxlib::Path::parse(longest).succeeded(), "the longest path was refused", "");
    const std::string too_long(stx_auxlib::Path::maximum_bytes + 1, 'a');
    const stx_auxlib::PathParse refused = stx_auxlib::Path::parse(too_long);
    require(!refused.succeeded() && *refused.failure() == stx_auxlib::PathFailure::too_long,
            "an overlong path was accepted", "");
}

void queries() {
    const stx_auxlib::Path relative = parse_or_fail("docs/report.tar.gz");
    require(!relative.is_absolute() && !relative.is_root() && relative.drive() == 0,
            "a relative path reported a root", relative.text());
    require(relative.file_name() == "report.tar.gz", "file_name is wrong", relative.text());
    require(relative.extension() == ".gz", "extension is wrong", relative.text());
    require(relative.parent().text() == "docs", "parent is wrong", relative.text());

    const stx_auxlib::Path root = parse_or_fail("/");
    require(root.is_absolute() && root.is_root(), "the root is not a root", root.text());
    require(root.parent() == root && root.file_name().empty(), "the root has a parent", "/");
    const stx_auxlib::Path drive = parse_or_fail("e:/");
    require(drive.is_root() && drive.drive() == 'E', "a drive root is wrong", drive.text());
    require(parse_or_fail("E:/a").parent() == drive, "a drive child's parent is wrong", "E:/a");

    require(parse_or_fail(".profile").extension().empty(), "a dot file has an extension", ".profile");
    require(parse_or_fail("a.").extension() == ".", "a trailing dot is lost", "a.");
    require(parse_or_fail("noext").extension().empty(), "a plain name has an extension", "noext");
    require(parse_or_fail(".").file_name().empty(), "\".\" has a file name", ".");
    require(parse_or_fail("../..").file_name().empty(), "\"..\" has a file name", "../..");
    require(parse_or_fail(".").parent().text() == "..", "the parent of \".\" is wrong", ".");
    require(parse_or_fail("..").parent().text() == "../..", "the parent of \"..\" is wrong", "..");
    require(parse_or_fail("a").parent().text() == ".", "the parent of \"a\" is wrong", "a");

    const stx_auxlib::Path default_path{};
    require(default_path.text() == ".", "the default path is not \".\"", default_path.text());
}

void joins() {
    const stx_auxlib::Path base = parse_or_fail("/srv/data");
    const stx_auxlib::PathParse joined = base.join(parse_or_fail("../logs/today"));
    require(joined.succeeded() && (*joined.path()).text() == "/srv/logs/today", "join is wrong",
            "/srv/data + ../logs/today");
    const stx_auxlib::PathParse escaping = base.join(parse_or_fail("../../.."));
    require(!escaping.succeeded() && *escaping.failure() == stx_auxlib::PathFailure::escapes_root,
            "a join escaped the root", "/srv/data + ../../..");
    const stx_auxlib::PathParse absolute = base.join(parse_or_fail("/etc"));
    require(!absolute.succeeded() &&
                *absolute.failure() == stx_auxlib::PathFailure::absolute_join,
            "an absolute join was accepted", "/srv/data + /etc");
    const stx_auxlib::PathParse onto_dot = parse_or_fail(".").join(parse_or_fail("x/y"));
    require(onto_dot.succeeded() && (*onto_dot.path()).text() == "x/y", "join onto \".\" is wrong",
            ". + x/y");
    const stx_auxlib::PathParse onto_root = parse_or_fail("/").join(parse_or_fail("x"));
    require(onto_root.succeeded() && (*onto_root.path()).text() == "/x", "join onto \"/\" is wrong",
            "/ + x");
}

} // namespace

int main() {
    normalization_table();
    refusals();
    queries();
    joins();
    std::puts("stx_auxlib paths tests passed");
    return 0;
}
