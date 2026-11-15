#ifndef SHELL_COMMANDS_H
#define SHELL_COMMANDS_H

#include <string>
#include <vector>

int execute_command(const std::string& input);
int run_builtin_command(const std::vector<std::string>& args);
std::vector<std::string> split(const std::string& str, char delimiter);

#endif // SHELL_COMMANDS_H