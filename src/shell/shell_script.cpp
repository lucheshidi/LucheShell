#include "shell_script.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "../header.h"
#include "shell_commands.h"
#include "shell_input.h"
#include "color_codes.h"
#include "../version.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void print_version() {
    std::string version_text = "DuckShell " + std::string(DUCKSHELL_VERSION);
    auto color_it = shell_global_vars.find("DUCKSHELL_VERSION_COLOR");
    if (color_it != shell_global_vars.end() && !color_it->second.empty()) {
        version_text = color_it->second + version_text + "\033[0m";
    }
    std::cout << translate_color_codes(version_text) << std::endl;
}

namespace {
struct Function {
    std::vector<std::string> parameters;
    std::vector<std::string> body;
};

std::unordered_map<std::string, Function> functions;
std::unordered_map<std::string, std::string> aliases;
std::vector<std::string> positional_arguments;
int last_status = 0;
int exit_code = 0;
bool exit_requested = false;
bool returned = false;
bool breaking = false;
bool continuing = false;
int function_depth = 0;
std::vector<std::unordered_map<std::string, std::optional<std::string>>> function_scopes;

void preserve_function_variable(const std::string& name) {
    if (function_scopes.empty() || function_scopes.back().count(name)) return;
    const auto variable = shell_global_vars.find(name);
    if (variable == shell_global_vars.end()) function_scopes.back()[name] = std::nullopt;
    else function_scopes.back()[name] = variable->second;
}

void restore_function_scope() {
    for (const auto& variable : function_scopes.back()) {
        if (variable.second) shell_global_vars[variable.first] = *variable.second;
        else shell_global_vars.erase(variable.first);
    }
    function_scopes.pop_back();
}

std::string trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool starts_with_ci(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && lower(text.substr(0, prefix.size())) == lower(prefix);
}

std::string get_variable(const std::string& name) {
    if (name == "?") return std::to_string(last_status);
    if (name == "#") return std::to_string(positional_arguments.size());
    if (name == "@" || name == "*") {
        std::string result;
        for (size_t i = 0; i < positional_arguments.size(); ++i) {
            if (i) result.push_back(' ');
            result += positional_arguments[i];
        }
        return result;
    }
    if (name == "0") return "DuckShell";
    if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        const size_t index = static_cast<size_t>(std::strtoul(name.c_str(), nullptr, 10));
        if (index > 0 && index <= positional_arguments.size()) return positional_arguments[index - 1];
        return {};
    }
    const auto variable = shell_global_vars.find(name);
    if (variable != shell_global_vars.end()) return variable->second;
    const char* environment = std::getenv(name.c_str());
    return environment ? environment : "";
}

void decode_escapes(const std::string& input, std::string& output) {
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    auto append_utf8 = [](std::string& target, unsigned int codepoint) {
        if (codepoint <= 0x7F) target.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7FF) {
            target.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            target.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0x10FFFF) {
            target.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    };
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\' || i + 1 >= input.size()) {
            output.push_back(input[i]);
            continue;
        }
        const char ch = input[++i];
        if (ch == 'x' || ch == 'u' || ch == 'U') {
            const size_t digits = ch == 'x' ? 2 : ch == 'u' ? 4 : 8;
            unsigned int codepoint = 0;
            bool valid = i + digits < input.size();
            for (size_t digit = 0; valid && digit < digits; ++digit) {
                const int number = hex_value(input[i + 1 + digit]);
                if (number < 0) valid = false;
                else codepoint = (codepoint << 4) | static_cast<unsigned int>(number);
            }
            if (valid && (ch == 'x' || (codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)))) {
                if (ch == 'x') output.push_back(static_cast<char>(codepoint));
                else append_utf8(output, codepoint);
                i += digits;
                continue;
            }
            output.push_back('\\');
            output.push_back(ch);
            continue;
        }
        if (ch >= '0' && ch <= '7') {
            int code = ch - '0';
            int digits = 1;
            while (digits < 3 && i + 1 < input.size() && input[i + 1] >= '0' && input[i + 1] <= '7') {
                code = code * 8 + (input[++i] - '0');
                ++digits;
            }
            output.push_back(static_cast<char>(code));
            continue;
        }
        switch (ch) {
            case 'a': output.push_back('\a'); break;
            case 'b': output.push_back('\b'); break;
            case 'e': output.push_back('\033'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'v': output.push_back('\v'); break;
            case '\\': output.push_back('\\'); break;
            case '"': output.push_back('"'); break;
            case '\'': output.push_back('\''); break;
            default: output.push_back('\\'); output.push_back(ch); break;
        }
    }
}

std::string expand_parameter(const std::string& expression) {
    const size_t replace_pos = expression.find(".replace(");
    if (replace_pos != std::string::npos) {
        const std::string name = expression.substr(0, replace_pos);
        std::string arguments = expression.substr(replace_pos + 9);
        if (!arguments.empty() && arguments.back() == ')') arguments.pop_back();
        const size_t comma = arguments.find(',');
        if (comma != std::string::npos) {
            auto clean = [](std::string value) {
                value = trim(value);
                if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
                    value = value.substr(1, value.size() - 2);
                }
                return value;
            };
            const std::string old_value = clean(arguments.substr(0, comma));
            const std::string new_value = clean(arguments.substr(comma + 1));
            std::string value = get_variable(name);
            if (!old_value.empty()) {
                size_t position = 0;
                while ((position = value.find(old_value, position)) != std::string::npos) {
                    value.replace(position, old_value.size(), new_value);
                    position += new_value.size();
                }
            }
            return value;
        }
    }
    const size_t default_pos = expression.find(":-");
    if (default_pos != std::string::npos) {
        const std::string name = expression.substr(0, default_pos);
        const std::string value = get_variable(name);
        return value.empty() ? expression.substr(default_pos + 2) : value;
    }
    if (!expression.empty() && expression[0] == '#') return std::to_string(get_variable(expression.substr(1)).size());
    return get_variable(expression);
}

std::string evaluate_arithmetic(const std::string& expression);

