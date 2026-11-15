#pragma once

#include <windows.h>

#include <string>

// Сведения об одном окне (PLAN.md §7 п.5): класс, заголовок, стили, геометрия,
// состояние, cloaked-флаг. Сведения о процессе (exe, cmdline), AUMID и папка
// Проводника добавляются отдельным вызовом GetWindowDetails (этап 4) — чтобы не
// делать десятки системных вызовов для окон, которые всё равно отфильтруются.
namespace sm {

// Флаги DWMWA_CLOAKED (PLAN.md §9.0): «накрытое» окно продолжает жить, у него есть
// HWND и IsWindowVisible() == TRUE, но DWM его не рисует.
enum CloakedFlag : DWORD {
    CloakedNone = 0x0,
    CloakedApp = 0x1,        // DWM_CLOAKED_APP — спрятало само приложение (UWP suspended)
    CloakedShell = 0x2,      // DWM_CLOAKED_SHELL — окно на другом виртуальном десктопе
    CloakedInherited = 0x4,  // DWM_CLOAKED_INHERITED — owned-окно накрытого окна
};

struct WindowInfo {
    HWND hwnd = nullptr;
    int z = 0;                // позиция в z-order: 0 — самое верхнее окно экрана
    std::wstring cls;         // GetClassNameW
    std::wstring title;       // заголовок (WM_GETTEXT с тайм-аутом: чужие окна могут висеть)
    DWORD pid = 0;            // процесс-владелец окна
    DWORD style = 0;          // GWL_STYLE
    DWORD exStyle = 0;        // GWL_EXSTYLE
    DWORD cloaked = 0;        // набор флагов CloakedFlag
    bool visible = false;     // IsWindowVisible
    bool owned = false;       // есть владелец (GetWindow GW_OWNER)
    WINDOWPLACEMENT placement{};  // showCmd + rcNormalPosition («нормальная» позиция)
    bool placementOk = false;
    RECT rect{};              // GetWindowRect — текущие границы в координатах экрана
    bool rectOk = false;
    RECT frame{};             // DWMWA_EXTENDED_FRAME_BOUNDS — видимые границы без тени
    bool frameOk = false;

    // Заполняются GetWindowDetails: exe и командная строка процесса, AppUserModelID
    // окна, папка для окна Проводника. Пустая строка = данных нет / не применимо.
    std::wstring exePath;
    std::wstring exeName;
    std::wstring cmdline;
    std::wstring aumid;
    std::wstring folder;
    DWORD exeError = 0;       // почему exePath пуст (OpenProcess/QueryFullProcessImageName)

    // Адрес активной вкладки — только у окон браузеров. Заполняется отдельным
    // вызовом, а не в GetWindowDetails: UIA — это межпроцессные вызовы, на
    // неотвечающем окне каждый из них ждёт до таймаута (см. browser_url.cpp), и
    // делать их для окон, которые в слепке не встречаются, бессмысленно. Когда адрес
    // нужен (save — для слепка, restore — для сопоставления), он читается всегда:
    // время не критично, важно не перепутать окна (PLAN.md §8.1, §8.3 п.2).
    std::wstring url;
};

// Заполняет out. false — окна уже нет (успело закрыться между EnumWindows и вызовом).
bool GetWindowInfo(HWND hwnd, WindowInfo* out);

// Дополняет info сведениями о процессе окна (exe, cmdline), AppUserModelID и
// папкой Проводника (PLAN.md §7 п.5, §9.2). Вызывать только для окон-кандидатов:
// на каждое окно это OpenProcess и запросы к COM, для сотен отфильтрованных окон
// работа была бы лишней.
bool GetWindowDetails(WindowInfo* info);

// Строки для отчёта: cloaked-флаг («—», «shell», «app», «inh», их сочетания),
// состояние окна («normal»/«max»/«min»), координаты «x,y WxH».
const wchar_t* CloakedText(DWORD cloaked);
const wchar_t* ShowStateText(int showCmd);
const wchar_t* ShowStateText(const WindowInfo& info);
std::wstring RectText(const RECT& rect);
std::wstring RectText(const WindowInfo& info);  // frame, если DWM ответил, иначе rect

// «Нормальная» позиция окна (rcNormalPosition из WINDOWPLACEMENT) — именно она пойдёт
// в слепок как rect (PLAN.md §9.0 п.5, §8.4 п.6). Текущие границы для этого не годятся:
// у развёрнутого окна это весь монитор, у свёрнутого — служебные -32000. Пустая
// строка — окно не ответило на GetWindowPlacement.
std::wstring NormalRectText(const WindowInfo& info);

}  // namespace sm
