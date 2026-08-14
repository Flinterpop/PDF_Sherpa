#include "Metadata.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "PathUtf8.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Python's str.strip() with no argument removes ASCII whitespace; the .toc
// format is ASCII-structured (the topic text may be any UTF-8, but the
// delimiters are not), so an ASCII-only trim matches it exactly and cannot
// slice a multi-byte sequence -- every byte of a UTF-8 continuation has the
// high bit set and so is never mistaken for a space.
std::string_view trim(std::string_view text)
{
    const auto is_space = [](unsigned char c) {
        return std::isspace(c) != 0;
    };
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// The first run of decimal digits in `text`, as Python's re.search(r"\d+")
// finds it.  Returns false when there is none.
bool first_number(std::string_view text, int* out)
{
    assert(out != nullptr);
    const std::size_t start = text.find_first_of("0123456789");
    if (start == std::string_view::npos) {
        return false;
    }
    std::size_t end = start;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
        ++end;
    }
    // A page number wider than an int is not a typo we should paper over, but
    // it is also not worth an exception: clamp and let the caller's page
    // bounds check reject it against the real document.
    long long value = 0;
    for (std::size_t i = start; i < end; ++i) {
        if (value > 1'000'000'000LL) {
            break;
        }
        value = value * 10 + (text[i] - '0');
    }
    *out = static_cast<int>(value);
    return true;
}

// Read a whole file as UTF-8 text.  Returns false if it could not be opened.
bool read_text_file(const fs::path& path, std::string* out)
{
    assert(out != nullptr);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *out = buffer.str();
    // Python opens these with encoding="utf-8", which silently consumes a BOM
    // only for "utf-8-sig"; it does NOT here, so a BOM would land in the first
    // topic's text.  Strip it anyway: a BOM in a hand-edited .toc is a Notepad
    // artefact, and keeping parity with a bug helps nobody.
    static constexpr std::string_view kBom = "\xEF\xBB\xBF";
    if (out->size() >= kBom.size() &&
        std::string_view(*out).substr(0, kBom.size()) == kBom) {
        out->erase(0, kBom.size());
    }
    return true;
}