std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string word;
    char quote = 0;
    bool word_started = false;
    auto flush = [&]() {
        if (word_started) {
            tokens.push_back(word);
            word.clear();
            word_started = false;
        }
    };

    for (size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (quote == '\'') {
            if (ch == '\'') quote = 0;
            else word.push_back(ch);
            word_started = true;
            continue;
        }
        if (quote == '"') {
            if (ch == '"') {
                quote = 0;
                word_started = true;
            } else if (ch == '\\' && i + 1 < input.size() && (input[i + 1] == '"' || input[i + 1] == '\\' || input[i + 1] == '$')) {
                word.push_back(input[++i]);
                word_started = true;
            } else if (ch == '$') {
                std::string name;
                if (i + 2 < input.size() && input[i + 1] == '(' && input[i + 2] == '(') {
                    const size_t end = input.find("))", i + 3);
                    if (end != std::string::npos) {
                        word += evaluate_arithmetic(input.substr(i + 3, end - i - 3));
                        i = end + 1;
                    } else word.push_back(ch);
                } else if (i + 1 < input.size() && input[i + 1] == '{') {
                    const size_t end = input.find('}', i + 2);
                    if (end != std::string::npos) {
                        name = input.substr(i + 2, end - i - 2);
                        i = end;
                    } else word.push_back(ch);
                } else {
                    size_t end = i + 1;
                    if (end < input.size() && (input[end] == '?' || input[end] == '#' || input[end] == '@' || input[end] == '*')) ++end;
                    else while (end < input.size() && (std::isalnum(static_cast<unsigned char>(input[end])) || input[end] == '_')) ++end;
                    if (end == i + 1) word.push_back(ch);
                    else {
                        name = input.substr(i + 1, end - i - 1);
                        i = end - 1;
                    }
                }
                if (!name.empty()) word += expand_parameter(name);
                word_started = true;
            } else {
                word.push_back(ch);
                word_started = true;
            }
            continue;
        }

        if (ch == '$' && i + 1 < input.size() && input[i + 1] == '\'') {
            const size_t end = input.find('\'', i + 2);
            if (end != std::string::npos) {
                decode_escapes(input.substr(i + 2, end - i - 2), word);
                word_started = true;
                i = end;
            } else {
                word.push_back(ch);
                word_started = true;
            }
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
            word_started = true;
        } else if (ch == '\\' && i + 1 < input.size()) {
            word.push_back(input[++i]);
            word_started = true;
        } else if (ch == '$') {
            std::string name;
            if (i + 2 < input.size() && input[i + 1] == '(' && input[i + 2] == '(') {
                const size_t end = input.find("))", i + 3);
                if (end != std::string::npos) {
                    word += evaluate_arithmetic(input.substr(i + 3, end - i - 3));
                    i = end + 1;
                } else word.push_back(ch);
                word_started = true;
            } else if (i + 1 < input.size() && input[i + 1] == '{') {
                const size_t end = input.find('}', i + 2);
                if (end != std::string::npos) {
                    name = input.substr(i + 2, end - i - 2);
                    i = end;
                } else word.push_back(ch);
            } else {
                size_t end = i + 1;
                if (end < input.size() && (input[end] == '?' || input[end] == '#' || input[end] == '@' || input[end] == '*')) ++end;
                else while (end < input.size() && (std::isalnum(static_cast<unsigned char>(input[end])) || input[end] == '_')) ++end;
                if (end == i + 1) word.push_back(ch);
                else {
                    name = input.substr(i + 1, end - i - 1);
                    i = end - 1;
                }
            }
            if (!name.empty()) word += expand_parameter(name);
            word_started = true;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            flush();
        } else if ((ch == '&' || ch == '|') && i + 1 < input.size() && input[i + 1] == ch) {
            flush();
            tokens.emplace_back(input.substr(i, 2));
            ++i;
        } else if (ch == ';') {
            flush();
            tokens.emplace_back(";");
        } else if (ch == '!' && word.empty() && !word_started) {
            tokens.emplace_back("!");
        } else {
            word.push_back(ch);
            word_started = true;
        }
    }
    flush();
    return tokens;
}

struct Value {
    std::string text;
    bool numeric = false;
    double number = 0;
};

Value make_value(const std::string& text) {
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (end && end != text.c_str() && *end == '\0') return {text, true, number};
    return {text, false, 0};
}

std::string format_number(double number) {
    std::ostringstream output;
    output << std::setprecision(15) << number;
    return output.str();
}

class ExpressionParser {
public:
    explicit ExpressionParser(const std::string& expression) : tokens_(tokenize_expression(expression)) {}
    Value parse() { return parse_or(); }

private:
    std::vector<std::string> tokens_;
    size_t index_ = 0;

    static std::vector<std::string> tokenize_expression(const std::string& expression) {
        std::vector<std::string> result;
        for (size_t i = 0; i < expression.size();) {
            const char ch = expression[i];
            if (std::isspace(static_cast<unsigned char>(ch))) { ++i; continue; }
            if (ch == '\'' || ch == '"') {
                const char quote = ch;
                std::string value;
                ++i;
                while (i < expression.size() && expression[i] != quote) {
                    if (expression[i] == '\\' && i + 1 < expression.size()) ++i;
                    value.push_back(expression[i++]);
                }
                if (i < expression.size()) ++i;
                result.push_back("\x01" + value);
            } else if (std::string("<>=!&|+-*/%()").find(ch) != std::string::npos) {
                std::string op(1, ch);
                if (i + 1 < expression.size() && (expression.substr(i, 2) == "<=" || expression.substr(i, 2) == ">=" || expression.substr(i, 2) == "==" || expression.substr(i, 2) == "!=" || expression.substr(i, 2) == "<>" || expression.substr(i, 2) == "&&" || expression.substr(i, 2) == "||")) {
                    op = expression.substr(i, 2);
                    i += 2;
                } else ++i;
                result.push_back(op);
            } else {
                size_t end = i;
                while (end < expression.size() && !std::isspace(static_cast<unsigned char>(expression[end])) && std::string("<>=!&|+-*/%()").find(expression[end]) == std::string::npos) ++end;
                result.push_back(expression.substr(i, end - i));
                i = end;
            }
        }
        return result;
    }

    std::string peek() const { return index_ < tokens_.size() ? tokens_[index_] : ""; }
    bool consume(const std::string& value) {
        if (lower(peek()) != lower(value)) return false;
        ++index_;
        return true;
    }
    static bool truth(const Value& value) { return value.numeric ? value.number != 0 : !value.text.empty() && value.text != "0"; }
    static Value boolean(bool value) { return {value ? "1" : "0", true, value ? 1.0 : 0.0}; }

