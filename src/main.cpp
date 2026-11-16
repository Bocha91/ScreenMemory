// ScreenMemory — сохранение и восстановление раскладки окон по виртуальным рабочим столам.
//
// Этап 1 (каркас): разбор командной строки, справка, коды возврата, логирование.
// Реализация save/restore добавляется на этапах 2–8 (см. PLAN.md §12).

#include <windows.h>

#include <conio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "admin_log.h"
#include "browser_url.h"
#include "config.h"
#include "launcher.h"
#include "log.h"
#include "matching.h"
#include "process_info.h"
#include "registry_desktops.h"
#include "scheduler.h"
#include "storage.h"
#include "vdesk.h"
#include "version.h"
#include "window_enum.h"
#include "window_info.h"

namespace {

enum ExitCode {
    kExitOk = 0,
    kExitError = 1,        // ошибка либо восстановлено не всё
    kExitNoLayout = 2,     // нет файла раскладки или он битый
    kExitNoDesktops = 3,   // не удалось получить список виртуальных десктопов
    kExitNoRights = 5,     // AutoSave/AutoRestore/Uninstall запущены без прав администратора
    kExitInternalApi = 6,  // нужен перенос окон, а внутренний интерфейс оболочки не прошёл самопроверку
    kExitUsage = 64,       // неверная командная строка (как EX_USAGE)
};

struct CommandLine {
    std::wstring command;  // "save" | "restore" | "autosave" | "autorestore" | "uninstall"
    std::wstring from;     // ключ --from: файл, из которого восстанавливать (пусто — layout.txt)
    bool help = false;
    bool version = false;
    bool list = false;
    bool verbose = false;
    bool dryRun = false;
    bool hidden = false;   // --hidden: окно консоли скрыто, клавишу не ждём (задачи Планировщика)
};

void PrintBanner() {
    sm::Info(L"ScreenMemory %s", SM_VERSION_WSTRING);
    sm::Info(L"каталог: %s", sm::ExeDir().c_str());
    sm::Info(L"");
}

void PrintUsage() {
    sm::Info(L"Использование:");
    sm::Info(L"  ScreenMemory.exe save        сохранить текущую раскладку окон в layout.txt");
    sm::Info(L"  ScreenMemory.exe restore     восстановить раскладку из layout.txt");
    sm::Info(L"  ScreenMemory.exe restore --from <файл>");
    sm::Info(L"                               восстановить из указанного файла (любой layout-*.txt:");
    sm::Info(L"                               рабочая версия, «вчерашний день», «прежняя конфигурация»)");
    sm::Info(L"  ScreenMemory.exe AutoSave    поставить в Планировщик заданий сохранение каждые N мин");
    sm::Info(L"  ScreenMemory.exe AutoRestore поставить в Планировщик восстановление при входе в систему");
    sm::Info(L"  ScreenMemory.exe Uninstall   снять задачи AutoSave/AutoRestore и автозагрузку");
    sm::Info(L"  ScreenMemory.exe             эта справка");
    sm::Info(L"");
    sm::Info(L"AutoSave, AutoRestore и Uninstall требуют прав администратора и не сочетаются");
    sm::Info(L"ни друг с другом, ни с save/restore: за запуск — ровно одна команда.");
    sm::Info(L"Интервал AutoSave и задержка AutoRestore берутся из Config.ini.");
    sm::Info(L"");
    sm::Info(L"Отладочные ключи:");
    sm::Info(L"  --verbose    подробный лог в консоль и в файл screenmemory.log");
    sm::Info(L"  --list       показать десктопы, pinned-приложения и окна; ничего не менять");
    sm::Info(L"  --dry-run    с restore — показать план; с AutoSave/AutoRestore/Uninstall —");
    sm::Info(L"               только проверить: показать XML задачи или найденное, ничего не менять");
    sm::Info(L"  --hidden     скрыть окно консоли и не ждать клавишу (так задачи и запускают");
    sm::Info(L"               программу; при ошибке лог всё равно пишется в screenmemory.log)");
    sm::Info(L"  --version    версия программы");
    sm::Info(L"  --help, /?   эта справка");
    sm::Info(L"");
    sm::Info(L"Коды возврата: 0 — успех, 1 — восстановлено не всё (в том числе когда");
    sm::Info(L"              сохранённых десктопов нет и окна уехали на десктоп 0),");
    sm::Info(L"              2 — нет файла раскладки, 3 — нет списка десктопов,");
    sm::Info(L"              5 — нужны права администратора,");
    sm::Info(L"              6 — перенос окон между десктопами невозможен (другая сборка");
    sm::Info(L"              Windows: внутренний интерфейс оболочки не прошёл самопроверку),");
    sm::Info(L"              64 — неверная команда.");
    sm::Info(L"");
    sm::Info(L"Настройки — Config.ini рядом с exe (файл необязателен):");
    sm::Info(L"  prelaunch=<путь к программе>  запустить её перед restore и ждать,");
    sm::Info(L"                                пока не закроют её окно");
    sm::Info(L"  prelaunch-args=<аргументы>    аргументы для неё");
    sm::Info(L"  prelaunch-timeout=<сек>       сколько ждать окно, 0 — без ограничения");
    sm::Info(L"  AutoSave=<минуты>             интервал задачи AutoSave (по умолчанию 10)");
    sm::Info(L"  AutoRestoreDelay=<сек>        задержка задачи AutoRestore после входа");
    sm::Info(L"                                (по умолчанию 30, 0 — без задержки)");
    sm::Info(L"  Layouts=<число>               сколько рабочих версий слепка хранить");
    sm::Info(L"                                (по умолчанию 3; защищённые копии не в счёт)");
    sm::Info(L"  settle-wait=<сек>             сколько ждать «успокоения» окон перед");
    sm::Info(L"                                запуском (по умолчанию 10, 0 — не ждать)");
    sm::Info(L"  save предварительный шаг не выполняет: он только записывает то,");
    sm::Info(L"  что видно сейчас.");
}

// Предварительный шаг (CONTEXT п. 64): программа из Config.ini, которая приводит в порядок
// мониторы. Второй монитор — телевизор: он включается позже компьютера, и рабочий стол на
// него не приходит, пока состояние не поправит сторонняя утилита. Пока её окно открыто,
// наши действия не начинаются. Нет файла, нет настройки, сбой запуска — просто идём дальше:
// остаться совсем без сохранения или восстановления хуже, чем сделать их на «кривом» экране.
// Вызывается только для restore: save ничего не восстанавливает, он лишь записывает то, что
// видно сейчас, — внешняя программа мониторов ему не нужна.
void RunPreLaunchStep() {
    sm::Config config;
    std::wstring error;
    if (!sm::LoadConfig(&config, &error)) {
        sm::Warn(L"Config.ini: %s — предварительный шаг пропущен", error.c_str());
        return;
    }
    if (!config.exists) {
        sm::Trace(L"Config.ini: файла %s нет — предварительный шаг пропущен", config.path.c_str());
        return;
    }
    for (const std::wstring& key : config.unknownKeys) {
        sm::Trace(L"Config.ini: ключ «%s» не знаком этой версии — пропущен", key.c_str());
    }
    if (config.preLaunch.empty()) {
        sm::Trace(L"Config.ini: prelaunch не задан — предварительный шаг пропущен");
        return;
    }
    sm::Trace(L"Config.ini: %s, prelaunch=%s, timeout=%d с", config.path.c_str(),
              config.preLaunch.c_str(), config.preLaunchTimeoutSec);

    std::wstring commandLine = config.preLaunch;
    if (!config.preLaunchArgs.empty()) {
        commandLine += L" " + config.preLaunchArgs;
    }
    sm::Info(L"Предварительная программа: %s", commandLine.c_str());
    sm::Info(L"  закройте её окно — после этого работа продолжится");

    DWORD elapsed = 0;
    DWORD errorCode = 0;
    std::wstring launchError;
    const sm::PreLaunchResult result =
        sm::RunPreLaunch(config.preLaunch, config.preLaunchArgs, config.preLaunchTimeoutSec,
                         &elapsed, &launchError, &errorCode);
    switch (result) {
        case sm::PreLaunchResult::NotConfigured:
            break;
        case sm::PreLaunchResult::Missing:
            sm::Warn(L"программы нет: %s (%s) — продолжаю без неё", config.preLaunch.c_str(),
                     launchError.c_str());
            break;
        case sm::PreLaunchResult::LaunchFailed:
            sm::Warn(L"не удалось запустить %s (%s) — продолжаю без неё",
                     config.preLaunch.c_str(), launchError.c_str());
            break;
        case sm::PreLaunchResult::WaitTimeout:
            sm::Warn(L"окно программы не закрыто за %d с — продолжаю, не дожидаясь",
                     config.preLaunchTimeoutSec);
            break;
        case sm::PreLaunchResult::Done:
            sm::Info(L"  программа закрыта (ждали %.1f с), работаю дальше",
                     static_cast<double>(elapsed) / 1000.0);
            break;
    }
    sm::Info(L"");
}

// Консоль создана специально для нас (двойной щелчок по ярлыку)?
// Тогда в конце ждём нажатия клавиши, иначе окно схлопнется раньше, чем прочитаешь отчёт.
bool ConsoleIsOurs() {
    DWORD pids[4] = {};
    DWORD count = GetConsoleProcessList(pids, 4);
    return count <= 1;
}

void WaitForKeyIfConsoleIsOurs() {
    if (!ConsoleIsOurs()) {
        return;
    }
    sm::Info(L"");
    sm::Info(L"Нажмите любую клавишу для выхода...");
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    (void)_getwch();
}

// Имя десктопа из реестра; Explorer не даёт стереть имя полностью — вместо пустой
// строки он пишет один пробел, поэтому имя из одних пробелов тоже считаем пустым.
std::wstring DesktopLabel(const sm::DesktopInfo& desktop) {
    const bool blank = desktop.name.find_first_not_of(L" \t") == std::wstring::npos;
    return blank ? std::wstring(L"(без имени)") : L"«" + desktop.name + L"»";
}

const wchar_t* PinnedKindTag(const std::wstring& kind) {
    if (kind == L"known") {
        return L"папка";
    }
    if (kind == L"aumid") {
        return L"AUMID";
    }
    return L"путь ";
}

struct WindowGroup {
    GUID guid{};
    std::vector<const sm::WindowInfo*> items;
};

// Ширина колонки exe: длинные имена (ApplicationFrameHost.exe) обрезаем — полный
// путь всё равно печатается ниже, в деталях.
constexpr size_t kExeColumn = 18;

std::wstring FitText(const std::wstring& text, size_t width) {
    if (text.size() <= width) {
        return text;
    }
    if (width == 0) {
        return std::wstring();
    }
    return text.substr(0, width - 1) + L"…";
}

// Детали окна (этап 4, PLAN.md §12): папка Проводника печатается всегда — для
// окон Проводника это главный ключ восстановления (заголовки повторяются);
// полный путь exe, командная строка и AUMID — только в --verbose.
void PrintWindowDetails(const sm::WindowInfo& w, bool verbose) {
    if (!w.folder.empty()) {
        sm::Info(L"          папка  %s", w.folder.c_str());
    }
    // Свёрнутое окно стоит в служебной позиции (-32000), развёрнутое занимает монитор
    // целиком — в обоих случаях колонка rect не говорит, куда окно вернётся. В слепок
    // пойдёт нормальная позиция (rcNormalPosition), её и печатаем.
    if (w.placementOk && w.placement.showCmd != SW_SHOWNORMAL) {
        sm::Info(L"          place  %s", sm::NormalRectText(w).c_str());
    }
    if (!verbose) {
        return;
    }
    if (!w.exePath.empty()) {
        sm::Info(L"          exe    %s", w.exePath.c_str());
    } else if (w.exeError != 0) {
        sm::Info(L"          exe    (недоступен: %s)", sm::Win32ErrorText(w.exeError).c_str());
    }
    if (!w.cmdline.empty()) {
        sm::Info(L"          cmd    %s", w.cmdline.c_str());
    }
    if (!w.aumid.empty()) {
        sm::Info(L"          aumid  %s", w.aumid.c_str());
    }
}

void PrintWindowGroup(const std::wstring& label, bool current,
                      const std::vector<const sm::WindowInfo*>& items) {
    sm::Info(L"");
    sm::Info(L"  %s%s — окон %d", label.c_str(), current ? L"   <= текущий" : L"",
             static_cast<int>(items.size()));
    if (items.empty()) {
        sm::Info(L"    (окон нет)");
        return;
    }
    sm::Info(L"     z  cloaked    сост.  pid    %-18s %-24s %-26s %s", L"exe", L"rect",
             L"класс", L"заголовок");
    const bool verbose = sm::IsVerbose();
    for (const sm::WindowInfo* w : items) {
        // Имя exe в таблице — чтобы окно опознавалось без чтения заголовка;
        // полный путь, командная строка и AUMID печатаются ниже, в деталях.
        const std::wstring exeCell = FitText(w->exeName.empty() ? L"—" : w->exeName, kExeColumn);
        if (verbose) {
            sm::Info(L"    %2d  %-10s %-6s %-6lu %p %-18s %-24s %-26s %s", w->z,
                     sm::CloakedText(w->cloaked), sm::ShowStateText(*w),
                     static_cast<unsigned long>(w->pid), static_cast<void*>(w->hwnd),
                     exeCell.c_str(), sm::RectText(*w).c_str(), w->cls.c_str(),
                     w->title.c_str());
        } else {
            sm::Info(L"    %2d  %-10s %-6s %-6lu %-18s %-24s %-26s %s", w->z,
                     sm::CloakedText(w->cloaked), sm::ShowStateText(*w),
                     static_cast<unsigned long>(w->pid), exeCell.c_str(),
                     sm::RectText(*w).c_str(), w->cls.c_str(), w->title.c_str());
        }
        PrintWindowDetails(*w, verbose);
    }
}

// Окна всех виртуальных десктопов, сгруппированные по десктопу (PLAN.md §12, этап 3).
// Главная проверка этапа: окно, открытое на другом десктопе, видно здесь с cloaked=shell.
void PrintWindows(const sm::VirtualDesktopManager& vdm,
                  const std::vector<sm::DesktopInfo>& desktops, int currentIndex) {
    std::vector<sm::WindowInfo> windows;
    sm::WindowEnumStats stats;
    if (!sm::CollectWindows(&windows, &stats)) {
        sm::Warn(L"перечисление окон не удалось");
        return;
    }

    sm::Info(L"Окна: кандидатов %d из %d верхнеуровневых; cloaked: shell %d (другие десктопы), app %d",
             stats.candidates, stats.total, stats.cloakedShell, stats.cloakedApp);
    sm::Info(L"  shell — окно на другом виртуальном столе (в слепок попадает), "
             L"app — окно спрятало приложение (UWP suspended)");
    sm::Info(L"  exe прочитан у %d, AUMID — у %d, папка Проводника — у %d",
             stats.exeKnown, stats.aumidKnown, stats.folderKnown);
    if (stats.candidates == 0) {
        sm::Info(L"  пропущено: невидимых %d, чужих процессов %d, tool-window %d, owned %d, "
                 L"служебных классов %d, без заголовка %d, пустых UWP-рамок %d, "
                 L"унаследованно-cloaked %d",
                 stats.skippedInvisible, stats.skippedProcess, stats.skippedTool,
                 stats.skippedOwned, stats.skippedBlacklist, stats.skippedNoTitle,
                 stats.skippedEmptyUwp, stats.skippedInherited);
    }

    // Группировка по десктопу окна; окна без определившегося десктопа — отдельной группой.
    std::vector<WindowGroup> groups;
    std::vector<const sm::WindowInfo*> unknown;
    for (const sm::WindowInfo& w : windows) {
        GUID guid{};
        if (!vdm.ok() || !vdm.GetWindowDesktopId(w.hwnd, &guid, false) || sm::IsNullGuid(guid)) {
            unknown.push_back(&w);
            continue;
        }
        WindowGroup* group = nullptr;
        for (WindowGroup& g : groups) {
            if (IsEqualGUID(g.guid, guid) != FALSE) {
                group = &g;
                break;
            }
        }
        if (group == nullptr) {
            groups.push_back(WindowGroup{guid, {}});
            group = &groups.back();
        }
        group->items.push_back(&w);
    }

    for (const sm::DesktopInfo& d : desktops) {
        std::vector<const sm::WindowInfo*> items;
        for (const WindowGroup& g : groups) {
            if (IsEqualGUID(g.guid, d.guid) != FALSE) {
                items = g.items;
                break;
            }
        }
        wchar_t prefix[32] = L"";
        swprintf_s(prefix, L"[%d] ", d.index);
        PrintWindowGroup(std::wstring(prefix) + DesktopLabel(d), d.index == currentIndex, items);
    }

    // Десктопы, которых нет в списке из реестра. На практике это GUID-маркер
    // pinned-приложений («показывать на всех рабочих столах») — не настоящий десктоп.
    for (const WindowGroup& g : groups) {
        if (sm::FindDesktopIndex(desktops, g.guid) < 0) {
            PrintWindowGroup(L"GUID " + sm::GuidToString(g.guid) +
                                 L" (нет в списке десктопов — маркер pinned-окон?)",
                             false, g.items);
        }
    }

    if (!unknown.empty()) {
        PrintWindowGroup(L"десктоп не определён", false, unknown);
    }
}

// Запись окна до сортировки: глобальный z-order нужен, чтобы разложить окна по
// десктопам «сверху вниз» (PLAN.md §7 п.6).
struct PendingWindow {
    sm::LayoutWindow window;
    int z = 0;
};

const wchar_t* SaveShowStateText(int showCmd) {
    switch (showCmd) {
        case SW_SHOWMAXIMIZED:
            return L"maximized";
        case SW_SHOWMINIMIZED:
            return L"minimized";
        default:
            return L"normal";
    }
}

// Окно помечено в реестре как «показывать на всех рабочих столах»?
bool IsPinnedWindow(const std::vector<sm::PinnedApp>& pinnedApps, const sm::WindowInfo& info) {
    for (const sm::PinnedApp& pinned : pinnedApps) {
        if (sm::PinnedMatches(pinned, info.exePath, info.aumid)) {
            return true;
        }
    }
    return false;
}

bool NearlyEqual(const RECT& a, const RECT& b, LONG tolerance) {
    return std::abs(a.left - b.left) <= tolerance && std::abs(a.top - b.top) <= tolerance &&
           std::abs(a.right - b.right) <= tolerance && std::abs(a.bottom - b.bottom) <= tolerance;
}

// Дубль? Окна одного приложения с одинаковым заголовком на одном десктопе и почти
// в тех же координатах (PLAN.md §7 п.7). Pinned-окно пишется ровно один раз.
bool IsDuplicate(const std::vector<sm::LayoutWindow>& accepted,
                 const sm::LayoutWindow& candidate) {
    for (const sm::LayoutWindow& kept : accepted) {
        if (kept.exe != candidate.exe || kept.cls != candidate.cls ||
            kept.title != candidate.title) {
            continue;
        }
        if (kept.pinned != 0 || candidate.pinned != 0) {
            return true;
        }
        if (kept.desktop == candidate.desktop && NearlyEqual(kept.rect, candidate.rect, 8)) {
            return true;
        }
    }
    return false;
}

// Одно живое окно → запись слепка: десктоп, pinned, монитор-якорь и mrel
// (PLAN.md §7 п.5, §8.4 п.6).
sm::LayoutWindow MakeLayoutWindow(const sm::WindowInfo& info,
                                  const sm::VirtualDesktopManager& vdm,
                                  const std::vector<sm::DesktopInfo>& desktops,
                                  const std::vector<sm::PinnedApp>& pinnedApps,
                                  int fallbackDesktop) {
    sm::LayoutWindow window;

    GUID guid{};
    const bool haveGuid =
        vdm.ok() && vdm.GetWindowDesktopId(info.hwnd, &guid, false) && !sm::IsNullGuid(guid);
    int desktopIndex = haveGuid ? sm::FindDesktopIndex(desktops, guid) : -1;

    // GUID, которого нет в VirtualDesktopIDs, — служебный маркер pinned-окон
    // (проверено на этой машине: один и тот же GUID у всех pinned-окон), а не десктоп.
    window.pinned = (IsPinnedWindow(pinnedApps, info) || (haveGuid && desktopIndex < 0)) ? 1 : 0;
    if (desktopIndex < 0) {
        desktopIndex = fallbackDesktop;
    }
    window.desktop = desktopIndex;
    if (desktopIndex >= 0 && desktopIndex < static_cast<int>(desktops.size())) {
        window.desktopName = desktops[static_cast<size_t>(desktopIndex)].name;
    }

    window.exe = info.exePath;
    window.cmdline = info.cmdline;
    // Адрес страницы браузера (PLAN.md §8.3 п.2): в cmdline его нет — он живёт в
    // профиле, поэтому читаем через UI Automation. Сбой UIA не фатален: окно
    // сохранится без адреса, а при restore такая запись не запускается (§9.3).
    if (sm::IsBrowserExe(info.exeName)) {
        window.url = sm::ReadBrowserUrl(info.hwnd);
        sm::Trace(L"  браузер «%s»: адрес «%s»", info.title.c_str(),
                  window.url.empty() ? L"(не прочитан)" : window.url.c_str());
    }
    window.cls = info.cls;
    window.folder = info.folder;
    window.title = info.title;
    window.aumid = info.aumid;
    window.show = info.placementOk ? SaveShowStateText(info.placement.showCmd) : L"normal";
    window.uwp = (info.cloaked & sm::CloakedApp) != 0 ? 1 : 0;
    // Права окна (PLAN.md §9.3): restore поднимет это приложение с повышением, а
    // остальные — с обычными правами. Узнать не удалось (-1) — считается обычным:
    // попытка запуска без повышения либо сработает, либо уйдёт в admin.log.
    window.elevated = sm::ProcessElevation(info.pid) == 1 ? 1 : 0;
    if (window.elevated != 0) {
        sm::Trace(L"  окно «%s» открыто от администратора", info.title.c_str());
    }
    window.frame = info.frame;
    window.frameOk = info.frameOk;

    // Для maximized/minimized текущие границы бесполезны (монитор целиком либо
    // служебные -32000) — берём rcNormalPosition из WINDOWPLACEMENT.
    const RECT normal = info.placementOk ? info.placement.rcNormalPosition : info.rect;
    window.rect = normal;

    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    HMONITOR handle = MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST);
    if (handle != nullptr && GetMonitorInfoW(handle, &monitor)) {
        window.monitor = monitor.szDevice;
        window.mrel.left = normal.left - monitor.rcMonitor.left;
        window.mrel.top = normal.top - monitor.rcMonitor.top;
        window.mrel.right = normal.right - monitor.rcMonitor.left;
        window.mrel.bottom = normal.bottom - monitor.rcMonitor.top;
    } else {
        window.mrel = normal;  // без якоря координаты остаются абсолютными
    }
    return window;
}

