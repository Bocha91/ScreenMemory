#include "scheduler.h"

#include <sddl.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#include "log.h"

namespace sm {

const wchar_t* const kAutoSaveTaskName = L"ScreenMemory AutoSave";
const wchar_t* const kAutoRestoreTaskName = L"ScreenMemory AutoRestore";
const wchar_t* const kRunValueName = L"ScreenMemory";

namespace {

const wchar_t* const kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Максимальные значения — те же, что проверяет UI Планировщика.
constexpr int kMaxIntervalMinutes = 24 * 60;
constexpr int kMaxDelaySeconds = 60 * 60;

std::wstring TrimText(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

// schtasks.exe берём из системного каталога: подменённый файл из PATH запускать нельзя.
std::wstring SchtasksPath() {
    wchar_t dir[MAX_PATH + 1] = {};
    const UINT length = GetSystemDirectoryW(dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"schtasks.exe";
    }
    return JoinPath(dir, L"schtasks.exe");
}

// Вывод внешней программы в консольной кодировке (OEM), при неудаче — ANSI.
std::wstring DecodeConsoleBytes(const std::string& bytes) {
    if (bytes.empty()) {
        return std::wstring();
    }
    int length = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()),
                                     nullptr, 0);
    UINT codePage = CP_OEMCP;
    if (length <= 0) {
        codePage = CP_ACP;
        length = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
                                     nullptr, 0);
    }
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring text(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(),
                        length);
    return text;
}

// Последняя непустая строка: schtasks печатает «ОШИБКА: …», потом пустую строку.
std::wstring LastMeaningfulLine(const std::wstring& text) {
    std::wstring best;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t newline = text.find(L'\n', pos);
        const std::wstring line = TrimText(
            text.substr(pos, (newline == std::wstring::npos) ? std::wstring::npos : newline - pos));
        if (!line.empty()) {
            best = line;
        }
        pos = (newline == std::wstring::npos) ? text.size() + 1 : newline + 1;
    }
    return best;
}

// Запуск внешней программы без окна с захватом stdout+stderr. Командная строка
// передаётся как есть (CreateProcessW разбирает её сам), код возврата — в exitCode.
bool RunCapture(const std::wstring& commandLine, DWORD* exitCode, std::wstring* output,
                std::wstring* error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
        if (error != nullptr) {
            *error = L"не удалось создать канал для schtasks: " + Win32ErrorText(GetLastError());
        }
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;

    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD startError = GetLastError();
    // Свой конец канала закрываем сразу: иначе чтение не увидит конец вывода.
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        if (error != nullptr) {
            *error = L"не удалось запустить " + commandLine + L": " +
                     Win32ErrorText(startError);
        }
        return false;
    }

    std::string bytes;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        bytes.append(chunk, got);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != nullptr) {
        *exitCode = code;
    }
    if (output != nullptr) {
        *output = DecodeConsoleBytes(bytes);
    }
    return true;
}

std::wstring XmlEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        switch (ch) {
            case L'&': out.append(L"&amp;"); break;
            case L'<': out.append(L"&lt;"); break;
            case L'>': out.append(L"&gt;"); break;
            case L'"': out.append(L"&quot;"); break;
            case L'\'': out.append(L"&apos;"); break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

// SID текущего пользователя: Планировщику нужен именно он, иначе задача «при входе»
// привязывается к несуществующему пользователю и регистрация отклоняется.
bool CurrentUserSid(std::wstring* sid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return false;
    }
    std::vector<BYTE> buffer(size);
    const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) {
        return false;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &text)) {
        return false;
    }
    *sid = text;
    LocalFree(text);
    return true;
}

std::wstring IsoTimeText(const SYSTEMTIME& st) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buffer;
}

// Момент первого запуска: «сейчас + интервал», чтобы первый save не сработал сразу
// после установки (StartWhenAvailable иначе догонит «пропущенный» запуск).
std::wstring IsoStartBoundary(int addMinutes) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) {
        return IsoTimeText(st);
    }
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    value.QuadPart += static_cast<ULONGLONG>(addMinutes) * 60ULL * 10000000ULL;
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    SYSTEMTIME shifted{};
    if (!FileTimeToSystemTime(&ft, &shifted)) {
        return IsoTimeText(st);
    }
    return IsoTimeText(shifted);
}