    Value parse_or() {
        Value left = parse_and();
        while (consume("or") || consume("||")) {
            const std::string op = lower(tokens_[index_ - 1]);
            Value right = parse_and();
            left = boolean(truth(left) || truth(right));
            (void)op;
        }
        return left;
    }
    Value parse_and() {
        Value left = parse_compare();
        while (consume("and") || consume("&&")) {
            Value right = parse_compare();
            left = boolean(truth(left) && truth(right));
        }
        return left;
    }
    Value parse_compare() {
        Value left = parse_add();
        const std::string op = peek();
        if (consume("=") || consume("==") || consume("!=") || consume("<>") || consume("<") || consume(">") || consume("<=") || consume(">=")) {
            Value right = parse_add();
            bool result = false;
            if (left.numeric && right.numeric) {
                if (op == "=" || op == "==") result = left.number == right.number;
                else if (op == "!=" || op == "<>") result = left.number != right.number;
                else if (op == "<") result = left.number < right.number;
                else if (op == ">") result = left.number > right.number;
                else if (op == "<=") result = left.number <= right.number;
                else result = left.number >= right.number;
            } else {
                if (op == "=" || op == "==") result = left.text == right.text;
                else if (op == "!=" || op == "<>") result = left.text != right.text;
                else if (op == "<") result = left.text < right.text;
                else if (op == ">") result = left.text > right.text;
                else if (op == "<=") result = left.text <= right.text;
                else result = left.text >= right.text;
            }
            return boolean(result);
        }
        return left;
    }
    Value parse_add() {
        Value left = parse_multiply();
        for (;;) {
            if (consume("+") || consume("&")) {
                const std::string op = tokens_[index_ - 1];
                Value right = parse_multiply();
                if (op == "&" || !left.numeric || !right.numeric) left = make_value(left.text + right.text);
                else left = make_value(format_number(left.number + right.number));
            } else if (consume("-")) {
                Value right = parse_multiply();
                left = make_value(format_number(left.number - right.number));
            } else return left;
        }
    }
    Value parse_multiply() {
        Value left = parse_unary();
        for (;;) {
            if (consume("*")) left = make_value(format_number(left.number * parse_unary().number));
            else if (consume("/")) {
                const double divisor = parse_unary().number;
                left = make_value(divisor == 0 ? "0" : format_number(left.number / divisor));
            } else if (consume("%") || consume("mod")) {
                const double divisor = parse_unary().number;
                left = make_value(divisor == 0 ? "0" : format_number(std::fmod(left.number, divisor)));
            } else return left;
        }
    }
    Value parse_unary() {
        if (consume("not") || consume("!") || consume("-")) {
            const std::string op = lower(tokens_[index_ - 1]);
            Value value = parse_unary();
            if (op == "-") return make_value(format_number(-value.number));
            return boolean(!truth(value));
        }
        if (consume("+")) return parse_unary();
        if (consume("(")) {
            Value value = parse_or();
            consume(")");
            return value;
        }
        const std::string token = peek();
        if (token.empty()) return {};
        ++index_;
        if (token[0] == '\x01') return make_value(token.substr(1));
        if (lower(token) == "true") return boolean(true);
        if (lower(token) == "false") return boolean(false);
        if (token[0] == '$') return make_value(expand_parameter(token.substr(token.size() > 1 && token[1] == '{' ? 2 : 1)));
        if (shell_global_vars.count(token) || std::getenv(token.c_str())) return make_value(get_variable(token));
        return make_value(token);
    }
};

std::string evaluate_arithmetic(const std::string& expression) {
    return ExpressionParser(expression).parse().text;
}

bool expression_truth(const std::string& text) {
    try {
        const Value value = ExpressionParser(text).parse();
        return value.numeric ? value.number != 0 : !value.text.empty() && value.text != "0";
    } catch (...) {
        return false;
    }
}

std::vector<std::string> split_script_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    char quote = 0;
    int parens = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quote) {
            line.push_back(ch);
            if (ch == quote && (i == 0 || text[i - 1] != '\\')) quote = 0;
            continue;
        }
        if (ch == '\'' || ch == '"') { quote = ch; line.push_back(ch); continue; }
        if (ch == '(') ++parens;
        else if (ch == ')' && parens > 0) --parens;
        if ((ch == '\n' || (ch == ';' && parens == 0))) {
            lines.push_back(trim(line));
            line.clear();
        } else line.push_back(ch);
    }
    if (!line.empty()) lines.push_back(trim(line));
    return lines;
}

bool is_stop_line(const std::string& line, const std::vector<std::string>& stops) {
    const std::string lowered = lower(trim(line));
    for (const auto& stop : stops) {
        if (lowered == lower(stop) || (stop == "elif" && starts_with_ci(lowered, "elif ")) ||
            (stop == "elseif" && starts_with_ci(lowered, "elseif ")) || (stop == "elseif" && starts_with_ci(lowered, "else if ")) ||
            (stop == "loop" && starts_with_ci(lowered, "loop ")) || (stop == "next" && starts_with_ci(lowered, "next "))) return true;
    }
    return false;
}

bool is_opener(const std::string& line) {
    const std::string value = lower(trim(line));
    return starts_with_ci(value, "if ") || starts_with_ci(value, "while ") || starts_with_ci(value, "until ") ||
           starts_with_ci(value, "for ") || starts_with_ci(value, "do ") || starts_with_ci(value, "function ") ||
           std::regex_match(value, std::regex(R"([a-z_][a-z0-9_]*\s*\([^)]*\)\s*\{?)"));
}

bool is_closer(const std::string& line) {
    const std::string value = lower(trim(line));
    return value == "fi" || value == "done" || value == "}" || value == "wend" || value == "loop" ||
           starts_with_ci(value, "end if") || starts_with_ci(value, "end function") || starts_with_ci(value, "end sub") ||
           starts_with_ci(value, "next") || starts_with_ci(value, "loop ");
}

std::string resolve_path(const std::string& value);

size_t find_stop(const std::vector<std::string>& lines, size_t position, const std::vector<std::string>& stops) {
    size_t depth = 0;
    for (size_t i = position; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (depth == 0 && is_stop_line(line, stops)) return i;
        if (is_opener(line)) ++depth;
        else if (is_closer(line) && depth) --depth;
    }
    return lines.size();
}

