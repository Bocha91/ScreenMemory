#include "launcher.h"

#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "browser_url.h"
#include "log.h"
#include "process_info.h"

namespace sm {
namespace {

// Внутренняя строка перезапуска VirtualBoxVM.exe «как есть» не запускается
// (PLAN.md §9.4) — из неё берём только GUID виртуальной машины.
const wchar_t* const kStartVmKey = L"--startvm";

std::wstring Lower(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && Lower(a) == Lower(b);
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Каталог файла: рабочая папка процесса. Приложения ищут рядом с собой конфиги
// и плагины, поэтому запускать их из каталога ScreenMemory.exe нельзя.
std::wstring DirectoryOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring Quote(const std::wstring& text) {
    return L"\"" + text + L"\"";
}

// Первый токен командной строки: в кавычках либо до первого пробела.
std::wstring FirstToken(const std::wstring& cmdline) {
    size_t i = 0;
    while (i < cmdline.size() && iswspace(cmdline[i]) != 0) {
        ++i;
    }
    if (i >= cmdline.size()) {
        return std::wstring();
    }
    if (cmdline[i] == L'"') {
        const size_t close = cmdline.find(L'"', i + 1);
        if (close == std::wstring::npos) {
            return cmdline.substr(i + 1);
        }
        return cmdline.substr(i + 1, close - i - 1);
    }
    size_t end = i;
    while (end < cmdline.size() && iswspace(cmdline[end]) == 0) {
        ++end;
    }
    return cmdline.substr(i, end - i);
}

// GUID из «... --startvm <GUID> ...» (проверено на VirtualBox 7.1).
std::wstring StartVmArgument(const std::wstring& cmdline) {
    const std::wstring lower = Lower(cmdline);
    size_t pos = lower.find(kStartVmKey);
    if (pos == std::wstring::npos) {
        return std::wstring();
    }
    pos += wcslen(kStartVmKey);
    while (pos < cmdline.size() && iswspace(cmdline[pos]) != 0) {
        ++pos;
    }
    size_t end = pos;
    while (end < cmdline.size() && iswspace(cmdline[end]) == 0) {
        ++end;
    }
    return cmdline.substr(pos, end - pos);
}

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Хвост командной строки после первого токена, если этот токен — тот же exe:
// «"C:\App\app.exe" --flag» → «--flag». Профиль браузера, который лежит в cmdline,
// страницу не восстанавливает, поэтому для браузеров этот хвост не используется.
std::wstring ArgumentsOf(const std::wstring& cmdline, const std::wstring& exe) {
    if (cmdline.empty() || exe.empty()) {
        return std::wstring();
    }
    const std::wstring first = FirstToken(cmdline);
    if (first.empty() || !SameText(first, exe)) {
        return std::wstring();
    }
    size_t i = cmdline.find_first_not_of(L" \t");
    if (i == std::wstring::npos) {
        return std::wstring();
    }
    if (cmdline[i] == L'"') {
        const size_t close = cmdline.find(L'"', i + 1);
        if (close == std::wstring::npos) {
            return std::wstring();
        }
        i = close + 1;
    } else {
        while (i < cmdline.size() && iswspace(cmdline[i]) == 0) {
            ++i;
        }
    }
    return Trim(cmdline.substr(i));
}

// Chromium-браузеры открывают переданный адрес новой вкладкой в уже существующем
// окне; --new-window гарантирует отдельное окно, которое restore потом расставит.
const wchar_t* NewWindowSwitch(const std::wstring& exeName) {
    if (SameText(exeName, L"firefox.exe")) {
        return L"-new-window";
    }
    if (SameText(exeName, L"iexplore.exe")) {
        return nullptr;  // IE отдельного ключа для этого не имеет
    }
    return L"--new-window";
}

std::wstring ExplorerPath() {
    wchar_t dir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"explorer.exe";  // CreateProcess найдёт его через PATH
    }
    return std::wstring(dir, len) + L"\\explorer.exe";
}

}  // namespace

const wchar_t* LaunchKindText(LaunchKind kind) {
    switch (kind) {
        case LaunchKind::Explorer:
            return L"explorer";
        case LaunchKind::Uwp:
            return L"uwp";
        case LaunchKind::VirtualBox:
            return L"virtualbox";
        case LaunchKind::Browser:
            return L"browser";
        case LaunchKind::Cmdline:
            return L"cmdline";
        case LaunchKind::Exe:
            return L"exe";
        default:
            return L"none";
    }
}

bool IsConsoleClass(const std::wstring& cls) {
    // VirtualConsoleClass — ConEmu, CASCADIA_HOSTING_WINDOW_CLASS — Windows Terminal.
    // PseudoConsoleWindow — окно консоли, созданной через ConPTY.
    return SameText(cls, L"ConsoleWindowClass") || SameText(cls, L"VirtualConsoleClass") ||
           SameText(cls, L"PseudoConsoleWindow") ||
           SameText(cls, L"CASCADIA_HOSTING_WINDOW_CLASS");
}

LaunchPlan PlanLaunch(const LayoutWindow& window) {
    LaunchPlan plan;

    // PLAN.md §9.4: в cmdline консольного окна лежит командная строка хоста, а не
    // того, что в консоли запущено. Повторный запуск открыл бы пустое окно, поэтому
    // такие записи не перезапускаем — они уходят в admin.log как ручная работа.
    if (IsConsoleClass(window.cls)) {
        plan.skipReason = L"console";
        return plan;
    }

    // Браузер: адреса страницы в cmdline нет (он в профиле), поэтому запуск «как
    // есть» открыл бы пустое окно или чужую вкладку. Адрес сохраняется при save
    // через UI Automation (browser_url.cpp); если его нет — запись не запускаем и
    // уводим в admin.log как «открыть вручную» (CONTEXT п. 58).
    if (IsBrowserExe(FileNameOf(window.exe))) {
        if (!FileExists(window.exe)) {
            plan.skipReason = L"no-command";
            return plan;
        }
        if (window.url.empty()) {
            plan.skipReason = L"browser-no-url";
            return plan;
        }
        plan.kind = LaunchKind::Browser;
        plan.app = window.exe;
        plan.args = Quote(window.url);
        if (const wchar_t* switchText = NewWindowSwitch(FileNameOf(window.exe))) {
            plan.args = std::wstring(switchText) + L" " + plan.args;
        }
        plan.text = Quote(plan.app) + L" " + plan.args;
        return plan;
    }

    // Проводник: папка — единственный надёжный ключ (§9.2).
    if (!window.folder.empty()) {
        plan.kind = LaunchKind::Explorer;
        plan.app = ExplorerPath();
        plan.args = Quote(window.folder);
        plan.text = Quote(plan.app) + L" " + plan.args;
        return plan;
    }

    if (window.uwp != 0 && !window.aumid.empty()) {
        plan.kind = LaunchKind::Uwp;
        plan.app = window.aumid;
        plan.text = L"UWP " + window.aumid;
        return plan;
    }

    // Виртуальная машина: внутренняя cmdline не запускается, собираем заново.
    if (SameText(FileNameOf(window.exe), L"VirtualBoxVM.exe")) {
        const std::wstring vm = StartVmArgument(window.cmdline);
        if (!vm.empty() && FileExists(window.exe)) {
            plan.kind = LaunchKind::VirtualBox;
            plan.app = window.exe;
            plan.args = std::wstring(kStartVmKey) + L" " + vm;
            plan.text = Quote(plan.app) + L" " + plan.args;
            return plan;
        }
        plan.skipReason = L"no-command";  // GUID ВМ в cmdline не найден
        return plan;
    }

    // cmdline годится, только если начинается с существующего файла: иначе это
    // строка хоста (браузер, Electron) и запуск по ней откроет не то окно.
    if (!window.cmdline.empty() && FileExists(FirstToken(window.cmdline))) {
        plan.kind = LaunchKind::Cmdline;
        plan.args = window.cmdline;
        plan.text = window.cmdline;
        return plan;
    }

    if (FileExists(window.exe)) {
        plan.kind = LaunchKind::Exe;
        plan.app = window.exe;
        plan.text = Quote(window.exe);
        return plan;
    }

    plan.skipReason = L"no-command";
    return plan;
}
std::wstring AdminCommand(const LayoutWindow& window) {
    if (window.exe.empty()) {
        return std::wstring();
    }
    std::wstring command = Quote(window.exe);
    // Браузер: параметр — адрес страницы. Профиль из cmdline страницу не открывает,
    // поэтому без адреса в журнал попадает только путь к exe (CONTEXT п. 58).
    if (IsBrowserExe(FileNameOf(window.exe))) {
        if (!window.url.empty()) {
            command += L" ";
            command += Quote(window.url);
        }
        return command;
    }
    // Проводник: папка — то, что действительно восстанавливает окно (§9.2).
    if (!window.folder.empty()) {
        command += L" ";
        command += Quote(window.folder);
        return command;
    }
    const std::wstring args = ArgumentsOf(window.cmdline, window.exe);
    if (!args.empty()) {
        command += L" ";
        command += args;
    }
    return command;
}



bool RunLaunch(const LaunchPlan& plan, DWORD* pid, DWORD* errorCode, std::wstring* error) {
    if (pid != nullptr) {
        *pid = 0;
    }
    if (errorCode != nullptr) {
        *errorCode = 0;
    }
    if (plan.kind == LaunchKind::None) {
        if (error != nullptr) {
            *error = L"команда запуска не собрана";
        }
        return false;
    }

    // UWP запускается не процессом, а менеджером активации (PLAN.md §8.3 п.2).
    if (plan.kind == LaunchKind::Uwp) {
        IApplicationActivationManager* manager = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                      CLSCTX_LOCAL_SERVER, IID_IApplicationActivationManager,
                                      reinterpret_cast<void**>(&manager));
        if (SUCCEEDED(hr) && manager == nullptr) {
            hr = E_NOINTERFACE;
        }
        if (FAILED(hr)) {
            if (errorCode != nullptr) {
                *errorCode = static_cast<DWORD>(hr);
            }
            if (error != nullptr) {
                *error = L"IApplicationActivationManager: " + HrText(hr);
            }
            return false;
        }
        DWORD appPid = 0;
        hr = manager->ActivateApplication(plan.app.c_str(), nullptr, AO_NONE, &appPid);
        manager->Release();
        if (FAILED(hr)) {
            if (errorCode != nullptr) {
                *errorCode = static_cast<DWORD>(hr);
            }
            if (error != nullptr) {
                *error = L"ActivateApplication: " + HrText(hr);
            }
            return false;
        }
        if (pid != nullptr) {
            *pid = appPid;
        }
        return true;
    }