int RunSave(const CommandLine& /*cl*/) {
    std::wstring error;

    // Единственная настройка, которая нужна save, — сколько рабочих версий держать
    // (ключ Layouts, PLAN.md §6.1). Сбой чтения Config.ini — не повод не сохранять слепок.
    sm::Config config;
    std::wstring configError;
    if (!sm::LoadConfig(&config, &configError)) {
        sm::Warn(L"Config.ini: %s — беру значения по умолчанию", configError.c_str());
    }
    sm::Trace(L"Config.ini: рабочих версий слепка %d", config.layoutVersions);

    // COM нужен для IVirtualDesktopManager: без него не узнать десктоп окна.
    if (!sm::InitCom(&error)) {
        sm::Error(L"%s", error.c_str());
        return kExitError;
    }

    sm::VirtualDesktopManager vdm;
    if (!vdm.Init(&error)) {
        sm::Warn(L"IVirtualDesktopManager недоступен: %s", error.c_str());
    }

    std::vector<sm::DesktopInfo> desktops;
    if (!sm::ReadDesktops(&desktops, &error)) {
        sm::Warn(L"список десктопов из реестра не прочитан: %s", error.c_str());
        if (!sm::CollectDesktopsFallback(vdm, &desktops, &error)) {
            sm::Error(L"не удалось получить список виртуальных десктопов: %s", error.c_str());
            return kExitError;
        }
        sm::Warn(L"порядок десктопов приблизительный: восстановлен по окнам");
    }

    std::vector<sm::PinnedApp> pinnedApps;
    if (!sm::ReadPinnedApps(&pinnedApps, &error)) {
        sm::Warn(L"pinned-приложения не прочитаны: %s", error.c_str());
    }

    sm::CurrentDesktopInfo current;
    sm::DetectCurrentDesktop(vdm, &current, &desktops);
    int currentIndex = current.known ? sm::FindDesktopIndex(desktops, current.guid) : -1;
    if (currentIndex < 0) {
        currentIndex = 0;  // окно без определимого десктопа считаем окном десктопа 0
    }

    sm::Layout layout;
    layout.saved = sm::NowTimestamp();
    layout.screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    layout.screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    for (const sm::DesktopInfo& desktop : desktops) {
        sm::LayoutDesktop item;
        item.index = desktop.index;
        item.guid = desktop.guid;
        item.name = desktop.name;
        layout.desktops.push_back(item);
    }
    if (!sm::CollectMonitors(&layout.monitors)) {
        sm::Warn(L"перечислить мониторы не удалось: секция [monitor.N] останется пустой");
    }

    std::vector<sm::WindowInfo> windows;
    sm::WindowEnumStats stats;
    if (!sm::CollectWindows(&windows, &stats)) {
        sm::Error(L"перечисление окон не удалось");
        return kExitError;
    }

    std::vector<PendingWindow> pending;
    pending.reserve(windows.size());
    for (const sm::WindowInfo& info : windows) {
        PendingWindow item;
        item.window = MakeLayoutWindow(info, vdm, desktops, pinnedApps, currentIndex);
        item.z = info.z;
        pending.push_back(std::move(item));
    }

    // Порядок в файле: по десктопу, внутри — сверху вниз по z-order (PLAN.md §7 п.6).
    std::sort(pending.begin(), pending.end(),
              [](const PendingWindow& a, const PendingWindow& b) {
                  if (a.window.desktop != b.window.desktop) {
                      return a.window.desktop < b.window.desktop;
                  }
                  return a.z < b.z;
              });

    int duplicates = 0;
    int pinnedCount = 0;
    int desktopCursor = -1;
    int zg = 0;
    for (const PendingWindow& item : pending) {
        if (IsDuplicate(layout.windows, item.window)) {
            ++duplicates;
            continue;
        }
        if (item.window.desktop != desktopCursor) {
            desktopCursor = item.window.desktop;
            zg = 0;
        }
        sm::LayoutWindow window = item.window;
        window.zg = zg++;
        if (window.pinned != 0) {
            ++pinnedCount;
        }
        layout.windows.push_back(std::move(window));
    }

    // AutoSave срабатывает каждые 10 минут, и переписывать файл каждый раз незачем:
    // если состав, координаты и состояние окон те же, layout.txt остаётся прежним и
    // ротация не крутится (иначе диск изнашивался бы впустую, PLAN.md §11).
    sm::Layout previous;
    std::wstring previousError;
    bool previousOk = false;
    if (sm::ReadLayoutFile(sm::ExeDir(), &previous, nullptr, &previousError)) {
        previousOk = true;
        const std::wstring reason = sm::LayoutDiffReason(previous, layout);
        if (reason.empty()) {
            sm::Info(L"Слепок не изменился с %s — файл не перезаписан", previous.saved.c_str());
            sm::Info(L"  десктопов %d, мониторов %d, окон %d (pinned %d)",
                     static_cast<int>(layout.desktops.size()),
                     static_cast<int>(layout.monitors.size()),
                     static_cast<int>(layout.windows.size()), pinnedCount);
            sm::Trace(L"  ротации нет: содержимое layout.txt совпадает с новым слепком");
            return kExitOk;
        }
        sm::Trace(L"слепок отличается от прежнего: %s", reason.c_str());
    } else {
        sm::Trace(L"прежний слепок не прочитан (%s) — пишу новый", previousError.c_str());
    }

    sm::LayoutSaveResult saved;
    if (!sm::WriteLayoutFile(sm::ExeDir(), layout, previousOk ? &previous : nullptr,
                             config.layoutVersions, &saved, &error)) {
        sm::Error(L"не удалось записать раскладку: %s", error.c_str());
        return kExitError;
    }

    sm::Info(L"Слепок сохранён: %s", saved.path.c_str());
    sm::Info(L"  десктопов %d, мониторов %d, окон %d (pinned %d)",
             static_cast<int>(layout.desktops.size()),
             static_cast<int>(layout.monitors.size()),
             static_cast<int>(layout.windows.size()), pinnedCount);
    if (duplicates > 0) {
        // Пояснение приклеено к числу, иначе строка читается как «что-то потеряли»
        // (CONTEXT п. 63): пропускаются только копии уже записанных окон.
        sm::Info(L"  кандидатов %d из %d верхнеуровневых окон, дублей пропущено %d (копии уже записанных окон)",
                 stats.candidates, stats.total, duplicates);
    } else {
        sm::Info(L"  кандидатов %d из %d верхнеуровневых окон", stats.candidates, stats.total);
    }
    // Адрес страницы — то, без чего браузер не восстановить (PLAN.md §8.3): показываем
    // итог, иначе сбой UIA виден только в --verbose, а при restore — как browser-no-url.
    int browserCount = 0;
    int browserWithUrl = 0;
    for (const sm::LayoutWindow& window : layout.windows) {
        if (!sm::IsBrowserExe(sm::FileNameOf(window.exe))) {
            continue;
        }
        ++browserCount;
        if (!window.url.empty()) {
            ++browserWithUrl;
        }
    }
    if (browserCount > 0) {
        sm::Info(L"  браузеров %d, адрес страницы прочитан у %d", browserCount, browserWithUrl);
    }
    // Смену конфигурации видно сразу: это не «ещё одна копия», а защищённый слепок, к
    // которому потом возвращаются через restore --from (PLAN.md §6.1).
    if (!saved.configCopy.empty()) {
        sm::Info(L"  конфигурация мониторов/десктопов изменилась: прежний слепок — %s",
                 saved.configCopy.c_str());
    }
    // Ротация версий и число удалённых старых копий — всегда так, поэтому в отчёт не
    // выводим: место для ротации сообщает Trace, когда включён --verbose (CONTEXT п. 63).
    sm::Trace(L"  предыдущая версия: %s; старых версий удалено: %d",
              saved.rotated.empty() ? L"—" : saved.rotated.c_str(), saved.pruned);

    // Самопроверка формата (--verbose): читаем только что записанный файл обратно.
    // Ловит расхождения записи и разбора (экранирование, «;» в заголовках, многострочные
    // заголовки) сразу при save, а не на restore. На результат save не влияет.
    if (sm::IsVerbose()) {
        sm::Layout check;
        std::wstring readError;
        if (!sm::ReadLayoutFile(sm::ExeDir(), &check, nullptr, &readError)) {
            sm::Warn(L"самопроверка: файл не читается обратно: %s", readError.c_str());
        } else {
            int mismatches = 0;
            const size_t common = (std::min)(layout.windows.size(), check.windows.size());
            for (size_t i = 0; i < common; ++i) {
                const sm::LayoutWindow& written = layout.windows[i];
                const sm::LayoutWindow& read = check.windows[i];
                if (written.exe != read.exe || written.cls != read.cls ||
                    written.title != read.title || written.desktop != read.desktop ||
                    written.mrel.left != read.mrel.left || written.mrel.top != read.mrel.top ||
                    written.mrel.right != read.mrel.right ||
                    written.mrel.bottom != read.mrel.bottom) {
                    ++mismatches;
                }
            }
            const bool sameCounts = layout.windows.size() == check.windows.size() &&
                                    layout.desktops.size() == check.desktops.size() &&
                                    layout.monitors.size() == check.monitors.size();
            if (!sameCounts || mismatches != 0 || layout.saved != check.saved) {
                sm::Warn(L"самопроверка: расхождения (окон %d из %d, полей %d)",
                         static_cast<int>(check.windows.size()),
                         static_cast<int>(layout.windows.size()), mismatches);
            } else {
                sm::Trace(L"самопроверка: файл прочитан обратно без расхождений");
            }
        }
    }
    return kExitOk;
}