bool test_command(const std::vector<std::string>& args) {
    size_t begin = args.size() > 1 && (args[0] == "[" || args[0] == "test") ? 1 : 0;
    size_t end = args.size();
    if (end > begin && args.back() == "]") --end;
    std::vector<std::string> values(args.begin() + static_cast<std::ptrdiff_t>(begin), args.begin() + static_cast<std::ptrdiff_t>(end));
    if (values.empty()) return false;
    if (values[0] == "!") {
        values.erase(values.begin());
        if (values.size() == 1) return values[0].empty();
        return !test_command(values);
    }
    if (values.size() == 1) return !values[0].empty() && values[0] != "0";
    if (values.size() == 2) {
        if (values[0] == "-n") return !values[1].empty();
        if (values[0] == "-z") return values[1].empty();
        if (values[0] == "-e" || values[0] == "-f" || values[0] == "-d" || values[0] == "-s") {
            const std::string path = resolve_path(values[1]);
            std::error_code error;
            if (values[0] == "-f") return std::filesystem::is_regular_file(path, error);
            if (values[0] == "-d") return std::filesystem::is_directory(path, error);
            if (!std::filesystem::exists(path, error)) return false;
            if (values[0] != "-s" || !std::filesystem::is_regular_file(path, error)) return values[0] != "-s";
            return std::filesystem::file_size(path, error) > 0 && !error;
        }
        return false;
    }
    if (values.size() >= 3) {
        const std::string& lhs = values[0];
        const std::string& op = values[1];
        const std::string& rhs = values[2];
        if (op == "=" || op == "==") return lhs == rhs;
        if (op == "!=" || op == "<>") return lhs != rhs;
        if (op == "-eq" || op == "-ne" || op == "-lt" || op == "-le" || op == "-gt" || op == "-ge") {
            const long long a = std::strtoll(lhs.c_str(), nullptr, 10), b = std::strtoll(rhs.c_str(), nullptr, 10);
            if (op == "-eq") return a == b;
            if (op == "-ne") return a != b;
            if (op == "-lt") return a < b;
            if (op == "-le") return a <= b;
            if (op == "-gt") return a > b;
            return a >= b;
        }
        if (op == "<") return lhs < rhs;
        if (op == ">") return lhs > rhs;
    }
    return false;
}

std::string resolve_path(const std::string& value) {
    if (value.empty()) return value;
    std::string path = value;
    if (path[0] == '~') path = home_dir + path.substr(1);
    if (path.size() > 1 && path[1] == ':') return path;
    if (path[0] == '/' || path[0] == '\\') return path;
    return dir_now + "/" + path;
}

int execute_lines(const std::vector<std::string>& lines, size_t& position, const std::vector<std::string>& stops);

int execute_simple(const std::string& source) {
    std::vector<std::string> tokens = tokenize(source);
    if (tokens.empty()) return 0;
    if (tokens[0] == "!") {
        tokens.erase(tokens.begin());
        return execute_simple(source.substr(source.find('!') + 1)) == 0 ? 1 : 0;
    }
    int status = 0;
    std::vector<std::string> command;
    std::string next_operator;
    auto run = [&]() {
        if (command.empty()) return;
        auto alias = aliases.find(command[0]);
        if (alias != aliases.end()) {
            std::vector<std::string> expanded = tokenize(alias->second);
            expanded.insert(expanded.end(), command.begin() + 1, command.end());
            command.swap(expanded);
        }
        if (lower(command[0]) == "call" && command.size() > 1) {
            std::string target = command[1];
            for (size_t i = 2; i < command.size(); ++i) target += " " + command[i];
            const size_t open = target.find('(');
            const size_t close = target.rfind(')');
            if (open != std::string::npos && close > open) {
                std::vector<std::string> call = {target.substr(0, open)};
                std::istringstream input(target.substr(open + 1, close - open - 1));
                std::string argument;
                while (std::getline(input, argument, ',')) {
                    const auto parsed = tokenize(trim(argument));
                    call.push_back(parsed.empty() ? "" : parsed.front());
                }
                command.swap(call);
            } else command.erase(command.begin());
        }
        const std::string name = command[0];
        if (name == "true" || name == ":") status = 0;
        else if (name == "false") status = 1;
        else if (name == "test" || name == "[") status = test_command(command) ? 0 : 1;
        else if (name == "exit") {
            exit_code = command.size() > 1 ? std::atoi(command[1].c_str()) : last_status;
            exit_requested = true;
            status = exit_code;
        } else if (name == "return") {
            exit_code = command.size() > 1 ? std::atoi(command[1].c_str()) : last_status;
            returned = true;
            status = exit_code;
        } else if (name == "break" || name == "continue") {
            if (name == "break") breaking = true; else continuing = true;
            status = 0;
        } else if (name == "unset") {
            for (size_t i = 1; i < command.size(); ++i) shell_global_vars.erase(command[i]);
            status = 0;
        } else if (name == "local" || name == "Dim") {
            status = 0;
            for (size_t i = 1; i < command.size(); ++i) {
                const size_t equals = command[i].find('=');
                const std::string key = command[i].substr(0, equals);
                preserve_function_variable(key);
                if (equals != std::string::npos) shell_global_vars[key] = command[i].substr(equals + 1);
                else if (!shell_global_vars.count(key)) shell_global_vars[key] = "";
            }
        } else if (name == "shift") {
            const size_t count = command.size() > 1 ? static_cast<size_t>(std::max(0, std::atoi(command[1].c_str()))) : 1;
            for (size_t i = 0; i < count && !positional_arguments.empty(); ++i) positional_arguments.erase(positional_arguments.begin());
            status = 0;
        } else if (name == "alias") {
            if (command.size() == 1) {
                for (const auto& entry : aliases) std::cout << "alias " << entry.first << "='" << entry.second << "'\n";
            } else for (size_t i = 1; i < command.size(); ++i) {
                const size_t equals = command[i].find('=');
                if (equals == std::string::npos) {
                    const auto found = aliases.find(command[i]);
                    if (found != aliases.end()) std::cout << "alias " << found->first << "='" << found->second << "'\n";
                } else aliases[command[i].substr(0, equals)] = command[i].substr(equals + 1);
            }
            status = 0;
        } else if (name == "unalias") {
            for (size_t i = 1; i < command.size(); ++i) aliases.erase(command[i]);
            status = 0;
        } else if (name == "source" || name == ".") {
            if (command.size() < 2) status = 2;
            else status = run_script_file(resolve_path(command[1]), std::vector<std::string>(command.begin() + 2, command.end()));
        } else if (name == "printf") {
            if (command.size() > 1) {
                std::string format = command[1];
                std::string output;
                decode_escapes(format, output);
                size_t arg_index = 2;
                std::string formatted;
                for (size_t i = 0; i < output.size(); ++i) {
                    if (output[i] == '%' && i + 1 < output.size()) {
                        const char spec = output[++i];
                        if (spec == '%') formatted.push_back('%');
                        else if (arg_index < command.size()) formatted += command[arg_index++];
                        else { formatted.push_back('%'); formatted.push_back(spec); }
                    } else formatted.push_back(output[i]);
                }
                std::cout << translate_color_codes(formatted);
                status = 0;
            } else status = 0;
        } else if (name == "read") {
            std::string value;
            std::getline(std::cin, value);
            if (command.size() > 1) shell_global_vars[command[1]] = value;
            status = std::cin ? 0 : 1;
        } else if (name == "let") {
            status = command.size() > 1 && expression_truth(command[1]) ? 0 : 1;
        } else if (name == "type") {
            if (command.size() > 1) {
                const std::string& target = command[1];
                if (functions.count(target)) std::cout << target << " is a function\n";
                else if (aliases.count(target)) std::cout << target << " is aliased to " << aliases[target] << '\n';
                else std::cout << target << " is a DuckShell command or external program\n";
            }
            status = 0;
        } else if (name == "history") {
            size_t index = 1;
            for (const auto& entry : command_history) std::cout << index++ << "  " << entry << '\n';
            status = 0;
        } else if (name == "reload") {
            bool profile_only = false;
            for (size_t i = 1; i < command.size(); ++i) {
                if (command[i] == "--profile-only") profile_only = true;
            }
            // 先保存 PROFILE 路径
            const auto profile_it = shell_global_vars.find("PROFILE");
            const std::string profile_path = (profile_it != shell_global_vars.end()) ? profile_it->second : "";
            if (profile_path.empty()) {
                std::cerr << "reload: PROFILE variable not set\n";
                status = 1;
            } else if (profile_only) {
                // 只重新加载 profile，先重置 profile 中可能设置的变量（保留核心变量）
                std::vector<std::string> to_keep = {"PROFILE", "HOME", "PATH", "PROMPT", "PS1", "PS2"};
                std::unordered_map<std::string, std::string> kept;
                for (const std::string& k : to_keep) {
                    auto it = shell_global_vars.find(k);
                    if (it != shell_global_vars.end()) kept[k] = it->second;
                }
                shell_global_vars.clear();
                shell_global_vars = kept;
                // 重新加载 profile
                status = run_script_file(profile_path, {});
                // 重新打印版本号
                print_version();
            } else {
                // 完全重置：清空所有变量、函数、别名
                shell_global_vars.clear();
                functions.clear();
                aliases.clear();
                positional_arguments.clear();
                last_status = 0;
                // 恢复核心变量
                shell_global_vars["HOME"] = home_dir;
                shell_global_vars["PROFILE"] = profile_path;
                const char* env_path = std::getenv("PATH");
                if (env_path) shell_global_vars["PATH"] = env_path;
                // 重新加载 profile
                status = run_script_file(profile_path, {});
                // 重新打印版本号
                print_version();
            }
        } else if (name == "eval") {
            std::string code;
            for (size_t i = 1; i < command.size(); ++i) { if (i > 1) code += ' '; code += command[i]; }
            const auto nested = split_script_lines(code);
            size_t nested_pos = 0;
            status = execute_lines(nested, nested_pos, {});
        } else if (functions.count(name)) {
            const Function function = functions[name];
            const auto saved_arguments = positional_arguments;
            function_scopes.emplace_back();
            for (size_t i = 0; i < function.parameters.size(); ++i) {
                preserve_function_variable(function.parameters[i]);
                shell_global_vars[function.parameters[i]] = i + 1 < command.size() ? command[i + 1] : "";
            }
            positional_arguments.assign(command.begin() + 1, command.end());
            ++function_depth;
            const bool previous_returned = returned;
            returned = false;
            size_t function_pos = 0;
            status = execute_lines(function.body, function_pos, {});
            if (returned) { status = exit_code; returned = previous_returned; }
            --function_depth;
            restore_function_scope();
            positional_arguments = saved_arguments;
        } else {
            status = run_builtin_command(command);
        }
        command.clear();
        last_status = status;
    };

    for (const auto& token : tokens) {
        if (token == "&&" || token == "||" || token == ";") {
            if (next_operator.empty() || (next_operator == "&&" && status == 0) || (next_operator == "||" && status != 0)) run();
            else command.clear();
            next_operator = token;
        } else command.push_back(token);
    }
    if (next_operator.empty() || (next_operator == "&&" && status == 0) || (next_operator == "||" && status != 0)) run();
    return status;
}

