#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <wrl/event.h>
#include "WebView2.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"
#include "miniz.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <memory>

using namespace Microsoft::WRL;
namespace fs = std::filesystem;
using json = nlohmann::json;

// Custom Windows message for UI thread dispatching
#define WM_LAUNCHER_DISPATCH (WM_USER + 101)

// Globals
HWND g_hWnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;

std::mutex g_dispatchMutex;
std::vector<std::function<void()>> g_dispatchQueue;

const std::string REPO_OWNER = "Welloty";
const std::string REPO_NAME = "fireline";

std::atomic<bool> g_isGameRunning = false;
std::atomic<bool> g_isUpdating = false;
std::atomic<bool> g_isChecking = false;
std::atomic<bool> g_cancelUpdate = false;

struct ReleaseData {
    std::string tagName;
    std::string title;
    std::string body;
    std::string publishedAt;
    std::string downloadUrl;
    size_t assetSize = 0;
    bool hasZip = false;
};

ReleaseData g_latestRelease;
std::mutex g_releaseMutex;

// Forward declarations
void SetupWebViewEvents();
void HandleWebAction(const std::string& action, const json& payload);
void check_updates_async(bool isUserInitiated);
void start_update_async();
void launch_game_async();

// App directories
fs::path get_app_dir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
}

// Debug logging helper
void LogDebug(const std::string& msg) {
    try {
        fs::path logPath = get_app_dir() / "launcher_debug.log";
        std::ofstream f(logPath, std::ios::app);
        if (f.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            f << "[" << time_t_now << "] " << msg << std::endl;
        }
    } catch (...) {}
}

// Dispatch task to UI thread
void DispatchToUI(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        g_dispatchQueue.push_back(task);
    }
    if (g_hWnd) {
        PostMessageW(g_hWnd, WM_LAUNCHER_DISPATCH, 0, 0);
    }
}

// Convert filesystem path to valid UTF-8 string
std::string path_to_utf8(const fs::path& p) {
    std::wstring ws = p.wstring();
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string u8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &u8[0], len, nullptr, nullptr);
    return u8;
}

// Send JSON event to WebView frontend
void SendWebEvent(const std::string& event, const json& data) {
    DispatchToUI([event, data]() {
        if (!g_webview) return;
        try {
            json msg = {
                {"event", event},
                {"data", data}
            };
            std::string s = msg.dump(-1, ' ', false, json::error_handler_t::replace);
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            if (len <= 0) return;
            std::wstring ws(len, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
            if (!ws.empty() && ws.back() == L'\0') ws.pop_back();

            g_webview->PostWebMessageAsJson(ws.c_str());
        } catch (const std::exception& e) {
            LogDebug("SendWebEvent exception: " + std::string(e.what()));
        } catch (...) {
            LogDebug("SendWebEvent unknown exception");
        }
    });
}

fs::path get_version_file() {
    return get_app_dir() / "version.txt";
}

fs::path get_game_dir() {
    return get_app_dir() / "game";
}

fs::path get_game_exe() {
    return get_app_dir() / "game" / "Fireline.exe";
}

fs::path get_temp_zip() {
    return get_app_dir() / "update_temp.zip";
}

fs::path get_ui_dir() {
    std::error_code ec;
    fs::path dir = get_app_dir() / "ui";
    if (fs::exists(dir / "index.html", ec)) return dir;

    try {
        fs::path devDir = get_app_dir().parent_path().parent_path() / "launcher" / "ui";
        if (fs::exists(devDir / "index.html", ec)) return devDir;

        fs::path rootUi = get_app_dir().parent_path() / "ui";
        if (fs::exists(rootUi / "index.html", ec)) return rootUi;
    } catch (...) {}

    return dir;
}

fs::path get_webview_data_dir() {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        fs::path p = fs::path(localAppData) / "FirelineLauncher" / "WebView2";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
    return get_app_dir() / "WebView2Data";
}

std::string get_local_version() {
    fs::path vfile = get_version_file();
    std::error_code ec;
    if (!fs::exists(vfile, ec)) {
        return "v0.0.0";
    }
    std::ifstream file(vfile);
    std::string v;
    file >> v;
    return v.empty() ? "v0.0.0" : v;
}

void save_local_version(const std::string& version) {
    std::ofstream file(get_version_file());
    file << version;
}

// URL parsing helper
bool parse_https_url(const std::string& url, std::string& host, std::string& path) {
    const std::string prefix = "https://";
    if (url.rfind(prefix, 0) != 0) return false;
    std::string rest = url.substr(prefix.length());
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    } else {
        host = rest;
        path = "/";
    }
    return true;
}