// ----------------------------------------------------------------- restore ---

// Одна пара «запись ↔ живое окно»: геометрия через SetWindowPlacement (PLAN.md
// §8.2 п.2) и перенос на десктоп (§8.2 п.3–4). false — применить не удалось,
// причина в problem. movedDesktop — окно действительно переехало на другой стол.
bool ApplyMatch(const sm::LayoutWindow& saved, HWND hwnd, const RECT& target, int showCmd,
                const sm::DesktopTarget& desktopTarget, const sm::VirtualDesktopManager& vdm,
                bool* movedDesktop, std::wstring* problem) {
    if (movedDesktop != nullptr) {
        *movedDesktop = false;
    }

    // SetWindowPlacement — единственный корректный путь для maximized/minimized:
    // rcNormalPosition задаёт «нормальную» позицию, showCmd — состояние окна.
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) {
        *problem = L"GetWindowPlacement: " + sm::Win32ErrorText(GetLastError());
        return false;
    }
    wp.rcNormalPosition = target;
    wp.showCmd = showCmd;
    if (!SetWindowPlacement(hwnd, &wp)) {
        *problem = L"SetWindowPlacement: " + sm::Win32ErrorText(GetLastError());
        return false;
    }

    // UIPI: окно приложения, запущенного от администратора, изменения молча игнорирует
    // (PLAN.md §9.3) — поэтому проверяем, что позиция действительно применилась.
    WINDOWPLACEMENT after{};
    after.length = sizeof(after);
    if (GetWindowPlacement(hwnd, &after) && !NearlyEqual(after.rcNormalPosition, target, 2)) {
        *problem = L"позиция не применилась (окно приложения с правами администратора?)";
        return false;
    }

    // Pinned-окна видны на всех столах, MoveWindowToDesktop для них не работает
    // (PLAN.md §8.2 п.3) — им назначаем только геометрию.
    if (saved.pinned != 0 || sm::IsNullGuid(desktopTarget.guid)) {
        return true;
    }
    GUID current{};
    const bool known = vdm.ok() && vdm.GetWindowDesktopId(hwnd, &current, false) &&
                       !sm::IsNullGuid(current);
    if (known && IsEqualGUID(current, desktopTarget.guid) != FALSE) {
        return true;  // окно уже на месте: лишний вызов не нужен
    }
    if (!vdm.ok()) {
        *problem = L"MoveWindowToDesktop: IVirtualDesktopManager недоступен";
        return false;
    }
    // Только что запущенное окно оболочка регистрирует не сразу — поэтому даём три
    // попытки. Для окон, живущих давно, неудача повторится и попадёт в admin.log как
    // «move-desktop-failed».
    //
    // Успешный возврат вызова — ещё не перенос: оболочка может «принять» перенос и не
    // выполнить его, поэтому после каждого вызова перепроверяем фактический десктоп
    // окна (CONTEXT п. 52).
    HRESULT hr = E_FAIL;
    sm::Trace(L"  перенос %p на десктоп %s", static_cast<void*>(hwnd),
              sm::GuidToString(desktopTarget.guid).c_str());
    for (int attempt = 0; attempt < 3; ++attempt) {
        HRESULT callHr = E_FAIL;
        if (vdm.MoveWindowToDesktop(hwnd, desktopTarget.guid, &callHr)) {
            GUID now{};
            if (!vdm.GetWindowDesktopId(hwnd, &now, false) ||
                IsEqualGUID(now, desktopTarget.guid) != FALSE) {
                if (movedDesktop != nullptr) {
                    *movedDesktop = true;
                }
                return true;
            }
            if (sm::IsVerbose()) {
                sm::Trace(L"  перенос не подтвердился: %p на %s", static_cast<void*>(hwnd),
                          sm::GuidToString(now).c_str());
            }
            hr = HRESULT_FROM_WIN32(ERROR_RETRY);
        } else {
            hr = callHr;
        }
        if (attempt < 2) {
            Sleep(400);
        }
    }
    *problem = L"MoveWindowToDesktop: перенести окно не удалось (" + sm::HrText(hr) + L")";
    return false;
}

// Причина для admin.log по тексту ошибки применения (PLAN.md §9.3): молча
// проигнорированное перемещение (UIPI) и отказ доступа — это «move-blocked».
sm::AdminReason AdminReasonForProblem(const std::wstring& problem) {
    if (problem.find(L"позиция не применилась") != std::wstring::npos ||
        problem.find(L"Отказано в доступе") != std::wstring::npos) {
        return sm::AdminReason::MoveBlocked;
    }
    return sm::AdminReason::MoveDesktopFailed;
}

std::wstring DesktopNameOrStub(const std::wstring& name) {
    return name.empty() ? std::wstring(L"(без имени)") : L"«" + name + L"»";
}

