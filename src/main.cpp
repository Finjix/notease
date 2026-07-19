#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"Notease.MainWindow";
constexpr wchar_t kWindowTitle[] = L"Notease";
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\Notease.SingleInstance";
constexpr wchar_t kAutoStartSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoStartValueName[] = L"Notease";
constexpr int kApplicationIconId = 101;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowMessage = WM_APP + 2;
constexpr UINT kToggleAlwaysOnTopMessage = WM_APP + 3;
constexpr UINT kSaveTimer = 1;
constexpr UINT kTrayIconId = 1;

constexpr int kEditorControlId = 2001;
constexpr int kCollapseButtonId = 2002;
constexpr int kHideButtonId = 2003;

constexpr UINT kTrayAutoStartCommand = 1002;
constexpr UINT kTrayExitCommand = 1003;

constexpr int kNormalWidth = 380;
constexpr int kNormalHeight = 280;
constexpr int kCollapsedHeight = 32;
constexpr int kTitleBarHeight = 32;
constexpr int kTitleHoverWidth = 88;

struct Settings {
    bool autoStart = true;
    bool alwaysOnTop = false;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND editor = nullptr;
    WNDPROC editorWindowProc = nullptr;
    HWND collapseButton = nullptr;
    HWND hideButton = nullptr;
    WNDPROC collapseButtonWindowProc = nullptr;
    WNDPROC hideButtonWindowProc = nullptr;
    HANDLE mutex = nullptr;
    HFONT editorFont = nullptr;
    NOTIFYICONDATAW tray{};
    UINT taskbarCreatedMessage = 0;
    bool trayAdded = false;
    bool collapsed = false;
    int normalWidth = kNormalWidth;
    int normalHeight = kNormalHeight;
    Settings settings{};
};

AppState g_app;
ULONG_PTR g_gdiplusToken = 0;

LRESULT CALLBACK EditorWindowProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    if (message == WM_MOUSEWHEEL) {
        const int wheelSteps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        if (wheelSteps != 0) {
            UINT linesPerScroll = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,
                                  sizeof(linesPerScroll), &linesPerScroll, 0);
            if (linesPerScroll == WHEEL_PAGESCROLL) {
                RECT client{};
                GetClientRect(window, &client);
                linesPerScroll = static_cast<UINT>(std::max(1L, client.bottom / 20));
            }
            SendMessageW(window, EM_LINESCROLL, 0,
                         -wheelSteps * static_cast<int>(linesPerScroll));
        }
        return 0;
    }

    return CallWindowProcW(g_app.editorWindowProc, window, message, wParam,
                           lParam);
}

WNDPROC OriginalButtonWindowProc(HWND window) {
    if (window == g_app.collapseButton) {
        return g_app.collapseButtonWindowProc;
    }
    if (window == g_app.hideButton) {
        return g_app.hideButtonWindowProc;
    }
    return nullptr;
}

LRESULT CALLBACK ButtonWindowProc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    if (message == WM_ERASEBKGND || message == WM_MOUSEMOVE) {
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        SetCapture(window);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        if (GetCapture() == window) {
            ReleaseCapture();
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window, &client);
            if (PtInRect(&client, point) != FALSE) {
                HWND parent = GetParent(window);
                SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(window), BN_CLICKED),
                             reinterpret_cast<LPARAM>(window));
            }
        }
        return 0;
    }

    WNDPROC original = OriginalButtonWindowProc(window);
    return original == nullptr
               ? DefWindowProcW(window, message, wParam, lParam)
               : CallWindowProcW(original, window, message, wParam, lParam);
}

