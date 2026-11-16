#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Файл раскладки bin\layout.txt (PLAN.md §6): INI-подобный текст UTF-8, секции
// [desktop.N], [monitor.N], [window.N], строки «ключ=значение».
// Запись атомарная (layout.new → layout.txt), с ротацией рабочих версий и защищёнными
// копиями «вчерашний день» / «прежняя конфигурация» (§6.1).
namespace sm {

struct LayoutMonitor {
    std::wstring device;  // \\.\DISPLAY4 — якорь для mrel
    RECT rect{};          // положение монитора на виртуальном экране
    RECT work{};          // рабочая область (без панели задач)
    UINT dpi = 96;        // эффективный DPI (GetDpiForMonitor) либо 96
    int primary = 0;
};

struct LayoutDesktop {
    int index = 0;      // порядок из VirtualDesktopIDs
    GUID guid{};        // главный ключ восстановления
    std::wstring name;  // может быть пустым и повторяться
};

struct LayoutWindow {
    int desktop = 0;           // индекс десктопа из [desktop.N]
    std::wstring desktopName;  // имя на момент сохранения
    int pinned = 0;            // 1 = «показывать на всех рабочих столах»
    int zg = 0;                // z-order внутри десктопа, 0 — самое верхнее
    std::wstring exe;          // полный путь (сопоставление + запуск)
    std::wstring cmdline;      // для запуска с аргументами
    std::wstring url;          // адрес страницы (только окна браузеров, читается через UIA)
    std::wstring cls;          // класс окна
    std::wstring folder;       // только для Проводника
    std::wstring title;
    std::wstring aumid;        // UWP и pinned-сопоставление
    std::wstring monitor;      // device-имя монитора-якоря
    RECT mrel{};               // ОСНОВНОЕ: координаты от угла своего монитора
    RECT rect{};               // абсолютные координаты (rcNormalPosition)
    RECT frame{};              // видимые границы DWM — только диагностика
    bool frameOk = false;
    std::wstring show;         // normal | maximized | minimized
    int uwp = 0;               // 1 — окно прятало приложение (UWP suspended)
};

struct Layout {
    std::wstring saved;  // 2026-11-12T21:15:03
    int screenW = 0;     // SM_CXVIRTUALSCREEN на момент сохранения
    int screenH = 0;
    std::vector<LayoutDesktop> desktops;
    std::vector<LayoutMonitor> monitors;
    std::vector<LayoutWindow> windows;
};

// Метки времени формата: «2026-11-12T21:15:03» в заголовке и
// «2026-11-12_21-15-03» в именах ротированных файлов.
std::wstring NowTimestamp();
std::wstring FileTimestamp(const SYSTEMTIME& st);
std::wstring FileTimestamp(const FILETIME& ft);

// Список мониторов (EnumDisplayMonitors) в порядке, который отдаёт система —
// индекс N в [monitor.N] равен порядку обхода.
bool CollectMonitors(std::vector<LayoutMonitor>* out);

// Экранирование значений: \, \n, \r, ; (заголовки окон бывают многострочными,
// а «;» в файле начинает комментарий). Обратная операция — UnescapeValue.
std::wstring EscapeValue(const std::wstring& value);
std::wstring UnescapeValue(const std::wstring& value);

// RECT ↔ «left,top,right,bottom».
std::wstring RectToText(const RECT& rect);
bool ParseRect(const std::wstring& text, RECT* out);

// Полный текст файла раскладки.
std::wstring SerializeLayout(const Layout& layout);

// Разбор текста. false — нет заголовка формата (битые значения отдельных полей
// не фатальны: неизвестные ключи и строки игнорируются, PLAN.md §6).
bool ParseLayout(const std::wstring& text, Layout* out, std::wstring* error);

// Сравнение двух раскладок «по существу»: состав десктопов, мониторов и окон с
// координатами и состоянием. Порядок наложения окон (zg) и порядок записей не
// сравниваются: z-order меняется от каждого клика по окну, и save не должен из-за этого
// переписывать файл (иначе AutoSave раз в 10 минут изнашивал бы диск впустую). Нужно save:
// раскладка не изменилась — файл остаётся прежним, ротации нет.
bool SameLayoutContent(const Layout& a, const Layout& b);

// Причина отличия для --verbose: пустая строка — раскладки совпадают, иначе короткое
// пояснение вида «окно «…» стало другим: было …». Нужна, чтобы «файл перезаписан» не
// выглядел необъяснимым.
std::wstring LayoutDiffReason(const Layout& a, const Layout& b);

// Сколько рабочих версий слепка держится в bin по умолчанию (PLAN.md §6.1). Ключ Layouts
// в Config.ini меняет это число, допустимо 1…kMaxLayoutVersions.
constexpr int kDefaultLayoutVersions = 3;
constexpr int kMaxLayoutVersions = 20;

// Защищённые копии (PLAN.md §6.1). Имена начинаются с layout-, поэтому их видно рядом с
// рабочими версиями, но чистка (PruneOldVersions) их не трогает: она удаляет только файлы
// с меткой времени, а всё остальное из layout-*.txt остаётся навсегда.
//   «цепочка дней» сдвигается restore при старте новой сессии,
//   «прежняя конфигурация» сдвигается save при смене мониторов или набора десктопов.
constexpr int kDayChainLength = 3;
constexpr int kConfigChainLength = 3;
extern const wchar_t* const kDayChainNames[kDayChainLength];
extern const wchar_t* const kConfigChainNames[kConfigChainLength];

// Итог записи слепка — для отчёта save.
struct LayoutSaveResult {
    std::wstring path;        // dir\layout.txt
    std::wstring rotated;     // имя файла ротации (пусто — предыдущего слепка не было)
    std::wstring configCopy;  // имя копии «прежняя конфигурация» (пусто — конфигурация та же)
    int pruned = 0;           // сколько рабочих версий удалено чисткой
};

// Запись слепка в dir\layout.txt по правилам PLAN.md §6.1: layout.new → защищённая копия
// при смене конфигурации → ротация в layout-<время>.txt → замена → чистка версий сверх
// versions. previous — прежний слепок (nullptr, если прочитать его не удалось): по нему
// решается, изменилась ли конфигурация мониторов и набор десктопов.
bool WriteLayoutFile(const std::wstring& dir, const Layout& layout, const Layout* previous,
                     int versions, LayoutSaveResult* result, std::wstring* error);

// Чтение dir\layout.txt (нужно restore, этап 6).
bool ReadLayoutFile(const std::wstring& dir, Layout* out, std::wstring* path,
                    std::wstring* error);

// Чтение раскладки из произвольного файла (restore --from): абсолютный путь — как есть,
// иначе имя ищется рядом с exe. В path возвращается разобранный путь.
bool ReadLayoutFromFile(const std::wstring& dir, const std::wstring& file, Layout* out,
                        std::wstring* path, std::wstring* error);

// Изменилась ли конфигурация: состав и геометрия мониторов (device, rect, primary) плюс
// набор GUID десктопов. Имена десктопов и порядок мониторов изменением не считаются.
bool SameConfigSignature(const Layout& a, const Layout& b);

// Начало текущей загрузки системы в формате метки «2026-11-12T21:15:03» (местное время):
// слепок с меньшей меткой принадлежит прошлой сессии. Пустая строка — время не узнать.
std::wstring SessionStartStamp();

// Сдвиг «цепочки дней» при старте сессии (PLAN.md §6.1 п.5): слепок, с которым
// восстанавливаемся, становится «вчерашним», прежний «вчерашний» — «позавчерашним» и т. д.
// Сдвига нет, если слепок создан в текущей сессии или уже стоит во «вчерашнем» — повторный
// restore в тот же день цепочку не портит. Вызывает restore, не save.
struct DayChainResult {
    bool shifted = false;
    std::wstring reason;     // почему не сдвигали (для --verbose)
    std::wstring yesterday;  // имя файла «вчерашний день»
    std::wstring previous;   // метка слепка, лежавшего там до сдвига (пусто — файла не было)
};

bool ShiftDayChain(const std::wstring& dir, const Layout& layout, DayChainResult* result,
                   std::wstring* error);

}  // namespace sm
