#include <algorithm>
#include <iomanip>
#include <cctype>
#include <deque>
#include <vector>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#include <conio.h>
#endif

#include <cstdint>
#include <regex>

#include "../header.h"
#include "shell_input.h"

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

// 声明全局变量用于命令历史记录
std::deque<std::string> command_history;
size_t history_index = 0;
bool g_input_eof = false;

namespace {
void write_output(const std::string& value) {
    std::cout.write(value.data(), static_cast<std::streamsize>(value.size()));
    std::cout.flush();
}

std::string move_cursor(char direction, size_t count) {
    return count ? "\033[" + std::to_string(count) + direction : std::string();
}

std::string visible_prompt_line(const std::string& prompt) {
    const size_t newline = prompt.find_last_of('\n');
    std::string line = newline == std::string::npos ? prompt : prompt.substr(newline + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::string redraw_line(const std::string& prompt, const std::string& buffer, size_t cursor_pos) {
    std::string output = "\033[?25l\r" + visible_prompt_line(prompt) + buffer + "\033[K";
    output += move_cursor('D', buffer.size() - cursor_pos);
    output += "\033[?25h";
    return output;
}

std::string redraw_after_delete(const std::string& buffer, size_t cursor_pos, bool backspace) {
    const std::string suffix = buffer.substr(cursor_pos);
    std::string output = backspace ? "\033[1D" : std::string();
    output += suffix + "\033[K" + move_cursor('D', suffix.size());
    return output;
}
}

// 交互式逐字符读取一行，支持在未回车时按下 Ctrl+L 清屏
std::string read_line_interactive(const std::string& prompt_shown) {
    std::string current_buffer;
    size_t cursor_pos = 0;  // 光标在缓冲区中的位置

#ifdef _WIN32
    // 如果不是交互式终端，或者正在调试器中运行，_getch() 可能无法工作
    // 我们检查 stdin 是否连接到控制台
    if (!_isatty(_fileno(stdin))) {
        std::string line;
        if (std::getline(std::cin, line)) {
            // 移除可能存在的 \r (Windows 下 getline 配合 \n 定界符时可能留下 \r)
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            return line;
        }
        g_input_eof = true;
        return "";
    }

    // Windows 实现
    // 清理缓冲区中残留的换行符
    while (_kbhit()) {
        (void)_getch();
    }

    // 初始打印 prompt
    std::cout << prompt_shown;
    std::cout.flush();

    for (;;) {
        int ch = _getch();

        // 处理 EOF 或错误
        if (ch == -1 || ch == 26 || ch == 4) {
            if (current_buffer.empty()) {
                g_input_eof = true;
                return "";
            }
            std::cout << "\n";
            break;
        }
        if (ch == 3) {
            std::cout << "^C\n";
            return "";
        }

        // 处理普通回车 (LF)
        if (ch == '\n') {
            std::cout << "\n";
            break;
        }

        // 特殊键处理 (功能键)
        if (ch == 0 || ch == 0xE0) {
            int ch2 = _getch(); // 获取第二个字节

            if (ch2 == 72) { // 上箭头键
                if (!command_history.empty() && history_index > 0) {
                    history_index--;
                    current_buffer = command_history[history_index];
                    cursor_pos = current_buffer.length();
                    write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
                }
                continue;
            }
            else if (ch2 == 80) { // 下箭头键
                if (!command_history.empty() && history_index < command_history.size()) {
                    history_index++;
                    if (history_index < command_history.size()) {
                        current_buffer = command_history[history_index];
                    } else {
                        current_buffer.clear();
                    }
                    cursor_pos = current_buffer.length();
                    write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
                }
                continue;
            }
            else if (ch2 == 75) { // 左箭头键
                if (cursor_pos > 0) {
                    cursor_pos--;
                    write_output(move_cursor('D', 1));
                }
                continue;
            }
            else if (ch2 == 77) { // 右箭头键
                if (cursor_pos < current_buffer.length()) {
                    cursor_pos++;
                    write_output(move_cursor('C', 1));
                }
                continue;
            }
            else if (ch2 == 71) { cursor_pos = 0; write_output(redraw_line(prompt_shown, current_buffer, cursor_pos)); }
            else if (ch2 == 79) { cursor_pos = current_buffer.size(); write_output(redraw_line(prompt_shown, current_buffer, cursor_pos)); }
            else if (ch2 == 83 && cursor_pos < current_buffer.size()) {
                current_buffer.erase(cursor_pos, 1);
                write_output(redraw_after_delete(current_buffer, cursor_pos, false));
            }
        }

        // 回车 (CR)
        if (ch == '\r') {
            std::cout << "\n";
            break;
        }

        // 处理退格键
        if (ch == 8 /* BS */ || ch == 127) {
            if (!current_buffer.empty() && cursor_pos > 0) {
                current_buffer.erase(cursor_pos - 1, 1);
                cursor_pos--;

                write_output(redraw_after_delete(current_buffer, cursor_pos, true));
            }
            continue;
        }

        // 处理 Ctrl+L (FF, 0x0C)
        if (ch == 12) {
            system("cls");
            write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
            continue;
        }

        // 忽略不可打印控制字符
        if (ch >= 32 && ch != 127) {
            const bool at_end = cursor_pos == current_buffer.size();
            current_buffer.insert(cursor_pos, 1, static_cast<char>(ch));
            cursor_pos++;
            if (at_end) write_output(std::string(1, static_cast<char>(ch)));
            else {
                const std::string suffix = current_buffer.substr(cursor_pos);
                write_output(std::string(1, static_cast<char>(ch)) + suffix + move_cursor('D', suffix.size()));
            }
        }
    }
#else
    // POSIX 版本
    if (!isatty(STDIN_FILENO)) {
        std::string line;
        if (std::getline(std::cin, line)) {
            return line;
        }
        g_input_eof = true;
        return "";
    }

    termios oldt{};
    termios newt{};
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ISIG); // 输入由 shell 处理，保留 ECHO 由我们自己输出
    newt.c_lflag &= ~(ECHO);
    newt.c_cc[VMIN] = 1;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // 初始打印 prompt
    std::cout << prompt_shown;
    std::cout.flush();

    for (;;) {
        unsigned char ch = 0;
        ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            g_input_eof = true;
            return "";
        }

        if (ch == 3) {
            std::cout << "^C\n";
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return "";
        }
        if (ch == 4 && current_buffer.empty()) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            g_input_eof = true;
            return "";
        }

