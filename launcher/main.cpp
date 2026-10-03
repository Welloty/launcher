#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// Configuration
const std::string REPO_OWNER = "Welloty";
const std::string REPO_NAME = "fireline";
const std::string CURRENT_VERSION_FILE = "version.txt";
const std::string TEMP_ZIP = "update_temp.zip";
const std::string GAME_DIR = "game";
const std::string GAME_EXE = "game\\Fireline.exe";

// Read local version from file
std::string get_local_version() {
    if (!fs::exists(CURRENT_VERSION_FILE)) {
        return "v0.0.0";
    }
    std::ifstream file(CURRENT_VERSION_FILE);
    std::string version;
    file >> version;
    return version.empty() ? "v0.0.0" : version;
}

// Write local version to file
void save_local_version(const std::string& version) {
    std::ofstream file(CURRENT_VERSION_FILE);
    file << version;
}

// Download file from GitHub release
bool download_file(const std::string& host, const std::string& path, const std::string& output_filename) {
    httplib::Client cli("https://" + host);
    cli.set_follow_location(true);

    std::ofstream ofs(output_filename, std::ios::binary);
    if (!ofs.is_open()) return false;

    auto res = cli.Get(path.c_str(), httplib::Headers{
        {"User-Agent", "CPP-Game-Launcher"}
        }, [&](const char* data, size_t data_length) {
            ofs.write(data, data_length);
            return true;
        });

    return res && res->status == 200;
}

// Unpackaging the zip file
bool extract_zip(const std::string& zip_path, const std::string& target_dir) {
    std::cout << "[INFO] Распаковка обновлений..." << std::endl;
#ifdef _WIN32
    std::string cmd = "powershell -NoProfile -Command \"Expand-Archive -Path '" +
        zip_path + "' -DestinationPath '" + target_dir + "' -Force\"";
    int result = std::system(cmd.c_str());
    return result == 0;
#else
    std::string cmd = "unzip -o \"" + zip_path + "\" -d \"" + target_dir + "\"";
    int result = std::system(cmd.c_str());
    return result == 0;
#endif
}

// Check and perform update from GitHub
void check_and_update() {
    std::string local_version = get_local_version();
    std::cout << "\n[INFO] Текущая версия игры: " << local_version << std::endl;

    httplib::Client cli("https://api.github.com");
    httplib::Headers headers = {
        {"User-Agent", "CPP-Game-Launcher"},
        {"Accept", "application/vnd.github.v3+json"}
    };

    std::string api_path = "/repos/" + REPO_OWNER + "/" + REPO_NAME + "/releases/latest";
    auto res = cli.Get(api_path.c_str(), headers);

    if (!res || res->status != 200) {
        std::cerr << "[WARN] Не удалось проверить обновления на GitHub." << std::endl;
        return;
    }

    try {
        json release_info = json::parse(res->body);
        std::string latest_version = release_info["tag_name"].get<std::string>();
        std::cout << "[INFO] Последняя версия на GitHub: " << latest_version << std::endl;

        if (latest_version != local_version) {
            std::cout << "[INFO] Найдено новое обновление! Скачивание..." << std::endl;

            std::string download_url = "";
            for (const auto& asset : release_info["assets"]) {
                std::string name = asset["name"].get<std::string>();
                if (name.rfind(".zip") != std::string::npos) {
                    download_url = asset["browser_download_url"].get<std::string>();
                    break;
                }
            }

            if (download_url.empty()) {
                std::cerr << "[ERROR] В релизе не найден .zip файл!" << std::endl;
                return;
            }

            std::string url_prefix = "https://";
            std::string clean_url = download_url.substr(url_prefix.length());
            size_t slash_pos = clean_url.find('/');
            std::string host = clean_url.substr(0, slash_pos);
            std::string path = clean_url.substr(slash_pos);

            if (download_file(host, path, TEMP_ZIP)) {
                std::cout << "[INFO] Файл успешно скачан." << std::endl;

                if (extract_zip(TEMP_ZIP, GAME_DIR)) {
                    save_local_version(latest_version);
                    std::cout << "[SUCCESS] Игра успешно обновлена до версии " << latest_version << "!" << std::endl;
                }
                else {
                    std::cerr << "[ERROR] Ошибка при распаковке архива." << std::endl;
                }

                fs::remove(TEMP_ZIP);
            }
            else {
                std::cerr << "[ERROR] Ошибка при скачивании обновления." << std::endl;
            }
        }
        else {
            std::cout << "[INFO] У вас установлена последняя версия." << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Ошибка обработки ответа GitHub API: " << e.what() << std::endl;
    }
}

// Start game executable with specified working directory
void launch_game(const std::string& exe_path, const std::string& work_dir) {
    std::cout << "[INFO] Запуск игры: " << exe_path << "..." << std::endl;
#ifdef _WIN32
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    LPCSTR working_directory = work_dir.empty() ? NULL : work_dir.c_str();

    if (CreateProcessA(exe_path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, working_directory, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else {
        std::cerr << "[ERROR] Не удалось запустить " << exe_path << std::endl;
    }
#else
    std::string cmd = "./" + exe_path + " &";
    std::system(cmd.c_str());
#endif
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::cout << "===========================" << std::endl;
    std::cout << "       Launcher v1.0    " << std::endl;
    std::cout << "===========================" << std::endl;

    // Автоматическая проверка обновлений при старте
    check_and_update();

    bool running = true;
    while (running) {
        std::cout << "\n---------------------------" << std::endl;
        std::cout << "1. Запуск игры" << std::endl;
        std::cout << "2. Проверка обновлений" << std::endl;
        std::cout << "3. Выход" << std::endl;
        std::cout << "Выберите действие (1-3): ";

        int choice = 0;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            std::cout << "[WARN] Пожалуйста, введите число 1, 2 или 3." << std::endl;
            continue;
        }

        switch (choice) {
        case 1:
            launch_game(GAME_EXE, GAME_DIR);
            break;
        case 2:
            check_and_update();
            break;
        case 3:
            std::cout << "Выход из лаунчера..." << std::endl;
            running = false;
            break;
        default:
            std::cout << "[WARN] Неверный выбор! Введите 1, 2 или 3." << std::endl;
            break;
        }
    }

    return 0;
}