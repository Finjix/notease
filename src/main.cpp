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
#include <dwmapi.h>
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
#include <limits>
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
constexpr int kNewButtonId = 2002;
constexpr int kCollapseButtonId = 2003;
constexpr int kHideButtonId = 2004;
constexpr int kDeleteButtonId = 2005;

constexpr UINT kTrayAutoStartCommand = 1002;
constexpr UINT kTrayExitCommand = 1003;

constexpr int kNormalWidth = 380;
constexpr int kNormalHeight = 280;
constexpr int kMinimumWidth = 260;
constexpr int kMinimumHeight = 180;
constexpr int kTitleBarHeight = 32;
constexpr int kTitleHoverWidth = 88;
constexpr int kResizeBorderWidth = 6;
constexpr DWORD kDwmBorderColorAttribute = 34;
constexpr DWORD kDwmColorNone = 0xfffffffe;

struct Settings {
    bool autoStart = true;
    bool alwaysOnTop = false;
    bool windowPositionValid = false;
    int windowLeft = 0;
    int windowTop = 0;
    bool windowSizeValid = false;
    int windowWidth = 0;
    int windowHeight = 0;
};

struct PersistedInstance {
    int id = 0;
    std::wstring note;
    Settings settings{};
};

struct WindowState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND editor = nullptr;
    WNDPROC editorWindowProc = nullptr;
    HWND newButton = nullptr;
    HWND collapseButton = nullptr;
    HWND hideButton = nullptr;
    HWND deleteButton = nullptr;
    WNDPROC newButtonWindowProc = nullptr;
    WNDPROC collapseButtonWindowProc = nullptr;
    WNDPROC hideButtonWindowProc = nullptr;
    WNDPROC deleteButtonWindowProc = nullptr;
    HFONT editorFont = nullptr;
    bool loadingEditor = false;
    bool mother = false;
    bool deleted = false;
    bool registered = false;
    int instanceId = 0;
    std::wstring initialText;
    Settings settings{};
};

struct AppState {
    HINSTANCE instance = nullptr;
    WindowState* mother = nullptr;
    std::vector<WindowState*> windows;
    std::vector<PersistedInstance> pendingInstances;
    int nextInstanceId = 1;
    HANDLE mutex = nullptr;
    NOTIFYICONDATAW tray{};
    UINT taskbarCreatedMessage = 0;
    bool trayAdded = false;
    bool loadingEditor = false;
    Settings settings{};
};

AppState g_app;
WindowState g_mother;
ULONG_PTR g_gdiplusToken = 0;

WindowState* GetWindowState(HWND window) {
    return reinterpret_cast<WindowState*>(GetWindowLongPtrW(
        window, GWLP_USERDATA));
}

WindowState* GetButtonWindowState(HWND button) {
    return GetWindowState(GetParent(button));
}

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

    WindowState* state = GetWindowState(GetParent(window));
    return state == nullptr || state->editorWindowProc == nullptr
               ? DefWindowProcW(window, message, wParam, lParam)
               : CallWindowProcW(state->editorWindowProc, window, message,
                                 wParam, lParam);
}