    // CreateProcessW требует изменяемую строку.
    std::wstring command;
    LPCWSTR appName = nullptr;
    if (plan.kind == LaunchKind::Cmdline) {
        command = plan.args;
    } else {
        command = Quote(plan.app);
        if (!plan.args.empty()) {
            command += L" " + plan.args;
        }
        appName = plan.app.c_str();
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    const std::wstring workDir = DirectoryOf(plan.app.empty() ? FirstToken(plan.args) : plan.app);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(appName, buffer.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        const DWORD code = GetLastError();
        if (errorCode != nullptr) {
            *errorCode = code;
        }
        if (error != nullptr) {
            *error = L"CreateProcess: " + Win32ErrorText(code);
        }
        return false;
    }
    if (pid != nullptr) {
        *pid = pi.dwProcessId;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

namespace {

// Порог принятия окна: совпал PID (10), класс (5), заголовок (5) или «окно этого
// класса появилось после запуска» (1).
struct WaitContext {
    DWORD pid = 0;                    // PID запущенного процесса (0 — неизвестен)
    DWORD selfPid = 0;                // свой PID: свои консольные окна не считаем
    std::wstring cls;                 // класс окна из слепка
    std::wstring title;               // заголовок из слепка
    const std::vector<HWND>* known = nullptr;  // окна, существовавшие до запуска
    HWND found = nullptr;
    int score = 0;
};

bool IsKnownWindow(const std::vector<HWND>* known, HWND hwnd) {
    return known != nullptr && std::find(known->begin(), known->end(), hwnd) != known->end();
}

BOOL CALLBACK WaitProc(HWND hwnd, LPARAM param) {
    WaitContext* ctx = reinterpret_cast<WaitContext*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;  // owned-окно: диалог, панель, подсказка
    }
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ctx->selfPid) {
        return TRUE;
    }

    int score = 0;
    const bool pidMatch = ctx->pid != 0 && pid == ctx->pid;
    if (pidMatch) {
        score += 10;
    }
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls)));
    const bool classMatch = !ctx->cls.empty() && SameText(cls, ctx->cls);
    if (classMatch) {
        score += 5;
    }
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
    const bool titleMatch = !ctx->title.empty() && ctx->title == title;
    if (titleMatch) {
        score += 5;
    }
    // Окно того же класса, которого до запуска не было: подходит и Explorer/UWP,
    // где PID не наш, а заголовок приложение ставит своё («img» вместо пути).
    const bool freshMatch = classMatch && !IsKnownWindow(ctx->known, hwnd);
    if (freshMatch) {
        ++score;
    }

    // Ни одного признака «это наше окно» — чужое окно трогать нельзя: легко утащить
    // его на чужое место.
    if (!pidMatch && !titleMatch && !freshMatch) {
        return TRUE;
    }
    if (score > ctx->score) {
        ctx->score = score;
        ctx->found = hwnd;
    }
    return TRUE;
}

}  // namespace