// Download file with redirect handling and progress tracking
bool download_url_with_progress(const std::string& initial_url, const fs::path& output_path,
    std::function<void(size_t downloaded, size_t total, double speedMbS)> progress_cb) {
    std::string current_url = initial_url;
    int max_redirects = 6;

    while (max_redirects-- > 0 && !g_cancelUpdate) {
        std::string host, path;
        if (!parse_https_url(current_url, host, path)) {
            return false;
        }

        httplib::Client cli("https://" + host);
        cli.set_follow_location(false);
        cli.set_connection_timeout(15, 0);
        cli.set_read_timeout(30, 0);

        std::ofstream ofs(output_path, std::ios::binary);
        if (!ofs.is_open()) return false;

        size_t downloaded = 0;
        size_t total_size = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_cb_time = start_time;

        auto res = cli.Get(path.c_str(), httplib::Headers{
            {"User-Agent", "Fireline-Launcher"}
        },
        [&](const httplib::Response& response) {
            if (response.has_header("Content-Length")) {
                try {
                    total_size = std::stoull(response.get_header_value("Content-Length"));
                } catch (...) {}
            }
            return true;
        },
        [&](const char* data, size_t data_length) {
            if (g_cancelUpdate) return false;
            ofs.write(data, data_length);
            downloaded += data_length;

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cb_time).count();
            if (elapsed > 100) {
                last_cb_time = now;
                double total_sec = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count() / 1000.0;
                double speed = (total_sec > 0.05) ? ((double)downloaded / (1024.0 * 1024.0)) / total_sec : 0.0;
                if (progress_cb) progress_cb(downloaded, total_size, speed);
            }
            return true;
        });

        ofs.close();

        if (g_cancelUpdate) {
            std::error_code ec;
            fs::remove(output_path, ec);
            return false;
        }

        if (res) {
            if (res->status == 301 || res->status == 302 || res->status == 307 || res->status == 308) {
                if (res->has_header("Location")) {
                    current_url = res->get_header_value("Location");
                    continue;
                }
            }
            if (res->status == 200) {
                if (progress_cb && total_size > 0) {
                    double total_sec = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() / 1000.0;
                    double speed = (total_sec > 0.05) ? ((double)downloaded / (1024.0 * 1024.0)) / total_sec : 0.0;
                    progress_cb(downloaded, total_size, speed);
                }
                return true;
            }
        }
        break;
    }
    return false;
}

// In-process ZIP extraction with file-by-file progress
bool extract_zip_miniz(const fs::path& zip_fs_path, const fs::path& target_dir,
    std::function<void(int percent, int current, int total, const std::string& filename)> progress_cb) {
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    std::string zip_path_utf8 = path_to_utf8(zip_fs_path);
    if (!mz_zip_reader_init_file(&zip_archive, zip_path_utf8.c_str(), 0)) {
        return false;
    }

    mz_uint num_files = mz_zip_reader_get_num_files(&zip_archive);
    auto last_update = std::chrono::steady_clock::now();

    for (mz_uint i = 0; i < num_files; ++i) {
        if (g_cancelUpdate) {
            mz_zip_reader_end(&zip_archive);
            return false;
        }

        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat)) continue;

        fs::path relPath(reinterpret_cast<const char8_t*>(file_stat.m_filename));
        fs::path destPath = target_dir / relPath;

        if (mz_zip_reader_is_file_a_directory(&zip_archive, i)) {
            std::error_code ec;
            fs::create_directories(destPath, ec);
        } else {
            std::error_code ec;
            fs::create_directories(destPath.parent_path(), ec);
            std::string destUtf8 = path_to_utf8(destPath);
            mz_zip_reader_extract_to_file(&zip_archive, i, destUtf8.c_str(), 0);
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count() > 70 || i == num_files - 1) {
            last_update = now;
            if (progress_cb) {
                int percent = (int)(((i + 1) * 100) / (num_files > 0 ? num_files : 1));
                progress_cb(percent, (int)(i + 1), (int)num_files, file_stat.m_filename);
            }
        }
    }

    mz_zip_reader_end(&zip_archive);
    return true;
}