bool is_vbs_if(const std::string& line) {
    if (!starts_with_ci(line, "if ")) return false;
    const std::string lowered = lower(line);
    return lowered.find(" then") != std::string::npos;
}

std::string condition_text(std::string line, const std::string& keyword) {
    line = trim(line);
    line = line.substr(keyword.size());
    line = trim(line);
    if (!line.empty() && line.back() == ';') line.pop_back();
    const std::string lowered = lower(line);
    const size_t then_pos = lowered.rfind("then");
    if (then_pos != std::string::npos && then_pos + 4 == lowered.size()) line = trim(line.substr(0, then_pos));
    return line;
}

bool command_condition(const std::string& condition) {
    std::string text = trim(condition);
    if (text.empty()) return false;
    if (text.front() == '[' && text.back() == ']') return execute_simple(text) == 0;
    if (text.front() == '(' && text.back() == ')') return expression_truth(text.substr(1, text.size() - 2));
    return execute_simple(text) == 0;
}

void skip_optional_marker(const std::vector<std::string>& lines, size_t& position, const std::string& marker) {
    if (position < lines.size() && lower(trim(lines[position])) == lower(marker)) ++position;
}

int execute_if(const std::vector<std::string>& lines, size_t& position) {
    std::string header = trim(lines[position++]);
    const bool vbs = is_vbs_if(header);
    bool matched = false;
    int status = 0;
    std::string condition = condition_text(header, "if");
    if (vbs) {
        const size_t then_pos = lower(condition).rfind("then");
        if (then_pos != std::string::npos) condition = trim(condition.substr(0, then_pos));
    }
    bool active = vbs ? expression_truth(condition) : command_condition(condition);
    skip_optional_marker(lines, position, "then");
    if (active) {
        matched = true;
        status = execute_lines(lines, position, vbs ? std::vector<std::string>{"elseif", "else if", "else", "end if"} : std::vector<std::string>{"elif", "else", "fi"});
    } else {
        position = find_stop(lines, position, vbs ? std::vector<std::string>{"elseif", "else if", "else", "end if"} : std::vector<std::string>{"elif", "else", "fi"});
    }
    while (position < lines.size()) {
        const std::string marker = lower(trim(lines[position]));
        if ((!vbs && marker == "fi") || (vbs && starts_with_ci(marker, "end if"))) { ++position; break; }
        if ((!vbs && starts_with_ci(marker, "elif ")) || (vbs && (starts_with_ci(marker, "elseif ") || starts_with_ci(marker, "else if ")))) {
            std::string next_condition = trim(lines[position++]);
            const bool is_elseif = starts_with_ci(next_condition, "elseif ");
            next_condition = next_condition.substr(is_elseif ? 6 : (vbs ? 8 : 4));
            if (vbs) {
                const size_t then_pos = lower(next_condition).rfind("then");
                if (then_pos != std::string::npos) next_condition = trim(next_condition.substr(0, then_pos));
            }
            const bool branch = !matched && (vbs ? expression_truth(next_condition) : command_condition(next_condition));
            skip_optional_marker(lines, position, "then");
            if (branch) {
                matched = true;
                status = execute_lines(lines, position, vbs ? std::vector<std::string>{"elseif", "else if", "else", "end if"} : std::vector<std::string>{"elif", "else", "fi"});
            } else position = find_stop(lines, position, vbs ? std::vector<std::string>{"elseif", "else if", "else", "end if"} : std::vector<std::string>{"elif", "else", "fi"});
        } else if (marker == "else") {
            ++position;
            if (!matched) status = execute_lines(lines, position, vbs ? std::vector<std::string>{"end if"} : std::vector<std::string>{"fi"});
            else position = find_stop(lines, position, vbs ? std::vector<std::string>{"end if"} : std::vector<std::string>{"fi"});
            matched = true;
        } else { ++position; }
    }
    return status;
}