HWND WaitForWindow(DWORD pid, const LayoutWindow& saved, const std::vector<HWND>& knownWindows,
                   int timeoutMs, bool* processExited) {
    WaitContext ctx;
    ctx.pid = pid;
    ctx.selfPid = GetCurrentProcessId();
    ctx.cls = saved.cls;
    ctx.title = saved.title;
    ctx.known = &knownWindows;

    if (processExited != nullptr) {
        *processExited = false;
    }
    // Процесс мог умереть сразу после старта (неверная команда, мгновенный выход):
    // тогда окна не будет никогда, и ждать 60 с незачем. У UWP pid == 0 — ждём.
    HANDLE process = (pid != 0) ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;

    const ULONGLONG wait = static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
    const ULONGLONG deadline = GetTickCount64() + wait;
    HWND found = nullptr;
    for (;;) {
        ctx.found = nullptr;
        ctx.score = 0;
        EnumWindows(WaitProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found != nullptr) {
            found = ctx.found;
            break;
        }
        if (process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            // Процесс завершился — но окно могло появиться и исчезнуть между опросами
            // (например, приложение стартовало и упало). Последний перебор сделан выше.
            if (processExited != nullptr) {
                *processExited = true;
            }
            Trace(L"  процесс %lu завершился, окно не появилось — ожидание прекращено", pid);
            break;
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(250);
    }
    if (process != nullptr) {
        CloseHandle(process);
    }
    return found;
}

bool WaitForBrowserPage(HWND hwnd, const std::wstring& expectedUrl, int timeoutMs) {
    const ULONGLONG wait = static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
    const ULONGLONG deadline = GetTickCount64() + wait;
    std::wstring previous;
    for (;;) {
        if (IsWindow(hwnd) == FALSE) {
            Trace(L"  окно %p закрылось, не показав страницу", static_cast<void*>(hwnd));
            return false;
        }
        const std::wstring url = ReadBrowserUrl(hwnd);
        if (!url.empty() && !previous.empty() && SamePage(previous, url)) {
            const bool expectedKnown = !expectedUrl.empty();
            const bool same = expectedKnown && SamePage(url, expectedUrl);
            Trace(L"  браузер: страница «%s»%s", url.c_str(),
                  (expectedKnown && !same) ? L" (адрес отличается от сохранённого — редирект)" : L"");
            return true;
        }
        if (!url.empty()) {
            previous = url;
        }
        if (GetTickCount64() >= deadline) {
            Trace(L"  браузер: адрес страницы не подтвердился за %d мс — переносим окно как есть",
                  timeoutMs);
            return false;
        }
        Sleep(500);
    }
}

namespace {

// Видимые верхнеуровневые окна класса cls, кроме своих: и прогретое окно браузера,
// и пустые окна «Новая вкладка» ищутся именно по классу (у Firefox — MozillaWindowClass).
struct ClassContext {
    const std::wstring* cls = nullptr;
    std::vector<HWND>* out = nullptr;
    DWORD selfPid = 0;
};

BOOL CALLBACK CollectClassProc(HWND hwnd, LPARAM param) {
    ClassContext* ctx = reinterpret_cast<ClassContext*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ctx->selfPid) {
        return TRUE;
    }
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls)));
    if (!SameText(cls, *ctx->cls)) {
        return TRUE;
    }
    ctx->out->push_back(hwnd);
    return TRUE;
}