// GitHub check
void check_updates_async(bool isUserInitiated) {
    if (g_isChecking) return;
    g_isChecking = true;
    LogDebug("check_updates_async: started");

    std::thread([isUserInitiated]() {
        try {
            LogDebug("check_updates thread: creating client");
            httplib::Client cli("https://api.github.com");
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(15, 0);
            httplib::Headers headers = {
                {"User-Agent", "Fireline-Launcher"},
                {"Accept", "application/vnd.github.v3+json"}
            };

            std::string api_path = "/repos/" + REPO_OWNER + "/" + REPO_NAME + "/releases/latest";
            LogDebug("check_updates thread: GET " + api_path);
            auto res = cli.Get(api_path.c_str(), headers);
            LogDebug("check_updates thread: response = " + (res ? std::to_string(res->status) : "null"));

            g_isChecking = false;

            if (!res || res->status != 200) {
                std::string errMsg = "Не удалось проверить обновления на GitHub (код " + (res ? std::to_string(res->status) : "timeout") + ").";
                SendWebEvent("ERROR", {{"message", errMsg}});
                return;
            }

            json release_info = json::parse(res->body);
            ReleaseData rd;
            rd.tagName = release_info.value("tag_name", "");
            rd.title = release_info.value("name", rd.tagName);
            rd.body = release_info.value("body", "");
            rd.publishedAt = release_info.value("published_at", "");

            if (release_info.contains("assets") && release_info["assets"].is_array()) {
                for (const auto& asset : release_info["assets"]) {
                    std::string name = asset.value("name", "");
                    if (name.rfind(".zip") != std::string::npos) {
                        rd.downloadUrl = asset.value("browser_download_url", "");
                        rd.assetSize = asset.value("size", (size_t)0);
                        rd.hasZip = true;
                        break;
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_releaseMutex);
                g_latestRelease = rd;
            }

            std::string localVer = get_local_version();
            std::error_code ec;
            bool gameExists = fs::exists(get_game_exe(), ec);
            LogDebug("check_updates thread: localVer=" + localVer + ", latest=" + rd.tagName + ", gameExists=" + std::to_string(gameExists));

            if (!gameExists || rd.tagName != localVer) {
                SendWebEvent("UPDATE_AVAILABLE", {
                    {"latestVersion", rd.tagName},
                    {"title", rd.title},
                    {"body", rd.body},
                    {"publishedAt", rd.publishedAt},
                    {"assetSize", rd.assetSize},
                    {"isGameInstalled", gameExists}
                });
            } else {
                SendWebEvent("UPDATE_NOT_FOUND", {
                    {"currentVersion", localVer},
                    {"latestVersion", rd.tagName},
                    {"title", rd.title},
                    {"body", rd.body},
                    {"publishedAt", rd.publishedAt}
                });
            }
        } catch (const std::exception& e) {
            g_isChecking = false;
            LogDebug("check_updates thread exception: " + std::string(e.what()));
            SendWebEvent("ERROR", {{"message", std::string("Ошибка парсинга: ") + e.what()}});
        } catch (...) {
            g_isChecking = false;
            LogDebug("check_updates thread unknown exception");
            SendWebEvent("ERROR", {{"message", "Неизвестная ошибка при проверке релиза."}});
        }
    }).detach();
}

// Download & Install
void start_update_async() {
    if (g_isUpdating) return;
    g_isUpdating = true;
    g_cancelUpdate = false;
    LogDebug("start_update_async: started");

    std::thread([]() {
        try {
            ReleaseData rd;
            {
                std::lock_guard<std::mutex> lock(g_releaseMutex);
                rd = g_latestRelease;
            }

            if (!rd.hasZip || rd.downloadUrl.empty()) {
                SendWebEvent("ERROR", {{"message", "В релизе GitHub не найден zip-архив."}});
                g_isUpdating = false;
                return;
            }

            fs::path tempZip = get_temp_zip();
            SendWebEvent("STATUS_CHANGED", {
                {"status", "DOWNLOADING"},
                {"headline", "Загрузка обновления..."},
                {"detail", "Подготовка к скачиванию"}
            });

            // 1. Download
            bool downloadOk = download_url_with_progress(rd.downloadUrl, tempZip, [](size_t dl, size_t total, double speed) {
                int percent = (total > 0) ? (int)((dl * 100) / total) : 0;
                SendWebEvent("DOWNLOAD_PROGRESS", {
                    {"percent", percent},
                    {"downloadedBytes", dl},
                    {"totalBytes", total},
                    {"speedMbS", speed}
                });
            });

            if (g_cancelUpdate) {
                std::error_code ec;
                fs::remove(tempZip, ec);
                g_isUpdating = false;
                std::error_code ec2;
                SendWebEvent("INIT_STATE", {
                    {"localVersion", get_local_version()},
                    {"isGameInstalled", fs::exists(get_game_exe(), ec2)},
                    {"isGameRunning", g_isGameRunning.load()},
                    {"gamePath", path_to_utf8(get_game_dir())}
                });
                return;
            }

            if (!downloadOk || !fs::exists(tempZip)) {
                SendWebEvent("ERROR", {{"message", "Ошибка при скачивании обновления."}});
                g_isUpdating = false;
                return;
            }

            // 2. Extract
            SendWebEvent("STATUS_CHANGED", {
                {"status", "EXTRACTING"},
                {"headline", "Установка файлов..."},
                {"detail", "Распаковка архива"}
            });

            fs::path gameDir = get_game_dir();
            std::error_code ec;
            fs::create_directories(gameDir, ec);

            bool extractOk = extract_zip_miniz(tempZip, gameDir, [](int percent, int current, int total, const std::string& filename) {
                SendWebEvent("EXTRACT_PROGRESS", {
                    {"percent", percent},
                    {"extractedFiles", current},
                    {"totalFiles", total},
                    {"currentFile", filename}
                });
            });

            fs::remove(tempZip, ec);

            if (g_cancelUpdate) {
                g_isUpdating = false;
                return;
            }

            if (!extractOk) {
                SendWebEvent("ERROR", {{"message", "Ошибка при распаковке архива игры."}});
                g_isUpdating = false;
                return;
            }

            // 3. Save version
            save_local_version(rd.tagName);
            g_isUpdating = false;

            SendWebEvent("UPDATE_COMPLETE", {
                {"version", rd.tagName}
            });
        } catch (const std::exception& e) {
            g_isUpdating = false;
            LogDebug("start_update exception: " + std::string(e.what()));
            SendWebEvent("ERROR", {{"message", std::string("Ошибка обновления: ") + e.what()}});
        } catch (...) {
            g_isUpdating = false;
            LogDebug("start_update unknown exception");
            SendWebEvent("ERROR", {{"message", "Неизвестная ошибка при обновлении."}});
        }
    }).detach();
}

// Game launch and process monitoring
void launch_game_async() {
    fs::path exePath = get_game_exe();
    fs::path workDir = get_game_dir();

    std::error_code ec;
    if (!fs::exists(exePath, ec)) {
        SendWebEvent("ERROR", {{"message", "Файл игры Fireline.exe не найден."}});
        return;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    std::wstring exeStr = exePath.wstring();
    std::wstring dirStr = workDir.wstring();

    if (CreateProcessW(exeStr.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dirStr.c_str(), &si, &pi)) {
        g_isGameRunning = true;
        SendWebEvent("GAME_STARTED", {});

        HANDLE hProc = pi.hProcess;
        CloseHandle(pi.hThread);

        std::thread([hProc]() {
            WaitForSingleObject(hProc, INFINITE);
            CloseHandle(hProc);
            g_isGameRunning = false;
            SendWebEvent("GAME_STOPPED", {});
        }).detach();
    } else {
        SendWebEvent("ERROR", {{"message", "Не удалось запустить процесс Fireline.exe."}});
    }
}

// Handle message from Web
void HandleWebAction(const std::string& action, const json& payload) {
    try {
        LogDebug("HandleWebAction: " + action);
        if (action == "WINDOW_DRAG") {
            ReleaseCapture();
            SendMessageW(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        else if (action == "WINDOW_MINIMIZE") {
            ShowWindow(g_hWnd, SW_MINIMIZE);
        }
        else if (action == "WINDOW_CLOSE") {
            PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
        }
        else if (action == "OPEN_FOLDER") {
            fs::path gdir = get_game_dir();
            std::error_code ec;
            fs::create_directories(gdir, ec);
            ShellExecuteW(nullptr, L"open", gdir.wstring().c_str(), nullptr, nullptr, SW_SHOW);
        }
        else if (action == "OPEN_LINK") {
            std::string url = payload.value("url", "https://github.com/Welloty/fireline");
            int len = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
            if (len > 0) {
                std::wstring ws(len, 0);
                MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &ws[0], len);
                ShellExecuteW(nullptr, L"open", ws.c_str(), nullptr, nullptr, SW_SHOW);
            }
        }
        else if (action == "UI_READY") {
            std::error_code ec;
            SendWebEvent("INIT_STATE", {
                {"localVersion", get_local_version()},
                {"isGameInstalled", fs::exists(get_game_exe(), ec)},
                {"isGameRunning", g_isGameRunning.load()},
                {"gamePath", path_to_utf8(get_game_dir())}
            });
            check_updates_async(false);
        }
        else if (action == "CHECK_UPDATE") {
            check_updates_async(true);
        }
        else if (action == "START_UPDATE") {
            start_update_async();
        }
        else if (action == "CANCEL_UPDATE") {
            g_cancelUpdate = true;
        }
        else if (action == "LAUNCH_GAME") {
            launch_game_async();
        }
    } catch (const std::exception& e) {
        LogDebug("HandleWebAction exception: " + std::string(e.what()));
    } catch (...) {
        LogDebug("HandleWebAction unknown exception");
    }
}

// Setup IPC Events
void SetupWebViewEvents() {
    if (!g_webview) return;
    EventRegistrationToken token;
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR messageRaw = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&messageRaw)) && messageRaw) {
                    int size_needed = WideCharToMultiByte(CP_UTF8, 0, messageRaw, -1, NULL, 0, NULL, NULL);
                    std::string jsonStr(size_needed, 0);
                    WideCharToMultiByte(CP_UTF8, 0, messageRaw, -1, &jsonStr[0], size_needed, NULL, NULL);
                    CoTaskMemFree(messageRaw);
                    if (!jsonStr.empty() && jsonStr.back() == '\0') jsonStr.pop_back();

                    try {
                        json j = json::parse(jsonStr);
                        std::string action = j.value("action", "");
                        json payload = j.value("payload", json::object());
                        HandleWebAction(action, payload);
                    } catch (const std::exception& e) {
                        LogDebug("WebMessage parse error: " + std::string(e.what()));
                    } catch (...) {
                        LogDebug("WebMessage parse unknown error");
                    }
                }
                return S_OK;
            }
        ).Get(), &token
    );
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_controller) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        return 0;

    case WM_LAUNCHER_DISPATCH: {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(g_dispatchMutex);
            tasks.swap(g_dispatchQueue);
        }
        for (auto& task : tasks) {
            if (task) {
                try {
                    task();
                } catch (const std::exception& e) {
                    LogDebug("Dispatch task exception: " + std::string(e.what()));
                } catch (...) {
                    LogDebug("Dispatch task unknown exception");
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// Entry Point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    LogDebug("=== wWinMain start ===");

    // Initialize COM (STA is required for WebView2)
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogDebug("CoInitializeEx returned: " + std::to_string(hrCom));

    // Enable DPI awareness
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        LogDebug("DPI awareness set successfully");
    } catch (...) {
        LogDebug("SetProcessDpiAwarenessContext ignored");
    }

    // Register Window Class
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = L"FirelineLauncherClass";

    if (!RegisterClassExW(&wcex)) {
        LogDebug("RegisterClassExW failed");
        return 1;
    }

    // Window dimensions
    const int windowWidth = 980;
    const int windowHeight = 620;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowWidth) / 2;
    int posY = (screenH - windowHeight) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"FirelineLauncherClass",
        L"Fireline Launcher",
        WS_POPUP | WS_MINIMIZEBOX,
        posX, posY, windowWidth, windowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hWnd) {
        LogDebug("CreateWindowExW failed");
        return 1;
    }

    g_hWnd = hWnd;
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    LogDebug("Window created and shown");

    // Initialize WebView2
    fs::path userDataFolder = get_webview_data_dir();
    LogDebug("userDataFolder: " + userDataFolder.string());

    HRESULT hrEnv = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.wstring().c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                LogDebug("Environment callback: result = " + std::to_string(result));
                if (FAILED(result) || !env) {
                    LogDebug("Failed to create environment: hr = " + std::to_string(result));
                    MessageBoxW(hWnd, L"Не удалось загрузить Microsoft Edge WebView2.\nПожалуйста, убедитесь, что в системе установлен WebView2 Runtime.", L"Ошибка запуска", MB_ICONERROR);
                    return S_OK;
                }

                HRESULT hrCtrl = env->CreateCoreWebView2Controller(
                    hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            LogDebug("Controller callback: result = " + std::to_string(result));
                            if (FAILED(result) || !controller) return S_OK;

                            g_controller = controller;
                            controller->get_CoreWebView2(&g_webview);

                            // Size WebView to fit window
                            RECT bounds;
                            GetClientRect(hWnd, &bounds);
                            controller->put_Bounds(bounds);

                            // Settings
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                            }

                            // Host local UI
                            ComPtr<ICoreWebView2_3> webview3;
                            fs::path uiPath = get_ui_dir();
                            LogDebug("Resolved UI Path: " + uiPath.string());

                            if (SUCCEEDED(g_webview.As(&webview3)) && webview3) {
                                HRESULT hrMap = webview3->SetVirtualHostNameToFolderMapping(
                                    L"launcher.local",
                                    uiPath.wstring().c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
                                );
                                LogDebug("SetVirtualHostNameToFolderMapping hr = " + std::to_string(hrMap));
                                g_webview->Navigate(L"https://launcher.local/index.html");
                            } else {
                                fs::path indexHtml = uiPath / "index.html";
                                LogDebug("Fallback Navigate: " + indexHtml.string());
                                g_webview->Navigate(indexHtml.wstring().c_str());
                            }

                            // Bind IPC events
                            SetupWebViewEvents();
                            LogDebug("SetupWebViewEvents complete");
                            return S_OK;
                        }
                    ).Get()
                );
                LogDebug("env->CreateCoreWebView2Controller called: hr = " + std::to_string(hrCtrl));
                return S_OK;
            }
        ).Get()
    );
    LogDebug("CreateCoreWebView2EnvironmentWithOptions called: hr = " + std::to_string(hrEnv));

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogDebug("Exiting wWinMain");
    CoUninitialize();
    return (int)msg.wParam;
}