// Окна запущенных приложений «успокоились»? Часть приложений выставляет своё окно уже
// после появления: nw.js (AudioPL) и некоторые Electron-приложения ставят окно по своему
// сохранённому месту, и перенос в тот же момент они переигрывают — окно остаётся посреди
// экрана. Поэтому геометрию ставим только после того, как прямоугольники всех окон
// перестали меняться: одинаковые два опроса подряд (шаг 500 мс), но не дольше maxMs.
void WaitRectsStable(const std::vector<HWND>& windows, int quietMs, int maxMs) {
    if (windows.empty()) {
        return;
    }
    std::vector<RECT> previous(windows.size());
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline = started + static_cast<ULONGLONG>(maxMs < 0 ? 0 : maxMs);
    ULONGLONG stableSince = started;
    for (;;) {
        Sleep(500);
        bool changed = false;
        for (size_t i = 0; i < windows.size(); ++i) {
            RECT now{};
            if (IsWindow(windows[i]) == FALSE || GetWindowRect(windows[i], &now) == FALSE) {
                continue;  // окно закрылось — его не ждём
            }
            if (now.left != previous[i].left || now.top != previous[i].top ||
                now.right != previous[i].right || now.bottom != previous[i].bottom) {
                changed = true;
                previous[i] = now;
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (changed) {
            stableSince = now;
        } else if (now - stableSince >= static_cast<ULONGLONG>(quietMs < 0 ? 0 : quietMs)) {
            sm::Trace(L"  окна запущенных приложений на месте: ждали %.1f с",
                      static_cast<double>(now - started) / 1000.0);
            return;
        }
        if (now >= deadline) {
            sm::Trace(L"  прямоугольники окон всё ещё меняются через %.1f с — переношу как есть",
                      static_cast<double>(now - started) / 1000.0);
            return;
        }
    }
}

// Карта «десктоп из файла → десктоп сейчас» (PLAN.md §8.0 п.3). Печатается всегда:
// если окна уехали не туда, по ней сразу видно, на каком шаге mapping ошибся.
void PrintDesktopMap(const sm::Layout& layout, const std::vector<sm::DesktopTarget>& targets) {
    int byGuid = 0;
    int byName = 0;
    int byIndex = 0;
    int fallback = 0;
    for (const sm::DesktopTarget& target : targets) {
        switch (target.kind) {
            case sm::DesktopMatchKind::Guid:
                ++byGuid;
                break;
            case sm::DesktopMatchKind::Name:
                ++byName;
                break;
            case sm::DesktopMatchKind::Index:
                ++byIndex;
                break;
            default:
                ++fallback;
                break;
        }
    }
    sm::Info(L"Десктопы: в файле %d; по GUID %d, по имени %d, по индексу %d, fallback %d",
             static_cast<int>(layout.desktops.size()), byGuid, byName, byIndex, fallback);

    for (size_t i = 0; i < layout.desktops.size() && i < targets.size(); ++i) {
        const sm::LayoutDesktop& saved = layout.desktops[i];
        const sm::DesktopTarget& target = targets[i];
        std::wstring notes;
        if (target.renamed) {
            notes += L", переименован";
        }
        if (target.nameNotFound) {
            notes += L", имени нет в системе";
        }
        if (target.ambiguousName) {
            notes += L", имя не уникально";
        }
        sm::Info(L"  [%d] %s → [%d] %s   по: %s%s", saved.index,
                 DesktopNameOrStub(saved.name).c_str(), target.currentIndex,
                 DesktopNameOrStub(target.currentName).c_str(),
                 sm::DesktopMatchKindText(target.kind), notes.c_str());
    }
}

int RunRestore(const CommandLine& cl) {
    std::wstring error;

    // Настройка, которая нужна restore, — settle-wait (PLAN.md §8.3 п.0): сколько ждать
    // «успокоения» окон, которые система и браузеры восстанавливают сами. Сбой чтения
    // Config.ini — не повод не восстанавливать раскладку: работаем со значением по умолчанию.
    sm::Config config;
    std::wstring configError;
    if (!sm::LoadConfig(&config, &configError)) {
        sm::Warn(L"Config.ini: %s — беру значения по умолчанию", configError.c_str());
    }
    sm::Trace(L"Config.ini: settle-wait=%d с", config.settleWaitSec);

    std::wstring path;
    sm::Layout layout;
    const bool fromFile = !cl.from.empty();
    const bool read =
        fromFile ? sm::ReadLayoutFromFile(sm::ExeDir(), cl.from, &layout, &path, &error)
                 : sm::ReadLayoutFile(sm::ExeDir(), &layout, &path, &error);
    if (!read) {
        sm::Error(L"%s", error.c_str());
        sm::Info(L"подсказка: снять слепок заново — ScreenMemory.exe save; восстановить из "
                 L"другого файла — ScreenMemory.exe restore --from layout-<время>.txt "
                 L"(есть ещё layout-вчерашний-день.txt и layout-прежняя-конфигурация.txt)");
        return kExitNoLayout;
    }

    sm::Info(L"Раскладка: %s", path.c_str());
    sm::Info(L"  сохранена %s; десктопов %d, мониторов %d, окон %d", layout.saved.c_str(),
             static_cast<int>(layout.desktops.size()), static_cast<int>(layout.monitors.size()),
             static_cast<int>(layout.windows.size()));
    if (cl.dryRun) {
        sm::Info(L"  режим --dry-run: окна не перемещаются, ничего не запускается");
    }

    // Цепочка дней (PLAN.md §6.1 п.5) двигается здесь, а не в save: «новая сессия» — это
    // именно восстановление после загрузки. Слепок, с которым восстанавливаемся, становится
    // «вчерашним», прежний «вчерашний» уезжает в «позавчерашний» и т. д. Второй и
    // следующие restore в той же сессии цепочку не трогают: слепок либо снят уже после
    // загрузки, либо уже стоит во «вчерашнем». С файлом --from цепочку не двигаем: это
    // осознанный откат, а не начало нового дня.
    if (!cl.dryRun && !fromFile) {
        sm::DayChainResult chain;
        std::wstring chainError;
        if (!sm::ShiftDayChain(sm::ExeDir(), layout, &chain, &chainError)) {
            sm::Warn(L"цепочка дней не сдвинута: %s", chainError.c_str());
        } else if (chain.shifted) {
            sm::Info(L"  цепочка дней сдвинута: %s ← слепок %s", chain.yesterday.c_str(),
                     layout.saved.c_str());
            sm::Trace(L"  прежде «вчерашним» был слепок %s",
                      chain.previous.empty() ? L"—" : chain.previous.c_str());
        } else {
            sm::Trace(L"  цепочка дней: %s", chain.reason.c_str());
        }
    }
    sm::Info(L"");

    if (!sm::InitCom(&error)) {
        sm::Error(L"%s", error.c_str());
        return kExitError;
    }

    sm::VirtualDesktopManager vdm;
    if (!vdm.Init(&error)) {
        sm::Warn(L"IVirtualDesktopManager недоступен: %s", error.c_str());
    }

    std::vector<sm::DesktopInfo> desktops;
    if (!sm::ReadDesktops(&desktops, &error)) {
        sm::Warn(L"список десктопов из реестра не прочитан: %s", error.c_str());
        if (!sm::CollectDesktopsFallback(vdm, &desktops, &error)) {
            sm::Error(L"не удалось получить список виртуальных десктопов: %s", error.c_str());
            return kExitNoDesktops;
        }
        sm::Warn(L"порядок десктопов приблизительный: восстановлен по окнам");
    }

    std::vector<sm::DesktopTarget> targets;
    if (!sm::MapDesktops(layout, desktops, &targets, &error)) {
        sm::Error(L"%s", error.c_str());
        return kExitNoDesktops;
    }
    PrintDesktopMap(layout, targets);
    sm::Info(L"");

    // Мониторы сейчас: по ним пересчитывается mrel (PLAN.md §8.4).
    std::vector<sm::LayoutMonitor> liveMonitors;
    if (!sm::CollectMonitors(&liveMonitors)) {
        sm::Warn(L"мониторы не перечислены: координаты берём из абсолютных rect записей");
    }

    // ------------------------------------------------------------------------
    // Ранний запуск браузеров (CONTEXT п. 70, PLAN.md §8.3 п.0).
    //
    // Браузер при холодном старте сам восстанавливает сессию профиля («Продолжить с того
    // же места»), но делает это не сразу: на прогоне 16.11.2026 его окна продолжали
    // открываться спустя минуту после снимка фазы A, и фаза B открывала те же адреса
    // второй раз. Поэтому браузер, который есть в layout.txt и ещё не запущен, поднимаем
    // здесь — до снятия снимка, — и ждём AutoRestoreDelay секунд: за это время он
    // восстановит свои окна, они попадут в снимок и будут расставлены по адресам.
    // Уже запущенный браузер не трогаем: его сессия восстановлена при входе в систему.
    if (!cl.dryRun) {
        struct BrowserLaunch {
            std::wstring exe;
            std::wstring cls;
            int records = 0;
            bool elevated = false;  // хотя бы одна запись была открыта от администратора
        };
        std::vector<BrowserLaunch> browsers;
        for (const sm::LayoutWindow& record : layout.windows) {
            if (record.exe.empty() || !sm::IsBrowserExe(sm::FileNameOf(record.exe))) {
                continue;
            }
            BrowserLaunch* browser = nullptr;
            for (BrowserLaunch& item : browsers) {
                if (_wcsicmp(item.exe.c_str(), record.exe.c_str()) == 0) {
                    browser = &item;
                    break;
                }
            }
            if (browser == nullptr) {
                browsers.push_back(BrowserLaunch{record.exe, record.cls, 0, false});
                browser = &browsers.back();
            }
            if (browser->cls.empty()) {
                browser->cls = record.cls;
            }
            ++browser->records;
            if (record.elevated != 0) {
                browser->elevated = true;
            }
        }

        if (!browsers.empty()) {
            // Окна, которые есть до прогрева: по этому списку потом видно, что браузер
            // открыл уже после (пустое окно «Новая вкладка» его же и создано).
            std::vector<HWND> knownBefore;
            std::vector<sm::WindowInfo> before;
            sm::WindowEnumStats beforeStats;
            if (sm::CollectWindows(&before, &beforeStats)) {
                knownBefore.reserve(before.size());
                for (const sm::WindowInfo& info : before) {
                    knownBefore.push_back(info.hwnd);
                }
            }
            // Первое окно браузера ждём недолго: если его нет и через полминуты, прогрев
            // не удался — фаза B запустит страницы обычным путём.
            constexpr int kWarmTimeoutMs = 30000;
            int warmed = 0;
            for (const BrowserLaunch& browser : browsers) {
                if (sm::IsExeRunning(browser.exe)) {
                    sm::Trace(L"  браузер %s уже запущен — ранний запуск не нужен",
                              sm::FileNameOf(browser.exe).c_str());
                    continue;
                }
                // Прогрев — без адреса: страницы откроет фаза B, а здесь важно дать
                // браузеру восстановить сессию и получить свои окна в снимок фазы A.
                // Права — как у фазы B (PLAN.md §9.3): при повышенном restore обычный
                // браузер поднимается с токеном оболочки, иначе он станет администраторским
                // и не увидит профиль пользователя.
                const sm::LaunchRights rights =
                    (sm::IsElevated() && !browser.elevated) ? sm::LaunchRights::AsUser
                                                            : sm::LaunchRights::Default;
                std::vector<HWND> known = knownBefore;
                if (!sm::WarmUpBrowser(browser.exe, browser.cls, &known, kWarmTimeoutMs, rights)) {
                    sm::Trace(L"  «%s»: ранний запуск не удался — работаю как обычно",
                              sm::FileNameOf(browser.exe).c_str());
                    continue;
                }
                ++warmed;
                sm::Info(L"  браузер не запущен: поднимаю сразу — %s (записей в раскладке: %d)",
                         sm::FileNameOf(browser.exe).c_str(), browser.records);
            }
            if (warmed > 0) {
                if (config.autoRestoreDelaySec > 0) {
                    sm::Info(L"  пауза %d с (AutoRestoreDelay): браузер восстанавливает свои окна",
                             config.autoRestoreDelaySec);
                    Sleep(static_cast<DWORD>(config.autoRestoreDelaySec) * 1000);
                }
                // Пустые окна, которые браузер открыл сам при холодном старте: в слепке их
                // нет, а на текущем десктопе они остались бы лишними. Окна восстановленной
                // сессии к этому моменту уже показывают свои адреса — их не трогаем.
                for (const BrowserLaunch& browser : browsers) {
                    if (browser.cls.empty() || !sm::IsExeRunning(browser.exe)) {
                        continue;
                    }
                    const int closed = sm::CloseEmptyBrowserWindows(browser.cls, knownBefore, nullptr);
                    if (closed > 0) {
                        sm::Trace(L"  «%s»: закрыто пустых окон после раннего запуска: %d",
                                  sm::FileNameOf(browser.exe).c_str(), closed);
                    }
                }
            }
            sm::Info(L"");
        }
    } else {
        sm::Trace(L"--dry-run: ранний запуск браузеров не выполняется");
    }

    std::vector<sm::WindowInfo> live;
    sm::WindowEnumStats stats;
    if (!sm::CollectWindows(&live, &stats)) {
        sm::Error(L"перечисление окон не удалось");
        return kExitError;
    }

    // Десктоп каждого живого окна: нужен и для матчинга (разрешение ничьих), и для
    // того, чтобы не звать MoveWindowToDesktop там, где окно уже на месте.
    std::vector<GUID> liveDesktop(live.size());
    for (size_t i = 0; i < live.size(); ++i) {
        GUID guid{};
        if (vdm.ok()) {
            vdm.GetWindowDesktopId(live[i].hwnd, &guid, false);
        }
        liveDesktop[i] = guid;
    }

    // Адрес страницы у живых окон браузеров (PLAN.md §8.1): по нему видно, что окно
    // показывает другую страницу, и тогда запись вкладки к нему не прилипает (CONTEXT
    // п. 55 — Chrome с баллом 210 утащили на чужой стол). Зачем условие: UIA — это
    // межпроцессные вызовы, и на неотвечающем окне каждый ждёт до таймаута, так что
    // читать адрес, когда в слепке не с чем его сравнивать (нет ни одной браузерной
    // записи с адресом), — работа без результата. Когда сравнивать есть с чем,
    // читаем у всех окон браузеров и с повтором: время не критично, качество важнее.
    bool needLiveUrls = false;
    for (const sm::LayoutWindow& window : layout.windows) {
        if (!window.url.empty() && sm::IsBrowserExe(sm::FileNameOf(window.exe))) {
            needLiveUrls = true;
            break;
        }
    }
    if (needLiveUrls) {
        int liveUrls = 0;
        for (size_t i = 0; i < live.size(); ++i) {
            if (!sm::IsBrowserExe(live[i].exeName)) {
                continue;
            }
            live[i].url = sm::ReadBrowserUrl(live[i].hwnd);
            if (live[i].url.empty()) {
                continue;
            }
            ++liveUrls;
            sm::Trace(L"  живой браузер «%s»: адрес «%s»", live[i].title.c_str(),
                      live[i].url.c_str());
        }
        // Второй заход для окон, где адрес не прочитался: UIA мог не успеть (окно
        // перерисовывается, вкладка ещё грузится). Время не критично — без адреса
        // окно браузера сопоставляется «слепым» и может прилипнуть к чужой записи.
        for (size_t i = 0; i < live.size(); ++i) {
            if (!sm::IsBrowserExe(live[i].exeName) || !live[i].url.empty()) {
                continue;
            }
            Sleep(500);
            live[i].url = sm::ReadBrowserUrl(live[i].hwnd);
            if (live[i].url.empty()) {
                sm::Trace(L"  живой браузер «%s»: адрес не прочитан и со второй попытки",
                          live[i].title.c_str());
                continue;
            }
            ++liveUrls;
            sm::Trace(L"  живой браузер «%s»: адрес «%s» (со второй попытки)",
                      live[i].title.c_str(), live[i].url.c_str());
        }
        sm::Trace(L"  адрес страницы прочитан у %d окон браузеров", liveUrls);
    }

    std::vector<sm::WindowMatch> matches;
    sm::MatchStats matchStats;
    sm::MatchWindows(layout, live, targets, liveDesktop, desktops, &matches, &matchStats);
    sm::Info(L"Окна: в файле %d, живых %d, сопоставлено %d", static_cast<int>(layout.windows.size()),
             stats.candidates, static_cast<int>(matches.size()));
    if (matchStats.ambiguous > 0) {
        // Записи, которые не сопоставлены намеренно: данные не позволяют отличить окна
        // одного приложения на разных столах. Живые окна не трогаем — эти записи уйдут
        // в фазу B, и приложение откроет себе отдельное окно (лучше дубль, чем окно
        // с чужим содержимым на неверном столе).
        sm::Warn(L"  неоднозначно: %d записей — одинаковые окна на разных десктопах не сопоставлены (окна не трогаем)",
                 matchStats.ambiguous);
    }
    if (sm::IsVerbose()) {
        for (const sm::WindowMatch& match : matches) {
            const sm::LayoutWindow& saved = layout.windows[match.saved];
            const sm::WindowInfo& info = live[match.live];
            sm::Trace(L"  пара %d б. (%s): файл «%s» [%s] ↔ %p «%s» [%s]", match.score,
                      match.why.c_str(), saved.title.c_str(), saved.show.c_str(),
                      static_cast<void*>(info.hwnd), info.title.c_str(),
                      sm::ShowStateText(info));
        }
    }

    // PLAN.md §9.1: перенести чужое окно на другой десктоп умеет только внутренний
    // интерфейс оболочки. Документированный IVirtualDesktopManager::MoveWindowToDesktop
    // для чужих окон отвечает E_ACCESSDENIED, поэтому «тихий» переход на него оставил бы
    // окна не на своих столах, а отчёт выглядел бы успешным. Если самопроверка
    // внутреннего интерфейса не прошла (другая сборка Windows), работу прекращаем ДО
    // первого изменения и объясняем причину.
    int internalExit = 0;
    {
        int needDesktopMoves = 0;
        for (const sm::WindowMatch& match : matches) {
            const sm::LayoutWindow& saved = layout.windows[match.saved];
            if (saved.pinned != 0) {
                continue;
            }
            const int slot = sm::DesktopSlot(layout, saved.desktop);
            if (slot < 0 || slot >= static_cast<int>(targets.size())) {
                continue;
            }
            const sm::DesktopTarget& target = targets[static_cast<size_t>(slot)];
            if (!sm::IsNullGuid(target.guid) &&
                IsEqualGUID(liveDesktop[match.live], target.guid) == FALSE) {
                ++needDesktopMoves;
            }
        }

        // Окна фазы B появятся на текущем десктопе: если их целевой стол не текущий,
        // перенос понадобится и для них.
        sm::CurrentDesktopInfo currentDesktop;
        if (sm::DetectCurrentDesktop(vdm, &currentDesktop, &desktops) && currentDesktop.known) {
            std::vector<char> hasWindow(layout.windows.size(), 0);
            for (const sm::WindowMatch& match : matches) {
                if (match.saved >= 0 && match.saved < static_cast<int>(hasWindow.size())) {
                    hasWindow[static_cast<size_t>(match.saved)] = 1;
                }
            }
            for (size_t i = 0; i < layout.windows.size(); ++i) {
                if (hasWindow[i] != 0) {
                    continue;
                }
                const sm::LayoutWindow& saved = layout.windows[i];
                if (saved.pinned != 0 || sm::PlanLaunch(saved).kind == sm::LaunchKind::None) {
                    continue;
                }
                const int slot = sm::DesktopSlot(layout, saved.desktop);
                if (slot < 0 || slot >= static_cast<int>(targets.size())) {
                    continue;
                }
                const sm::DesktopTarget& target = targets[static_cast<size_t>(slot)];
                if (!sm::IsNullGuid(target.guid) &&
                    IsEqualGUID(currentDesktop.guid, target.guid) == FALSE) {
                    ++needDesktopMoves;
                }
            }
        }

        if (needDesktopMoves > 0 && !sm::EnsureInternalApiUsable()) {
            sm::Error(L"перенос окон между десктопами невозможен: %s",
                      sm::InternalApiReason().c_str());
            sm::Error(L"  нужно перенести окон: %d", needDesktopMoves);
            sm::Error(L"  документированный IVirtualDesktopManager::MoveWindowToDesktop");
            sm::Error(L"  переносит только окна своего процесса, для чужих он отвечает");
            sm::Error(L"  E_ACCESSDENIED — окна остались бы не на своих десктопах");
            sm::Error(L"  ничего не изменено; что делать: сборка Windows отличается от");
            sm::Error(L"  рассчитанной — слоты интерфейсов проверены на Win10 1809–22H2 и");
            sm::Error(L"  Win11 21H2+; проверьте поддержку или соберите программу под неё");
            if (!cl.dryRun) {
                return kExitInternalApi;
            }
            internalExit = kExitInternalApi;
        }
    }

    // Фаза A (PLAN.md §8.2): геометрия + перенос на нужный десктоп. Фаза C (§8.5):
    // z-order внутри десктопа, чтобы окна легли так же, как были.
    int applied = 0;
    int movedToDesktop = 0;
    int wouldMove = 0;
    int failed = 0;
    // Окна, чей сохранённый десктоп не нашёлся: они уезжают на десктоп 0 (требование 15,
    // PLAN.md §8.6 — отдельный счётчик `fallback-desktop`). Считаем их по факту в конце
    // восстановления: `applied` и «оказалось на десктопе 0» — разные вещи, применение
    // могло и не пройти (CONTEXT п. 52).
    int fallbackWindows = 0;    // действительно оказались на десктопе 0
    int fallbackNotMoved = 0;   // должны были, но остались на другом столе
    int fallbackUnknown = 0;    // десктоп проверить не удалось (окно недоступно)
    int wouldFallback = 0;
    GUID fallbackGuid{};
    std::vector<HWND> fallbackCandidates;
    const auto rememberFallback = [&fallbackGuid, &fallbackCandidates](
                                      HWND hwnd, const sm::DesktopTarget& target) {
        if (sm::IsNullGuid(fallbackGuid)) {
            fallbackGuid = target.guid;
        }
        fallbackCandidates.push_back(hwnd);
    };
    std::vector<HWND> zOrderWindows;
    std::vector<int> zOrderZg;

    for (const sm::WindowMatch& match : matches) {
        const sm::LayoutWindow& saved = layout.windows[match.saved];
        const sm::WindowInfo& info = live[match.live];
        const int slot = sm::DesktopSlot(layout, saved.desktop);
        if (slot < 0 || slot >= static_cast<int>(targets.size())) {
            sm::Warn(L"«%s»: десктопа %d нет в файле", info.title.c_str(), saved.desktop);
            ++failed;
            continue;
        }
        const sm::DesktopTarget& target = targets[static_cast<size_t>(slot)];

        RECT rect{};
        std::wstring note;
        if (!sm::TargetRect(saved, layout.monitors, liveMonitors, &rect, &note)) {
            sm::Warn(L"«%s»: координаты не посчитаны (мониторы не перечислены)",
                     info.title.c_str());
            ++failed;
            continue;
        }

        // Перенос нужен только не-pinned окну, которое сейчас не на целевом десктопе.
        const bool needMove = saved.pinned == 0 && !sm::IsNullGuid(target.guid) &&
                              IsEqualGUID(liveDesktop[match.live], target.guid) == FALSE;

        // Кандидат «должно быть на десктопе 0» запоминается до применения: даже если
        // геометрию или перенос применить не удалось, в отчёте должно быть видно, где
        // окно оказалось на самом деле.
        if (saved.pinned == 0 && target.kind == sm::DesktopMatchKind::Fallback) {
            rememberFallback(info.hwnd, target);
        }

        if (cl.dryRun) {
            ++applied;
            if (needMove) {
                ++wouldMove;
            }
            if (saved.pinned == 0 && target.kind == sm::DesktopMatchKind::Fallback) {
                ++wouldFallback;
            }
            std::wstring extras;
            if (saved.pinned != 0) {
                extras += L"  pinned";
            }
            if (needMove) {
                extras += L"  перенос на десктоп";
            }
            if (!note.empty()) {
                extras += L"  (" + note + L")";
            }
            sm::Info(L"  %-44s %s → %s  стол [%d] %s  %s%s", FitText(info.title, 44).c_str(),
                     sm::NormalRectText(info).c_str(), sm::RectToText(rect).c_str(),
                     target.currentIndex, DesktopNameOrStub(target.currentName).c_str(),
                     saved.show.c_str(), extras.c_str());
            continue;
        }

        bool moved = false;
        std::wstring problem;
        if (!ApplyMatch(saved, info.hwnd, rect, sm::ShowCommandFor(saved.show), target, vdm, &moved,
                        &problem)) {
            sm::Warn(L"«%s»: %s", info.title.c_str(), problem.c_str());
            // PLAN.md §9.3: то, что требует прав администратора, уходит в admin.log
            // готовой командой — её достаточно скопировать в cmd.exe.
            sm::AddAdminEntry(AdminReasonForProblem(problem), sm::AdminCommand(saved));
            ++failed;
            continue;
        }
        ++applied;
        if (moved) {
            ++movedToDesktop;
        }
        zOrderWindows.push_back(info.hwnd);
        zOrderZg.push_back(saved.zg);
        if (sm::IsVerbose()) {
            sm::Trace(L"  применено: %p «%s» → %s, стол [%d]%s", static_cast<void*>(info.hwnd),
                      info.title.c_str(), sm::RectToText(rect).c_str(), target.currentIndex,
                      moved ? L", перенесено" : L"");
        }
    }

    // ------------------------------------------------------------------------
    // Перепроверка «окно могло открыться само» (PLAN.md §8.3 п.0, CONTEXT п. 69).
    //
    // Снимок окон для фазы A снят один раз. Но после входа в систему окна открывает не
    // только restore: Windows возвращает окна Проводника (`PersistBrowsers=1` —
    // «Восстанавливать прежние окна папок при входе») и приложения из автозагрузки,
    // браузер — сессию профиля («Продолжить с того же места»). Это идёт в те же секунды,
    // что и восстановление раскладки: на прогоне 16.11.2026 в снимке фазы A было 33 окна,
    // а через минуту — 72, и часть адресов Chrome открыл сам, пока фаза B открывала их же.
    // Дубль получается так: в фазе A окна записи ещё не было (пара не создана), фаза B
    // запускает приложение, а через секунду его окно появляется само.
    //
    // Поэтому перед каждым запуском окна снимаются заново, а запись до-сопоставляется с
    // окнами, которые ещё ничьи (те же правила, что в фазе A). Найденное окно просто
    // ставится на место — второй экземпляр приложения не запускается.
    std::vector<HWND> takenWindows;
    takenWindows.reserve(matches.size());
    for (const sm::WindowMatch& match : matches) {
        takenWindows.push_back(live[match.live].hwnd);
    }
    // Занято ли окно: сопоставлено в фазе A, запущено или найдено перепроверкой.
    const auto isWindowTaken = [&takenWindows](HWND hwnd) {
        return std::find(takenWindows.begin(), takenWindows.end(), hwnd) != takenWindows.end();
    };

    // Адрес страницы живого окна читается через UIA — это межпроцессные вызовы, повторять
    // их для окна, у которого адрес уже есть, незачем. Пустой адрес повторяем (страница
    // могла ещё грузиться), но не бесконечно: три попытки на окно.
    struct UrlCacheEntry {
        std::wstring url;
        int attempts = 0;
    };
    std::map<HWND, UrlCacheEntry> urlCache;
    for (const sm::WindowInfo& info : live) {
        if (!info.url.empty()) {
            urlCache[info.hwnd] = UrlCacheEntry{info.url, 1};
        }
    }

    // Снимок живых окон «прямо сейчас»: сведения о процессе, десктоп, адрес страницы.
    // false — перечислить окна не удалось (перепроверка тогда просто ничего не находит).
    const auto captureWindows = [&vdm, &urlCache](std::vector<sm::WindowInfo>* windows,
                                                  std::vector<GUID>* windowDesktops) {
        windows->clear();
        windowDesktops->clear();
        sm::WindowEnumStats snapshot;
        if (!sm::CollectWindows(windows, &snapshot)) {
            return false;
        }
        windowDesktops->assign(windows->size(), GUID{});
        for (size_t i = 0; i < windows->size(); ++i) {
            sm::WindowInfo& info = (*windows)[i];
            if (vdm.ok()) {
                GUID guid{};
                if (vdm.GetWindowDesktopId(info.hwnd, &guid, false)) {
                    (*windowDesktops)[i] = guid;
                }
            }
            if (!sm::IsBrowserExe(info.exeName)) {
                continue;
            }
            const auto cached = urlCache.find(info.hwnd);
            if (cached != urlCache.end() &&
                (!cached->second.url.empty() || cached->second.attempts >= 3)) {
                info.url = cached->second.url;
                continue;
            }
            UrlCacheEntry entry;
            entry.url = sm::ReadBrowserUrl(info.hwnd);
            entry.attempts = (cached != urlCache.end()) ? cached->second.attempts + 1 : 1;
            urlCache[info.hwnd] = entry;
            info.url = entry.url;
        }
        return true;
    };

    // Перепроверка одной записи: HWND подходящего живого окна либо nullptr. Окно здесь
    // только ищется — на место его ставит общий код фазы B (счётчики и строки отчёта
    // должны быть одни и те же).
    const auto recheckRecord = [&](int recordIndex) -> HWND {
        const sm::LayoutWindow& record = layout.windows[static_cast<size_t>(recordIndex)];
        std::vector<sm::WindowInfo> fresh;
        std::vector<GUID> freshDesktops;
        std::vector<char> taken;
        // У браузерной записи адрес страницы — главный признак содержимого окна. Если у
        // кого-то из ещё не занятых окон браузера адрес не прочитан, сопоставление
        // рискует прилепить запись к чужому окну (exe + class дают 150 при пороге 100),
        // поэтому даём окнам ещё пару попыток: UIA мог не успеть, страница могла грузиться.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (!captureWindows(&fresh, &freshDesktops)) {
                return nullptr;
            }
            taken.assign(fresh.size(), 0);
            bool unknownBrowserUrl = false;
            for (size_t i = 0; i < fresh.size(); ++i) {
                taken[i] = isWindowTaken(fresh[i].hwnd) ? 1 : 0;
                if (taken[i] == 0 && !record.url.empty() && fresh[i].url.empty() &&
                    sm::IsBrowserExe(fresh[i].exeName)) {
                    unknownBrowserUrl = true;
                }
            }
            if (!unknownBrowserUrl) {
                break;
            }
            Sleep(500);
        }
        sm::WindowMatch found;
        if (!sm::MatchRecord(recordIndex, layout, fresh, taken, targets, freshDesktops, desktops,
                             &found)) {
            return nullptr;
        }
        const sm::WindowInfo& info = fresh[static_cast<size_t>(found.live)];
        sm::Trace(L"  перепроверка: «%s» уже открыто (%d б.: %s) — запускать не нужно",
                  info.title.c_str(), found.score, found.why.c_str());
        return info.hwnd;
    };

    // Фаза B (PLAN.md §8.3): поднять приложения, окон которых нет. Консоль с известной
    // командой (`cmd.exe /c "term.bat"`) перезапускается в своём окне — CONTEXT п. 70;
    // консоль, у которой в cmdline только имя хоста без аргументов, не перезапускается
    // (§9.4: неизвестно, что в ней было запущено) — такая запись уходит в admin.log.
    // Окна, существовавшие до запуска: если после старта появилось окно нужного
    // класса, которого в этом списке нет, — это окно запущенного приложения.
    // Explorer и UWP создают окно в уже работающем процессе и пишут в заголовок
    // своё (например, «img» вместо полного пути), поэтому PID и заголовок не годятся.
    std::vector<HWND> knownWindows;
    knownWindows.reserve(live.size());
    for (const sm::WindowInfo& info : live) {
        knownWindows.push_back(info.hwnd);
    }

    std::vector<char> matched(layout.windows.size(), 0);
    for (const sm::WindowMatch& match : matches) {
        if (match.saved >= 0 && match.saved < static_cast<int>(matched.size())) {
            matched[static_cast<size_t>(match.saved)] = 1;
        }
    }

    // Ожидание «успокоения» перед запуском приложений (PLAN.md §8.3 п.0, CONTEXT п. 69):
    // снимок фазы A сделан раньше, а Windows (окна папок, приложения из автозагрузки) и
    // браузер (сессия профиля) продолжают открывать окна. Пока состав окон меняется,
    // запускать приложения нельзя — получится дубль. Ждём тишины (2 с без изменений), но
    // не дольше settle-wait секунд: если система «сыплет» окнами бесконечно, работаем по
    // перепроверке перед каждым запуском. dry-run ничего не ждёт: он ничего не меняет.
    int settleWaitedMs = 0;
    {
        bool anyToLaunch = false;
        for (size_t i = 0; i < layout.windows.size(); ++i) {
            if (matched[i] != 0) {
                continue;
            }
            const sm::LaunchPlan plan = sm::PlanLaunch(layout.windows[i]);
            if (plan.kind != sm::LaunchKind::None && plan.skipReason.empty()) {
                anyToLaunch = true;
                break;
            }
        }
        const int settleLimitMs = cl.dryRun ? 0 : config.settleWaitSec * 1000;
        const int kSettleStepMs = 500;
        const int kSettleQuietMs = 2000;
        if (anyToLaunch && settleLimitMs > 0) {
            std::vector<HWND> previous;
            previous.reserve(live.size());
            for (const sm::WindowInfo& info : live) {
                previous.push_back(info.hwnd);
            }
            std::sort(previous.begin(), previous.end());
            int quietMs = 0;
            while (settleWaitedMs < settleLimitMs && quietMs < kSettleQuietMs) {
                Sleep(kSettleStepMs);
                settleWaitedMs += kSettleStepMs;
                std::vector<sm::WindowInfo> now;
                std::vector<GUID> nowDesktops;
                if (!captureWindows(&now, &nowDesktops)) {
                    break;
                }
                std::vector<HWND> current;
                current.reserve(now.size());
                for (const sm::WindowInfo& info : now) {
                    current.push_back(info.hwnd);
                }
                std::sort(current.begin(), current.end());
                if (current == previous) {
                    quietMs += kSettleStepMs;
                    continue;
                }
                sm::Trace(L"  ожидание: окон стало %d (было %d) — состав ещё меняется",
                          static_cast<int>(current.size()), static_cast<int>(previous.size()));
                previous.swap(current);
                quietMs = 0;
            }
            sm::Trace(L"  окна «успокоились»: ждали %.1f с",
                      static_cast<double>(settleWaitedMs) / 1000.0);
        }
    }

    // Ожидание окна запущенного приложения. Время не критично, качество важнее
    // (требование пользователя): приложения медлят с открытием окон, поэтому ждём
    // до 60 с, а не 20. Досрочно прекращаем только если процесс завершился —
    // тогда окна не будет никогда.
    const int kLaunchTimeoutMs = 60000;
    // Браузер показывает окно раньше, чем открывает страницу: перед переносом ждём
    // подтверждения адреса (PLAN.md §8.3 п.5), чтобы не тащить окно в момент навигации.
    const int kBrowserReadyTimeoutMs = 30000;
    // Поиск окна браузера по адресу страницы: окно появляется за пару секунд, но при
    // холодном старте Firefox сначала восстанавливает сессию, поэтому запас есть.
    // Не нашли — обычное ожидание по процессу и классу (kLaunchTimeoutMs).
    const int kBrowserWindowTimeoutMs = 20000;
    // Пауза перед переносом: приложения, которые выставляют окно сами уже после появления
    // (nw.js — AudioPL), иначе переигрывают наш перенос. Ждём, пока прямоугольники окон
    // перестанут меняться: 2 с тишины, но не дольше 12 с.
    const int kRectsQuietMs = 2000;
    const int kRectsMaxMs = 12000;
    int launched = 0;
    int launchFailed = 0;
    int launchSkipped = 0;
    int wouldLaunch = 0;
    // Окна, которые открылись сами (перепроверка перед запуском): запускать не понадобилось.
    int foundOpen = 0;

    // Права восстановления (PLAN.md §9.3). Сам restore может быть повышен — «Запуск от
    // имени администратора» на ярлыке, AutoRestore, любая повышенная консоль. Тогда обычные
    // приложения запускаются с токеном оболочки (обычные права), а записи с elevated=1 —
    // с нашим токеном. При обычном restore наоборот: elevated=1 поднять нельзя, их место
    // в admin.log (UAC на каждое приложение был бы хуже).
    const bool selfElevated = sm::IsElevated();
    int adminOnly = 0;

    // Фаза B идёт тремя шагами (CONTEXT п. 70):
    //   1) запустить всё, что нужно, — подряд, не дожидаясь окон;
    //   2) дождаться окна каждого запущенного приложения и подтвердить страницы браузеров;
    //   3) поставить окна на место — геометрия и десктоп.
    // Шаги разделены из-за приложений, которые выставляют окно сами уже после появления:
    // перенос в момент старта они переигрывали, и окно оставалось посреди экрана
    // (AudioPL с командной строкой nw.exe — CONTEXT п. 70).
    struct PendingLaunch {
        size_t record = 0;              // индекс записи в layout.windows
        sm::LaunchPlan plan;
        sm::LaunchRights rights = sm::LaunchRights::Default;
        sm::DesktopTarget target;
        RECT rect{};
        DWORD pid = 0;                  // PID запущенного процесса (0 — неизвестен)
        HWND hwnd = nullptr;            // окно, найденное на шаге ожидания
        bool exited = false;            // процесс завершился, не показав окна
        std::vector<HWND> knownBefore;  // окна до старта: для пустых окон браузера
    };
    std::vector<PendingLaunch> pending;


    for (size_t i = 0; i < layout.windows.size(); ++i) {
        if (matched[i] != 0) {
            continue;
        }
        const sm::LayoutWindow& saved = layout.windows[i];
        const sm::LaunchPlan plan = sm::PlanLaunch(saved);

        const int slot = sm::DesktopSlot(layout, saved.desktop);
        if (slot < 0 || slot >= static_cast<int>(targets.size())) {
            sm::Warn(L"«%s»: десктопа %d нет в файле", saved.title.c_str(), saved.desktop);
            ++failed;
            continue;
        }
        const sm::DesktopTarget& target = targets[static_cast<size_t>(slot)];

        RECT rect{};
        std::wstring note;
        if (!sm::TargetRect(saved, layout.monitors, liveMonitors, &rect, &note)) {
            sm::Warn(L"«%s»: координаты не посчитаны (мониторы не перечислены)",
                     saved.title.c_str());
            ++failed;
            continue;
        }

        if (!plan.skipReason.empty()) {
            sm::AdminReason reason = sm::AdminReason::NoCommand;
            const wchar_t* why = L"команду запуска собрать не удалось";
            if (plan.skipReason == L"console") {
                reason = sm::AdminReason::Console;
                why = L"консоль — неизвестно, что в ней запущено";
            } else if (plan.skipReason == L"browser-no-url") {
                // Адрес страницы не сохранился: запуск без него открыл бы пустое окно.
                reason = sm::AdminReason::BrowserNoUrl;
                why = L"браузер — адрес страницы неизвестен";
            }
            ++launchSkipped;
            if (!cl.dryRun) {
                sm::AddAdminEntry(reason, sm::AdminCommand(saved));
            }
            sm::Info(L"  не запускаем: %-40s %s (%s)", FitText(saved.title, 40).c_str(), why,
                     cl.dryRun ? L"в настоящем запуске — admin.log" : L"admin.log");
            continue;
        }

        // Приложение было открыто от администратора (PLAN.md §9.3): обычным процессом его
        // не поднять (манифест requireAdministrator → ERROR_ELEVATION_REQUIRED), а UAC на
        // каждое такое приложение — несколько запросов подряд. Запись уходит в admin.log
        // с подсказкой: запустите restore от имени администратора — тогда он поднимет и
        // эти окна, а обычные приложения всё равно оставит обычными.
        if (saved.elevated != 0 && !selfElevated) {
            ++launchSkipped;
            ++adminOnly;
            if (!cl.dryRun) {
                sm::AddAdminEntry(sm::AdminReason::LaunchElevated, sm::AdminCommand(saved));
            }
            sm::Info(L"  не запускаем: %-40s окно было открыто от администратора (%s)",
                     FitText(saved.title, 40).c_str(),
                     cl.dryRun ? L"в настоящем запуске — admin.log" : L"admin.log");
            continue;
        }
        // Обычное приложение при повышенном restore — с токеном оболочки, иначе оно станет
        // администраторским. Записи elevated=1 уже прошли проверку выше: если мы повышенные,
        // им достаётся наш токен.
        const sm::LaunchRights rights = (selfElevated && saved.elevated == 0)
                                            ? sm::LaunchRights::AsUser
                                            : sm::LaunchRights::Default;

        // Pinned-приложение могло остаться запущенным без окна (значок в трее):
        // второй экземпляр не нужен (PLAN.md §8.3 п.1).
        if (saved.pinned != 0 && sm::IsExeRunning(saved.exe)) {
            ++launchSkipped;
            sm::Info(L"  не запускаем: %-40s уже запущено (pinned)",
                     FitText(saved.title, 40).c_str());
            continue;
        }

        if (cl.dryRun) {
            ++wouldLaunch;
            if (saved.pinned == 0 && target.kind == sm::DesktopMatchKind::Fallback) {
                ++wouldFallback;
            }
            sm::Info(L"  запустил бы: %-10s %s   → стол [%d] %s  %s%s",
                     sm::LaunchKindText(plan.kind), FitText(plan.text, 84).c_str(),
                     target.currentIndex, DesktopNameOrStub(target.currentName).c_str(),
                     saved.show.c_str(),
                     rights == sm::LaunchRights::AsUser ? L"  (как пользователь)" : L"");
            // В отчёте строка усечена до ширины экрана — полный текст плана нужен,
            // когда проверяешь, какой именно адрес/команда возьмётся при запуске.
            sm::Trace(L"  план запуска: %s", plan.text.c_str());
            continue;
        }

        // Перепроверка перед запуском (PLAN.md §8.3 п.0, CONTEXT п. 69): пока шла работа,
        // окно могло открыться само — система возвращает окна папок и приложения из
        // автозагрузки, браузер восстанавливает сессию профиля. Второй экземпляр тогда не
        // запускаем: найденное окно ставится на место по тем же правилам, что в фазе A.
        const HWND alreadyOpen = recheckRecord(static_cast<int>(i));
        if (alreadyOpen != nullptr) {
            if (saved.pinned == 0 && target.kind == sm::DesktopMatchKind::Fallback) {
                rememberFallback(alreadyOpen, target);
            }
            bool moved = false;
            std::wstring applyProblem;
            if (!ApplyMatch(saved, alreadyOpen, rect, sm::ShowCommandFor(saved.show), target, vdm,
                            &moved, &applyProblem)) {
                ++failed;
                sm::Warn(L"«%s»: %s", saved.title.c_str(), applyProblem.c_str());
                sm::AddAdminEntry(AdminReasonForProblem(applyProblem), sm::AdminCommand(saved));
                takenWindows.push_back(alreadyOpen);
                knownWindows.push_back(alreadyOpen);
                continue;
            }
            ++foundOpen;
            takenWindows.push_back(alreadyOpen);
            knownWindows.push_back(alreadyOpen);
            zOrderWindows.push_back(alreadyOpen);
            zOrderZg.push_back(saved.zg);
            sm::Info(L"  уже открыто: %-40s → %s  стол [%d] %s%s", FitText(saved.title, 40).c_str(),
                     sm::RectToText(rect).c_str(), target.currentIndex,
                     DesktopNameOrStub(target.currentName).c_str(), moved ? L", перенесено" : L"");
            continue;
        }

        // Firefox (и другой браузер, который восстанавливает сессию профиля) при холодном
        // старте открывает адрес вкладкой в восстановленном окне и оставляет на текущем
        // десктопе пустое окно «Новая вкладка»: при каждом повторе восстановления копий
        // адреса становится больше (CONTEXT п. 63). Если браузер ещё не запущен, сначала
        // поднимаем его без адреса — тогда запуск ниже даёт ровно одно новое окно со
        // страницей. Уже запущенный браузер холодного старта не делает: ничего не меняем.
        // Список окон «до запуска» запоминаем отдельно: прогрев пополнит knownWindows
        // своими окнами, а закрывать надо именно то, чего не было (см. ниже).
        const std::vector<HWND> knownBeforeLaunch = knownWindows;
        if (plan.kind == sm::LaunchKind::Browser &&
            !sm::WarmUpBrowser(saved.exe, saved.cls, &knownWindows, kLaunchTimeoutMs, rights)) {
            sm::Trace(L"  «%s»: прогрев браузера не удался — запускаем как обычно",
                      saved.title.c_str());
        }

        DWORD pid = 0;
        DWORD launchError = 0;
        std::wstring problem;
        if (!sm::RunLaunch(plan, rights, &pid, &launchError, &problem)) {
            ++launchFailed;
            if (launchError == ERROR_ELEVATION_REQUIRED) {
                // В слепке права не отмечены (старый файл или процесс не открылся при save),
                // но приложение требует повышения — та же подсказка, что и для elevated=1.
                ++adminOnly;
            }
            sm::Warn(L"«%s»: %s", saved.title.c_str(), problem.c_str());
            // Команда для ручного запуска — независимо от причины: в журнале должен
            // быть полный список того, что не удалось поднять автоматически.
            sm::AddAdminEntry(launchError == ERROR_ELEVATION_REQUIRED
                                  ? sm::AdminReason::LaunchElevated
                                  : sm::AdminReason::LaunchFailed,
                              sm::AdminCommand(saved));
            continue;
        }

        // Шаг 1 — только запуск: окно не ждём, геометрию не ставим. Часть приложений
        // выставляет своё окно уже после появления, поэтому перенос отложен до шага 3
        // (CONTEXT п. 70).
        PendingLaunch item;
        item.record = i;
        item.plan = plan;
        item.rights = rights;
        item.target = target;
        item.rect = rect;
        item.pid = pid;
        item.knownBefore = knownBeforeLaunch;
        pending.push_back(item);
        sm::Info(L"  запускаю: %-40s %s%s", FitText(saved.title, 40).c_str(),
                 sm::LaunchKindText(plan.kind),
                 rights == sm::LaunchRights::AsUser ? L" (как пользователь)" : L"");
    }

    // Шаг 2: ожидание окон. Приложения (редакторы, IDE, тяжёлые Electron) открывают окно
    // заметно позже старта процесса, время есть — ждём до 60 с. Досрочно прекращаем, если
    // процесс завершился: тогда окна не будет никогда. Окна соседних запусков не забираем:
    // приложения запущены все сразу, и две записи одного приложения иначе могли бы
    // поменяться окнами (otherPids в WaitForWindow).
    for (PendingLaunch& item : pending) {
        const sm::LayoutWindow& saved = layout.windows[item.record];
        std::vector<DWORD> otherPids;
        for (const PendingLaunch& other : pending) {
            if (&other != &item && other.pid != 0) {
                otherPids.push_back(other.pid);
            }
        }
        sm::Info(L"  ждём окно: %-40s (до %d с)", FitText(saved.title, 40).c_str(),
                 kLaunchTimeoutMs / 1000);
        HWND hwnd = nullptr;
        // Браузер ищем по адресу страницы: у Firefox окно создаёт уже работающий процесс,
        // а после прогрева на столе есть восстановленное окно сессии того же класса —
        // «новое окно класса» могло бы оказаться им (CONTEXT п. 63). Адрес не подтвердился
        // — обычный путь по процессу и классу.
        if (item.plan.kind == sm::LaunchKind::Browser && !saved.url.empty()) {
            hwnd = sm::WaitForBrowserWindow(saved.cls, saved.url, knownWindows,
                                            kBrowserWindowTimeoutMs);
        }
        if (hwnd == nullptr) {
            hwnd = sm::WaitForWindow(item.pid, saved, knownWindows, kLaunchTimeoutMs, &item.exited,
                                     &otherPids);
        }
        if (hwnd == nullptr) {
            ++launchFailed;
            if (item.exited) {
                sm::Warn(L"«%s»: приложение завершилось, не открыв окна", saved.title.c_str());
                sm::AddAdminEntry(sm::AdminReason::LaunchFailed, sm::AdminCommand(saved));
            } else {
                sm::Warn(L"«%s»: окно не появилось за %d с (процесс запущен)",
                         saved.title.c_str(), kLaunchTimeoutMs / 1000);
                sm::AddAdminEntry(sm::AdminReason::LaunchTimeout, sm::AdminCommand(saved));
            }
            continue;
        }
        item.hwnd = hwnd;
        knownWindows.push_back(hwnd);  // окно занято: другое приложение его не возьмёт
        takenWindows.push_back(hwnd);
        // Окно появилось: с этого момента оно тоже кандидат «должно быть на десктопе 0».
        if (saved.pinned == 0 && item.target.kind == sm::DesktopMatchKind::Fallback) {
            rememberFallback(hwnd, item.target);
        }
        // Браузер: не тащим окно, пока страница не начала открываться. Ожидание не
        // фатально — если адрес так и не подтвердился, окно всё равно ставим на место
        // (иначе оно останется посреди экрана), но факт пишем в Trace.
        if (item.plan.kind == sm::LaunchKind::Browser &&
            !sm::WaitForBrowserPage(hwnd, saved.url, kBrowserReadyTimeoutMs)) {
            sm::Trace(L"  «%s»: страница не подтвердилась — переносим окно как есть",
                      saved.title.c_str());
        }
        // Пустое окно, которое браузер открыл сам при старте (его нет в слепке), иначе
        // остаётся висеть на текущем десктопе как «окно со вкладкой по умолчанию».
        // Закрываем только пустые окна, появившиеся после запуска, и только не наше
        // (CONTEXT п. 63).
        if (item.plan.kind == sm::LaunchKind::Browser) {
            const int closed = sm::CloseEmptyBrowserWindows(saved.cls, item.knownBefore, hwnd);
            if (closed > 0) {
                sm::Trace(L"  «%s»: закрыто пустых окон браузера: %d", saved.title.c_str(),
                          closed);
            }
        }
    }

    // Шаг 3: окна на место. Сначала ждём, пока запущенные приложения перестанут двигать
    // свои окна сами (AudioPL выставляет своё окно уже после появления и наш перенос
    // переигрывал — CONTEXT п. 70), и только потом ставим геометрию и десктоп.
    if (!cl.dryRun) {
        std::vector<HWND> launchedWindows;
        for (const PendingLaunch& item : pending) {
            if (item.hwnd != nullptr) {
                launchedWindows.push_back(item.hwnd);
            }
        }
        if (!launchedWindows.empty()) {
            sm::Trace(L"  ждём, пока окна запущенных приложений перестанут двигаться сами");
            WaitRectsStable(launchedWindows, kRectsQuietMs, kRectsMaxMs);
        }
        for (PendingLaunch& item : pending) {
            if (item.hwnd == nullptr) {
                continue;
            }
            const sm::LayoutWindow& saved = layout.windows[item.record];
            bool moved = false;
            std::wstring applyProblem;
            if (!ApplyMatch(saved, item.hwnd, item.rect, sm::ShowCommandFor(saved.show), item.target,
                            vdm, &moved, &applyProblem)) {
                ++launchFailed;
                sm::Warn(L"«%s»: %s", saved.title.c_str(), applyProblem.c_str());
                sm::AddAdminEntry(AdminReasonForProblem(applyProblem), sm::AdminCommand(saved));
                continue;
            }
            ++launched;
            zOrderWindows.push_back(item.hwnd);
            zOrderZg.push_back(saved.zg);
            sm::Info(L"  запущено: %-40s → %s  стол [%d] %s%s", FitText(saved.title, 40).c_str(),
                     sm::RectToText(item.rect).c_str(), item.target.currentIndex,
                     DesktopNameOrStub(item.target.currentName).c_str(),
                     moved ? L", перенесено" : L"");
        }
    }

    // Фаза C: zg=0 — самое верхнее окно, поэтому поднимаем окна в порядке убывания zg,
    // и верхним в итоге остаётся запись с zg=0. Фокус не отбираем (SWP_NOACTIVATE).
    if (!cl.dryRun && !zOrderWindows.empty()) {
        std::vector<size_t> order(zOrderWindows.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&zOrderZg](size_t a, size_t b) {
            return zOrderZg[a] > zOrderZg[b];
        });
        for (const size_t index : order) {
            SetWindowPos(zOrderWindows[index], HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    // Итог по окнам, которым положено быть на десктопе 0: проверяем их фактический
    // десктоп, а не только успех применения — при неудачном переносе счётчик
    // `fallback-desktop` врал бы (CONTEXT п. 52). Окна фазы B уже в списке; окно могло
    // закрыться — такие не считаем.
    if (!cl.dryRun) {
        for (HWND hwnd : fallbackCandidates) {
            if (IsWindow(hwnd) == FALSE) {
                continue;
            }
            GUID now{};
            if (vdm.ok() && vdm.GetWindowDesktopId(hwnd, &now, false)) {
                if (IsEqualGUID(now, fallbackGuid) != FALSE) {
                    ++fallbackWindows;
                } else {
                    ++fallbackNotMoved;
                    if (sm::IsVerbose()) {
                        sm::Trace(L"  %p: вместо десктопа 0 оказалось на %s",
                                  static_cast<void*>(hwnd), sm::GuidToString(now).c_str());
                    }
                }
            } else {
                ++fallbackUnknown;
            }
        }
    }

    const int unmatched =
        static_cast<int>(layout.windows.size()) - static_cast<int>(matches.size());
    sm::Info(L"");
    if (cl.dryRun) {
        sm::Info(L"--dry-run: план построен, ничего не изменено");
        sm::Info(L"  было бы применено: %d, из них с переносом на другой десктоп: %d", applied,
                 wouldMove);
        if (wouldLaunch > 0 || launchSkipped > 0) {
            sm::Info(L"  было бы запущено: %d, не запускается: %d "
                     L"(консоль, браузер без адреса или нет команды)",
                     wouldLaunch, launchSkipped);
        }
        if (wouldFallback > 0) {
            sm::Info(L"  уехало бы на десктоп 0 (целевой десктоп не найден): %d", wouldFallback);
        }
    } else {
        sm::Info(L"Применено: %d, перенесено на другой десктоп: %d, z-order выставлен: %d", applied,
                 movedToDesktop, static_cast<int>(zOrderWindows.size()));
        if (foundOpen > 0) {
            sm::Info(L"Уже было открыто (запускать не понадобилось): %d", foundOpen);
        }
        if (launched > 0 || launchFailed > 0 || launchSkipped > 0) {
            sm::Info(L"Запущено: %d, не удалось поднять: %d, не запускается: %d", launched,
                     launchFailed, launchSkipped);
        }
        if (fallbackWindows > 0) {
            sm::Info(L"Оказалось на десктопе 0 (целевой десктоп не найден): %d", fallbackWindows);
        }
        if (fallbackNotMoved > 0) {
            sm::Warn(L"  на десктоп 0 увезти не удалось: %d", fallbackNotMoved);
        }
        if (fallbackUnknown > 0) {
            sm::Info(L"  десктоп не проверен (окно недоступно): %d", fallbackUnknown);
        }
    }
    if (unmatched > 0) {
        sm::Trace(L"  записей без живого окна: %d (фаза B)", unmatched);
    }

    const int problems = failed + launchFailed;
    if (problems > 0) {
        sm::Warn(L"  не удалось восстановить: %d (подробности выше)", problems);
    }
    if (fallbackWindows + fallbackNotMoved > 0) {
        sm::Warn(L"  окон на десктопе 0: %d — сохранённых десктопов нет в системе; "
                 L"пересохраните раскладку после возврата десктопов",
                 fallbackWindows + fallbackNotMoved);
    }
    // Подсказка про права (PLAN.md §9.3): обычный restore не поднимает приложения, которые
    // были открыты от администратора, — они в admin.log. Повышенный restore поднимет и их,
    // а остальные приложения всё равно запустит обычными правами.
    if (adminOnly > 0 && !selfElevated) {
        sm::Info(L"  окон от администратора не запущено: %d — запустите restore от имени "
                 L"администратора: обычные приложения он поднимет с обычными правами, а эти — "
                 L"с повышенными",
                 adminOnly);
    }
    // PLAN.md §9.3: если что-то требует ручной работы — показываем файл сразу.
    // Журнал всегда переписывается заново: он описывает только этот запуск.
    if (!cl.dryRun) {
        std::wstring adminError;
        if (!sm::WriteAdminLog(&adminError)) {
            sm::Warn(L"admin.log: записать не удалось (%s)", adminError.c_str());
        }
        if (sm::AdminEntryCount() > 0) {
            sm::Info(L"  требуют вмешательства человека: %d — открываю %s",
                     sm::AdminEntryCount(),
                     sm::JoinPath(sm::ExeDir(), L"admin.log").c_str());
            sm::OpenAdminLogIfNeeded();
        }
    }
    // PLAN.md §9.1: если перенос окон невозможен, --dry-run показал план, но код
    // возврата должен сказать, что восстановить раскладку не получится.
    if (internalExit != 0) {
        return internalExit;
    }
    // PLAN.md §8.6: код 1 — «восстановлено не всё». Записи admin.log означают ровно
    // это: окно осталось за человеком (UIPI, консоль, браузер без адреса), даже если
    // счётчики ошибок пусты. В --dry-run журнал не пишется (AdminEntryCount() == 0),
    // поэтому там код по-прежнему описывает только ошибки применения/запуска.
    const bool adminPending = sm::AdminEntryCount() > 0;
    return (problems > 0 || fallbackWindows > 0 || fallbackNotMoved > 0 || adminPending)
               ? kExitError
               : kExitOk;
}

int RunList(const CommandLine& /*cl*/) {



    std::wstring error;

    // COM нужен для IVirtualDesktopManager: без него не определить текущий десктоп.
    if (!sm::InitCom(&error)) {
        sm::Error(L"%s", error.c_str());
        return kExitError;
    }

    sm::VirtualDesktopManager vdm;
    if (!vdm.Init(&error)) {
        sm::Warn(L"IVirtualDesktopManager недоступен: %s", error.c_str());
    }

    std::vector<sm::DesktopInfo> desktops;
    std::wstring source = L"реестр, VirtualDesktopIDs";
    if (!sm::ReadDesktops(&desktops, &error)) {
        sm::Warn(L"список десктопов из реестра не прочитан: %s", error.c_str());
        if (!sm::CollectDesktopsFallback(vdm, &desktops, &error)) {
            sm::Error(L"не удалось получить список виртуальных десктопов: %s", error.c_str());
            return kExitError;
        }
        source = L"резервный способ (по окнам)";
    }

    sm::CurrentDesktopInfo current;
    sm::DetectCurrentDesktop(vdm, &current, &desktops);
    const int currentIndex = current.known ? sm::FindDesktopIndex(desktops, current.guid) : -1;

    sm::Info(L"Виртуальные десктопы: %d (источник: %s)", static_cast<int>(desktops.size()),
             source.c_str());
    if (current.known && currentIndex >= 0) {
        sm::Info(L"текущий десктоп: [%d] %s (по: %s)", currentIndex,
                 DesktopLabel(desktops[static_cast<size_t>(currentIndex)]).c_str(),
                 current.source.c_str());
    } else if (current.known) {
        sm::Info(L"текущий десктоп: %s, в списке отсутствует (по: %s)",
                 sm::GuidToString(current.guid).c_str(), current.source.c_str());
    } else {
        sm::Info(L"текущий десктоп: не определён (%s)", current.source.c_str());
        sm::Info(L"  подсказка: см. README, раздел «Особенности определения текущего десктопа»");
    }

    for (const sm::DesktopInfo& d : desktops) {
        const bool isCurrent = current.known && IsEqualGUID(d.guid, current.guid) != FALSE;
        sm::Info(L"  [%d] %s  %s%s", d.index, sm::GuidToString(d.guid).c_str(),
                 DesktopLabel(d).c_str(), isCurrent ? L"   <= текущий" : L"");
    }

    sm::Info(L"");
    std::vector<sm::PinnedApp> pinned;
    if (!sm::ReadPinnedApps(&pinned, &error)) {
        sm::Warn(L"pinned-приложения не прочитаны: %s", error.c_str());
    } else {
        sm::Info(L"Pinned-приложения (показывать на всех рабочих столах): %d",
                 static_cast<int>(pinned.size()));
        for (const sm::PinnedApp& p : pinned) {
            const std::wstring shown =
                (p.kind == L"known" && !p.resolved.empty()) ? p.resolved : p.raw;
            sm::Info(L"  %s  %s", PinnedKindTag(p.kind), shown.c_str());
        }
        if (pinned.empty()) {
            sm::Info(L"  (нет)");
        }
    }

    sm::Info(L"");
    PrintWindows(vdm, desktops, currentIndex);

    // Есть слепок — печатаем карту «десктоп из файла → десктоп сейчас» (PLAN.md §10):
    // это главный способ проверить §8.0, ничего не перемещая.
    sm::Info(L"");
    sm::Layout layout;
    std::wstring layoutPath;
    std::wstring layoutError;
    if (sm::ReadLayoutFile(sm::ExeDir(), &layout, &layoutPath, &layoutError)) {
        std::vector<sm::DesktopTarget> targets;
        if (sm::MapDesktops(layout, desktops, &targets, &layoutError)) {
            sm::Info(L"Карта десктопов из layout.txt (сохранён %s):", layout.saved.c_str());
            PrintDesktopMap(layout, targets);
        } else {
            sm::Warn(L"карта десктопов не построена: %s", layoutError.c_str());
        }
    } else {
        sm::Info(L"Файла раскладки нет: %s", layoutError.c_str());
    }

    return kExitOk;
}

// --- Автозапуск (PLAN.md §11) ---------------------------------------------------

// Настройки для AutoSave/AutoRestore: файла может не быть — тогда значения по умолчанию.
void LoadConfigForAutostart(sm::Config* config) {
    std::wstring error;
    if (!sm::LoadConfig(config, &error)) {
        sm::Warn(L"Config.ini: %s — беру значения автозапуска по умолчанию", error.c_str());
        *config = sm::Config();
        return;
    }
    if (!config->exists) {
        sm::Trace(L"Config.ini: файла %s нет — значения автозапуска по умолчанию",
                  config->path.c_str());
    }
    for (const std::wstring& key : config->unknownKeys) {
        sm::Trace(L"Config.ini: ключ «%s» не знаком этой версии — пропущен", key.c_str());
    }
}

// --- Права процесса и UAC (PLAN.md §9.3) ---------------------------------------
// Обычный запуск ScreenMemory повышенных прав не требует: с админским токеном
// CreateProcess передаёт его всем восстановленным приложениям, а окна приложений,
// запущенных от администратора, из нашего процесса не двигаются (UIPI). Флаг
// совместимости «Запуск от имени администратора» в манифесте не виден (там asInvoker),
// зато даёт запрос UAC на КАЖДЫЙ запуск — поэтому о нём говорим вслух, иначе
// пользователь видит только «разрешить этому приложению?» и не понимает причины.
bool RunAsAdminFlagSet() {
    HKEY key = nullptr;
    const LSTATUS open = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", 0,
        KEY_QUERY_VALUE, &key);
    if (open != ERROR_SUCCESS) {
        return false;
    }
    // Значение называется полным путём exe, а хранит набор флагов: «~ RUNASADMIN».
    wchar_t value[64] = L"";
    DWORD type = 0;
    DWORD size = sizeof(value) - sizeof(wchar_t);
    const LSTATUS read = RegQueryValueExW(key, sm::ExePath().c_str(), nullptr, &type,
                                          reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    // ERROR_MORE_DATA — флагов больше, чем влезло в буфер: RUNASADMIN стоит первым,
    // поэтому начала строки достаточно; REG_EXPAND_SZ встречается на старых системах.
    if ((read != ERROR_SUCCESS && read != ERROR_MORE_DATA) ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    value[_countof(value) - 1] = L'\0';
    return std::wstring(value).find(L"RUNASADMIN") != std::wstring::npos;
}

void WarnAboutElevation() {
    if (sm::IsElevated()) {
        // Не предупреждение, а пояснение (PLAN.md §9.3): раздача прав теперь раздельная —
        // обычные приложения получают токен оболочки, повышенными остаются только те окна,
        // которые и были открыты от администратора.
        sm::Info(L"процесс запущен с правами администратора: обычные приложения будут "
                 L"запущены с правами пользователя, а окна, открытые от администратора, — "
                 L"с повышенными");
    }
    if (RunAsAdminFlagSet()) {
        sm::Warn(L"на exe стоит флаг совместимости «Запуск от имени администратора»: "
                 L"Windows будет спрашивать UAC при каждом запуске");
        sm::Info(L"  разделение прав restore делает и без флага; флаг нужен только чтобы "
                 L"поднимать окна, открытые от администратора, — и тогда UAC спрашивают один "
                 L"раз за запуск, а не по разу на приложение");
        sm::Info(L"  снять: Свойства exe → «Совместимость» → снять галочку «Запускать эту "
                 L"программу от имени администратора»");
        sm::Info(L"  либо: reg delete \"HKCU\\Software\\Microsoft\\Windows NT\\CurrentVersion"
                 L"\\AppCompatFlags\\Layers\" /v \"%s\" /f", sm::ExePath().c_str());
    }
}

// Задачи Планировщика создаются для пользователя и требуют повышенных прав. Молча
// подменять их автозагрузкой в реестре нельзя: та работает иначе (без расписания и
// задержки), а «незаметно другое» поведение хуже понятной ошибки.
bool RequireElevation(const wchar_t* switchName) {
    if (sm::IsElevated()) {
        return true;
    }
    sm::Error(L"%s: нужны права администратора", switchName);
    sm::Info(L"");
    sm::Info(L"Задачи Планировщика заданий (и их удаление) Windows разрешает менять только");
    sm::Info(L"с повышенными правами: без них — «Отказано в доступе».");
    sm::Info(L"Закройте это окно и запустите ScreenMemory.exe от имени администратора:");
    sm::Info(L"правый щелчок по exe (или по ярлыку) → «Запуск от имени администратора».");
    sm::Info(L"Обычные save и restore повышенных прав не требуют.");
    return false;
}

void PrintTaskDryRun(const wchar_t* taskName, const sm::TaskInstallResult& result) {
    sm::Info(L"Проверка (%s): ничего не регистрирую", taskName);
    sm::Info(L"  задача: «%s» — %s", taskName, result.schedule.c_str());
    sm::Info(L"  XML задачи: %s", result.xmlPath.c_str());
    sm::Info(L"  зарегистрировать вручную: schtasks /Create /TN \"%s\" /XML \"%s\" /F",
             taskName, result.xmlPath.c_str());
}

int RunAutoSave(const CommandLine& cl) {
    if (!cl.dryRun && !RequireElevation(L"AutoSave")) {
        return kExitNoRights;
    }
    sm::Config config;
    LoadConfigForAutostart(&config);
    sm::TaskOptions options;
    options.dryRun = cl.dryRun;
    sm::TaskInstallResult result;
    std::wstring error;
    if (!sm::InstallAutoSaveTask(config.autoSaveMinutes, options, &result, &error)) {
        sm::Error(L"AutoSave: %s", error.c_str());
        return kExitError;
    }
    if (cl.dryRun) {
        PrintTaskDryRun(sm::kAutoSaveTaskName, result);
        return kExitOk;
    }
    sm::Info(L"Задача «%s» %s: %s", sm::kAutoSaveTaskName,
             result.alreadyExisted ? L"обновлена (была установлена)" : L"создана",
             result.schedule.c_str());
    sm::Info(L"  команда: \"%s\" save --hidden", sm::ExePath().c_str());
    if (config.autoSaveSet) {
        sm::Info(L"  интервал из Config.ini: AutoSave=%d", config.autoSaveMinutes);
    } else {
        sm::Info(L"  интервала в Config.ini нет — взято значение по умолчанию 10 мин");
    }
    sm::Info(L"  снять автозапуск: ScreenMemory.exe Uninstall");
    return kExitOk;
}

int RunAutoRestore(const CommandLine& cl) {
    if (!cl.dryRun && !RequireElevation(L"AutoRestore")) {
        return kExitNoRights;
    }
    sm::Config config;
    LoadConfigForAutostart(&config);
    sm::TaskOptions options;
    options.dryRun = cl.dryRun;
    sm::TaskInstallResult result;
    std::wstring error;
    if (!sm::InstallAutoRestoreTask(config.autoRestoreDelaySec, options, &result, &error)) {
        sm::Error(L"AutoRestore: %s", error.c_str());
        return kExitError;
    }
    if (cl.dryRun) {
        PrintTaskDryRun(sm::kAutoRestoreTaskName, result);
        return kExitOk;
    }
    sm::Info(L"Задача «%s» %s: %s", sm::kAutoRestoreTaskName,
             result.alreadyExisted ? L"обновлена (была установлена)" : L"создана",
             result.schedule.c_str());
    sm::Info(L"  команда: \"%s\" restore --hidden", sm::ExePath().c_str());
    if (config.autoRestoreDelaySet) {
        sm::Info(L"  задержка из Config.ini: AutoRestoreDelay=%d с", config.autoRestoreDelaySec);
    } else {
        sm::Info(L"  задержки в Config.ini нет — взято значение по умолчанию 30 с");
    }
    sm::Info(L"  задержка нужна, чтобы автозагрузка успела открыть свои приложения:");
    sm::Info(L"  restore только расставит окна уже запущенных программ.");
    sm::Info(L"  снять автозапуск: ScreenMemory.exe Uninstall");
    return kExitOk;
}

int RunUninstall(const CommandLine& cl) {
    if (!cl.dryRun && !RequireElevation(L"Uninstall")) {
        return kExitNoRights;
    }
    sm::TaskOptions options;
    options.dryRun = cl.dryRun;

    bool failed = false;
    int foundCount = 0;
    const wchar_t* const tasks[] = {sm::kAutoSaveTaskName, sm::kAutoRestoreTaskName};
    for (const wchar_t* name : tasks) {
        bool found = false;
        std::wstring error;
        if (!sm::DeleteTask(name, options, &found, &error)) {
            sm::Error(L"задача «%s»: %s", name, error.c_str());
            failed = true;
            continue;
        }
        if (!found) {
            sm::Trace(L"задача «%s» — не было", name);
            continue;
        }
        ++foundCount;
        sm::Info(L"  задача «%s» — %s", name, cl.dryRun ? L"есть" : L"удалена");
    }

    bool runFound = false;
    std::wstring runError;
    if (!sm::DeleteRunValue(options, &runFound, &runError)) {
        sm::Error(L"автозагрузка (реестр): %s", runError.c_str());
        failed = true;
    } else if (runFound) {
        ++foundCount;
        sm::Info(L"  автозагрузка (HKCU\\...\\Run, значение «%s») — %s", sm::kRunValueName,
                 cl.dryRun ? L"есть" : L"удалена");
    } else {
        sm::Trace(L"автозагрузка (реестр) — значения «%s» нет", sm::kRunValueName);
    }

    if (failed) {
        sm::Error(L"Uninstall: снять удалось не всё (см. сообщения выше)");
        return kExitError;
    }
    if (foundCount == 0) {
        sm::Info(cl.dryRun ? L"Проверка (Uninstall): автозапуск не установлен — снимать нечего"
                           : L"Автозапуск не был установлен — снимать нечего.");
    } else if (cl.dryRun) {
        sm::Info(L"Проверка (Uninstall): ничего не меняю");
    } else {
        sm::Info(L"Автозапуск снят.");
    }
    return kExitOk;
}

bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

// Команды программы. Сочетать их нельзя: за один запуск — ровно одно действие.
bool IsKnownCommand(const std::wstring& arg) {
    return EqualsIgnoreCase(arg, L"save") || EqualsIgnoreCase(arg, L"restore") ||
           EqualsIgnoreCase(arg, L"autosave") || EqualsIgnoreCase(arg, L"autorestore") ||
           EqualsIgnoreCase(arg, L"uninstall");
}

// Возвращает true, если разбор успешен. При ошибке печатает причину.
bool ParseCommandLine(int argc, wchar_t** argv, CommandLine* cl, std::wstring* error) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];

        if (EqualsIgnoreCase(arg, L"--help") || EqualsIgnoreCase(arg, L"-h") ||
            EqualsIgnoreCase(arg, L"/?") || EqualsIgnoreCase(arg, L"-?")) {
            cl->help = true;
        } else if (EqualsIgnoreCase(arg, L"--version")) {
            cl->version = true;
        } else if (EqualsIgnoreCase(arg, L"--verbose")) {
            cl->verbose = true;
        } else if (EqualsIgnoreCase(arg, L"--list")) {
            cl->list = true;
        } else if (EqualsIgnoreCase(arg, L"--dry-run")) {
            cl->dryRun = true;
        } else if (EqualsIgnoreCase(arg, L"--hidden")) {
            cl->hidden = true;
        } else if (EqualsIgnoreCase(arg, L"--from") ||
                   (arg.size() > 7 && _wcsnicmp(arg.c_str(), L"--from=", 7) == 0)) {
            // Файл для restore: --from <файл> или --from=<файл>. Путь с пробелами cmd.exe
            // отдаёт одним argv, кавычки уже сняты — дополнительно ничего не разбираем.
            std::wstring value;
            if (arg.size() > 7) {
                value = arg.substr(7);
            } else if (i + 1 < argc) {
                value = argv[++i];
            }
            if (value.empty() || value[0] == L'-') {
                *error = L"ключ --from требует имени файла, например "
                         L"--from layout-вчерашний-день.txt";
                return false;
            }
            cl->from = value;
        } else if (!arg.empty() && arg[0] == L'-') {
            *error = L"неизвестный ключ: " + arg;
            return false;
        } else if (cl->command.empty()) {
            cl->command = arg;
        } else if (IsKnownCommand(arg)) {
            // «save restore» — почти наверняка ошибка в скрипте, и лучше сказать об этом прямо.
            *error = L"команды взаимоисключающие: «" + cl->command + L"» и «" + arg +
                     L"» нельзя указывать вместе";
            return false;
        } else {
            *error = L"лишний параметр: " + arg;
            return false;
        }
    }

    if (!cl->command.empty() && !IsKnownCommand(cl->command)) {
        *error = L"неизвестная команда: " + cl->command;
        return false;
    }
    if (!cl->command.empty() && (cl->list || cl->help || cl->version)) {
        *error = L"команду «" + cl->command +
                 L"» нельзя сочетать с --list, --help или --version: за запуск — одно действие";
        return false;
    }
    if (cl->list && (cl->help || cl->version)) {
        *error = L"ключи --list, --help и --version взаимоисключающие";
        return false;
    }
    if (cl->dryRun && cl->command.empty()) {
        *error = L"ключ --dry-run применяется только с командой: restore, AutoSave, "
                 L"AutoRestore или Uninstall";
        return false;
    }
    if (cl->hidden && cl->command.empty()) {
        *error = L"ключ --hidden применяется только с командой";
        return false;
    }
    if (!cl->from.empty() && !EqualsIgnoreCase(cl->command, L"restore")) {
        *error = L"ключ --from применяется только с командой restore";
        return false;
    }
    return true;
}

}  // namespace

