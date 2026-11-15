#include "admin_log.h"

#include <shellapi.h>

#include <string>
#include <vector>

#include "log.h"
#include "storage.h"

namespace sm {
namespace {

// Строки этого запуска. В файл они уходят одним куском в конце (WriteAdminLog):
// файл всегда переписывается целиком, поэтому склеить две сессии невозможно.
std::vector<std::wstring> g_lines;

std::wstring AdminLogPath() {
    return JoinPath(ExeDir(), L"admin.log");
}

// Команда должна оставаться одной строкой: переводы строк в ней (бывает в
// cmdline консоли) разорвали бы формат «одна строка — одна команда».
std::wstring OneLine(const std::wstring& text) {
    size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    std::wstring out;
    out.reserve(last - first + 1);
    for (size_t i = first; i <= last; ++i) {
        out += (text[i] == L'\r' || text[i] == L'\n') ? L' ' : text[i];
    }
    return out;
}

bool ToUtf8(const std::wstring& text, std::string* out, std::wstring* error) {
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        if (error != nullptr) {
            *error = L"WideCharToMultiByte: " + Win32ErrorText(GetLastError());
        }
        return false;
    }
    out->assign(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &(*out)[0], need,
                        nullptr, nullptr);
    return true;
}

// Перезапись файла целиком (CREATE_ALWAYS) + BOM: файл открывают в блокноте, а без
// BOM блокнот принимает UTF-8 за ANSI — кириллица превращается в мусор.
// Блокнот прошлого запуска держит файл с FILE_SHARE_READ|FILE_SHARE_WRITE, так что
// перезапись проходит даже когда старое окно ещё не закрыто.
bool WriteAllUtf8(const std::wstring& path, const std::vector<std::wstring>& lines,
                  std::wstring* error) {
    std::string payload;
    payload.reserve(256 + 128 * lines.size());
    payload += "\xEF\xBB\xBF";
    for (const std::wstring& line : lines) {
        std::string utf8;
        if (!ToUtf8(line, &utf8, error)) {
            return false;
        }
        payload += utf8;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = L"CreateFile: " + Win32ErrorText(GetLastError());
        }
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written,
                              nullptr) != FALSE &&
                    written == payload.size();
    if (!ok && error != nullptr) {
        *error = L"WriteFile: " + Win32ErrorText(GetLastError());
    }
    CloseHandle(file);
    return ok;
}

bool IsNotepadClass(const wchar_t* cls) {
    // «Notepad» в Windows 10, «NotepadWClass»/«Notepad» — в более новых сборках.
    return wcsstr(cls, L"Notepad") != nullptr;
}

BOOL CALLBACK CollectNotepadProc(HWND hwnd, LPARAM param) {
    auto* windows = reinterpret_cast<std::vector<HWND>*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls)));
    if (!IsNotepadClass(cls)) {
        return TRUE;
    }
    wchar_t title[MAX_PATH * 2] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
    if (wcsstr(title, L"admin.log") == nullptr) {
        return TRUE;
    }
    windows->push_back(hwnd);
    return TRUE;
}

// Закрыть окна блокнота с admin.log прошлого запуска. Иначе они копятся на экране,
// а в самом старом окне остаётся прошлая сессия: блокнот не перечитывает файл.
// Закрываем только класс «Notepad»: чужой редактор с похожим заголовком не трогаем.
int CloseOpenAdminLogWindows() {
    std::vector<HWND> windows;
    EnumWindows(CollectNotepadProc, reinterpret_cast<LPARAM>(&windows));
    if (windows.empty()) {
        return 0;
    }
    for (const HWND hwnd : windows) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    // Дать блокноту закрыться: файл не изменялся, диалога «сохранить?» не будет.
    for (int i = 0; i < 20; ++i) {
        bool alive = false;
        for (const HWND hwnd : windows) {
            if (IsWindow(hwnd) != FALSE) {
                alive = true;
                break;
            }
        }
        if (!alive) {
            break;
        }
        Sleep(50);
    }
    return static_cast<int>(windows.size());
}

}  // namespace

const wchar_t* AdminReasonText(AdminReason reason) {
    switch (reason) {
        case AdminReason::MoveBlocked:
            return L"move-blocked";
        case AdminReason::MoveDesktopFailed:
            return L"move-desktop-failed";
        case AdminReason::LaunchElevated:
            return L"launch-elevation-required";
        case AdminReason::LaunchFailed:
            return L"launch-failed";
        case AdminReason::LaunchTimeout:
            return L"launch-timeout";
        case AdminReason::Console:
            return L"console";
        case AdminReason::BrowserNoUrl:
            return L"browser-no-url";
        default:
            return L"no-command";
    }
}

void AddAdminEntry(AdminReason reason, const std::wstring& command) {
    const std::wstring line = OneLine(command);
    if (line.empty()) {
        // Печатать в журнале нечего: команда без пути к exe бесполезна.
        Trace(L"admin.log: %s — команда неизвестна, строка не записана",
              AdminReasonText(reason));
        return;
    }
    g_lines.push_back(line + L"\r\n");
    Trace(L"admin.log: %s | %s", AdminReasonText(reason), line.c_str());
}

int AdminEntryCount() {
    return static_cast<int>(g_lines.size());
}

bool WriteAdminLog(std::wstring* error) {
    const std::wstring path = AdminLogPath();
    if (g_lines.empty()) {
        // Журнал описывает последний запуск: записей нет — старого файла быть не должно,
        // иначе он выглядит как свежий и вводит в заблуждение.
        if (DeleteFileW(path.c_str()) != FALSE) {
            Trace(L"admin.log: записей нет, старый файл удалён");
            return true;
        }
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            return true;  // журнала и не было
        }
        // Файл держит блокнот прошлого запуска: закрываем наши окна и пробуем ещё раз,
        // иначе осталось бы окно со старой сессией, которой уже нет на диске.
        const int closed = CloseOpenAdminLogWindows();
        if (closed > 0) {
            Trace(L"admin.log: записей нет, закрыто окон блокнота: %d", closed);
        }
        if (DeleteFileW(path.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (error != nullptr) {
            *error = L"DeleteFile: " + Win32ErrorText(GetLastError());
        }
        return false;
    }
    if (!WriteAllUtf8(path, g_lines, error)) {
        return false;
    }
    Trace(L"admin.log: записан заново (%d строк)", AdminEntryCount());
    return true;
}

void OpenAdminLogIfNeeded() {
    if (g_lines.empty()) {
        return;
    }
    const int closed = CloseOpenAdminLogWindows();
    if (closed > 0) {
        Trace(L"admin.log: закрыто окон блокнота с прошлой сессией: %d", closed);
    }
    const std::wstring path = AdminLogPath();
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        const DWORD code = static_cast<DWORD>(reinterpret_cast<INT_PTR>(result));
        Warn(L"admin.log: открыть в блокноте не удалось (%s); файл: %s",
             Win32ErrorText(code).c_str(), path.c_str());
    }
}

}  // namespace sm