std::wstring IsoInterval(int minutes) {
    if (minutes % (24 * 60) == 0 && minutes >= 24 * 60) {
        return L"P" + std::to_wstring(minutes / (24 * 60)) + L"D";
    }
    if (minutes % 60 == 0 && minutes >= 60) {
        return L"PT" + std::to_wstring(minutes / 60) + L"H";
    }
    return L"PT" + std::to_wstring(minutes) + L"M";
}

std::wstring IsoDelay(int seconds) {
    if (seconds <= 0) {
        return std::wstring();
    }
    if (seconds % 60 == 0) {
        return L"PT" + std::to_wstring(seconds / 60) + L"M";
    }
    return L"PT" + std::to_wstring(seconds) + L"S";
}

std::wstring FormatIntervalText(int minutes) {
    if (minutes % 60 == 0 && minutes >= 60) {
        return L"каждые " + std::to_wstring(minutes / 60) + L" ч";
    }
    return L"каждые " + std::to_wstring(minutes) + L" мин";
}

std::wstring BuildTaskXml(const std::wstring& description, const std::wstring& triggers,
                          const std::wstring& arguments, const std::wstring& sid,
                          const wchar_t* executionTimeLimit) {
    const std::wstring exe = ExePath();
    std::wstring xml;
    xml.reserve(2048);
    xml.append(L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n");
    xml.append(L"<Task version=\"1.2\" "
               L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
    xml.append(L"  <RegistrationInfo>\r\n    <Description>");
    xml.append(XmlEscape(description));
    xml.append(L"</Description>\r\n  </RegistrationInfo>\r\n  <Triggers>\r\n");
    xml.append(triggers);
    xml.append(L"  </Triggers>\r\n  <Principals>\r\n    <Principal id=\"Author\">\r\n");
    xml.append(L"      <UserId>");
    xml.append(XmlEscape(sid));
    xml.append(L"</UserId>\r\n      <LogonType>InteractiveToken</LogonType>\r\n");
    xml.append(L"      <RunLevel>LeastPrivilege</RunLevel>\r\n    </Principal>\r\n");
    xml.append(L"  </Principals>\r\n  <Settings>\r\n");
    xml.append(L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n");
    // Ноутбук на батарее — обычная ситуация: сохранение и восстановление нужны и там.
    xml.append(L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n");
    xml.append(L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n");
    xml.append(L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n");
    xml.append(L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n");
    xml.append(L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n");
    xml.append(L"    <IdleSettings>\r\n      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n");
    xml.append(L"      <RestartOnIdle>false</RestartOnIdle>\r\n    </IdleSettings>\r\n");
    xml.append(L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n");
    xml.append(L"    <Enabled>true</Enabled>\r\n    <Hidden>false</Hidden>\r\n");
    xml.append(L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n    <WakeToRun>false</WakeToRun>\r\n");
    xml.append(L"    <ExecutionTimeLimit>");
    xml.append(executionTimeLimit);
    xml.append(L"</ExecutionTimeLimit>\r\n    <Priority>7</Priority>\r\n  </Settings>\r\n");
    xml.append(L"  <Actions Context=\"Author\">\r\n    <Exec>\r\n      <Command>");
    xml.append(XmlEscape(exe));
    xml.append(L"</Command>\r\n      <Arguments>");
    xml.append(XmlEscape(arguments));
    xml.append(L"</Arguments>\r\n      <WorkingDirectory>");
    xml.append(XmlEscape(ExeDir()));
    xml.append(L"</WorkingDirectory>\r\n    </Exec>\r\n  </Actions>\r\n</Task>\r\n");
    return xml;
}

// UTF-16LE с BOM: schtasks ждёт именно Unicode-файл (объявление encoding="UTF-16").
bool WriteXmlFile(const std::wstring& path, const std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    BOOL ok = WriteFile(file, bom, sizeof(bom), &written, nullptr);
    if (ok != FALSE) {
        ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                       &written, nullptr);
    }
    const DWORD writeError = GetLastError();
    CloseHandle(file);
    if (ok == FALSE) {
        SetLastError(writeError);
    }
    return ok != FALSE;
}

std::wstring TempXmlPath(const wchar_t* fileBase) {
    wchar_t dir[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH, dir) == 0) {
        return std::wstring();
    }
    return JoinPath(dir, std::wstring(L"ScreenMemory-") + fileBase + L".xml");
}

bool QueryTaskExists(const wchar_t* name, bool* exists, std::wstring* error) {
    const std::wstring command = L"\"" + SchtasksPath() + L"\" /Query /TN \"" + name + L"\"";
    DWORD code = 0;
    std::wstring output;
    if (!RunCapture(command, &code, &output, error)) {
        return false;
    }
    // Код 1 — «задачи нет»; прочие коды тоже считаем отсутствием: регистрация
    // с /F всё равно попробует создать задачу заново.
    *exists = (code == 0);
    return true;
}

// Собрать XML, положить его во временный файл и (кроме dryRun) зарегистрировать задачу.
bool RegisterTask(const wchar_t* name, const wchar_t* fileBase, const std::wstring& xml,
                  const TaskOptions& options, TaskInstallResult* result, std::wstring* error) {
    const std::wstring path = TempXmlPath(fileBase);
    if (path.empty()) {
        if (error != nullptr) {
            *error = L"не удалось получить путь к временному файлу";
        }
        return false;
    }
    if (!WriteXmlFile(path, xml)) {
        if (error != nullptr) {
            *error = L"не удалось записать " + path + L": " + Win32ErrorText(GetLastError());
        }
        return false;
    }
    if (result != nullptr) {
        result->xmlPath = path;
    }
    if (options.dryRun) {
        return true;  // файл оставляем: его можно посмотреть и зарегистрировать вручную
    }

    bool existed = false;
    std::wstring queryError;
    if (QueryTaskExists(name, &existed, &queryError) && result != nullptr) {
        result->alreadyExisted = existed;
    }

    const std::wstring command =
        L"\"" + SchtasksPath() + L"\" /Create /TN \"" + name + L"\" /XML \"" + path + L"\" /F";
    DWORD code = 0;
    std::wstring output;
    const bool started = RunCapture(command, &code, &output, error);
    DeleteFileW(path.c_str());
    if (!started) {
        return false;
    }
    if (code != 0) {
        if (error != nullptr) {
            *error = L"Планировщик заданий отказал (код " + std::to_wstring(code) + L")";
            const std::wstring detail = LastMeaningfulLine(output);
            if (!detail.empty()) {
                *error += L": " + detail;
            }
        }
        return false;
    }
    Trace(L"зарегистрирована задача «%s»: %s", name, command.c_str());
    return true;
}

}  // namespace

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

bool InstallAutoSaveTask(int intervalMinutes, const TaskOptions& options,
                         TaskInstallResult* result, std::wstring* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (result != nullptr) {
        *result = TaskInstallResult();
    }
    if (intervalMinutes < 1) {
        intervalMinutes = 1;
    }
    if (intervalMinutes > kMaxIntervalMinutes) {
        intervalMinutes = kMaxIntervalMinutes;
    }

    std::wstring sid;
    if (!CurrentUserSid(&sid)) {
        if (error != nullptr) {
            *error = L"не удалось определить SID пользователя: " + Win32ErrorText(GetLastError());
        }
        return false;
    }

    std::wstring triggers;
    triggers.append(L"    <CalendarTrigger>\r\n      <Repetition>\r\n        <Interval>");
    triggers.append(IsoInterval(intervalMinutes));
    triggers.append(L"</Interval>\r\n        <StopAtDurationEnd>false</StopAtDurationEnd>\r\n");
    triggers.append(L"      </Repetition>\r\n      <StartBoundary>");
    triggers.append(IsoStartBoundary(intervalMinutes));
    triggers.append(L"</StartBoundary>\r\n      <Enabled>true</Enabled>\r\n");
    triggers.append(L"      <ScheduleByDay>\r\n        <DaysInterval>1</DaysInterval>\r\n");
    triggers.append(L"      </ScheduleByDay>\r\n    </CalendarTrigger>\r\n");

    const std::wstring xml = BuildTaskXml(
        L"ScreenMemory: промежуточное сохранение раскладки окон (каждые " +
            std::to_wstring(intervalMinutes) + L" мин)",
        triggers, L"save --hidden", sid, L"PT10M");
    if (result != nullptr) {
        result->schedule = FormatIntervalText(intervalMinutes);
    }
    return RegisterTask(kAutoSaveTaskName, L"AutoSave", xml, options, result, error);
}

bool InstallAutoRestoreTask(int delaySeconds, const TaskOptions& options,
                            TaskInstallResult* result, std::wstring* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (result != nullptr) {
        *result = TaskInstallResult();
    }
    if (delaySeconds < 0) {
        delaySeconds = 0;
    }
    if (delaySeconds > kMaxDelaySeconds) {
        delaySeconds = kMaxDelaySeconds;
    }

    std::wstring sid;
    if (!CurrentUserSid(&sid)) {
        if (error != nullptr) {
            *error = L"не удалось определить SID пользователя: " + Win32ErrorText(GetLastError());
        }
        return false;
    }

    std::wstring triggers;
    triggers.append(L"    <LogonTrigger>\r\n      <Enabled>true</Enabled>\r\n");
    const std::wstring delay = IsoDelay(delaySeconds);
    if (!delay.empty()) {
        triggers.append(L"      <Delay>");
        triggers.append(delay);
        triggers.append(L"</Delay>\r\n");
    }
    triggers.append(L"    </LogonTrigger>\r\n");

    const std::wstring description =
        L"ScreenMemory: восстановление раскладки окон при входе в систему";
    // PT0S — без ограничения: restore может ждать prelaunch и запускать приложения.
    const std::wstring xml = BuildTaskXml(description, triggers, L"restore --hidden", sid, L"PT0S");
    if (result != nullptr) {
        result->schedule = (delaySeconds > 0)
                               ? L"при входе в систему, задержка " +
                                     std::to_wstring(delaySeconds) + L" с"
                               : std::wstring(L"при входе в систему, без задержки");
    }
    return RegisterTask(kAutoRestoreTaskName, L"AutoRestore", xml, options, result, error);
}

bool DeleteTask(const wchar_t* name, const TaskOptions& options, bool* found,
                std::wstring* error) {
    if (found != nullptr) {
        *found = false;
    }
    bool exists = false;
    if (!QueryTaskExists(name, &exists, error)) {
        return false;
    }
    if (found != nullptr) {
        *found = exists;
    }
    if (!exists || options.dryRun) {
        return true;
    }

    const std::wstring command =
        L"\"" + SchtasksPath() + L"\" /Delete /TN \"" + name + L"\" /F";
    DWORD code = 0;
    std::wstring output;
    if (!RunCapture(command, &code, &output, error)) {
        return false;
    }
    if (code != 0) {
        if (error != nullptr) {
            *error = L"Планировщик не дал удалить задачу «" + std::wstring(name) +
                     L"» (код " + std::to_wstring(code) + L")";
            const std::wstring detail = LastMeaningfulLine(output);
            if (!detail.empty()) {
                *error += L": " + detail;
            }
        }
        return false;
    }
    Trace(L"задача «%s» удалена", name);
    return true;
}

bool DeleteRunValue(const TaskOptions& options, bool* found, std::wstring* error) {
    if (found != nullptr) {
        *found = false;
    }
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                                         KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (opened == ERROR_FILE_NOT_FOUND) {
        return true;  // раздела нет — автозагрузки тоже нет
    }
    if (opened != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"не удалось открыть HKCU\\" + std::wstring(kRunKeyPath) + L": " +
                     Win32ErrorText(static_cast<DWORD>(opened));
        }
        return false;
    }

    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS queried =
        RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
    if (queried == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return true;
    }
    if (queried != ERROR_SUCCESS && queried != ERROR_MORE_DATA) {
        RegCloseKey(key);
        if (error != nullptr) {
            *error = L"не удалось прочитать значение " + std::wstring(kRunValueName) + L": " +
                     Win32ErrorText(static_cast<DWORD>(queried));
        }
        return false;
    }
    if (found != nullptr) {
        *found = true;
    }
    if (options.dryRun) {
        RegCloseKey(key);
        return true;
    }

    const LSTATUS deleted = RegDeleteValueW(key, kRunValueName);
    RegCloseKey(key);
    if (deleted != ERROR_SUCCESS) {
        if (found != nullptr) {
            *found = false;
        }
        if (error != nullptr) {
            *error = L"не удалось удалить значение " + std::wstring(kRunValueName) + L": " +
                     Win32ErrorText(static_cast<DWORD>(deleted));
        }
        return false;
    }
    Trace(L"значение автозагрузки «%s» удалено", kRunValueName);
    return true;
}

}  // namespace sm

