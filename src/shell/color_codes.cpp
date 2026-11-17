#include "color_codes.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
bool enabled(const char* setting) {
    const char* value = std::getenv(setting);
    return !value || std::string(value) != "0";
}

std::string hex_color(const std::string& digits) {
    if (digits.size() != 6) return {};
    for (unsigned char ch : digits) {
        if (!std::isxdigit(ch)) return {};
    }
    const int red = std::stoi(digits.substr(0, 2), nullptr, 16);
    const int green = std::stoi(digits.substr(2, 2), nullptr, 16);
    const int blue = std::stoi(digits.substr(4, 2), nullptr, 16);
    return "\033[0;38;2;" + std::to_string(red) + ";" + std::to_string(green) + ";" + std::to_string(blue) + "m";
}

std::string color_code(char code) {
    static const std::array<int, 16> ansi = {30, 34, 32, 36, 31, 35, 33, 37, 90, 94, 92, 96, 91, 95, 93, 97};
    if (code >= 'A' && code <= 'Z') code = static_cast<char>(code - 'A' + 'a');
    if (code >= '0' && code <= '9') return "\033[0;" + std::to_string(ansi[code - '0']) + "m";
    if (code >= 'a' && code <= 'f') return "\033[0;" + std::to_string(ansi[10 + code - 'a']) + "m";
    switch (code) {
        case 'k': return "\033[5m";
        case 'l': return "\033[1m";
        case 'm': return "\033[9m";
        case 'n': return "\033[4m";
        case 'o': return "\033[3m";
        case 'r': return "\033[0m";
        default: return {};
    }
}

std::string section_sign_bytes() {
    std::string bytes("\xC2\xA7", 2);
#ifdef _WIN32
    const UINT code_page = GetConsoleCP();
    CPINFO info{};
    if (code_page != 0 && code_page != CP_UTF8 && GetCPInfo(code_page, &info) && info.MaxCharSize > 1) {
        const wchar_t section_sign = L'\u00A7';
        BOOL used_default = FALSE;
        const int length = WideCharToMultiByte(code_page, 0, &section_sign, 1, nullptr, 0, nullptr, &used_default);
        if (length > 0 && !used_default) {
            std::string local_bytes(static_cast<size_t>(length), '\0');
            if (WideCharToMultiByte(code_page, 0, &section_sign, 1, local_bytes.data(), length, nullptr, &used_default) == length && !used_default) {
                bytes += local_bytes;
            }
        }
    }
#endif
    return bytes;
}

size_t section_sign_length(const std::string& value, size_t position) {
    static const std::string utf8_sign("\xC2\xA7", 2);
    if (value.compare(position, utf8_sign.size(), utf8_sign) == 0) return utf8_sign.size();
#ifdef _WIN32
    static const std::string local_sign = [] {
        const std::string all = section_sign_bytes();
        return all.size() > 2 ? all.substr(2) : std::string();
    }();
    if (!local_sign.empty() && value.compare(position, local_sign.size(), local_sign) == 0) return local_sign.size();
#endif
    return 0;
}

bool is_hex(char ch) {
    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

std::string named_tag(const std::string& tag) {
    static const std::unordered_map<std::string, std::string> tags = {
        {"black", "0"}, {"dark_blue", "1"}, {"dark_green", "2"}, {"dark_aqua", "3"},
        {"dark_red", "4"}, {"dark_purple", "5"}, {"gold", "6"}, {"gray", "7"},
        {"dark_gray", "8"}, {"blue", "9"}, {"green", "a"}, {"aqua", "b"},
        {"red", "c"}, {"light_purple", "d"}, {"yellow", "e"}, {"white", "f"},
        {"obfuscated", "k"}, {"bold", "l"}, {"strikethrough", "m"},
        {"underline", "n"}, {"italic", "o"}, {"reset", "r"}
    };
    const auto found = tags.find(tag);
    return found == tags.end() ? std::string() : color_code(found->second[0]);
}
}

std::string translate_color_codes(const std::string& value) {
    if (!enabled("DUCKSHELL_COLOR_CODES")) return value;

    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        if (value[i] == '&' && i + 1 < value.size()) {
            if (value[i + 1] == '#' && i + 8 <= value.size()) {
                const std::string ansi = hex_color(value.substr(i + 2, 6));
                if (!ansi.empty()) {
                    result += ansi;
                    i += 8;
                    continue;
                }
            }
            const std::string ansi = color_code(value[i + 1]);
            if (!ansi.empty()) {
                result += ansi;
                i += 2;
                continue;
            }
        }

        if (value[i] == '{') {
            const size_t end = value.find('}', i + 1);
            if (end != std::string::npos) {
                std::string tag = value.substr(i + 1, end - i - 1);
                for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const std::string ansi = tag.size() == 7 && tag[0] == '#' ? hex_color(tag.substr(1)) : named_tag(tag);
                if (!ansi.empty()) {
                    result += ansi;
                    i = end + 1;
                    continue;
                }
            }
        }

        const size_t sign_length = section_sign_length(value, i);
        if (sign_length > 0 && i + sign_length < value.size()) {
            const size_t code_position = i + sign_length;
            if (value[code_position] == 'x' || value[code_position] == 'X') {
                size_t position = code_position + 1;
                std::string digits;
                bool valid = true;
                for (int digit = 0; digit < 6; ++digit) {
                    const size_t next_sign_length = section_sign_length(value, position);
                    if (!next_sign_length || position + next_sign_length >= value.size() || !is_hex(value[position + next_sign_length])) {
                        valid = false;
                        break;
                    }
                    digits.push_back(value[position + next_sign_length]);
                    position += next_sign_length + 1;
                }
                if (valid) {
                    result += hex_color(digits);
                    i = position;
                    continue;
                }
            }
            const std::string ansi = color_code(value[code_position]);
            if (!ansi.empty()) {
                result += ansi;
                i = code_position + 1;
                continue;
            }
        }

        result.push_back(value[i++]);
    }
    return result;
}

std::string translate_external_argument(const std::string& value) {
    if (!enabled("DUCKSHELL_EXTERNAL_COLOR_CODES") || !enabled("DUCKSHELL_COLOR_CODES")) return value;
    if (value.find("://") != std::string::npos) return value;
    try {
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::path(value), error)) return value;
    } catch (const std::exception&) {
        // Try UTF-8 below when the native path encoding cannot represent this argument.
    }
    try {
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::u8path(value), error)) return value;
    } catch (const std::exception&) {
        // A non-path argument can contain bytes that are not valid UTF-8; treat it as text.
    }
    return translate_color_codes(value);
}