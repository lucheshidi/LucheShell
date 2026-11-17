#include <algorithm>
#include <iomanip>
#include <cctype>
#include <deque>
#include <vector>
#include <sstream>
#include <cstdint>
#include <regex>
#include <ctime>
#include <cstdlib>

#include "../header.h"
#include "shell_commands.h"
#include "shell_script.h"
#include "shell_input.h"
#include "color_codes.h"
#include "../plugins/plugin_manager.h"
#include "../plugins/plugins_interface.h"
#include <deque>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
std::string expand_prompt(std::string prompt) {
    std::string result;
    const char* user_value = std::getenv("USERNAME");
    if (!user_value) user_value = std::getenv("USER");
    const std::string user = user_value ? user_value : "user";
    char host_buffer[256]{};
#ifdef _WIN32
    DWORD host_size = sizeof(host_buffer);
    GetComputerNameA(host_buffer, &host_size);
#else
    gethostname(host_buffer, sizeof(host_buffer) - 1);
#endif
    const std::string host = host_buffer[0] ? host_buffer : "host";
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char time_buffer[32]{};
    std::strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", &local_time);
    char date_buffer[64]{};
    std::strftime(date_buffer, sizeof(date_buffer), "%a %b %d", &local_time);
    std::string short_dir = dir_now;
    const size_t separator = short_dir.find_last_of("/\\");
    if (separator != std::string::npos && separator + 1 < short_dir.size()) short_dir = short_dir.substr(separator + 1);

    for (size_t i = 0; i < prompt.size(); ++i) {
        if (prompt[i] == '$') {
            size_t end = i + 1;
            std::string name;
            if (end < prompt.size() && prompt[end] == '{') {
                const size_t close = prompt.find('}', end + 1);
                if (close != std::string::npos) {
                    name = prompt.substr(end + 1, close - end - 1);
                    i = close;
                }
            } else {
                while (end < prompt.size() && (std::isalnum(static_cast<unsigned char>(prompt[end])) || prompt[end] == '_')) ++end;
                if (end > i + 1) {
                    name = prompt.substr(i + 1, end - i - 1);
                    i = end - 1;
                }
            }
            if (!name.empty()) {
                const auto found = shell_global_vars.find(name);
                if (found != shell_global_vars.end()) result += found->second;
                else if (const char* value = std::getenv(name.c_str())) result += value;
                continue;
            }
            result.push_back('$');
            continue;
        }
        if (prompt[i] == '\\' && i + 1 < prompt.size()) {
            const char ch = prompt[++i];
            switch (ch) {
                case 'u': result += user; break;
                case 'h': result += host; break;
                case 'w': result += dir_now; break;
                case 'W': result += short_dir; break;
                case 't': result += time_buffer; break;
                case 'd': result += date_buffer; break;
                case '$': result += '$'; break;
                case 'n': result += '\n'; break;
                case 'e': result += '\033'; break;
                case '[': case ']': break;
                case '0':
                    if (i + 2 < prompt.size() && prompt.compare(i, 3, "033") == 0) {
                        result.push_back('\033');
                        i += 2;
                    } else { result.push_back('\\'); result.push_back(ch); }
                    break;
                default: result.push_back('\\'); result.push_back(ch); break;
            }
        } else result.push_back(prompt[i]);
    }
    return translate_color_codes(result);
}

std::string get_prompt() {
    std::string selected;
    auto prompt = shell_global_vars.find("PROMPT");
    if (prompt == shell_global_vars.end()) prompt = shell_global_vars.find("PS1");
    if (prompt != shell_global_vars.end()) selected = prompt->second;
    if (selected.empty()) {
        const char* environment = std::getenv("PROMPT");
        if (!environment || !*environment) environment = std::getenv("PS1");
        if (environment) selected = environment;
    }
    if (!selected.empty()) return expand_prompt(selected);
    for (auto plugin : loaded_plugin_instances) {
        const std::string plugin_prompt = plugin->get_prompt();
        if (!plugin_prompt.empty()) return translate_color_codes(plugin_prompt);
    }
    return translate_color_codes(std::string("DUCKSHELL { ") + dir_now + " }> ");
}
}

/**
 * The startup function for DuckShell includes no input parameter and yes parameters and returns an exit code.
 * @param param
 * @return Exit Code
 */
int startup(const std::string &param) {
    if (param.empty()) {
        while (!shell_should_exit()) {
            std::string command = read_line_interactive(get_prompt());
            if (g_input_eof) break;
            while (script_is_incomplete(command)) {
                const auto secondary = shell_global_vars.find("PS2");
                const std::string continuation_prompt = secondary == shell_global_vars.end() ? "> " : expand_prompt(secondary->second);
                const std::string next_line = read_line_interactive(continuation_prompt);
                if (g_input_eof) break;
                command += "\n" + next_line;
            }
            if (g_input_eof) break;

            // 添加命令到历史记录（除非是空命令或与上一条相同）
            if (!command.empty() && (command_history.empty() || command_history.back() != command)) {
                command_history.push_back(command);
                if (command_history.size() > 100) {
                    command_history.pop_front();
                }
            }
            history_index = command_history.size();

            if (command == "quit") {
                break;
            }

            if (!command.empty()) {
                execute_command(command);
                std::cout.flush();
                std::cerr.flush();
            }
        }
        return shell_should_exit() ? shell_exit_code() : 0;
    }
    return execute_command(param);
}