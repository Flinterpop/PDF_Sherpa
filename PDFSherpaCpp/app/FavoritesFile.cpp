#include "FavoritesFile.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace pdfsherpa {
namespace {

using json = nlohmann::json;

// Favorites are compared case-insensitively on the ASCII range, because the
// entries are Windows paths and "Manuals/a.pdf" and "manuals/A.pdf" are the
// same file.  Separators are normalised too: the app writes '/', but a
// hand-edited file may well use '\'.
std::string compare_key(std::string text)
{
    for (char& c : text) {
        if (c == '\\') {
            c = '/';
        } else {
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return text;
}

}  // namespace

std::string favorites_to_json(const std::vector<std::string>& favorites)
{
    json document;
    document["favorites"] = json::array();
    for (const std::string& favorite : favorites) {
        document["favorites"].push_back(favorite);
    }
    // Two-space indent and a trailing newline: this file is meant to be read
    // and hand-edited, not just round-tripped.
    return document.dump(2) + "\n";
}

FavoritesFile parse_favorites_json(const std::string& text)
{
    FavoritesFile result;

    const json document = json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        result.error = "not valid JSON";
        return result;
    }

    const json* array = nullptr;
    if (document.is_array()) {
        // A bare top-level array, which is what a user is most likely to
        // hand-write.
        array = &document;
    } else if (document.is_object() && document.contains("favorites") &&
               document["favorites"].is_array()) {
        array = &document["favorites"];
    }

    if (array == nullptr) {
        result.ok = true;   // it parsed; it simply is not a favorites file
        result.had_list = false;
        return result;
    }

    result.had_list = true;
    for (const json& entry : *array) {
        if (!entry.is_string()) {
            continue;  // skip junk rather than failing the whole import
        }
        std::string value = entry.get<std::string>();
        if (value.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        result.favorites.push_back(std::move(value));
    }

    result.ok = true;
    return result;
}

std::vector<std::string> merge_favorites(const std::vector<std::string>& existing,
                                         const std::vector<std::string>& incoming,
                                         std::size_t cap)
{
    std::vector<std::string> merged;
    std::vector<std::string> seen;

    const auto push = [&merged, &seen, cap](const std::string& value) {
        if (merged.size() >= cap) {
            return;
        }
        const std::string key = compare_key(value);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            return;
        }
        seen.push_back(key);
        merged.push_back(value);
    };

    // Existing first, so an import cannot reorder what the user already had.
    for (const std::string& value : existing) {
        push(value);
    }
    for (const std::string& value : incoming) {
        push(value);
    }
    return merged;
}

}  // namespace pdfsherpa