bool loop_condition(const std::string& header, const std::string& keyword, bool vbs) {
    std::string condition = condition_text(header, keyword);
    if (vbs) {
        if (starts_with_ci(condition, "while ")) condition = trim(condition.substr(6));
        else if (starts_with_ci(condition, "until ")) condition = trim(condition.substr(6));
        return expression_truth(condition);
    }
    return command_condition(condition);
}

int execute_while(const std::vector<std::string>& lines, size_t& position) {
    const std::string header = trim(lines[position++]);
    const bool until = starts_with_ci(header, "until ");
    const size_t vbs_end = find_stop(lines, position, {"wend"});
    const size_t posix_end = find_stop(lines, position, {"done"});
    const bool vbs = vbs_end < posix_end;
    const size_t body_start = position;
    skip_optional_marker(lines, position, "do");
    const size_t actual_body_start = position;
    size_t body_end = find_stop(lines, actual_body_start, vbs ? std::vector<std::string>{"wend"} : std::vector<std::string>{"done"});
    int status = 0;
    for (size_t iteration = 0; iteration < 100000 && !exit_requested; ++iteration) {
        bool condition = loop_condition(header, vbs ? "while" : (until ? "until" : "while"), vbs);
        if (until) condition = !condition;
        if (!condition) break;
        size_t body_pos = actual_body_start;
        breaking = continuing = false;
        status = execute_lines(lines, body_pos, vbs ? std::vector<std::string>{"wend"} : std::vector<std::string>{"done"});
        if (breaking) { breaking = false; break; }
        if (returned || exit_requested) break;
        (void)body_end;
        if (body_pos == actual_body_start) break;
    }
    position = find_stop(lines, actual_body_start, vbs ? std::vector<std::string>{"wend"} : std::vector<std::string>{"done"});
    if (position < lines.size()) ++position;
    (void)body_start;
    return status;
}

bool assign_value(const std::string& assignment) {
    const size_t equals = assignment.find('=');
    if (equals == std::string::npos) return false;
    const std::string key = trim(assignment.substr(0, equals));
    if (key.empty()) return false;
    std::string value = trim(assignment.substr(equals + 1));
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') || (value.front() == '"' && value.back() == '"'))) value = value.substr(1, value.size() - 2);
    shell_global_vars[key] = value;
    return true;
}

int execute_for(const std::vector<std::string>& lines, size_t& position) {
    const std::string header = trim(lines[position++]);
    const size_t body_start = position;
    skip_optional_marker(lines, position, "do");
    const size_t actual_body_start = position;
    std::smatch arithmetic_match;
    if (std::regex_search(header, arithmetic_match, std::regex(R"(for\s*\(\((.*)\)\))", std::regex::icase))) {
        std::istringstream clauses_input(arithmetic_match[1].str());
        std::string initialize, condition, update;
        std::getline(clauses_input, initialize, ';');
        std::getline(clauses_input, condition, ';');
        std::getline(clauses_input, update, ';');
        auto assign_arithmetic = [](const std::string& statement) {
            const size_t equals = statement.find('=');
            if (equals == std::string::npos) return;
            const std::string name = trim(statement.substr(0, equals));
            const std::string expression = trim(statement.substr(equals + 1));
            if (!name.empty()) shell_global_vars[name] = evaluate_arithmetic(expression);
        };
        auto update_arithmetic = [&](const std::string& statement) {
            const std::string value = trim(statement);
            std::smatch update_match;
            if (!std::regex_match(value, update_match, std::regex(R"(([A-Za-z_][A-Za-z0-9_]*)\s*(\+\+|--|\+=|-=|=)\s*(.*))"))) return;
            const std::string name = update_match[1].str();
            const std::string op = update_match[2].str();
            const double current = std::strtod(get_variable(name).c_str(), nullptr);
            double next = current;
            if (op == "++") next += 1;
            else if (op == "--") next -= 1;
            else {
                const double amount = std::strtod(evaluate_arithmetic(update_match[3].str()).c_str(), nullptr);
                if (op == "+=") next += amount;
                else if (op == "-=") next -= amount;
                else next = amount;
            }
            shell_global_vars[name] = format_number(next);
        };
        assign_arithmetic(initialize);
        int status = 0;
        for (size_t iteration = 0; iteration < 100000 && !exit_requested; ++iteration) {
            if (!trim(condition).empty() && !expression_truth(condition)) break;
            size_t body_pos = actual_body_start;
            breaking = continuing = false;
            status = execute_lines(lines, body_pos, {"done"});
            if (breaking) { breaking = false; break; }
            if (returned || exit_requested) break;
            continuing = false;
            update_arithmetic(update);
        }
        position = find_stop(lines, actual_body_start, {"done"});
        if (position < lines.size()) ++position;
        (void)body_start;
        return status;
    }
    const bool vbs = std::regex_search(header, std::regex(R"(for\s+(?:each\s+\w+\s+in\s+|\w+\s*=))", std::regex::icase));
    const std::string lowered = lower(header);
    std::vector<std::string> values;
    std::string variable;
    long long first = 0, last = -1, step = 1;
    bool range = false;
    if (vbs) {
        std::smatch match;
        if (std::regex_search(header, match, std::regex(R"(for\s+each\s+(\w+)\s+in\s+(.+))", std::regex::icase))) {
            variable = match[1].str();
            std::istringstream input(match[2].str());
            std::string item;
            while (input >> item) values.push_back(item);
        } else if (std::regex_search(header, match, std::regex(R"(for\s+(\w+)\s*=\s*(-?\d+)\s+to\s+(-?\d+)(?:\s+step\s+(-?\d+))?)", std::regex::icase))) {
            variable = match[1].str();
            first = std::stoll(match[2].str());
            last = std::stoll(match[3].str());
            if (match[4].matched) step = std::stoll(match[4].str());
            if (step == 0) step = 1;
            range = true;
        }
    } else {
        std::smatch match;
        if (std::regex_search(header, match, std::regex(R"(for\s+(\w+)\s+in\s+(.+))", std::regex::icase))) {
            variable = match[1].str();
            values = tokenize(match[2].str());
        }
    }
    if (range) {
        for (long long value = first; (step > 0 ? value <= last : value >= last) && !exit_requested; value += step) {
            shell_global_vars[variable] = std::to_string(value);
            size_t body_pos = actual_body_start;
            breaking = continuing = false;
            execute_lines(lines, body_pos, {"next"});
            if (breaking) { breaking = false; break; }
            if (returned || exit_requested) break;
        }
    } else {
        for (const auto& value : values) {
            shell_global_vars[variable] = value;
            size_t body_pos = actual_body_start;
            breaking = continuing = false;
            execute_lines(lines, body_pos, vbs ? std::vector<std::string>{"next"} : std::vector<std::string>{"done"});
            if (breaking) { breaking = false; break; }
            if (returned || exit_requested) break;
        }
    }
    const std::vector<std::string> stop = vbs ? std::vector<std::string>{"next"} : std::vector<std::string>{"done"};
    position = find_stop(lines, actual_body_start, stop);
    if (position < lines.size()) ++position;
    (void)body_start;
    (void)lowered;
    return last_status;
}