int ScaleForDpi(int value, HWND window) {
    const UINT dpi = window == nullptr ? 96 : GetDpiForWindow(window);
    return MulDiv(value, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
}

std::wstring LastErrorText(DWORD error = GetLastError()) {
    if (error == ERROR_SUCCESS) {
        return L"未知错误";
    }

    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return L"错误代码 " + std::to_wstring(error);
    }

    std::wstring result(buffer, length);
    LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(1024);
    while (true) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::wstring(buffer.begin(), buffer.end());
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path ExecutableDirectory() {
    const std::wstring executablePath = CurrentExecutablePath();
    if (executablePath.empty()) {
        return {};
    }
    return std::filesystem::path(executablePath).parent_path();
}

std::filesystem::path NoteaseFilePath() {
    const std::filesystem::path directory = ExecutableDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / L"notease.json";
}

bool EnsureParentDirectory(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    return !error;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int inputLength = static_cast<int>(value.size());
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     value.data(), inputLength, nullptr, 0);
    if (length == 0) {
        length = MultiByteToWideChar(CP_UTF8, 0, value.data(), inputLength,
                                     nullptr, 0);
    }
    if (length == 0) {
        return {};
    }

    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), inputLength, result.data(),
                        length);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int inputLength = static_cast<int>(value.size());
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                     value.data(), inputLength, nullptr, 0,
                                     nullptr, nullptr);
    if (length == 0) {
        length = WideCharToMultiByte(CP_UTF8, 0, value.data(), inputLength,
                                     nullptr, 0, nullptr, nullptr);
    }
    if (length == 0) {
        return {};
    }

    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), inputLength, result.data(),
                        length, nullptr, nullptr);
    return result;
}

std::string JsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 16);
    const char* hex = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                result += "\\u00";
                result += hex[character >> 4];
                result += hex[character & 0x0f];
            } else {
                result += static_cast<char>(character);
            }
        }
    }
    return result;
}

bool ParseJsonStringField(const std::string& json, const std::string& key,
                          std::string& value) {
    const std::string marker = "\"" + key + "\"";
    const size_t keyPosition = json.find(marker);
    if (keyPosition == std::string::npos) return false;
    size_t position = json.find(':', keyPosition + marker.size());
    if (position == std::string::npos) return false;
    ++position;
    while (position < json.size() && isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (position >= json.size() || json[position] != '"') return false;
    ++position;
    value.clear();
    while (position < json.size()) {
        const unsigned char character = static_cast<unsigned char>(json[position++]);
        if (character == '"') return true;
        if (character != '\\' || position >= json.size()) return false;
        const char escaped = json[position++];
        switch (escaped) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: return false;
        }
    }
    return false;
}

