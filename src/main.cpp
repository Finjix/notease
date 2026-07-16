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
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
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

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowMessage = WM_APP + 2;
constexpr UINT kSaveTimer = 1;
constexpr UINT kTrayIconId = 1;

constexpr UINT kTrayShowCommand = 1001;
constexpr UINT kTrayAutoStartCommand = 1002;
constexpr UINT kTrayExitCommand = 1003;
constexpr int kControlNone = 0;
constexpr int kControlCollapse = 1;
constexpr int kControlHide = 2;

constexpr int kNormalWidth = 380;
constexpr int kNormalHeight = 280;
constexpr int kCollapsedHeight = 44;
constexpr int kTitleBarHeight = 44;

struct Settings {
    bool autoStart = true;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND editor = nullptr;
    HANDLE mutex = nullptr;
    HFONT editorFont = nullptr;
    NOTIFYICONDATAW tray{};
    UINT taskbarCreatedMessage = 0;
    bool trayAdded = false;
    bool collapsed = false;
    int hoveredControl = kControlNone;
    int normalWidth = kNormalWidth;
    int normalHeight = kNormalHeight;
    Settings settings{};
};

AppState g_app;

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

std::wstring EnvironmentVariable(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(length + 1);
    length = GetEnvironmentVariableW(name, buffer.data(),
                                     static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return {};
    }
    buffer.resize(length);
    return std::wstring(buffer.begin(), buffer.end());
}

std::filesystem::path ExecutableDirectory() {
    const std::wstring executablePath = CurrentExecutablePath();
    if (executablePath.empty()) {
        return {};
    }
    return std::filesystem::path(executablePath).parent_path();
}

std::filesystem::path SettingsFilePath() {
    const std::filesystem::path directory = ExecutableDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / L"setting.ini";
}

void DebugAutoStart(const wchar_t* step, LSTATUS status) {
    const std::filesystem::path directory = ExecutableDirectory();
    if (directory.empty()) {
        return;
    }
    std::wofstream file(directory / L"autostart-debug.txt", std::ios::app);
    if (file.is_open()) {
        file << step << L"=" << status << L"\n";
    }
}

std::filesystem::path NoteFilePath() {
    const std::wstring localAppData = EnvironmentVariable(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        return std::filesystem::path(localAppData) / L"Notease" / L"note.txt";
    }

    const std::filesystem::path directory = ExecutableDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / L"note.txt";
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

std::wstring LoadNoteText() {
    const std::filesystem::path path = NoteFilePath();
    if (path.empty()) {
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
    return Utf8ToWide(content);
}

bool SaveNoteText(HWND editor) {
    if (editor == nullptr) {
        return false;
    }

    const int length = GetWindowTextLengthW(editor);
    std::wstring content(length + 1, L'\0');
    GetWindowTextW(editor, content.data(), length + 1);
    content.resize(length);

    const std::filesystem::path path = NoteFilePath();
    if (path.empty() || !EnsureParentDirectory(path)) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    const std::string utf8 = WideToUtf8(content);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(file);
}

bool LoadSettings(Settings& settings) {
    settings = Settings{};
    const std::filesystem::path path = SettingsFilePath();
    if (path.empty()) {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "autostart=0") {
            settings.autoStart = false;
        } else if (line == "autostart=1") {
            settings.autoStart = true;
        }
    }
    return true;
}

bool SaveSettings(const Settings& settings, std::wstring* errorMessage = nullptr) {
    const std::filesystem::path path = SettingsFilePath();
    if (path.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法定位当前 exe 所在目录。";
        }
        return false;
    }

    if (!EnsureParentDirectory(path)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法创建配置文件目录。";
        }
        return false;
    }

    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法写入 " + path.wstring() + L"。";
        }
        return false;
    }

    file << "autostart=" << (settings.autoStart ? "1" : "0") << "\n";
    if (!file && errorMessage != nullptr) {
        *errorMessage = L"写入配置文件时发生错误。";
    }
    return static_cast<bool>(file);
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
        DebugAutoStart(L"disable.open", openStatus);
        if (openStatus == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (openStatus != ERROR_SUCCESS) {
            return fail(L"无法打开当前用户的自启动注册表项：" +
                        LastErrorText(openStatus));
        }

        LSTATUS deleteStatus = RegDeleteValueW(key, kAutoStartValueName);
        DebugAutoStart(L"disable.delete", deleteStatus);
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
    DebugAutoStart(L"enable.create", createStatus);
    if (createStatus != ERROR_SUCCESS) {
        return fail(L"无法打开当前用户的自启动注册表项：" +
                    LastErrorText(createStatus));
    }

    const std::wstring command = L"\"" + executablePath + L"\"";
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const LSTATUS setStatus = RegSetValueExW(
        key, kAutoStartValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    DebugAutoStart(L"enable.set", setStatus);
    RegCloseKey(key);
    if (setStatus != ERROR_SUCCESS) {
        return fail(L"无法开启 Notease 自启动：" + LastErrorText(setStatus));
    }
    return true;
}

void ShowError(const std::wstring& message) {
    MessageBoxW(g_app.window, message.c_str(), L"Notease", MB_OK | MB_ICONWARNING);
}

RECT CollapseButtonRect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int height = ScaleForDpi(kTitleBarHeight, window);
    const int width = ScaleForDpi(58, window);
    const int gap = ScaleForDpi(6, window);
    const int right = client.right - ScaleForDpi(10, window);
    return {right - width - gap - width, ScaleForDpi(5, window),
            right - gap, height - ScaleForDpi(5, window)};
}