bool parse_function_header(const std::string& line, std::string& name, std::vector<std::string>& parameters, bool& vbs) {
    std::smatch match;
    const std::string header = trim(line);
    const bool posix_function = starts_with_ci(header, "function ") && header.find('{') != std::string::npos;
    const bool vbs_function = (starts_with_ci(header, "function ") && !posix_function) || starts_with_ci(header, "sub ");
    if (posix_function && std::regex_match(line, match, std::regex(R"(\s*function\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(\s*([^)]*)\s*\))?\s*\{\s*)", std::regex::icase))) {
        name = match[1].str();
        std::istringstream input(match[2].str());
        std::string parameter;
        while (std::getline(input, parameter, ',')) if (!trim(parameter).empty()) parameters.push_back(trim(parameter));
        vbs = false;
        return true;
    }
    if (!vbs_function && std::regex_match(line, match, std::regex(R"(\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*([^)]*)\s*\)\s*\{?\s*)", std::regex::icase))) {
        name = match[1].str();
        std::istringstream input(match[2].str());
        std::string parameter;
        while (std::getline(input, parameter, ',')) if (!trim(parameter).empty()) parameters.push_back(trim(parameter));
        vbs = false;
        return true;
    }
    if (std::regex_match(line, match, std::regex(R"(\s*(function|sub)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(([^)]*)\))?\s*)", std::regex::icase))) {
        name = match[2].str();
        std::string parameter_list = match[3].str();
        std::replace(parameter_list.begin(), parameter_list.end(), ',', ' ');
        std::istringstream input(parameter_list);
        std::string parameter;
        while (input >> parameter) {
            if (parameter.back() == ',') parameter.pop_back();
            if (!parameter.empty()) parameters.push_back(parameter);
        }
        vbs = true;
        return true;
    }
    return false;
}

