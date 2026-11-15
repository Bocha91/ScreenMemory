#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "window_info.h"

// Перечисление окон-кандидатов в слепок (PLAN.md §7 п.3–4).
//
// Главное: EnumWindows отдаёт окна ВСЕХ виртуальных десктопов. Окна неактивных
// десктопов «накрыты» оболочкой (DWM_CLOAKED_SHELL), но живые и подлежат сохранению -
// ради них программа и написана. Отбрасываем только унаследованно-cloaked (0x4).
namespace sm {

struct WindowEnumStats {
    int total = 0;             // всего верхнеуровневых окон (то, что отдал EnumWindows)
    int candidates = 0;        // сколько попало в список
    int cloakedShell = 0;      // из них окна других виртуальных десктопов
    int cloakedApp = 0;        // из них «спрятаны приложением» (UWP suspended)
    int skippedInvisible = 0;  // IsWindowVisible == FALSE
    int skippedProcess = 0;    // свой процесс или окно без процесса (PID 0)
    int skippedTool = 0;       // WS_EX_TOOLWINDOW
    int skippedOwned = 0;      // owned-окно (кроме диалогов #32770 с заголовком)
    int skippedBlacklist = 0;  // служебные классы оболочки
    int skippedNoTitle = 0;    // пустой заголовок и класс не из whitelist
    int skippedEmptyUwp = 0;   // пустая UWP-рамка: ApplicationFrameWindow без CoreWindow
    int skippedInherited = 0;  // DWM_CLOAKED_INHERITED — служебное/owned-окно накрытого
    int exeKnown = 0;          // у скольких кандидатов прочитан путь exe
    int aumidKnown = 0;        // у скольких кандидатов есть AppUserModelID
    int folderKnown = 0;       // у скольких окон Проводника прочитана папка
};

// Список кандидатов в порядке z-order: первый элемент — самое верхнее окно экрана.
bool CollectWindows(std::vector<WindowInfo>* out, WindowEnumStats* stats);

// Проверки классов — нужны и здесь, и на этапах save/restore.
bool IsBlacklistedClass(const std::wstring& cls);
bool IsWhitelistedClass(const std::wstring& cls);

}  // namespace sm