RECT HideButtonRect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int height = ScaleForDpi(kTitleBarHeight, window);
    const int width = ScaleForDpi(58, window);
    const int right = client.right - ScaleForDpi(10, window);
    return {right - width, ScaleForDpi(5, window), right,
            height - ScaleForDpi(5, window)};
}

int ControlAtPoint(HWND window, POINT point) {
    RECT rectangle = CollapseButtonRect(window);
    if (PtInRect(&rectangle, point)) {
        return kControlCollapse;
    }
    rectangle = HideButtonRect(window);
    if (PtInRect(&rectangle, point)) {
        return kControlHide;
    }
    return kControlNone;
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
    const int padding = ScaleForDpi(12, window);
    const int top = ScaleForDpi(kTitleBarHeight, window) + padding;
    MoveWindow(g_app.editor, padding, top, client.right - padding * 2,
               client.bottom - top - padding, TRUE);
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
    if (previous != nullptr) {
        DeleteObject(previous);
    }
}

void ToggleCollapse(HWND window) {
    RECT rectangle{};
    GetWindowRect(window, &rectangle);
    g_app.collapsed = !g_app.collapsed;
    const int height = g_app.collapsed
                           ? ScaleForDpi(kCollapsedHeight, window)
                           : g_app.normalHeight;
    SetWindowPos(window, HWND_TOPMOST, rectangle.left, rectangle.top,
                 rectangle.right - rectangle.left, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    LayoutEditor(window);
    InvalidateRect(window, nullptr, FALSE);
}

void ShowNoteWindow(HWND window) {
    if (g_app.collapsed) {
        g_app.collapsed = false;
        RECT rectangle{};
        GetWindowRect(window, &rectangle);
        SetWindowPos(window, HWND_TOPMOST, rectangle.left, rectangle.top,
                     rectangle.right - rectangle.left, g_app.normalHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        LayoutEditor(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    ShowWindow(window, SW_SHOWNORMAL);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    if (g_app.editor != nullptr) {
        SetFocus(g_app.editor);
    }
}

void HideNoteWindow(HWND window) {
    SaveNoteText(g_app.editor);
    ShowWindow(window, SW_HIDE);
    g_app.hoveredControl = kControlNone;
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
    g_app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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

void ShowTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kTrayShowCommand, L"显示便签");
    const std::wstring autoStartText =
        g_app.settings.autoStart ? L"自启动 √" : L"自启动 ×";
    AppendMenuW(menu, MF_STRING | (g_app.settings.autoStart ? MF_CHECKED : 0),
                kTrayAutoStartCommand, autoStartText.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出程序");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   cursor.x, cursor.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void PaintButton(HDC deviceContext, HWND window, const RECT& rectangle,
                 const std::wstring& text, int control) {
    const bool hovered = g_app.hoveredControl == control;
    const COLORREF background = hovered ? RGB(255, 238, 166) : RGB(250, 225, 125);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(deviceContext, &rectangle, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(218, 190, 82));
    HGDIOBJ oldPen = SelectObject(deviceContext, pen);
    HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(NULL_BRUSH));
    Rectangle(deviceContext, rectangle.left, rectangle.top, rectangle.right,
              rectangle.bottom);
    SelectObject(deviceContext, oldBrush);
    SelectObject(deviceContext, oldPen);
    DeleteObject(pen);

    HFONT font = CreateFontW(
        -ScaleForDpi(12, window), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(deviceContext, font);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(86, 67, 20));
    RECT textRectangle = rectangle;
    DrawTextW(deviceContext, text.c_str(), -1, &textRectangle,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
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
        -ScaleForDpi(15, window), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(deviceContext, titleFont);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(77, 59, 16));
    RECT titleText{ScaleForDpi(12, window), 0, titleRectangle.right,
                   titleRectangle.bottom};
    DrawTextW(deviceContext, L"Notease", -1, &titleText,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(deviceContext, oldFont);
    DeleteObject(titleFont);

    PaintButton(deviceContext, window, CollapseButtonRect(window),
                g_app.collapsed ? L"展开" : L"收起", kControlCollapse);
    PaintButton(deviceContext, window, HideButtonRect(window), L"隐藏",
                kControlHide);

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
        g_app.editor = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN | WS_VSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(1001), g_app.instance,
            nullptr);
        if (g_app.editor == nullptr) {
            return -1;
        }

        SendMessageW(g_app.editor, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        SendMessageW(g_app.editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(8, 8));
        UpdateEditorFont(window);
        SetWindowTextW(g_app.editor, LoadNoteText().c_str());
        LayoutEditor(window);
        SetWindowRegion(window);
        return 0;
    }

    case WM_SIZE:
        SetWindowRegion(window);
        LayoutEditor(window);
        return 0;

    case WM_DPICHANGED:
        UpdateEditorFont(window);
        LayoutEditor(window);
        SetWindowRegion(window);
        return 0;

    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        const int titleHeight = ScaleForDpi(kTitleBarHeight, window);
        if (point.y < titleHeight) {
            return ControlAtPoint(window, point) == kControlNone ? HTCAPTION : HTCLIENT;
        }
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int control = ControlAtPoint(window, point);
        if (control != g_app.hoveredControl) {
            g_app.hoveredControl = control;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window;
        TrackMouseEvent(&tracking);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.hoveredControl = kControlNone;
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int control = ControlAtPoint(window, point);
        if (control == kControlCollapse) {
            ToggleCollapse(window);
        } else if (control == kControlHide) {
            HideNoteWindow(window);
        }
        return 0;
    }

    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == g_app.editor &&
            HIWORD(wParam) == EN_CHANGE) {
            SetTimer(window, kSaveTimer, 500, nullptr);
            return 0;
        }
        if (LOWORD(wParam) == kTrayShowCommand) {
            ShowNoteWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == kTrayAutoStartCommand) {
            ToggleAutoStart();
            return 0;
        }
        if (LOWORD(wParam) == kTrayExitCommand) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

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

    case kTrayMessage:
        switch (static_cast<UINT>(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowNoteWindow(window);
            return 0;
        case WM_RBUTTONUP:
            ShowTrayMenu(window);
            return 0;
        default:
            return 0;
        }

    case kShowMessage:
        ShowNoteWindow(window);
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

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (mutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClassName, nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, kShowMessage, 0, 0);
        }
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
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass) == 0) {
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
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClassName, kWindowTitle,
        WS_POPUP, left, top, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(kWindowClassName, instance);
        CloseHandle(mutex);
        return 1;
    }
    g_app.window = window;

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
    return static_cast<int>(message.wParam);
}