WNDPROC OriginalButtonWindowProc(HWND window) {
    WindowState* state = GetButtonWindowState(window);
    if (state == nullptr) {
        return nullptr;
    }
    if (window == state->newButton) {
        return state->newButtonWindowProc;
    }
    if (window == state->collapseButton) {
        return state->collapseButtonWindowProc;
    }
    if (window == state->hideButton) {
        return state->hideButtonWindowProc;
    }
    if (window == state->deleteButton) {
        return state->deleteButtonWindowProc;
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
    if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
        if (GetCapture() == window) {
            ReleaseCapture();
        }
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

size_t SkipJsonWhitespace(const std::string& json, size_t position) {
    while (position < json.size() &&
           isspace(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    return position;
}

bool AppendUtf8CodePoint(std::string& value, unsigned int codePoint) {
    if (codePoint <= 0x7f) {
        value += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7ff) {
        value += static_cast<char>(0xc0 | (codePoint >> 6));
        value += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else if (codePoint <= 0xffff) {
        value += static_cast<char>(0xe0 | (codePoint >> 12));
        value += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        value += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else if (codePoint <= 0x10ffff) {
        value += static_cast<char>(0xf0 | (codePoint >> 18));
        value += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
        value += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        value += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else {
        return false;
    }
    return true;
}

bool ParseJsonStringAt(const std::string& json, size_t& position,
                       std::string& value) {
    if (position >= json.size() || json[position++] != '"') return false;
    value.clear();
    while (position < json.size()) {
        const unsigned char character = static_cast<unsigned char>(json[position++]);
        if (character == '"') return true;
        if (character != '\\') {
            value += static_cast<char>(character);
            continue;
        }
        if (position >= json.size()) return false;
        const char escaped = json[position++];
        switch (escaped) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'u': {
            if (position + 4 > json.size()) return false;
            unsigned int codePoint = 0;
            for (int index = 0; index < 4; ++index) {
                const char digit = json[position++];
                codePoint <<= 4;
                if (digit >= '0' && digit <= '9') codePoint += digit - '0';
                else if (digit >= 'a' && digit <= 'f') codePoint += digit - 'a' + 10;
                else if (digit >= 'A' && digit <= 'F') codePoint += digit - 'A' + 10;
                else return false;
            }
            if (!AppendUtf8CodePoint(value, codePoint)) return false;
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool SkipJsonValue(const std::string& json, size_t& position) {
    position = SkipJsonWhitespace(json, position);
    if (position >= json.size()) {
        return false;
    }

    if (json[position] == '"') {
        std::string ignored;
        return ParseJsonStringAt(json, position, ignored);
    }

    if (json[position] == '{' || json[position] == '[') {
        const char opening = json[position++];
        const char closing = opening == '{' ? '}' : ']';
        while (true) {
            position = SkipJsonWhitespace(json, position);
            if (position >= json.size()) {
                return false;
            }
            if (json[position] == closing) {
                ++position;
                return true;
            }
            if (opening == '{') {
                std::string ignoredKey;
                if (!ParseJsonStringAt(json, position, ignoredKey)) {
                    return false;
                }
                position = SkipJsonWhitespace(json, position);
                if (position >= json.size() || json[position++] != ':') {
                    return false;
                }
            }
            if (!SkipJsonValue(json, position)) {
                return false;
            }
            position = SkipJsonWhitespace(json, position);
            if (position < json.size() && json[position] == ',') {
                ++position;
                continue;
            }
            if (position < json.size() && json[position] == closing) {
                ++position;
                return true;
            }
            return false;
        }
    }

    const size_t start = position;
    while (position < json.size() && json[position] != ',' &&
           json[position] != '}' && json[position] != ']') {
        ++position;
    }
    return position > start;
}

bool FindJsonFieldValue(const std::string& json, const std::string& key,
                        size_t& valuePosition) {
    size_t position = SkipJsonWhitespace(json, 0);
    if (position >= json.size() || json[position++] != '{') return false;
    while (true) {
        position = SkipJsonWhitespace(json, position);
        if (position >= json.size() || json[position] == '}') return false;
        std::string fieldName;
        if (!ParseJsonStringAt(json, position, fieldName)) return false;
        position = SkipJsonWhitespace(json, position);
        if (position >= json.size() || json[position++] != ':') return false;
        position = SkipJsonWhitespace(json, position);
        if (fieldName == key) {
            valuePosition = position;
            return true;
        }
        if (!SkipJsonValue(json, position)) return false;
        position = SkipJsonWhitespace(json, position);
        if (position < json.size() && json[position] == ',') {
            ++position;
            continue;
        }
        if (position < json.size() && json[position] == '}') {
            return false;
        }
        return false;
    }
}

bool ParseJsonStringField(const std::string& json, const std::string& key,
                          std::string& value) {
    size_t position = 0;
    return FindJsonFieldValue(json, key, position) &&
           ParseJsonStringAt(json, position, value);
}

bool ParseJsonBoolField(const std::string& json, const std::string& key, bool& value) {
    size_t position = 0;
    if (!FindJsonFieldValue(json, key, position)) return false;
    if (json.compare(position, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (json.compare(position, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool ParseJsonIntField(const std::string& json, const std::string& key, int& value) {
    size_t position = 0;
    if (!FindJsonFieldValue(json, key, position) || position >= json.size()) {
        return false;
    }

    const size_t start = position;
    if (json[position] == '-') {
        ++position;
    }
    const size_t digitsStart = position;
    while (position < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    if (position == digitsStart) return false;

    try {
        const long long parsed = std::stoll(json.substr(start, position - start));
        if (parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseJsonInstances(const std::string& json,
                        std::vector<PersistedInstance>& instances) {
    size_t position = 0;
    if (!FindJsonFieldValue(json, "instances", position)) {
        return true;
    }
    position = SkipJsonWhitespace(json, position);
    if (position >= json.size() || json[position++] != '[') {
        return false;
    }

    while (true) {
        position = SkipJsonWhitespace(json, position);
        if (position >= json.size()) {
            return false;
        }
        if (json[position] == ']') {
            return true;
        }
        const size_t objectStart = position;
        if (!SkipJsonValue(json, position) ||
            json[objectStart] != '{' || position <= objectStart) {
            return false;
        }

        const std::string object =
            json.substr(objectStart, position - objectStart);
        PersistedInstance instance;
        ParseJsonIntField(object, "id", instance.id);
        std::string note;
        if (ParseJsonStringField(object, "note", note)) {
            instance.note = Utf8ToWide(note);
        }
        ParseJsonBoolField(object, "alwaysOnTop", instance.settings.alwaysOnTop);
        ParseJsonBoolField(object, "windowPositionValid",
                           instance.settings.windowPositionValid);
        ParseJsonIntField(object, "windowLeft", instance.settings.windowLeft);
        ParseJsonIntField(object, "windowTop", instance.settings.windowTop);
        ParseJsonBoolField(object, "windowSizeValid",
                           instance.settings.windowSizeValid);
        ParseJsonIntField(object, "windowWidth", instance.settings.windowWidth);
        ParseJsonIntField(object, "windowHeight", instance.settings.windowHeight);
        if (instance.id > 0) {
            instances.push_back(std::move(instance));
        }

        position = SkipJsonWhitespace(json, position);
        if (position < json.size() && json[position] == ',') {
            ++position;
            continue;
        }
        if (position < json.size() && json[position] == ']') {
            return true;
        }
        return false;
    }
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
    if (ReadUtf8File(NoteaseFilePath(), json) &&
        ParseJsonStringField(json, "note", note)) {
        return Utf8ToWide(note);
    }
    return {};
}

bool SaveNoteaseFile(const std::wstring& note, const Settings& settings,
                     const std::vector<PersistedInstance>& instances,
                     std::wstring* errorMessage = nullptr) {
    const std::filesystem::path path = NoteaseFilePath();
    if (path.empty() || !EnsureParentDirectory(path)) {
        if (errorMessage != nullptr) *errorMessage = L"无法定位或创建 exe 所在目录。";
        return false;
    }
    std::filesystem::path temporaryPath = path;
    temporaryPath += L".tmp";
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (errorMessage != nullptr) *errorMessage = L"无法写入 " + path.wstring() + L"。";
        return false;
    }
    const std::string utf8 = WideToUtf8(note);
    file << "{\n  \"note\": \"" << JsonEscape(utf8)
         << "\",\n  \"autostart\": " << (settings.autoStart ? "true" : "false")
         << ",\n  \"alwaysOnTop\": "
         << (settings.alwaysOnTop ? "true" : "false")
         << ",\n  \"windowPositionValid\": "
         << (settings.windowPositionValid ? "true" : "false")
         << ",\n  \"windowLeft\": " << settings.windowLeft
         << ",\n  \"windowTop\": " << settings.windowTop
         << ",\n  \"windowSizeValid\": "
         << (settings.windowSizeValid ? "true" : "false")
         << ",\n  \"windowWidth\": " << settings.windowWidth
         << ",\n  \"windowHeight\": " << settings.windowHeight
         << ",\n  \"instances\": [";
    for (size_t index = 0; index < instances.size(); ++index) {
        const PersistedInstance& instance = instances[index];
        const Settings& instanceSettings = instance.settings;
        file << (index == 0 ? "\n" : ",\n")
             << "    {\n      \"id\": " << instance.id
             << ",\n      \"note\": \""
             << JsonEscape(WideToUtf8(instance.note))
             << "\",\n      \"alwaysOnTop\": "
             << (instanceSettings.alwaysOnTop ? "true" : "false")
             << ",\n      \"windowPositionValid\": "
             << (instanceSettings.windowPositionValid ? "true" : "false")
             << ",\n      \"windowLeft\": " << instanceSettings.windowLeft
             << ",\n      \"windowTop\": " << instanceSettings.windowTop
             << ",\n      \"windowSizeValid\": "
             << (instanceSettings.windowSizeValid ? "true" : "false")
             << ",\n      \"windowWidth\": " << instanceSettings.windowWidth
             << ",\n      \"windowHeight\": " << instanceSettings.windowHeight
             << "\n    }";
    }
    file << (instances.empty() ? "]\n}\n" : "\n  ]\n}\n");
    file.flush();
    if (!file) {
        file.close();
        DeleteFileW(temporaryPath.c_str());
        return false;
    }
    file.close();
    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }
    return true;
}

bool LoadSettings(Settings& settings) {
    settings = Settings{};
    std::string json;
    if (!ReadUtf8File(NoteaseFilePath(), json)) {
        return false;
    }

    bool autoStart = settings.autoStart;
    if (ParseJsonBoolField(json, "autostart", autoStart)) {
        settings.autoStart = autoStart;
    }
    bool alwaysOnTop = settings.alwaysOnTop;
    if (ParseJsonBoolField(json, "alwaysOnTop", alwaysOnTop)) {
        settings.alwaysOnTop = alwaysOnTop;
    }
    bool windowPositionValid = settings.windowPositionValid;
    if (ParseJsonBoolField(json, "windowPositionValid", windowPositionValid)) {
        settings.windowPositionValid = windowPositionValid;
    }
    int windowLeft = settings.windowLeft;
    if (ParseJsonIntField(json, "windowLeft", windowLeft)) {
        settings.windowLeft = windowLeft;
    }
    int windowTop = settings.windowTop;
    if (ParseJsonIntField(json, "windowTop", windowTop)) {
        settings.windowTop = windowTop;
    }
    bool windowSizeValid = settings.windowSizeValid;
    if (ParseJsonBoolField(json, "windowSizeValid", windowSizeValid)) {
        settings.windowSizeValid = windowSizeValid;
    }
    int windowWidth = settings.windowWidth;
    if (ParseJsonIntField(json, "windowWidth", windowWidth)) {
        settings.windowWidth = windowWidth;
    }
    int windowHeight = settings.windowHeight;
    if (ParseJsonIntField(json, "windowHeight", windowHeight)) {
        settings.windowHeight = windowHeight;
    }
    ParseJsonInstances(json, g_app.pendingInstances);
    for (const PersistedInstance& instance : g_app.pendingInstances) {
        g_app.nextInstanceId = std::max(g_app.nextInstanceId, instance.id + 1);
    }
    return !json.empty();
}

std::wstring ReadEditorText(HWND editor) {
    if (editor == nullptr) return {};

    const int length = GetWindowTextLengthW(editor);
    std::wstring content(length + 1, L'\0');
    GetWindowTextW(editor, content.data(), length + 1);
    content.resize(length);
    return content;
}

void CaptureWindowGeometry(WindowState* state);
void SaveCurrentWindowState(HWND window);

bool SaveSettings(const Settings& settings, std::wstring* errorMessage = nullptr) {
    WindowState* mother = g_app.mother;
    if (mother == nullptr) {
        return false;
    }

    CaptureWindowGeometry(mother);
    Settings savedSettings = settings;
    savedSettings.windowPositionValid = mother->settings.windowPositionValid;
    savedSettings.windowLeft = mother->settings.windowLeft;
    savedSettings.windowTop = mother->settings.windowTop;
    savedSettings.windowSizeValid = mother->settings.windowSizeValid;
    savedSettings.windowWidth = mother->settings.windowWidth;
    savedSettings.windowHeight = mother->settings.windowHeight;

    std::vector<PersistedInstance> instances;
    for (WindowState* state : g_app.windows) {
        if (state == nullptr || state->mother || state->deleted) {
            continue;
        }
        CaptureWindowGeometry(state);
        PersistedInstance instance;
        instance.id = state->instanceId;
        instance.note = ReadEditorText(state->editor);
        instance.settings = state->settings;
        instances.push_back(std::move(instance));
    }
    return SaveNoteaseFile(ReadEditorText(mother->editor), savedSettings,
                           instances, errorMessage);
}

bool SaveNoteText(WindowState* state) {
    if (state == nullptr || state->editor == nullptr) {
        return false;
    }
    SaveCurrentWindowState(state->window);
    return true;
}

void CaptureWindowGeometry(WindowState* state) {
    if (state == nullptr || state->window == nullptr) {
        return;
    }
    RECT windowRectangle{};
    if (!GetWindowRect(state->window, &windowRectangle)) {
        return;
    }

    state->settings.windowPositionValid = true;
    state->settings.windowLeft = windowRectangle.left;
    state->settings.windowTop = windowRectangle.top;
    state->settings.windowSizeValid = true;
    state->settings.windowWidth = windowRectangle.right - windowRectangle.left;
    state->settings.windowHeight = windowRectangle.bottom - windowRectangle.top;
}

std::vector<PersistedInstance> SnapshotInstances() {
    std::vector<PersistedInstance> instances;
    for (WindowState* state : g_app.windows) {
        if (state == nullptr || state->mother || state->deleted) {
            continue;
        }
        PersistedInstance instance;
        instance.id = state->instanceId;
        instance.note = ReadEditorText(state->editor);
        instance.settings = state->settings;
        instances.push_back(std::move(instance));
    }
    return instances;
}

void SaveAllWindowStates() {
    WindowState* mother = g_app.mother;
    if (mother == nullptr) {
        return;
    }
    for (WindowState* state : g_app.windows) {
        if (state != nullptr && !state->deleted) {
            CaptureWindowGeometry(state);
        }
    }
    SaveNoteaseFile(ReadEditorText(mother->editor), mother->settings,
                    SnapshotInstances());
}

void SaveCurrentWindowState(HWND window) {
    CaptureWindowGeometry(GetWindowState(window));
    SaveAllWindowStates();
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
    MessageBoxW(g_app.mother == nullptr ? nullptr : g_app.mother->window,
                message.c_str(), L"Notease", MB_OK | MB_ICONWARNING);
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

void DisableDwmBorder(HWND window) {
    const DWORD color = kDwmColorNone;
    DwmSetWindowAttribute(window, kDwmBorderColorAttribute, &color,
                          sizeof(color));
}

void LayoutEditor(HWND window) {
    WindowState* state = GetWindowState(window);
    if (state == nullptr || state->editor == nullptr) {
        return;
    }

    ShowWindow(state->editor, SW_SHOW);

    RECT client{};
    GetClientRect(window, &client);
    const int padding = ScaleForDpi(3, window);
    const int top = ScaleForDpi(kTitleBarHeight, window) + padding;
    MoveWindow(state->editor, padding, top, client.right - padding * 2,
               client.bottom - top - padding, TRUE);

    RECT editorClient{};
    GetClientRect(state->editor, &editorClient);
    const int textMargin = ScaleForDpi(8, window);
    RECT textRectangle{textMargin, 0, editorClient.right - textMargin,
                       editorClient.bottom};
    SendMessageW(state->editor, EM_SETRECTNP, 0,
                 reinterpret_cast<LPARAM>(&textRectangle));
}

void LayoutButtons(HWND window) {
    WindowState* state = GetWindowState(window);
    if (state == nullptr) {
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

    if (state->mother) {
        MoveWindow(state->newButton,
                   right - buttonWidth * 3 - gap * 2, top, buttonWidth,
                   buttonHeight, TRUE);
        MoveWindow(state->collapseButton,
                   right - buttonWidth * 2 - gap, top, buttonWidth,
                   buttonHeight, TRUE);
        MoveWindow(state->hideButton, right - buttonWidth, top, buttonWidth,
                   buttonHeight, TRUE);
    } else {
        MoveWindow(state->deleteButton, right - buttonWidth, top, buttonWidth,
                   buttonHeight, TRUE);
    }
}

void LayoutControls(HWND window) {
    LayoutEditor(window);
    LayoutButtons(window);
}

void UpdateEditorFont(HWND window) {
    WindowState* state = GetWindowState(window);
    if (state == nullptr || state->editor == nullptr) {
        return;
    }

    HFONT font = CreateFontW(
        -ScaleForDpi(16, window), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (font == nullptr) {
        return;
    }

    HFONT previous = state->editorFont;
    state->editorFont = font;
    SendMessageW(state->editor, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND button : {state->newButton, state->collapseButton,
                        state->hideButton, state->deleteButton}) {
        if (button != nullptr) {
            SendMessageW(button, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
    if (previous != nullptr) {
        DeleteObject(previous);
    }
}

void ShowNoteWindow(HWND window) {
    WindowState* state = GetWindowState(window);
    if (state == nullptr) {
        return;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    SetWindowPos(window, state->settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    // Activate the mother window before restoring child windows. Activating it
    // after the children would move it above them in non-topmost mode.
    SetForegroundWindow(window);
    if (state->mother) {
        for (WindowState* child : g_app.windows) {
            if (child != nullptr && !child->mother && !child->deleted) {
                ShowWindow(child->window, SW_SHOWNOACTIVATE);
                SetWindowPos(child->window,
                             child->settings.alwaysOnTop ? HWND_TOPMOST
                                                         : HWND_NOTOPMOST,
                             0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW |
                                 SWP_NOACTIVATE);
            }
        }
    }
}

void HideNoteWindow(HWND window) {
    WindowState* state = GetWindowState(window);
    if (state == nullptr) {
        return;
    }
    SaveCurrentWindowState(window);
    if (state->mother) {
        for (WindowState* child : g_app.windows) {
            if (child != nullptr && !child->mother && !child->deleted) {
                ShowWindow(child->window, SW_HIDE);
            }
        }
    }
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
    WindowState* mother = g_app.mother;
    if (mother == nullptr) {
        return;
    }
    const bool previous = mother->settings.autoStart;
    const bool desired = !previous;
    std::wstring error;
    if (!SetAutoStartEnabled(desired, &error)) {
        ShowError(error);
        return;
    }

    Settings next = mother->settings;
    next.autoStart = desired;
    if (!SaveSettings(next, &error)) {
        SetAutoStartEnabled(previous);
        ShowError(error);
        return;
    }

    mother->settings = next;
}

void ToggleAlwaysOnTop(HWND window) {
    WindowState* mother = g_app.mother;
    if (mother == nullptr) {
        return;
    }
    const bool previous = mother->settings.alwaysOnTop;
    Settings next = mother->settings;
    next.alwaysOnTop = !previous;
    for (WindowState* state : g_app.windows) {
        if (state != nullptr && !state->mother && !state->deleted) {
            state->settings.alwaysOnTop = next.alwaysOnTop;
        }
    }
    std::wstring error;
    if (!SaveSettings(next, &error)) {
        for (WindowState* state : g_app.windows) {
            if (state != nullptr && !state->mother && !state->deleted) {
                state->settings.alwaysOnTop = previous;
            }
        }
        ShowError(error);
        return;
    }

    mother->settings = next;
    ApplyAlwaysOnTop(window, next.alwaysOnTop);
    RedrawButton(mother->collapseButton);
    for (WindowState* state : g_app.windows) {
        if (state != nullptr && !state->mother && !state->deleted) {
            ApplyAlwaysOnTop(state->window, next.alwaysOnTop);
        }
    }
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
        (g_app.mother != nullptr && g_app.mother->settings.autoStart)
            ? L"自启动√"
            : L"自启动×";
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
    WindowState* state = GetWindowState(GetParent(item.hwndItem));
    const COLORREF background = RGB(248, 222, 116);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(deviceContext, &rectangle, brush);
    DeleteObject(brush);

    const bool pinIcon = item.CtlID == kCollapseButtonId;
    const bool plusIcon = item.CtlID == kNewButtonId;
    const bool trashIcon = item.CtlID == kDeleteButtonId;
    const bool symbolIcon = pinIcon || plusIcon || trashIcon;
    HFONT font = CreateFontW(
        -ScaleForDpi(symbolIcon ? 16 : 12, item.hwndItem), 0, 0, 0,
        pinIcon ? FW_BOLD : (emphasized ? FW_SEMIBOLD : FW_NORMAL),
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        symbolIcon ? L"Segoe MDL2 Assets" : L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(deviceContext, font);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(86, 67, 20));
    RECT textRectangle = rectangle;
    if (pinIcon) {
        const int centerX = (textRectangle.left + textRectangle.right) / 2;
        const int centerY = (textRectangle.top + textRectangle.bottom) / 2;
        constexpr double diagonalAngle = 0.7853981633974483;
        const double angle = state != nullptr && state->settings.alwaysOnTop
                                 ? 0.0
                                 : diagonalAngle;
        const double sine = std::sin(angle);
        const double cosine = std::cos(angle);
        // Keep the custom shape at a stable visual size. The button itself
        // still scales with DPI, while scaling this shape again made it too
        // large on high-DPI displays.
        constexpr double scale = 0.90;
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
    } else if (plusIcon) {
        const int lineWidth = ScaleForDpi(14, item.hwndItem);
        const int lineHeight = ScaleForDpi(2, item.hwndItem);
        const int centerX = (textRectangle.left + textRectangle.right) / 2;
        const int centerY = (textRectangle.top + textRectangle.bottom) / 2;
        RECT lineRectangle{centerX - lineWidth / 2, centerY - lineHeight / 2,
                           centerX + lineWidth / 2,
                           centerY + (lineHeight + 1) / 2};
        HBRUSH lineBrush = CreateSolidBrush(RGB(86, 67, 20));
        FillRect(deviceContext, &lineRectangle, lineBrush);
        RECT verticalRectangle{centerX - lineHeight / 2, centerY - lineWidth / 2,
                               centerX + (lineHeight + 1) / 2,
                               centerY + lineWidth / 2};
        FillRect(deviceContext, &verticalRectangle, lineBrush);
        DeleteObject(lineBrush);
    } else if (trashIcon) {
        DrawTextW(deviceContext, text.c_str(), -1, &textRectangle,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else if (emphasized) {
        const int lineWidth = ScaleForDpi(14, item.hwndItem);
        const int lineHeight = ScaleForDpi(2, item.hwndItem);
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
    WindowState* state = GetWindowState(window);
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH bodyBrush = CreateSolidBrush(RGB(255, 250, 221));
    FillRect(deviceContext, &client, bodyBrush);
    DeleteObject(bodyBrush);

    RECT titleRectangle = client;
    titleRectangle.bottom = ScaleForDpi(kTitleBarHeight, window);
    const COLORREF titleBackground = RGB(248, 222, 116);
    HBRUSH titleBrush = CreateSolidBrush(titleBackground);
    FillRect(deviceContext, &titleRectangle, titleBrush);
    DeleteObject(titleBrush);

    if (state == nullptr || state->mother) {
        HFONT titleFont = CreateFontW(
            -ScaleForDpi(14, window), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
        HGDIOBJ oldFont = SelectObject(deviceContext, titleFont);

        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, RGB(77, 59, 16));
        RECT titleText{ScaleForDpi(10, window), 0, titleRectangle.right,
                       titleRectangle.bottom};
        DrawTextW(deviceContext, L"Notease", -1, &titleText,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(deviceContext, oldFont);
        DeleteObject(titleFont);
    }

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
    WindowState* state = GetWindowState(window);
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        if (create == nullptr || create->lpCreateParams == nullptr) {
            return FALSE;
        }
        state = reinterpret_cast<WindowState*>(create->lpCreateParams);
        state->window = window;
        state->registered = true;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        g_app.windows.push_back(state);
        return TRUE;
    }

    if (g_app.taskbarCreatedMessage != 0 &&
        message == g_app.taskbarCreatedMessage && state != nullptr &&
        state->mother) {
        AddTrayIcon(window);
        return 0;
    }

    switch (message) {
    case WM_CREATE: {
        if (state == nullptr) {
            return -1;
        }
        // Setting the initial text sends EN_CHANGE. Ignore that notification
        // so startup cannot schedule a save before the editor is initialized.
        state->loadingEditor = true;
        state->editor = CreateWindowExW(
            0, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorControlId)),
            g_app.instance,
            nullptr);
        if (state->editor == nullptr) {
            return -1;
        }
        state->editorWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            state->editor, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(EditorWindowProc)));

        if (state->mother) {
            state->newButton = CreateWindowExW(
                0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNewButtonId)),
                g_app.instance, nullptr);
            state->collapseButton = CreateWindowExW(
                0, L"BUTTON", L"\xE841",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCollapseButtonId)),
                g_app.instance, nullptr);
            state->hideButton = CreateWindowExW(
                0, L"BUTTON", L"一", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHideButtonId)),
                g_app.instance, nullptr);
            if (state->newButton == nullptr || state->collapseButton == nullptr ||
                state->hideButton == nullptr) {
                return -1;
            }
        } else {
            state->deleteButton = CreateWindowExW(
                0, L"BUTTON", L"\xE74D",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeleteButtonId)),
                g_app.instance, nullptr);
            if (state->deleteButton == nullptr) {
                return -1;
            }
        }

        for (HWND button : {state->newButton, state->collapseButton,
                            state->hideButton, state->deleteButton}) {
            if (button == nullptr) {
                continue;
            }
            WNDPROC original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                button, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(ButtonWindowProc)));
            if (button == state->newButton) state->newButtonWindowProc = original;
            if (button == state->collapseButton) {
                state->collapseButtonWindowProc = original;
            }
            if (button == state->hideButton) state->hideButtonWindowProc = original;
            if (button == state->deleteButton) {
                state->deleteButtonWindowProc = original;
            }
        }
        SendMessageW(state->editor, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        UpdateEditorFont(window);
        SetWindowTextW(state->editor, state->initialText.c_str());
        state->initialText.clear();
        state->loadingEditor = false;
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

    case WM_EXITSIZEMOVE:
        // Persist the final position and size as soon as an interactive move
        // or resize ends. WM_CLOSE is not guaranteed for hiding, logoff, or
        // a system restart.
        KillTimer(window, kSaveTimer);
        SaveCurrentWindowState(window);
        return 0;

    case WM_QUERYENDSESSION:
        SaveCurrentWindowState(window);
        return TRUE;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            SaveCurrentWindowState(window);
        }
        return 0;

    case WM_NCACTIVATE:
        return TRUE;

    case WM_NCPAINT:
        return 0;

    case WM_NCCALCSIZE:
        return 0;

    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        RECT client{};
        GetClientRect(window, &client);
        const int resizeBorder = ScaleForDpi(kResizeBorderWidth, window);
        const bool resizeLeft = point.x < client.left + resizeBorder;
        const bool resizeRight = point.x >= client.right - resizeBorder;
        const bool resizeTop = point.y < client.top + resizeBorder;
        const bool resizeBottom = point.y >= client.bottom - resizeBorder;
        if (resizeTop && resizeLeft) return HTTOPLEFT;
        if (resizeTop && resizeRight) return HTTOPRIGHT;
        if (resizeBottom && resizeLeft) return HTBOTTOMLEFT;
        if (resizeBottom && resizeRight) return HTBOTTOMRIGHT;
        if (resizeLeft) return HTLEFT;
        if (resizeRight) return HTRIGHT;
        if (resizeTop) return HTTOP;
        if (resizeBottom) return HTBOTTOM;

        const int titleHeight = ScaleForDpi(kTitleBarHeight, window);
        if (point.y < titleHeight) {
            if (point.x >= ScaleForDpi(10, window) &&
                point.x < ScaleForDpi(kTitleHoverWidth, window)) {
                return HTCLIENT;
            }
            HWND child = ChildWindowFromPointEx(window, point,
                                                CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (child == state->newButton || child == state->collapseButton ||
                child == state->hideButton || child == state->deleteButton) {
                return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        if (limits != nullptr) {
            limits->ptMinTrackSize.x = ScaleForDpi(kMinimumWidth, window);
            limits->ptMinTrackSize.y = ScaleForDpi(kMinimumHeight, window);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        const int titleHeight = ScaleForDpi(kTitleBarHeight, window);
        const int titleClickWidth = ScaleForDpi(kTitleHoverWidth, window);
        if (GET_Y_LPARAM(lParam) < titleHeight &&
            GET_X_LPARAM(lParam) >= ScaleForDpi(10, window) &&
            GET_X_LPARAM(lParam) < titleClickWidth) {
            SetWindowTextW(state->editor, L"");
            SaveNoteText(state);
            SetFocus(state->editor);
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == state->editor &&
            HIWORD(wParam) == EN_CHANGE) {
            if (state->loadingEditor) {
                return 0;
            }
            SetTimer(window, kSaveTimer, 500, nullptr);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == state->newButton) {
            WindowState* newState = new WindowState;
            newState->instance = g_app.instance;
            newState->instanceId = g_app.nextInstanceId++;
            newState->settings = g_app.mother->settings;
            newState->settings.windowPositionValid = false;
            newState->settings.windowSizeValid = false;
            newState->initialText.clear();
            RECT motherRectangle{};
            GetWindowRect(window, &motherRectangle);
            const int offset = ScaleForDpi(
                24 * static_cast<int>(std::max<size_t>(1, g_app.windows.size())),
                window);
            const int width = ScaleForDpi(kNormalWidth, window);
            const int height = ScaleForDpi(kNormalHeight, window);
            newState->settings.windowPositionValid = true;
            newState->settings.windowLeft = motherRectangle.left + offset;
            newState->settings.windowTop = motherRectangle.top + offset;
            newState->settings.windowSizeValid = true;
            newState->settings.windowWidth = width;
            newState->settings.windowHeight = height;

            HWND newWindow = CreateWindowExW(
                WS_EX_TOOLWINDOW, kWindowClassName, kWindowTitle,
                WS_POPUP | WS_THICKFRAME, newState->settings.windowLeft,
                newState->settings.windowTop, width, height, nullptr, nullptr,
                g_app.instance, newState);
            if (newWindow == nullptr) {
                if (!newState->registered) {
                    delete newState;
                }
                ShowError(L"无法创建新的便签实例。");
                return 0;
            }
            DisableDwmBorder(newWindow);
            ApplyAlwaysOnTop(newWindow, newState->settings.alwaysOnTop);
            ShowWindow(newWindow, SW_SHOWNORMAL);
            UpdateWindow(newWindow);
            SetForegroundWindow(newWindow);
            SaveAllWindowStates();
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == state->collapseButton) {
            PostMessageW(window, kToggleAlwaysOnTopMessage, 0, 0);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == state->hideButton) {
            HideNoteWindow(window);
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == state->deleteButton) {
            state->deleted = true;
            KillTimer(window, kSaveTimer);
            SaveAllWindowStates();
            DestroyWindow(window);
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
            SaveCurrentWindowState(window);
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
        if (item->CtlID == kNewButtonId) {
            PaintButton(*item, L"+", false);
            return TRUE;
        }
        if (item->CtlID == kCollapseButtonId) {
            PaintButton(*item, L"\xE841", false);
            return TRUE;
        }
        if (item->CtlID == kHideButtonId) {
            PaintButton(*item, L"一", true);
            return TRUE;
        }
        if (item->CtlID == kDeleteButtonId) {
            PaintButton(*item, L"\xE74D", false);
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

    case WM_CLOSE: {
        KillTimer(window, kSaveTimer);
        if (state != nullptr && state->mother) {
            SaveAllWindowStates();
            std::vector<HWND> childWindows;
            for (WindowState* child : g_app.windows) {
                if (child != nullptr && !child->mother &&
                    child->window != nullptr) {
                    childWindows.push_back(child->window);
                }
            }
            for (HWND child : childWindows) {
                DestroyWindow(child);
            }
        } else {
            SaveCurrentWindowState(window);
        }
        DestroyWindow(window);
        return 0;
    }

    case WM_DESTROY:
        if (state != nullptr && state->mother) {
            RemoveTrayIcon();
            if (g_app.mutex != nullptr) {
                CloseHandle(g_app.mutex);
                g_app.mutex = nullptr;
            }
            PostQuitMessage(0);
        }
        return 0;

    case WM_NCDESTROY: {
        if (state != nullptr) {
            auto iterator = std::find(g_app.windows.begin(), g_app.windows.end(),
                                      state);
            if (iterator != g_app.windows.end()) {
                g_app.windows.erase(iterator);
            }
            if (state->editorFont != nullptr) {
                DeleteObject(state->editorFont);
                state->editorFont = nullptr;
            }
            if (state->mother) {
                g_app.mother = nullptr;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            if (!state->mother) {
                delete state;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

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
    g_mother = WindowState{};
    g_mother.instance = instance;
    g_mother.mother = true;
    g_mother.settings = settings;
    g_mother.initialText = LoadNoteText();
    g_app.mother = &g_mother;

    std::wstring startupWarning;
    if (!SetAutoStartEnabled(settings.autoStart, &startupWarning)) {
        startupWarning = L"自启动设置同步失败：" + startupWarning;
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

    const int minimumWidth = ScaleForDpi(kMinimumWidth, nullptr);
    const int minimumHeight = ScaleForDpi(kMinimumHeight, nullptr);
    int width = ScaleForDpi(kNormalWidth, nullptr);
    int height = ScaleForDpi(kNormalHeight, nullptr);
    if (settings.windowSizeValid) {
        width = std::max(minimumWidth, settings.windowWidth);
        height = std::max(minimumHeight, settings.windowHeight);
    }
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int left = (screenWidth - width) / 2;
    int top = (screenHeight - height) / 2;
    if (settings.windowPositionValid) {
        left = settings.windowLeft;
        top = settings.windowTop;
    }

    RECT savedRectangle{left, top, left + width, top + height};
    HMONITOR monitor = MonitorFromRect(&savedRectangle, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        const RECT workArea = monitorInfo.rcWork;
        const int workLeft = static_cast<int>(workArea.left);
        const int workTop = static_cast<int>(workArea.top);
        const int workRight = static_cast<int>(workArea.right);
        const int workBottom = static_cast<int>(workArea.bottom);
        width = std::min(width, std::max(minimumWidth, workRight - workLeft));
        height = std::min(height, std::max(minimumHeight, workBottom - workTop));
        const int maxLeft = std::max(workLeft, workRight - width);
        const int maxTop = std::max(workTop, workBottom - height);
        left = std::clamp(left, workLeft, maxLeft);
        top = std::clamp(top, workTop, maxTop);
    }

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClassName, kWindowTitle,
        WS_POPUP | WS_THICKFRAME, left, top, width, height, nullptr, nullptr,
        instance, &g_mother);
    if (window == nullptr) {
        UnregisterClassW(kWindowClassName, instance);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }
    DisableDwmBorder(window);
    ApplyAlwaysOnTop(window, settings.alwaysOnTop);

    if (!AddTrayIcon(window)) {
        MessageBoxW(window, L"无法创建通知区域图标，程序仍会运行。", L"Notease",
                    MB_OK | MB_ICONWARNING);
    }

    ShowWindow(window, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window);

    for (const PersistedInstance& persisted : g_app.pendingInstances) {
        WindowState* child = new WindowState;
        child->instance = g_app.instance;
        child->mother = false;
        child->instanceId = persisted.id;
        child->settings = persisted.settings;
        child->settings.alwaysOnTop = settings.alwaysOnTop;
        child->initialText = persisted.note;
        if (!child->settings.windowPositionValid) {
            child->settings.windowPositionValid = true;
            child->settings.windowLeft = left +
                ScaleForDpi(24 * static_cast<int>(g_app.windows.size()), window);
            child->settings.windowTop = top +
                ScaleForDpi(24 * static_cast<int>(g_app.windows.size()), window);
        }
        if (!child->settings.windowSizeValid) {
            child->settings.windowSizeValid = true;
            child->settings.windowWidth = width;
            child->settings.windowHeight = height;
        }
        HWND childWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW, kWindowClassName, kWindowTitle,
            WS_POPUP | WS_THICKFRAME, child->settings.windowLeft,
            child->settings.windowTop, child->settings.windowWidth,
            child->settings.windowHeight, nullptr, nullptr, g_app.instance,
            child);
        if (childWindow == nullptr) {
            if (!child->registered) {
                delete child;
            }
            continue;
        }
        DisableDwmBorder(childWindow);
        ApplyAlwaysOnTop(childWindow, child->settings.alwaysOnTop);
        ShowWindow(childWindow, SW_SHOWNORMAL);
        UpdateWindow(childWindow);
    }
    g_app.pendingInstances.clear();
    SaveAllWindowStates();

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
