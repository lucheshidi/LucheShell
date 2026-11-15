#include "header.h"
#include "plugins/plugin_manager.h"
#include "shell/shell_script.h"
#include "version.h"

#include <filesystem>

namespace {
std::string join_path(const std::string& base, const std::string& name) {
    return (std::filesystem::path(base) / name).string();
}
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // 启用 Windows 10+ 控制台的 ANSI 转义序列支持
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
            SetConsoleMode(hOut, dwMode);
        }
    }
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hErr, &dwMode)) {
            dwMode |= 0x0004;
            SetConsoleMode(hErr, dwMode);
        }
    }
#endif

    println("DuckShell " << DUCKSHELL_VERSION);
    // 调试信息输出
    //std::cout << "Home directory: " << home_dir << std::endl;
    //std::cout << "Current directory: " << dir_now << std::endl;

    // 构造duckshell配置目录和插件列表文件路径
    std::string duckshell_dir = home_dir + "/duckshell";
    std::string plugins_file = duckshell_dir + "/plugins.ls";
    std::string plugins_dir = duckshell_dir + "/plugins";

    // 检查并创建duckshell配置目录及插件文件
    struct stat info{};
    if (stat(duckshell_dir.c_str(), &info) != 0) {
#ifdef _WIN32
        // Windows系统下创建目录
        _mkdir(duckshell_dir.c_str());
        _mkdir(plugins_dir.c_str());
#else
        // Unix/Linux系统下创建目录，设置权限为0755
        mkdir(duckshell_dir.c_str(), 0755);
        mkdir(plugins_dir.c_str(), 0755);
#endif
        // 创建空的插件列表文件
        std::ofstream outfile(plugins_file);
        outfile << "// duckshell plugins list" << std::endl;
        outfile << "// Do not change this file!" << std::endl;
        outfile.close();

        // 以及仓库文件
        std::ofstream outfile_repo(duckshell_dir + "/repo.ls");
        outfile_repo << "// DuckShell repository list" << std::endl;
        outfile_repo << "// Do not change this file!" << std::endl;
    }
    else {
        // 目录已存在，检查插件列表文件和插件目录
        if (stat(plugins_file.c_str(), &info) != 0) {
            // 插件列表文件不存在，创建空文件
            std::ofstream outfile(plugins_file);
            outfile << "// DuckShell plugins list" << std::endl;
            outfile << "// Do not change this file!" << std::endl;
            outfile.close();

            // 以及仓库文件
            std::ofstream outfile_repo(duckshell_dir + "/repo.ls");
            outfile_repo << "// DuckShell repository list" << std::endl;
            outfile_repo << "// Do not change this file!" << std::endl;

        }

        if (stat(plugins_dir.c_str(), &info) != 0) {
#ifdef _WIN32
            // Windows系统下创建插件目录
            _mkdir(plugins_dir.c_str());
#else
            // Unix/Linux系统下创建插件目录，设置权限为0755
            mkdir(plugins_dir.c_str(), 0755);
#endif
        }
    }

    // 初始化插件系统
    PluginManager::loadPlugins();
    PluginManager::installAllPlugins(); // 扫描并安装所有插件
    PluginManager::buildCommandMap();   // 构建命令映射表

    bool no_profile = false;
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--norc") no_profile = true;
        else arguments.emplace_back(argv[i]);
    }

    const char* profile_override = std::getenv("DUCKSHELL_PROFILE");
    const std::string default_profile = join_path(join_path(home_dir, "duckshell"), "profile.dsh");
    const std::string profile = profile_override && *profile_override ? profile_override : default_profile;
    if (!no_profile && (!profile_override || !*profile_override) && !std::filesystem::exists(profile)) {
        std::ofstream profile_template(profile);
        profile_template << "# DuckShell startup profile\n"
                         << "# Uncomment and customize your prompt, for example:\n"
                         << "# set PROMPT = \"\\e[36mDUCKSHELL { \\w } > \\e[0m\"\n";
    }
    if (!no_profile && std::filesystem::exists(profile)) run_script_file(profile);

    if (!arguments.empty() && std::filesystem::is_regular_file(arguments.front())) {
        std::vector<std::string> script_arguments(arguments.begin() + 1, arguments.end());
        return run_script_file(arguments.front(), script_arguments);
    }
    if (arguments.empty()) {
        return startup();
    } else {
        std::string full_cmd;
        for (size_t i = 0; i < arguments.size(); ++i) {
            full_cmd += arguments[i];
            if (i + 1 < arguments.size()) full_cmd += " ";
        }
        return startup(full_cmd);
    }
}
