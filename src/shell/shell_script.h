#ifndef SHELL_SCRIPT_H
#define SHELL_SCRIPT_H

#include <string>
#include <vector>

int run_script_text(const std::string& text, const std::vector<std::string>& arguments = {});
int run_script_file(const std::string& path, const std::vector<std::string>& arguments = {});
bool script_is_incomplete(const std::string& text);
bool shell_should_exit();
int shell_exit_code();
void print_version();

#endif