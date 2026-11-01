// stx_auxlib::Path (include/stx_auxlib/paths.hpp). Pure computation, shared
// by every implementation; nothing here reaches a host.

#include "stx_auxlib/paths.hpp"

#include "unicode.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace stx_auxlib {

namespace {

bool is_ascii_letter(const char character) noexcept {
    const bool result =
        (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    return result;
}

char upper_ascii(const char character) noexcept {
    if (character >= 'a' && character <= 'z') return static_cast<char>(character - 'a' + 'A');
    return character;
}

// The root of unnormalized text: its normalized spelling and how many input
// bytes it covers ("X:" covers two; the '/' after it is a separator).
struct Root final {
    std::string spelling{};
    std::size_t consumed{0};
};

Root find_root(const std::string_view text) {
    Root root{};
    if (text[0] == '/') {
        root.spelling = "/";
        root.consumed = 1;
    } else if (text.size() >= 2 && is_ascii_letter(text[0]) && text[1] == ':' &&
               (text.size() == 2 || text[2] == '/')) {
        root.spelling.push_back(upper_ascii(text[0]));
        root.spelling.append(":/");
        root.consumed = 2;
    }
    return root;
}

} // namespace

Path::Path() : text_(".") {}

Path::Path(std::string normalized) : text_(std::move(normalized)) {}

PathParse Path::parse(const std::string_view text) {
    if (text.empty()) return PathParse::refused(PathFailure::empty);
    if (text.size() > maximum_bytes) return PathParse::refused(PathFailure::too_long);
    for (const char character : text) {
        if (character == '\0') return PathParse::refused(PathFailure::embedded_nul);
        if (character == '\\') return PathParse::refused(PathFailure::reserved_character);
    }
    if (!unicode::valid_utf8(text)) return PathParse::refused(PathFailure::invalid_utf8);

    const Root root = find_root(text);
    const std::string_view rest = text.substr(root.consumed);
    if (rest.find(':') != std::string_view::npos)
        return PathParse::refused(PathFailure::reserved_character);

    // Segments of `rest`, borrowed from `text`, after "." and ".." are applied.
    std::vector<std::string_view> segments{};
    std::size_t start = 0;
    while (start <= rest.size()) {
        std::size_t end = rest.find('/', start);
        if (end == std::string_view::npos) end = rest.size();
        const std::string_view segment = rest.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
                continue;
            }
            if (!root.spelling.empty()) return PathParse::refused(PathFailure::escapes_root);
        }
        segments.push_back(segment);
    }

    std::string normalized = root.spelling;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) normalized.push_back('/');
        normalized.append(segments[index]);
    }
    if (normalized.empty()) normalized = ".";
    if (normalized.size() > maximum_bytes) return PathParse::refused(PathFailure::too_long);
    PathParse result = PathParse::parsed(Path(std::move(normalized)));
    return result;
}

std::string_view Path::text() const noexcept { return text_; }

std::size_t Path::root_length() const noexcept {
    if (!text_.empty() && text_[0] == '/') return 1;
    if (text_.size() >= 3 && text_[1] == ':') return 3;
    return 0;
}

bool Path::is_absolute() const noexcept {
    const bool result = root_length() != 0;
    return result;
}

bool Path::is_root() const noexcept {
    const std::size_t root = root_length();
    const bool result = root != 0 && text_.size() == root;
    return result;
}

char Path::drive() const noexcept {
    if (root_length() == 3) return text_[0];
    return 0;
}

PathParse Path::join(const Path& relative) const {
    if (relative.is_absolute()) return PathParse::refused(PathFailure::absolute_join);
    std::string combined = text_;
    combined.push_back('/');
    combined.append(relative.text_);
    PathParse result = parse(combined);
    return result;
}

Path Path::parent() const {
    if (is_root()) return *this;
    std::string combined = text_;
    combined.append("/..");
    const PathParse parsed = parse(combined);
    const Path* const parent_path = parsed.path();
    if (parent_path == nullptr) return *this;
    Path result = *parent_path;
    return result;
}

std::string_view Path::file_name() const noexcept {
    const std::size_t root = root_length();
    const std::size_t slash = text_.rfind('/');
    std::size_t start = slash == std::string::npos ? 0 : slash + 1;
    if (start < root) start = root;
    const std::string_view name = std::string_view(text_).substr(start);
    if (name == "." || name == "..") return std::string_view();
    return name;
}

std::string_view Path::extension() const noexcept {
    const std::string_view name = file_name();
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return std::string_view();
    return name.substr(dot);
}

bool Path::operator==(const Path& other) const noexcept {
    const bool result = text_ == other.text_;
    return result;
}

bool Path::operator!=(const Path& other) const noexcept {
    const bool result = text_ != other.text_;
    return result;
}

PathParse::PathParse(State state) noexcept : state_(std::move(state)) {}

PathParse PathParse::parsed(Path path) noexcept {
    PathParse result(State(std::in_place_index<0>, std::move(path)));
    return result;
}

PathParse PathParse::refused(const PathFailure failure) noexcept {
    PathParse result(State(std::in_place_index<1>, failure));
    return result;
}

bool PathParse::succeeded() const noexcept {
    const bool result = std::holds_alternative<Path>(state_);
    return result;
}

const PathFailure* PathParse::failure() const noexcept {
    const PathFailure* const result = std::get_if<PathFailure>(&state_);
    return result;
}

const Path* PathParse::path() const noexcept {
    const Path* const result = std::get_if<Path>(&state_);
    return result;
}

} // namespace stx_auxlib