int execute_lines(const std::vector<std::string>& lines, size_t& position, const std::vector<std::string>& stops) {
    int status = last_status;
    while (position < lines.size() && !exit_requested && !returned && !breaking && !continuing) {
        const std::string line = trim(lines[position]);
        const bool vbs_comment = !line.empty() && line[0] == '\'' && line.find('\'', 1) == std::string::npos;
        if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0 || lower(line) == "rem" || starts_with_ci(line, "rem ") || vbs_comment) {
            ++position;
            continue;
        }
        if (is_stop_line(line, stops)) break;
        const std::string lowered = lower(line);

        if (starts_with_ci(line, "if ")) status = execute_if(lines, position);
        else if (starts_with_ci(line, "while ") || starts_with_ci(line, "until ")) status = execute_while(lines, position);
        else if (starts_with_ci(line, "for ")) status = execute_for(lines, position);
        else if (starts_with_ci(line, "do ")) {
            const std::string header = line;
            ++position;
            const size_t body_start = position;
            const size_t end = find_stop(lines, body_start, {"loop"});
            std::string precondition = trim(header.substr(2));
            const bool has_precondition = starts_with_ci(precondition, "while ") || starts_with_ci(precondition, "until ");
            const std::string postcondition = end < lines.size() ? trim(lines[end].substr(4)) : "";
            bool condition = true;
            for (size_t iteration = 0; condition && iteration < 100000 && !exit_requested; ++iteration) {
                if (has_precondition) {
                    condition = starts_with_ci(precondition, "while ")
                        ? expression_truth(trim(precondition.substr(6)))
                        : !expression_truth(trim(precondition.substr(6)));
                }
                if (!condition) break;
                size_t body_pos = body_start;
                breaking = continuing = false;
                status = execute_lines(lines, body_pos, {"loop"});
                if (breaking) { breaking = false; break; }
                if (returned || exit_requested) break;
                if (end >= lines.size()) break;
                if (!has_precondition) {
                    if (starts_with_ci(postcondition, "while ")) condition = expression_truth(trim(postcondition.substr(6)));
                    else if (starts_with_ci(postcondition, "until ")) condition = !expression_truth(trim(postcondition.substr(6)));
                    else break;
                }
            }
            position = end < lines.size() ? end + 1 : end;
        } else if (starts_with_ci(line, "function ") || std::regex_match(line, std::regex(R"([A-Za-z_][A-Za-z0-9_]*\s*\(\s*[^)]*\s*\)\s*\{?)"))) {
            std::string name;
            std::vector<std::string> parameters;
            bool vbs = false;
            if (parse_function_header(line, name, parameters, vbs)) {
                ++position;
                const auto stop = vbs ? std::vector<std::string>{"end function", "end sub"} : std::vector<std::string>{"}"};
                const size_t end = find_stop(lines, position, stop);
                Function function;
                function.parameters = std::move(parameters);
                function.body.assign(lines.begin() + static_cast<std::ptrdiff_t>(position), lines.begin() + static_cast<std::ptrdiff_t>(end));
                functions[name] = std::move(function);
                position = end < lines.size() ? end + 1 : end;
            } else { ++position; }
        } else if (starts_with_ci(line, "end if") || starts_with_ci(line, "end function") || starts_with_ci(line, "end sub") || lowered == "fi" || lowered == "done" || lowered == "wend" || lowered == "loop" || lowered == "}" || starts_with_ci(line, "next")) {
            ++position;
        } else if (starts_with_ci(line, "elseif ") || starts_with_ci(line, "else if ") || lowered == "else" || starts_with_ci(line, "elif ") || lowered == "then" || lowered == "do") {
            ++position;
        } else if (starts_with_ci(line, "exit for") || starts_with_ci(line, "exit do")) {
            breaking = true;
            ++position;
        } else if (starts_with_ci(line, "exit function") || starts_with_ci(line, "exit sub")) {
            returned = true;
            ++position;
        } else if (starts_with_ci(line, "set ") || starts_with_ci(line, "var ") || starts_with_ci(line, "export ")) {
            // Handle set/var/export keyword: strip the keyword and process the rest as assignment
            const std::string rest = trim(line.substr(line.find_first_of(" \t") + 1));
            const size_t equals = rest.find('=');
            if (equals != std::string::npos && rest.find("==", equals) == std::string::npos && rest.find("!=", equals) == std::string::npos && rest.find("<=", equals) == std::string::npos && rest.find(">=", equals) == std::string::npos) {
                const std::string left = trim(rest.substr(0, equals));
                const std::string right = trim(rest.substr(equals + 1));
                if (left.find_first_of(" \t") == std::string::npos && !left.empty()) {
                    std::string value;
                    const auto expanded = tokenize(right);
                    for (size_t i = 0; i < expanded.size(); ++i) {
                        if (i) value.push_back(' ');
                        value += expanded[i];
                    }
                    if (right.find("$((") == std::string::npos && right.find_first_of("+-*/()") != std::string::npos) {
                        try { value = ExpressionParser(right).parse().text; } catch (...) {}
                    }
                    shell_global_vars[left] = value;
#ifdef _WIN32
                    _putenv((left + "=" + value).c_str());
#else
                    setenv(left.c_str(), value.c_str(), 1);
#endif
                    status = 0;
                } else status = execute_simple(line);
            } else status = execute_simple(line);
            ++position;
        } else {
            const size_t equals = line.find('=');
            if (starts_with_ci(line, "dim ")) {
                std::string names = trim(line.substr(4));
                std::istringstream input(names);
                std::string name;
                while (input >> name) {
                    if (name.back() == ',') name.pop_back();
                    if (!name.empty()) {
                        preserve_function_variable(name);
                        if (!shell_global_vars.count(name)) shell_global_vars[name] = "";
                    }
                }
                status = 0;
            } else if (equals != std::string::npos && line.find("==") == std::string::npos && line.find("!=") == std::string::npos && line.find("<=") == std::string::npos && line.find(">=") == std::string::npos) {
                const std::string left = trim(line.substr(0, equals));
                const std::string right = trim(line.substr(equals + 1));
                if (left.find_first_of(" \t") == std::string::npos && !left.empty()) {
                    std::string value;
                    const auto expanded = tokenize(right);
                    for (size_t i = 0; i < expanded.size(); ++i) {
                        if (i) value.push_back(' ');
                        value += expanded[i];
                    }
                    if (right.find("$((") == std::string::npos && right.find_first_of("+-*/()") != std::string::npos) {
                        try { value = ExpressionParser(right).parse().text; } catch (...) {}
                    }
                    shell_global_vars[left] = value;
                    status = 0;
                } else status = execute_simple(line);
            } else status = execute_simple(line);
            ++position;
        }
        last_status = status;
    }
    return status;
}
} // namespace

int run_script_text(const std::string& text, const std::vector<std::string>& arguments) {
    const auto saved_arguments = positional_arguments;
    positional_arguments = arguments;
    const std::vector<std::string> lines = split_script_lines(text);
    size_t position = 0;
    const int status = execute_lines(lines, position, {});
    positional_arguments = saved_arguments;
    return status;
}

int run_script_file(const std::string& path, const std::vector<std::string>& arguments) {
    std::string contents;
#ifdef _WIN32
    // Read file as UTF-8 using Windows API to avoid locale-dependent std::ifstream
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "DuckShell: cannot open script: " << path << '\n';
        return 127;
    }
    DWORD fileSizeHigh = 0;
    DWORD fileSize = GetFileSize(hFile, &fileSizeHigh);
    if (fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        std::cerr << "DuckShell: cannot read script: " << path << '\n';
        return 127;
    }
    std::vector<char> buffer(fileSize);
    DWORD bytesRead = 0;
    if (ReadFile(hFile, buffer.data(), fileSize, &bytesRead, nullptr) && bytesRead == fileSize) {
        // Check for UTF-8 BOM and skip it
        if (bytesRead >= 3 && buffer[0] == (char)0xEF && buffer[1] == (char)0xBB && buffer[2] == (char)0xBF) {
            contents.assign(buffer.data() + 3, bytesRead - 3);
        } else {
            contents.assign(buffer.data(), bytesRead);
        }
    }
    CloseHandle(hFile);
#else
    std::ifstream input(path);
    if (!input) {
        std::cerr << "DuckShell: cannot open script: " << path << '\n';
        return 127;
    }
    std::ostringstream oss;
    oss << input.rdbuf();
    contents = oss.str();
#endif
    return run_script_text(contents, arguments);
}

bool script_is_incomplete(const std::string& text) {
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quote) {
            if (ch == quote && (i == 0 || text[i - 1] != '\\')) quote = 0;
        } else if (ch == '\'' || ch == '"') quote = ch;
        else if (ch == '{' || ch == '(') ++depth;
        else if ((ch == '}' || ch == ')') && depth > 0) --depth;
    }
    if (quote || depth > 0) return true;
    const auto lines = split_script_lines(text);
    int blocks = 0;
    for (const auto& line : lines) {
        if (is_opener(line)) ++blocks;
        else if (is_closer(line) && blocks > 0) --blocks;
    }
    return blocks > 0 || (!text.empty() && text.back() == '\\');
}

bool shell_should_exit() { return exit_requested; }
int shell_exit_code() { return exit_code; }