// Python's repr() of a str, close enough for the one place it appears: the
// parse-error message, which quotes the offending line back to the user.
std::string quoted(std::string_view text)
{
    std::string out = "'";
    out.reserve(text.size() + 2);
    for (const char c : text) {
        if (c == '\'' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// int(x) over a JSON value, accepting the shapes Python's int() accepts here:
// a whole number, a float (truncated toward zero), or a decimal string.
bool json_to_int(const json& value, int* out)
{
    assert(out != nullptr);
    if (value.is_number_integer()) {
        *out = value.get<int>();
        return true;
    }
    if (value.is_number_float()) {
        *out = static_cast<int>(value.get<double>());
        return true;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        try {
            std::size_t consumed = 0;
            const int parsed = std::stoi(text, &consumed);
            if (consumed != text.size()) {
                return false;
            }
            *out = parsed;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

}  // namespace

std::optional<fs::path> find_metadata_path(const fs::path& pdf_path)
{
    fs::path base = pdf_path;
    base.replace_extension();
    for (const char* extension : kMetadataExtensions) {
        fs::path candidate = base;
        candidate += extension;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

TopicLoadResult parse_toc_text(std::string_view text)
{
    TopicLoadResult result;
    std::size_t line_start = 0;
    int lineno = 0;

    while (line_start <= text.size()) {
        const std::size_t newline = text.find('\n', line_start);
        const std::size_t line_end =
            (newline == std::string_view::npos) ? text.size() : newline;
        std::string_view raw = text.substr(line_start, line_end - line_start);
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        ++lineno;

        const std::string_view line = trim(raw);
        if (!line.empty() && line.front() != '#') {
            // rpartition(':') when there is a colon, else rpartition(' ').
            // With neither, the topic half comes out empty and the line is an
            // error -- which is what Python does too.
            const char separator = (line.find(':') != std::string_view::npos) ? ':' : ' ';
            const std::size_t split = line.rfind(separator);

            std::string_view topic;
            std::string_view page_part;
            if (split != std::string_view::npos) {
                topic = trim(line.substr(0, split));
                page_part = line.substr(split + 1);
            }

            int page = 0;
            if (topic.empty() || !first_number(page_part, &page)) {
                result.error = "Line " + std::to_string(lineno) +
                               ": expected 'Topic: page', got " + quoted(raw);
                return result;
            }
            result.entries.push_back(TopicEntry{std::string(topic), page});
        }

        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }

    result.ok = true;
    return result;
}

TopicLoadResult parse_json_metadata(std::string_view text)
{
    TopicLoadResult result;
    const json document = json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        result.error = "not valid JSON";
        return result;
    }
    if (!document.is_array()) {
        result.error = "expected a JSON array of {\"topic\", \"page\"} objects";
        return result;
    }

    for (const json& item : document) {
        if (!item.is_object() || !item.contains("topic") || !item.contains("page")) {
            result.error = "every entry needs a \"topic\" and a \"page\"";
            return result;
        }
        // str(item["topic"]) in Python stringifies whatever is there; a
        // non-string topic is far more likely a typo than an intent, so dump
        // it back to its JSON text rather than guessing.
        const std::string topic = item["topic"].is_string()
                                      ? item["topic"].get<std::string>()
                                      : item["topic"].dump();
        int page = 0;
        if (!json_to_int(item["page"], &page)) {
            result.error = "\"page\" must be a whole number";
            return result;
        }
        result.entries.push_back(
            TopicEntry{std::string(trim(topic)), page});
    }

    result.ok = true;
    return result;
}

TopicLoadResult load_metadata(const fs::path& metadata_path)
{
    TopicLoadResult result;
    std::string text;
    if (!read_text_file(metadata_path, &text)) {
        result.error = "could not open the file";
        return result;
    }

    std::string extension = path_to_utf8(metadata_path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (extension == ".json") ? parse_json_metadata(text) : parse_toc_text(text);
}

fs::path bookmarks_path(const fs::path& pdf_path)
{
    fs::path path = pdf_path;
    path.replace_extension();
    path += ".bookmarks.json";
    return path;
}

std::vector<Bookmark> load_bookmarks(const fs::path& pdf_path,
                                     std::string* warning)
{
    std::vector<Bookmark> bookmarks;
    const fs::path path = bookmarks_path(pdf_path);

    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return bookmarks;  // the normal case: this PDF has no bookmarks
    }

    std::string text;
    if (!read_text_file(path, &text)) {
        if (warning != nullptr) {
            *warning = "Could not read " + path_to_utf8(path.filename());
        }
        return bookmarks;
    }

    const json document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_array()) {
        if (warning != nullptr) {
            *warning = path_to_utf8(path.filename()) + " is not valid JSON";
        }
        return bookmarks;
    }

    for (const json& item : document) {
        if (!item.is_object() || !item.contains("page") || !item.contains("title")) {
            continue;
        }
        int page = 0;
        if (!json_to_int(item["page"], &page)) {
            continue;
        }
        const std::string title = item["title"].is_string()
                                      ? item["title"].get<std::string>()
                                      : item["title"].dump();
        const std::string_view trimmed = trim(title);
        // Both guards are Python's: a page before the first one cannot be
        // navigated to, and an untitled bookmark is unclickable in the list.
        if (page >= 1 && !trimmed.empty()) {
            bookmarks.push_back(Bookmark{page, std::string(trimmed)});
        }
    }

    // Stable, so two bookmarks on one page keep the order they were added in
    // -- which is the order the user made them.
    std::stable_sort(bookmarks.begin(), bookmarks.end(),
                     [](const Bookmark& a, const Bookmark& b) {
                         return a.page < b.page;
                     });
    return bookmarks;
}

bool save_bookmarks(const fs::path& pdf_path,
                    const std::vector<Bookmark>& bookmarks)
{
    const fs::path path = bookmarks_path(pdf_path);

    if (bookmarks.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            fs::remove(path, ec);
            return !ec;
        }
        return true;
    }

    json document = json::array();
    for (const Bookmark& bookmark : bookmarks) {
        assert(bookmark.page >= 1);
        document.push_back(json{{"page", bookmark.page},
                                {"title", bookmark.title}});
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    // indent=2, matching what the Python app writes, so a sidecar edited by
    // hand between the two apps does not churn in diffs.
    stream << document.dump(2);
    stream.flush();
    return stream.good();
}

}  // namespace pdfsherpa