namespace {

// Падение не должно выглядеть как «программа молча исчезла»: без своего фильтра
// Windows 10 не всегда пишет событие в журнал, а вывод обрывается на полуслове.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    DWORD code = 0;
    void* address = nullptr;
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        code = info->ExceptionRecord->ExceptionCode;
        address = info->ExceptionRecord->ExceptionAddress;
    }
    sm::Error(L"ВНУТРЕННЯЯ ОШИБКА: исключение 0x%08lX по адресу %p — работа прервана",
              static_cast<unsigned long>(code), address);
    sm::ShutdownLog();
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashFilter);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    InstallCrashHandler();

    CommandLine cl;
    std::wstring error;
    bool parsed = ParseCommandLine(argc, argv, &cl, &error);

    // Задачу Планировщика консольное окно только мешает: --hidden прячет его сразу,
    // до первого сообщения (иначе окно успело бы мигнуть на экране).
    if (parsed && cl.hidden) {
        const HWND console = GetConsoleWindow();
        if (console != nullptr) {
            ShowWindow(console, SW_HIDE);
        }
    }

    // Лог-файл открываем после разбора: до этого неизвестно, включён ли --verbose.
    // В --hidden лог ведём всегда: окна не видно, и иначе ошибка задачи пропала бы молча.
    sm::InitLog(parsed && (cl.verbose || cl.hidden));

    int code = kExitOk;
    if (!parsed) {
        PrintBanner();
        sm::Error(L"%s", error.c_str());
        sm::Info(L"");
        PrintUsage();
        code = kExitUsage;
    } else if (cl.help) {
        PrintBanner();
        PrintUsage();
    } else if (cl.version) {
        sm::Info(L"ScreenMemory %s (x64)", SM_VERSION_WSTRING);
    } else if (cl.list) {
        PrintBanner();
        // Осмотр системы ничего не меняет — внешнюю программу мониторов для него не запускаем.
        sm::Trace(L"--list: предварительный шаг (prelaunch) не выполняется");
        code = RunList(cl);
    } else if (EqualsIgnoreCase(cl.command, L"save")) {
        PrintBanner();
        WarnAboutElevation();
        // save только записывает то, что видно сейчас: программа мониторов нужна перед
        // восстановлением, когда рабочий стол второго монитора ещё не поднялся.
        sm::Trace(L"save: предварительный шаг (prelaunch) не выполняется");
        code = RunSave(cl);
    } else if (EqualsIgnoreCase(cl.command, L"restore")) {
        PrintBanner();
        WarnAboutElevation();
        if (cl.dryRun) {
            // --dry-run ничего не меняет, поэтому и предварительный шаг не нужен.
            sm::Trace(L"--dry-run: предварительный шаг (prelaunch) не выполняется");
        } else {
            RunPreLaunchStep();
        }
        code = RunRestore(cl);
    } else if (EqualsIgnoreCase(cl.command, L"autosave")) {
        PrintBanner();
        code = RunAutoSave(cl);
    } else if (EqualsIgnoreCase(cl.command, L"autorestore")) {
        PrintBanner();
        code = RunAutoRestore(cl);
    } else if (EqualsIgnoreCase(cl.command, L"uninstall")) {
        PrintBanner();
        code = RunUninstall(cl);
    } else {
        // Без аргументов — короткая справка, код 0 (двойной щелчок по exe не должен «ругаться»).
        PrintBanner();
        PrintUsage();
    }

    if (!cl.hidden && (code != kExitOk || !cl.version)) {
        WaitForKeyIfConsoleIsOurs();
    }

    // COM закрываем последним: локальные объекты команд (IVirtualDesktopManager) уже разрушены.
    sm::UninitCom();
    sm::ShutdownLog();
    return code;
}