bool ParseJsonBoolField(const std::string& json, const std::string& key, bool& value) {
    const std::string marker = "\"" + key + "\"";
    const size_t keyPosition = json.find(marker);
    if (keyPosition == std::string::npos) return false;
    size_t position = json.find(':', keyPosition + marker.size());
    if (position == std::string::npos) return false;
    ++position;
    while (position < json.size() && isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (json.compare(position, 4, "true") == 0) { value = true; return true; }
    if (json.compare(position, 5, "false") == 0) { value = false; return true; }
    return false;
}

bool ReadUtf8File(const std::filesystem::path& path, std::string& content) {
    if (path.empty()) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
    return true;
}

std::wstring LoadNoteText() {
    std::string json;
    std::string note;
    if (ReadUtf8File(NoteaseFilePath(), json) && ParseJsonStringField(json, "note", note)) {
        return Utf8ToWide(note);
    }
    return {};
}

bool SaveNoteaseFile(const std::wstring& note, const Settings& settings,
                     std::wstring* errorMessage = nullptr) {
    const std::filesystem::path path = NoteaseFilePath();
    if (path.empty() || !EnsureParentDirectory(path)) {
        if (errorMessage != nullptr) *errorMessage = L"无法定位或创建 exe 所在目录。";
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (errorMessage != nullptr) *errorMessage = L"无法写入 " + path.wstring() + L"。";
        return false;
    }
    const std::string utf8 = WideToUtf8(note);
    file << "{\n  \"note\": \"" << JsonEscape(utf8)
         << "\",\n  \"autostart\": " << (settings.autoStart ? "true" : "false")
         << ",\n  \"alwaysOnTop\": "
         << (settings.alwaysOnTop ? "true" : "false") << "\n}\n";
    return static_cast<bool>(file);
}

bool LoadSettings(Settings& settings) {
    settings = Settings{};
    std::string json;
    bool autoStart = settings.autoStart;
    if (ReadUtf8File(NoteaseFilePath(), json) && ParseJsonBoolField(json, "autostart", autoStart)) {
        settings.autoStart = autoStart;
    }
    bool alwaysOnTop = settings.alwaysOnTop;
    if (ReadUtf8File(NoteaseFilePath(), json) &&
        ParseJsonBoolField(json, "alwaysOnTop", alwaysOnTop)) {
        settings.alwaysOnTop = alwaysOnTop;
    }
    return !json.empty();
}

bool SaveSettings(const Settings& settings, std::wstring* errorMessage = nullptr) {
    return SaveNoteaseFile(LoadNoteText(), settings, errorMessage);
}

bool SaveNoteText(HWND editor) {
    if (editor == nullptr) return false;

    const int length = GetWindowTextLengthW(editor);
    std::wstring content(length + 1, L'\0');
    GetWindowTextW(editor, content.data(), length + 1);
    content.resize(length);

    Settings settings;
    LoadSettings(settings);
    return SaveNoteaseFile(content, settings);
}

void ApplyAlwaysOnTop(HWND window, bool alwaysOnTop) {
    SetWindowPos(window, alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RedrawButton(HWND button) {
    if (button != nullptr) {
        RedrawWindow(button, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE);
    }
}

bool SetAutoStartEnabled(bool enabled, std::wstring* errorMessage = nullptr) {
    auto fail = [errorMessage](const std::wstring& message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    };

    if (!enabled) {
        HKEY key = nullptr;
        const LSTATUS openStatus = RegOpenKeyExW(
            HKEY_CURRENT_USER, kAutoStartSubKey, 0, KEY_SET_VALUE, &key);
        if (openStatus == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (openStatus != ERROR_SUCCESS) {
            return fail(L"无法打开当前用户的自启动注册表项：" +
                        LastErrorText(openStatus));
        }

        LSTATUS deleteStatus = RegDeleteValueW(key, kAutoStartValueName);
        if (deleteStatus == ERROR_FILE_NOT_FOUND) {
            deleteStatus = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        if (deleteStatus != ERROR_SUCCESS) {
            return fail(L"无法关闭 Notease 自启动：" +
                        LastErrorText(deleteStatus));
        }
        return true;
    }

    const std::wstring executablePath = CurrentExecutablePath();
    if (executablePath.empty()) {
        return fail(L"无法定位 Notease.exe 的路径。");
    }

    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER, kAutoStartSubKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, &disposition);
    if (createStatus != ERROR_SUCCESS) {
        return fail(L"无法打开当前用户的自启动注册表项：" +
                    LastErrorText(createStatus));
    }

    const std::wstring command = L"\"" + executablePath + L"\"";
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const LSTATUS setStatus = RegSetValueExW(
        key, kAutoStartValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    RegCloseKey(key);
    if (setStatus != ERROR_SUCCESS) {
        return fail(L"无法开启 Notease 自启动：" + LastErrorText(setStatus));
    }
    return true;
}

void ShowError(const std::wstring& message) {
    MessageBoxW(g_app.window, message.c_str(), L"Notease", MB_OK | MB_ICONWARNING);
}

void SetWindowRegion(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int radius = ScaleForDpi(12, window);
    HRGN region = CreateRoundRectRgn(client.left, client.top, client.right + 1,
                                     client.bottom + 1, radius, radius);
    if (region != nullptr && SetWindowRgn(window, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void LayoutEditor(HWND window) {
    if (g_app.editor == nullptr) {
        return;
    }

    const bool visible = !g_app.collapsed;
    ShowWindow(g_app.editor, visible ? SW_SHOW : SW_HIDE);
    if (!visible) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    const int padding = ScaleForDpi(3, window);
    const int top = ScaleForDpi(kTitleBarHeight, window) + padding;
    MoveWindow(g_app.editor, padding, top, client.right - padding * 2,
               client.bottom - top - padding, TRUE);

    RECT editorClient{};
    GetClientRect(g_app.editor, &editorClient);
    const int textMargin = ScaleForDpi(8, window);
    RECT textRectangle{textMargin, 0, editorClient.right - textMargin,
                       editorClient.bottom};
    SendMessageW(g_app.editor, EM_SETRECTNP, 0,
                 reinterpret_cast<LPARAM>(&textRectangle));
}

void LayoutButtons(HWND window) {
    if (g_app.collapseButton == nullptr || g_app.hideButton == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    const int titleHeight = ScaleForDpi(kTitleBarHeight, window);
    const int buttonHeight = titleHeight - ScaleForDpi(8, window);
    const int buttonWidth = ScaleForDpi(34, window);
    const int gap = ScaleForDpi(4, window);
    const int right = client.right - ScaleForDpi(8, window);
    const int top = ScaleForDpi(4, window);

    MoveWindow(g_app.collapseButton, right - buttonWidth - gap - buttonWidth,
               top, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_app.hideButton, right - buttonWidth, top, buttonWidth,
               buttonHeight, TRUE);
}

void LayoutControls(HWND window) {
    LayoutEditor(window);
    LayoutButtons(window);
}

void UpdateEditorFont(HWND window) {
    if (g_app.editor == nullptr) {
        return;
    }

    HFONT font = CreateFontW(
        -ScaleForDpi(16, window), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (font == nullptr) {
        return;
    }

    HFONT previous = g_app.editorFont;
    g_app.editorFont = font;
    SendMessageW(g_app.editor, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (g_app.collapseButton != nullptr) {
        SendMessageW(g_app.collapseButton, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (g_app.hideButton != nullptr) {
        SendMessageW(g_app.hideButton, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (previous != nullptr) {
        DeleteObject(previous);
    }
}

void ShowNoteWindow(HWND window) {
    if (g_app.collapsed) {
        g_app.collapsed = false;
        RECT rectangle{};
        GetWindowRect(window, &rectangle);
        SetWindowPos(window, g_app.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     rectangle.left, rectangle.top,
                     rectangle.right - rectangle.left, g_app.normalHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowTextW(g_app.collapseButton, L"");
        LayoutControls(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    ShowWindow(window, SW_SHOWNORMAL);
    SetWindowPos(window, g_app.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    if (g_app.editor != nullptr) {
        SetFocus(g_app.editor);
    }
}

void HideNoteWindow(HWND window) {
    SaveNoteText(g_app.editor);
    ShowWindow(window, SW_HIDE);
}

bool AddTrayIcon(HWND window) {
    if (g_app.trayAdded) {
        return true;
    }

    g_app.tray = NOTIFYICONDATAW{};
    g_app.tray.cbSize = sizeof(g_app.tray);
    g_app.tray.hWnd = window;
    g_app.tray.uID = kTrayIconId;
    g_app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_app.tray.uCallbackMessage = kTrayMessage;
    g_app.tray.hIcon = LoadIconW(
        g_app.instance, MAKEINTRESOURCEW(kApplicationIconId));
    lstrcpynW(g_app.tray.szTip, L"Notease 便签", ARRAYSIZE(g_app.tray.szTip));

    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &g_app.tray) == TRUE;
    if (g_app.trayAdded) {
        g_app.tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_app.tray);
    }
    return g_app.trayAdded;
}

void RemoveTrayIcon() {
    if (g_app.trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
        g_app.trayAdded = false;
    }
}

void ToggleAutoStart() {
    const bool previous = g_app.settings.autoStart;
    const bool desired = !previous;
    std::wstring error;
    if (!SetAutoStartEnabled(desired, &error)) {
        ShowError(error);
        return;
    }

    Settings next = g_app.settings;
    next.autoStart = desired;
    if (!SaveSettings(next, &error)) {
        SetAutoStartEnabled(previous);
        ShowError(error);
        return;
    }

    g_app.settings = next;
}

void ToggleAlwaysOnTop(HWND window) {
    const bool previous = g_app.settings.alwaysOnTop;
    Settings next = g_app.settings;
    next.alwaysOnTop = !previous;
    std::wstring error;
    if (!SaveSettings(next, &error)) {
        ShowError(error);
        return;
    }

    g_app.settings = next;
    ApplyAlwaysOnTop(window, next.alwaysOnTop);
    RedrawButton(g_app.collapseButton);
}

void HandleTrayCommand(HWND window, UINT command) {
    switch (command) {
    case kTrayAutoStartCommand:
        ToggleAutoStart();
        break;
    case kTrayExitCommand:
        PostMessageW(window, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

void ShowTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const std::wstring autoStartText =
        g_app.settings.autoStart ? L"自启动√" : L"自启动×";
    AppendMenuW(menu, MF_STRING, kTrayAutoStartCommand, autoStartText.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出程序");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN |
                  TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x, cursor.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    HandleTrayCommand(window, command);
}

void PaintButton(const DRAWITEMSTRUCT& item, const std::wstring& text,
                 bool emphasized) {
    HDC deviceContext = item.hDC;
    RECT rectangle = item.rcItem;
    const COLORREF background = RGB(248, 222, 116);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(deviceContext, &rectangle, brush);
    DeleteObject(brush);

    const bool pinIcon = item.CtlID == kCollapseButtonId;
    HFONT font = CreateFontW(
        -ScaleForDpi(pinIcon ? 16 : 12, g_app.window), 0, 0, 0,
        pinIcon ? FW_BOLD : (emphasized ? FW_SEMIBOLD : FW_NORMAL),
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        item.CtlID == kCollapseButtonId ? L"Segoe MDL2 Assets"
                                        : L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(deviceContext, font);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(86, 67, 20));
    RECT textRectangle = rectangle;
    if (pinIcon) {
        const int centerX = (textRectangle.left + textRectangle.right) / 2;
        const int centerY = (textRectangle.top + textRectangle.bottom) / 2;
        constexpr double diagonalAngle = 0.7853981633974483;
        const double angle = g_app.settings.alwaysOnTop ? 0.0 : diagonalAngle;
        const double sine = std::sin(angle);
        const double cosine = std::cos(angle);
        const double scale =
            static_cast<double>(ScaleForDpi(1, g_app.window)) * 0.58;
        const POINT shape[] = {
            {-6, -8}, {6, -8}, {6, -5}, {4, -5}, {4, -1}, {7, 1},
            {7, 3}, {2, 3}, {2, 7}, {0, 11}, {-2, 7}, {-2, 3},
            {-7, 3}, {-7, 1}, {-4, -1}, {-4, -5}, {-6, -5},
        };
        Gdiplus::PointF pinShape[ARRAYSIZE(shape)]{};
        for (size_t index = 0; index < ARRAYSIZE(shape); ++index) {
            const double x = static_cast<double>(shape[index].x) * scale;
            const double y = static_cast<double>(shape[index].y) * scale;
            pinShape[index].X = static_cast<Gdiplus::REAL>(
                centerX + x * cosine - y * sine);
            pinShape[index].Y = static_cast<Gdiplus::REAL>(
                centerY + x * sine + y * cosine);
        }
        Gdiplus::Graphics graphics(deviceContext);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::SolidBrush pinBrush(Gdiplus::Color(255, 86, 67, 20));
        graphics.FillPolygon(&pinBrush, pinShape, ARRAYSIZE(pinShape));
    } else if (emphasized) {
        const int lineWidth = ScaleForDpi(14, g_app.window);
        const int lineHeight = ScaleForDpi(2, g_app.window);
        const int centerX = (textRectangle.left + textRectangle.right) / 2;
        const int centerY = (textRectangle.top + textRectangle.bottom) / 2;
        RECT lineRectangle{centerX - lineWidth / 2, centerY - lineHeight / 2,
                           centerX + lineWidth / 2,
                           centerY + (lineHeight + 1) / 2};
        HBRUSH lineBrush = CreateSolidBrush(RGB(86, 67, 20));
        FillRect(deviceContext, &lineRectangle, lineBrush);
        DeleteObject(lineBrush);
    } else {
        DrawTextW(deviceContext, text.c_str(), -1, &textRectangle,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(deviceContext, oldFont);
    DeleteObject(font);
}

void PaintWindow(HWND window, HDC deviceContext) {
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH bodyBrush = CreateSolidBrush(RGB(255, 250, 221));
    FillRect(deviceContext, &client, bodyBrush);
    DeleteObject(bodyBrush);

    RECT titleRectangle = client;
    titleRectangle.bottom = ScaleForDpi(kTitleBarHeight, window);
    HBRUSH titleBrush = CreateSolidBrush(RGB(248, 222, 116));
    FillRect(deviceContext, &titleRectangle, titleBrush);
    DeleteObject(titleBrush);

    HFONT titleFont = CreateFontW(
        -ScaleForDpi(14, window), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(deviceContext, titleFont);

    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(77, 59, 16));
    RECT titleText{ScaleForDpi(10, window), 0, titleRectangle.right,
                   titleRectangle.bottom};
    DrawTextW(deviceContext, L"Notease", -1, &titleText,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(deviceContext, oldFont);
    DeleteObject(titleFont);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(211, 180, 65));
    HGDIOBJ oldPen = SelectObject(deviceContext, borderPen);
    HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(NULL_BRUSH));
    Rectangle(deviceContext, client.left, client.top, client.right, client.bottom);
    SelectObject(deviceContext, oldBrush);
    SelectObject(deviceContext, oldPen);
    DeleteObject(borderPen);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    if (g_app.taskbarCreatedMessage != 0 &&
        message == g_app.taskbarCreatedMessage) {
        AddTrayIcon(window);
        return 0;
    }

    switch (message) {
    case WM_CREATE: {
        g_app.normalWidth = ScaleForDpi(kNormalWidth, window);
        g_app.normalHeight = ScaleForDpi(kNormalHeight, window);
        RECT initialWindow{};
        if (GetWindowRect(window, &initialWindow) &&
            initialWindow.bottom > initialWindow.top &&
            initialWindow.right > initialWindow.left) {
            g_app.normalWidth = initialWindow.right - initialWindow.left;
            g_app.normalHeight = initialWindow.bottom - initialWindow.top;
        }
        g_app.editor = CreateWindowExW(
            0, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorControlId)),
            g_app.instance,
            nullptr);
        if (g_app.editor == nullptr) {
            return -1;
        }
        g_app.editorWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_app.editor, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(EditorWindowProc)));

        g_app.collapseButton = CreateWindowExW(
            0, L"BUTTON", L"\xE841",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCollapseButtonId)),
            g_app.instance, nullptr);
        g_app.hideButton = CreateWindowExW(
            0, L"BUTTON", L"一",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHideButtonId)),
            g_app.instance, nullptr);
        if (g_app.collapseButton == nullptr || g_app.hideButton == nullptr) {
            return -1;
        }
        g_app.collapseButtonWindowProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_app.collapseButton, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(ButtonWindowProc)));
        g_app.hideButtonWindowProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_app.hideButton, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(ButtonWindowProc)));
        SendMessageW(g_app.editor, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        UpdateEditorFont(window);
        SetWindowTextW(g_app.editor, LoadNoteText().c_str());
        LayoutControls(window);
        SetWindowRegion(window);
        return 0;
    }

    case WM_SIZE:
        SetWindowRegion(window);
        LayoutControls(window);
        return 0;

    case WM_DPICHANGED:
        UpdateEditorFont(window);
        LayoutControls(window);
        SetWindowRegion(window);
        return 0;

    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        const int titleHeight = ScaleForDpi(kTitleBarHeight, window);
        if (point.y < titleHeight) {
            if (point.x >= ScaleForDpi(10, window) &&
                point.x < ScaleForDpi(kTitleHoverWidth, window)) {
                return HTCLIENT;
            }
            HWND child = ChildWindowFromPointEx(window, point,
                                                CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (child == g_app.collapseButton || child == g_app.hideButton) {
                return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_LBUTTONUP: {
        const int titleHeight = ScaleForDpi(kTitleBarHeight, window);
        const int titleClickWidth = ScaleForDpi(kTitleHoverWidth, window);
        if (GET_Y_LPARAM(lParam) < titleHeight &&
            GET_X_LPARAM(lParam) >= ScaleForDpi(10, window) &&
            GET_X_LPARAM(lParam) < titleClickWidth) {
            SetWindowTextW(g_app.editor, L"");
            SaveNoteText(g_app.editor);
            SetFocus(g_app.editor);
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == g_app.editor &&
            HIWORD(wParam) == EN_CHANGE) {
            SetTimer(window, kSaveTimer, 500, nullptr);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == g_app.collapseButton) {
            PostMessageW(window, kToggleAlwaysOnTopMessage, 0, 0);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == g_app.hideButton) {
            HideNoteWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == kTrayAutoStartCommand ||
            LOWORD(wParam) == kTrayExitCommand) {
            HandleTrayCommand(window, LOWORD(wParam));
            return 0;
        }
        break;

    case WM_SETCURSOR: {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window, &cursor);
        if (cursor.x >= ScaleForDpi(10, window) &&
            cursor.x < ScaleForDpi(kTitleHoverWidth, window) &&
            cursor.y >= 0 && cursor.y < ScaleForDpi(kTitleBarHeight, window)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == kSaveTimer) {
            KillTimer(window, kSaveTimer);
            SaveNoteText(g_app.editor);
            return 0;
        }
        break;

    case WM_CTLCOLOREDIT: {
        static HBRUSH editBrush = CreateSolidBrush(RGB(255, 252, 232));
        HDC deviceContext = reinterpret_cast<HDC>(wParam);
        SetTextColor(deviceContext, RGB(62, 53, 28));
        SetBkColor(deviceContext, RGB(255, 252, 232));
        return reinterpret_cast<LRESULT>(editBrush);
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item == nullptr || item->CtlType != ODT_BUTTON) {
            break;
        }
        if (item->CtlID == kCollapseButtonId) {
            PaintButton(*item, L"\xE841", false);
            return TRUE;
        }
        if (item->CtlID == kHideButtonId) {
            PaintButton(*item, L"一", true);
            return TRUE;
        }
        break;
    }

    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowNoteWindow(window);
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(window);
            return 0;
        default:
            return 0;
        }

    case kShowMessage:
        ShowNoteWindow(window);
        return 0;

    case kToggleAlwaysOnTopMessage:
        ToggleAlwaysOnTop(window);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);
        PaintWindow(window, deviceContext);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, kSaveTimer);
        SaveNoteText(g_app.editor);
        RemoveTrayIcon();
        if (g_app.editorFont != nullptr) {
            DeleteObject(g_app.editorFont);
            g_app.editorFont = nullptr;
        }
        if (g_app.mutex != nullptr) {
            CloseHandle(g_app.mutex);
            g_app.mutex = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput,
                                nullptr) != Gdiplus::Ok) {
        return 1;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (mutex == nullptr) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClassName, nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, kShowMessage, 0, 0);
        }
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CloseHandle(mutex);
        return 0;
    }

    g_app.instance = instance;
    g_app.mutex = mutex;
    g_app.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    Settings settings;
    LoadSettings(settings);
    g_app.settings = settings;

    std::wstring startupWarning;
    if (!SetAutoStartEnabled(settings.autoStart, &startupWarning)) {
        startupWarning = L"自启动设置同步失败：" + startupWarning;
    }
    std::wstring settingsError;
    if (!SaveSettings(settings, &settingsError)) {
        if (!startupWarning.empty()) {
            startupWarning += L"\n";
        }
        startupWarning += settingsError;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kApplicationIconId));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass) == 0) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    const int width = ScaleForDpi(kNormalWidth, nullptr);
    const int height = ScaleForDpi(kNormalHeight, nullptr);
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int left = (screenWidth - width) / 2;
    const int top = (screenHeight - height) / 2;

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClassName, kWindowTitle,
        WS_POPUP, left, top, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(kWindowClassName, instance);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }
    g_app.window = window;
    ApplyAlwaysOnTop(window, settings.alwaysOnTop);

    if (!AddTrayIcon(window)) {
        MessageBoxW(window, L"无法创建通知区域图标，程序仍会运行。", L"Notease",
                    MB_OK | MB_ICONWARNING);
    }

    ShowWindow(window, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window);
    SetFocus(g_app.editor);

    if (!startupWarning.empty()) {
        ShowError(startupWarning);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnregisterClassW(kWindowClassName, instance);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(message.wParam);
}
