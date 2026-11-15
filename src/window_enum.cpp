#include "window_enum.h"

#include "log.h"

namespace {

// Служебные окна оболочки, панелей задач и «хостов» UWP (PLAN.md §7 п.3).
const wchar_t* const kBlacklist[] = {
    L"Progman",
    L"WorkerW",
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"Windows.UI.Core.CoreWindow",
    L"ApplicationManager_DesktopShellWindow",
    L"MultitaskingViewFrame",
    L"ForegroundStaging",
    L"XamlExplorerHostIslandWindow",
    L"TaskListThumbnailWnd",
    L"Windows.Internal.Shell.TabProxyWindow",
    L"DV2ControlHost",
    L"MsgrIMEWindowClass",
};

// Классы, у которых заголовок может быть пустым, а окно нам всё равно нужно:
// Проводник (заголовок = имя папки), UWP-рамка, диалог.
const wchar_t* const kWhitelist[] = {
    L"CabinetWClass",
    L"ExploreWClass",
    L"ApplicationFrameWindow",
    L"#32770",
};

template <size_t N>
bool InList(const wchar_t* const (&list)[N], const std::wstring& cls) {
    for (size_t i = 0; i < N; ++i) {
        if (_wcsicmp(list[i], cls.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// Есть ли среди потомков окна окно указанного класса (на глубину до depth уровней).
bool HasDescendantClass(HWND parent, const wchar_t* cls, int depth) {
    for (HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr); child != nullptr;
         child = FindWindowExW(parent, child, nullptr, nullptr)) {
        wchar_t buf[256] = L"";
        GetClassNameW(child, buf, _countof(buf));
        if (_wcsicmp(buf, cls) == 0) {
            return true;
        }
        if (depth > 1 && HasDescendantClass(child, cls, depth - 1)) {
            return true;
        }
    }
    return false;
}

// UWP-окно живёт внутри рамки ApplicationFrameWindow, а содержимое — дочернее
// Windows.UI.Core.CoreWindow. Рамка без CoreWindow и без заголовка — пустышка
// (Explorer держит такие по одной на монитор; в слепок они не нужны).
bool IsEmptyUwpFrame(const sm::WindowInfo& info) {
    if (info.cls != L"ApplicationFrameWindow" || !info.title.empty()) {
        return false;
    }
    return !HasDescendantClass(info.hwnd, L"Windows.UI.Core.CoreWindow", 3);
}

struct EnumContext {
    std::vector<sm::WindowInfo>* out = nullptr;
    sm::WindowEnumStats* stats = nullptr;
    DWORD ownPid = 0;
    int z = 0;  // порядковый номер окна в z-order (0 — самое верхнее)
};

// EnumWindows обходит окна сверху вниз по z-order — именно этот порядок и нужен
// для поля zg в слепке, поэтому z берём из счётчика вызовов, а не сортируем потом.
BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    sm::WindowEnumStats& stats = *ctx->stats;
    const int z = ctx->z++;
    ++stats.total;

    sm::WindowInfo info;
    if (!sm::GetWindowInfo(hwnd, &info)) {
        return TRUE;  // окно закрылось между вызовами
    }
    info.z = z;

    if (!info.visible) {
        ++stats.skippedInvisible;
        return TRUE;
    }
    if (info.pid == 0 || info.pid == ctx->ownPid) {
        ++stats.skippedProcess;
        return TRUE;
    }
    if ((info.exStyle & WS_EX_TOOLWINDOW) != 0) {
        ++stats.skippedTool;
        return TRUE;
    }
    // Owned-окна — служебные (палитры, всплывающие подсказки). Исключение: диалог
    // #32770 с заголовком — это как раз то, что пользователь считает окном.
    if (info.owned && !(info.cls == L"#32770" && !info.title.empty())) {
        ++stats.skippedOwned;
        return TRUE;
    }
    if (sm::IsBlacklistedClass(info.cls)) {
        ++stats.skippedBlacklist;
        return TRUE;
    }
    if (info.title.empty() && !sm::IsWhitelistedClass(info.cls)) {
        ++stats.skippedNoTitle;
        return TRUE;
    }
    if (IsEmptyUwpFrame(info)) {
        ++stats.skippedEmptyUwp;
        return TRUE;
    }
    // 0x4 — owned/дочерние окна накрытого окна: в слепок не пишем (PLAN.md §9.0 п.4).
    if ((info.cloaked & sm::CloakedInherited) != 0) {
        ++stats.skippedInherited;
        return TRUE;
    }

    if ((info.cloaked & sm::CloakedShell) != 0) {
        ++stats.cloakedShell;
    }
    if ((info.cloaked & sm::CloakedApp) != 0) {
        ++stats.cloakedApp;
    }

    // Детали (exe, cmdline, AUMID, папка Проводника) собираем только для окон,
    // прошедших фильтр: на каждое окно это OpenProcess и запросы к COM.
    sm::GetWindowDetails(&info);
    if (!info.exePath.empty()) {
        ++stats.exeKnown;
    }
    if (!info.aumid.empty()) {
        ++stats.aumidKnown;
    }
    if (!info.folder.empty()) {
        ++stats.folderKnown;
    }

    ctx->out->push_back(info);
    return TRUE;
}

}  // namespace

namespace sm {

bool CollectWindows(std::vector<WindowInfo>* out, WindowEnumStats* stats) {
    if (out == nullptr || stats == nullptr) {
        return false;
    }
    out->clear();
    *stats = WindowEnumStats{};

    EnumContext ctx;
    ctx.out = out;
    ctx.stats = stats;
    ctx.ownPid = GetCurrentProcessId();

    // Наш callback всегда возвращает TRUE, поэтому FALSE от EnumWindows — это ошибка
    // перечисления (например, окон стало слишком много). Уже собранное не выбрасываем.
    SetLastError(ERROR_SUCCESS);
    if (EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx)) == FALSE) {
        const DWORD err = GetLastError();
        if (err != ERROR_SUCCESS) {
            Trace(L"EnumWindows: ошибка %lu — список окон может быть неполным",
                  static_cast<unsigned long>(err));
        }
    }

    stats->candidates = static_cast<int>(out->size());
    if (IsVerbose()) {
        Trace(L"окна: всего %d, кандидатов %d (cloaked shell %d, app %d); пропущено: "
              L"невидимых %d, чужих процессов %d, tool-window %d, owned %d, служебных "
              L"классов %d, без заголовка %d, пустых UWP-рамок %d, унаследованно-cloaked %d",
              stats->total, stats->candidates, stats->cloakedShell, stats->cloakedApp,
              stats->skippedInvisible, stats->skippedProcess, stats->skippedTool,
              stats->skippedOwned, stats->skippedBlacklist, stats->skippedNoTitle,
              stats->skippedEmptyUwp, stats->skippedInherited);
        Trace(L"окна: exe прочитан у %d из %d, AUMID у %d, папка Проводника у %d",
              stats->exeKnown, stats->candidates, stats->aumidKnown, stats->folderKnown);
    }
    return true;
}

bool IsBlacklistedClass(const std::wstring& cls) {
    return InList(kBlacklist, cls);
}

bool IsWhitelistedClass(const std::wstring& cls) {
    return InList(kWhitelist, cls);
}

}  // namespace sm
