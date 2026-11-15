#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "version.h"

namespace {

bool g_verbose = false;
bool g_logReady = false;
FILE* g_log = nullptr;

std::wstring g_exePath;
std::wstring g_exeDir;
std::wstring g_logPath;
bool g_pathsReady = false;

// Кириллица в консоли: пишем wide-строку напрямую в консольный буфер.
// Если stdout перенаправлен (файл, канал, Планировщик) — отдаём UTF-8 без BOM.
void WriteConsoleUtf8(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.c_str(), static_cast<DWORD>(text.size()), nullptr, nullptr);
        return;
    }

    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return;
    }
    std::string buf(static_cast<size_t>(need), '\0');
    int got = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                  &buf[0], need, nullptr, nullptr);
    if (got > 0) {
        fwrite(buf.data(), 1, static_cast<size_t>(got), stdout);
        fflush(stdout);
    }
}

void WriteLogFile(const std::wstring& text) {
    if (g_log == nullptr || text.empty()) {
        return;
    }

    // Метка времени в файле — по ней удобно сопоставлять запуски.
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    std::wstring line = stamp;
    line += text;
    line += L"\r\n";

    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return;
    }
    std::string buf(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                        &buf[0], need, nullptr, nullptr);

    fwrite(buf.data(), 1, buf.size(), g_log);
    fflush(g_log);
}

std::wstring Format(const wchar_t* fmt, va_list args) {
    wchar_t buf[8192];
    int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    if (n < 0) {
        buf[_countof(buf) - 1] = L'\0';
    }
    return std::wstring(buf);
}

// Полный путь к exe и его каталог. Кэшируется: вызывается из логов и из storage.
void EnsurePaths() {
    if (g_pathsReady) {
        return;
    }
    g_pathsReady = true;

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
        if (n == 0) {
            path = L"ScreenMemory.exe";
            break;
        }
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        if (path.size() >= 32768) {  // длиннее не бывает
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);  // мало места — удваиваем буфер
    }
    g_exePath = path;

    size_t pos = g_exePath.find_last_of(L"\\/");
    g_exeDir = (pos == std::wstring::npos) ? std::wstring() : g_exePath.substr(0, pos);
}

}  // namespace

namespace sm {

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) {
        return name;
    }
    wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') {
        return dir + name;
    }
    return dir + L"\\" + name;
}

const std::wstring& ExePath() {
    EnsurePaths();
    return g_exePath;
}

const std::wstring& ExeDir() {
    EnsurePaths();
    return g_exeDir;
}

const std::wstring& LogFilePath() {
    EnsurePaths();
    return g_logPath;
}

std::wstring Win32ErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

    std::wstring text;
    if (n != 0 && buf != nullptr) {
        text.assign(buf, n);
    }
    if (buf != nullptr) {
        LocalFree(buf);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' ||
                             text.back() == L'\t')) {
        text.pop_back();
    }

    wchar_t head[32];
    swprintf_s(head, L"%lu", code);
    if (text.empty()) {
        return std::wstring(L"код ") + head;
    }
    return std::wstring(head) + L": " + text;
}

std::wstring HrText(HRESULT hr) {
    wchar_t head[32];
    swprintf_s(head, L"0x%08lX", static_cast<unsigned long>(hr));
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return std::wstring(head) + L": " + Win32ErrorText(HRESULT_CODE(hr));
    }
    return head;
}

void InitLog(bool verbose) {
    g_verbose = verbose;

    if (!verbose || g_logReady) {
        return;
    }
    g_logReady = true;

    // Основное место — RAM-диск (как и объектники), чтобы не изнашивать SSD.
    std::wstring dir = L"T:\\ScreenMemory";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        dir = ExeDir();
    }

    g_logPath = JoinPath(dir, L"screenmemory.log");

    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"ab") != 0 || f == nullptr) {
        g_log = nullptr;
        g_logPath.clear();
        return;
    }
    g_log = f;

    // BOM только в новый файл — иначе в середине появятся мусорные байты.
    // Важно: в режиме "ab" позиция до первой записи = 0, поэтому сначала встаём в конец.
    if (_fseeki64(g_log, 0, SEEK_END) != 0 || _ftelli64(g_log) == 0) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        fwrite(bom, 1, sizeof(bom), g_log);
        fflush(g_log);
    }

    WriteLogFile(std::wstring(L"=== старт ScreenMemory ") + SM_VERSION_WSTRING + L" ===");
}

void ShutdownLog() {
    if (g_log != nullptr) {
        WriteLogFile(L"=== завершение ===");
        fclose(g_log);
        g_log = nullptr;
    }
}

bool IsVerbose() {
    return g_verbose;
}

void Info(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::wstring text = Format(fmt, args);
    va_end(args);

    WriteConsoleUtf8(text + L"\n");
    WriteLogFile(text);
}

void Warn(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::wstring text = Format(fmt, args);
    va_end(args);

    std::wstring line = L"ВНИМАНИЕ: ";
    line += text;

    WriteConsoleUtf8(line + L"\n");
    WriteLogFile(line);
}

void Error(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::wstring text = Format(fmt, args);
    va_end(args);

    std::wstring line = L"ОШИБКА: ";
    line += text;

    WriteConsoleUtf8(line + L"\n");
    WriteLogFile(line);
}

void Trace(const wchar_t* fmt, ...) {
    if (!g_verbose) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::wstring text = Format(fmt, args);
    va_end(args);

    WriteConsoleUtf8(text + L"\n");
    WriteLogFile(text);
}

void Logf(Level level, const wchar_t* fmt, ...) {
    if (level == Level::Trace && !g_verbose) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    std::wstring text = Format(fmt, args);
    va_end(args);

    std::wstring line;
    if (level == Level::Warn) {
        line = L"ВНИМАНИЕ: ";
    } else if (level == Level::Error) {
        line = L"ОШИБКА: ";
    }
    line += text;

    WriteConsoleUtf8(line + L"\n");
    WriteLogFile(line);
}

}  // namespace sm