        // ANSI 转义序列处理 (方向键)
        if (ch == 27) { // ESC
            unsigned char ch2 = 0;
            n = ::read(STDIN_FILENO, &ch2, 1);
            if (n > 0 && ch2 == '[') {
                unsigned char ch3 = 0;
                n = ::read(STDIN_FILENO, &ch3, 1);
                if (n > 0) {
                    if (ch3 == '3') {
                        unsigned char tail = 0;
                        if (::read(STDIN_FILENO, &tail, 1) > 0 && tail == '~' && cursor_pos < current_buffer.size()) {
                            current_buffer.erase(cursor_pos, 1);
                            write_output(redraw_after_delete(current_buffer, cursor_pos, false));
                        }
                        continue;
                    }
                    if (ch3 == 'H' || ch3 == 'F') {
                        cursor_pos = ch3 == 'H' ? 0 : current_buffer.size();
                        write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
                        continue;
                    }
                    if (ch3 == 'A') { // 上箭头
                        if (!command_history.empty() && history_index > 0) {
                            history_index--;
                            current_buffer = command_history[history_index];
                            cursor_pos = current_buffer.length();
                            write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
                        }
                        continue;
                    }
                    else if (ch3 == 'B') { // 下箭头
                        if (!command_history.empty() && history_index < command_history.size()) {
                            history_index++;
                            if (history_index < command_history.size()) {
                                current_buffer = command_history[history_index];
                            } else {
                                current_buffer.clear();
                            }
                            cursor_pos = current_buffer.length();
                            write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
                        }
                        continue;
                    }
                    else if (ch3 == 'D') { // 左箭头
                        if (cursor_pos > 0) {
                            cursor_pos--;
                            write_output(move_cursor('D', 1));
                        }
                        continue;
                    }
                    else if (ch3 == 'C') { // 右箭头
                        if (cursor_pos < current_buffer.length()) {
                            cursor_pos++;
                            write_output(move_cursor('C', 1));
                        }
                        continue;
                    }
                }
            }
        }

        if (ch == '\n' || ch == '\r') {
            std::cout << "\n";
            break;
        }

        if (ch == 8 || ch == 127) { // backspace/delete
            if (!current_buffer.empty() && cursor_pos > 0) {
                current_buffer.erase(cursor_pos - 1, 1);
                cursor_pos--;
                write_output(redraw_after_delete(current_buffer, cursor_pos, true));
            }
            continue;
        }

        if (ch == 12) { // Ctrl+L
#ifdef _WIN32
            if (system("cls") != 0) {
                // 忽略错误
            }
#else
            if (system("clear") != 0) {
                // 忽略错误
            }
#endif
            write_output(redraw_line(prompt_shown, current_buffer, cursor_pos));
            continue;
        }

        if (ch >= 32 && ch != 127) {
            const bool at_end = cursor_pos == current_buffer.size();
            current_buffer.insert(cursor_pos, 1, static_cast<char>(ch));
            cursor_pos++;
            if (at_end) write_output(std::string(1, static_cast<char>(ch)));
            else {
                const std::string suffix = current_buffer.substr(cursor_pos);
                write_output(std::string(1, static_cast<char>(ch)) + suffix + move_cursor('D', suffix.size()));
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

    return current_buffer;
}