std::vector<HWND> WindowsOfClass(const std::wstring& cls) {
    std::vector<HWND> windows;
    if (cls.empty()) {
        return windows;
    }
    ClassContext ctx;
    ctx.cls = &cls;
    ctx.out = &windows;
    ctx.selfPid = GetCurrentProcessId();
    EnumWindows(CollectClassProc, reinterpret_cast<LPARAM>(&ctx));
    return windows;
}

// Заголовок окна, которое браузер открывает сам, без страницы: у Firefox это
// «Mozilla Firefox» с вкладкой about:newtab, у Chromium — «Новая вкладка».
bool IsStartupTitle(const std::wstring& title) {
    static const wchar_t* const kTitles[] = {
        L"", L"Mozilla Firefox", L"Новая вкладка", L"New Tab", L"Новая вкладка — Mozilla Firefox",
        L"New Tab — Mozilla Firefox", L"about:newtab", L"about:home",
    };
    for (const wchar_t* known : kTitles) {
        if (SameText(title, known)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool WarmUpBrowser(const std::wstring& exePath, const std::wstring& cls,
                   std::vector<HWND>* knownWindows, int timeoutMs) {
    if (exePath.empty() || cls.empty()) {
        return true;  // окно ищется по классу: без него прогрев бессмыслен
    }
    if (IsExeRunning(exePath)) {
        return true;  // браузер уже работает — холодного старта и восстановления сессии не будет
    }

    LaunchPlan warm;
    warm.kind = LaunchKind::Exe;
    warm.app = exePath;
    warm.text = Quote(exePath);
    DWORD pid = 0;
    DWORD code = 0;
    std::wstring error;
    if (!RunLaunch(warm, &pid, &code, &error)) {
        Trace(L"  прогрев браузера: %s", error.c_str());
        return false;
    }
    Trace(L"  браузер не был запущен: поднимаю его без адреса, чтобы он восстановил сессию профиля");

    const ULONGLONG deadline =
        GetTickCount64() + static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
    bool appeared = false;
    for (;;) {
        for (HWND hwnd : WindowsOfClass(cls)) {
            if (!IsKnownWindow(knownWindows, hwnd)) {
                appeared = true;
                break;
            }
        }
        if (appeared || GetTickCount64() >= deadline) {
            break;
        }
        Sleep(250);
    }

    // Всё открытое после прогрева — «известные» окна для последующего запуска: иначе
    // восстановленное окно сессии выглядело бы только что открытым и переехало бы
    // вместо нового окна с адресом. Снимаем окна ПОСЛЕ паузы: стартовое окно «Новая
    // вкладка» появляется позже окна восстановленной сессии, и оно тоже должно
    // считаться «известным», иначе поиск нового окна возьмёт его.
    if (!appeared) {
        Trace(L"  прогрев браузера: окно не появилось за %d мс", timeoutMs);
        return false;
    }
    // Даём восстановлению сессии доиграть: адрес, открытый в этот момент, Firefox иначе
    // добавляет вкладкой к восстанавливаемому окну.
    Sleep(2500);
    if (knownWindows != nullptr) {
        for (HWND hwnd : WindowsOfClass(cls)) {
            if (!IsKnownWindow(knownWindows, hwnd)) {
                knownWindows->push_back(hwnd);
            }
        }
    }
    return true;
}

int CloseEmptyBrowserWindows(const std::wstring& cls, const std::vector<HWND>& knownWindows,
                             HWND keep) {
    int closed = 0;
    // Окно-пустышка появляется не сразу за окном с адресом (Firefox сначала восстанавливает
    // сессию, потом открывает своё стартовое окно), поэтому попыток несколько.
    for (int attempt = 0; attempt < 4; ++attempt) {
        int closedNow = 0;
        for (HWND hwnd : WindowsOfClass(cls)) {
            if (hwnd == keep || IsKnownWindow(&knownWindows, hwnd) || IsWindow(hwnd) == FALSE) {
                continue;  // наше окно либо оно было до запуска — не наше дело
            }
            const std::wstring url = ReadBrowserUrl(hwnd);
            const bool aboutPage = !url.empty() && Lower(url).rfind(L"about:", 0) == 0;
            wchar_t title[512] = {};
            GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
            // Окно с настоящей страницей не трогаем: пустой адрес при «живом» заголовке —
            // это либо не успевшая ответить адресная строка, либо окно пользователя.
            if (!aboutPage && !(url.empty() && IsStartupTitle(title))) {
                continue;
            }
            if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
                ++closedNow;
                Trace(L"  браузер: закрыто пустое окно «%s» (%p) — его нет в слепке", title,
                      static_cast<void*>(hwnd));
            }
        }
        closed += closedNow;
        if (closedNow > 0) {
            break;
        }
        Sleep(700);
    }
    return closed;
}

HWND WaitForBrowserWindow(const std::wstring& cls, const std::wstring& expectedUrl,
                          const std::vector<HWND>& knownWindows, int timeoutMs) {
    if (cls.empty()) {
        return nullptr;
    }
    const ULONGLONG wait = static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
    const ULONGLONG deadline = GetTickCount64() + wait;
    HWND other = nullptr;  // окно с непустым, но другим адресом — обычно редирект
    for (;;) {
        for (HWND hwnd : WindowsOfClass(cls)) {
            if (IsKnownWindow(&knownWindows, hwnd)) {
                continue;
            }
            const std::wstring url = ReadBrowserUrl(hwnd);
            if (url.empty()) {
                continue;  // адресная строка ещё не ответила: окно не определилось
            }
            if (!expectedUrl.empty() && SamePage(url, expectedUrl)) {
                Trace(L"  браузер: окно %p открыло нужный адрес «%s»", static_cast<void*>(hwnd),
                      url.c_str());
                return hwnd;
            }
            if (other == nullptr) {
                other = hwnd;
            }
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(300);
    }
    if (other != nullptr) {
        Trace(L"  браузер: точный адрес не подтвердился за %d мс — беру окно %p (редирект)",
              timeoutMs, static_cast<void*>(other));
    }
    return other;
}

bool IsExeRunning(const std::wstring& exePath) {
    if (exePath.empty()) {
        return false;
    }
    // 2048 процессов с запасом: на этой машине их около трёхсот.
    std::vector<DWORD> pids(2048, 0);
    DWORD needed = 0;
    if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &needed)) {
        return false;
    }
    const size_t count = needed / sizeof(DWORD);
    const DWORD selfPid = GetCurrentProcessId();
    for (size_t i = 0; i < count; ++i) {
        if (pids[i] == 0 || pids[i] == selfPid) {
            continue;
        }
        ProcessInfo info;
        if (ReadProcessInfo(pids[i], &info) && SameText(info.exePath, exePath)) {
            return true;
        }
    }
    return false;
}

namespace {

// Программа, которая завершилась, не показав окна (консольная утилита), считается
// отработавшей: даём немного времени на появление окна «дочерней» копии и идём дальше.
constexpr ULONGLONG kPreLaunchNoWindowGraceMs = 3000;

struct PreLaunchWindowsContext {
    const std::wstring* exePath = nullptr;
    DWORD selfPid = 0;
    int count = 0;
};

// Видимые верхнеуровневые окна программы. Сверяем путь exe, а не PID: утилита может
// показать окно из уже запущенной копии, а свой процесс сразу завершить.
BOOL CALLBACK PreLaunchWindowProc(HWND hwnd, LPARAM param) {
    PreLaunchWindowsContext* ctx = reinterpret_cast<PreLaunchWindowsContext*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ctx->selfPid) {
        return TRUE;
    }
    const ProcessInfo& info = ProcessInfoFor(pid);
    if (info.exePath.empty() || !SameText(info.exePath, *ctx->exePath)) {
        return TRUE;
    }
    ++ctx->count;
    return TRUE;
}

int VisibleWindowsOfExe(const std::wstring& exePath) {
    PreLaunchWindowsContext ctx;
    ctx.exePath = &exePath;
    ctx.selfPid = GetCurrentProcessId();
    EnumWindows(PreLaunchWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.count;
}

}  // namespace

PreLaunchResult RunPreLaunch(const std::wstring& exePath, const std::wstring& args, int timeoutSec,
                             DWORD* elapsedMs, std::wstring* error, DWORD* errorCode) {
    if (elapsedMs != nullptr) {
        *elapsedMs = 0;
    }
    if (errorCode != nullptr) {
        *errorCode = 0;
    }
    if (error != nullptr) {
        error->clear();
    }
    if (exePath.empty()) {
        return PreLaunchResult::NotConfigured;
    }
    if (!FileExists(exePath)) {
        if (errorCode != nullptr) {
            *errorCode = ERROR_FILE_NOT_FOUND;
        }
        if (error != nullptr) {
            *error = L"файл не найден";
        }
        return PreLaunchResult::Missing;
    }

    std::wstring commandLine = Quote(exePath);
    if (!args.empty()) {
        commandLine += L" " + args;
    }
    std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring workDir = DirectoryOf(exePath);
    const ULONGLONG started = GetTickCount64();
    // Рабочая папка — папка программы: утилиты ищут рядом с собой ini-файлы и профили.
    if (CreateProcessW(exePath.c_str(), mutableLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       workDir.empty() ? nullptr : workDir.c_str(), &si, &pi) == FALSE) {
        const DWORD code = GetLastError();
        if (errorCode != nullptr) {
            *errorCode = code;
        }
        if (error != nullptr) {
            *error = Win32ErrorText(code);
        }
        return PreLaunchResult::LaunchFailed;
    }
    CloseHandle(pi.hThread);
    Trace(L"предварительная программа запущена: %s (pid %lu)", commandLine.c_str(),
          static_cast<unsigned long>(pi.dwProcessId));

    bool appeared = false;   // окно программы уже видели
    ULONGLONG exitedAt = 0;  // 0 — процесс ещё жив (или окно снова появилось)
    PreLaunchResult result = PreLaunchResult::Done;
    for (;;) {
        const int windows = VisibleWindowsOfExe(exePath);
        if (windows > 0) {
            appeared = true;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            if (exitedAt == 0) {
                exitedAt = GetTickCount64();
            }
        } else {
            exitedAt = 0;
        }

        if (appeared && windows == 0) {
            break;  // окно закрыли — шаг выполнен
        }
        if (!appeared && exitedAt != 0 && GetTickCount64() - exitedAt >= kPreLaunchNoWindowGraceMs) {
            Trace(L"предварительная программа завершилась, не показав окна — идём дальше");
            break;
        }
        if (timeoutSec > 0 &&
            GetTickCount64() - started >= static_cast<ULONGLONG>(timeoutSec) * 1000ULL) {
            result = PreLaunchResult::WaitTimeout;
            break;
        }
        Sleep(250);
    }

    if (elapsedMs != nullptr) {
        *elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
    }
    CloseHandle(pi.hProcess);
    return result;
}

}  // namespace sm
