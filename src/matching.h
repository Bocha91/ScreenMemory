#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "registry_desktops.h"
#include "storage.h"
#include "window_info.h"

// Сопоставление живых окон со слепком (PLAN.md §8.0–8.1) и расчёт целевой
// геометрии (§8.4). Здесь только вычисления: ни одно окно не перемещается и ни
// один десктоп не переключается — применение делает main.cpp при restore.
//
// Разделение нужно ради --dry-run: план строится теми же функциями, что и при
// настоящем восстановлении, поэтому «что было бы сделано» совпадает с тем, что
// будет сделано.
namespace sm {

// Как сохранённый десктоп нашёлся среди актуальных (PLAN.md §8.0 п.2).
// Порядок приоритета — имя, номер, GUID (CONTEXT п. 57).
enum class DesktopMatchKind {
    Name,      // имя совпало — главный признак: десктоп удалили и создали заново
    Index,     // имени в системе нет (или у записи его нет) — берём тот же номер
    Guid,      // номер занят другим десктопом — GUID совпал: тот же десктоп
    Fallback,  // ничего не нашли — окна идут на десктоп 0 (требование 15)
};

// «имя» | «индекс» | «GUID» | «fallback» — для отчёта.
const wchar_t* DesktopMatchKindText(DesktopMatchKind kind);

struct DesktopTarget {
    DesktopMatchKind kind = DesktopMatchKind::Fallback;
    int currentIndex = -1;       // индекс найденного десктопа сейчас; -1 — не найден
    GUID guid{};                 // GUID десктопа сейчас (цель для MoveWindowToDesktop)
    std::wstring currentName;    // имя десктопа сейчас (пусто, если имени нет)
    bool renamed = false;        // GUID совпал, но имя изменилось
    bool nameNotFound = false;   // у записи было имя, а десктопа с таким именем нет
    bool ambiguousName = false;  // имён-дублей несколько — взяли ближайший по индексу
};

// Карта «десктоп из файла → десктоп сейчас». targets получает по элементу на
// каждый layout.desktops, в том же порядке. false — актуальный список пуст.
bool MapDesktops(const Layout& layout, const std::vector<DesktopInfo>& current,
                 std::vector<DesktopTarget>* targets, std::wstring* error);

// Позиция десктопа в layout.desktops по его индексу из файла; -1, если такого нет.
// Нужна потому, что [window.N] desktop= ссылается на [desktop.N] index=, а не на
// порядок секций в файле (совпадают они только при записи нашим save).
int DesktopSlot(const Layout& layout, int desktopIndex);

struct WindowMatch {
    int saved = -1;           // индекс в layout.windows
    int live = -1;            // индекс в списке живых окон
    int score = 0;            // сумма баллов (PLAN.md §8.1)
    std::wstring why;         // «exe class title …» — для --verbose
};

// Счётчики сопоставления — для отчёта.
struct MatchStats {
    // Записи, которые не сопоставлены из-за неоднозначности: несколько окон этого же
    // приложения дали одинаковый максимальный балл, окна сидят на разных десктопах,
    // а известные признаки (адрес страницы, папка, заголовок) их не различают.
    // Такой выбор решает, какое окно куда поедет, — при нехватке данных окна одного
    // приложения с разных столов перепутались бы, поэтому пара не создаётся вовсе
    // (требование пользователя; CONTEXT п. 61). Запись уходит в фазу B, живые окна
    // остаются на месте.
    int ambiguous = 0;
};

// Жадное сопоставление по убыванию score: каждое окно и каждая запись
// используются один раз. Порог: 100 (совпал exe) либо 90 (Проводник: class+folder).
//
// Дополнительно к таблице §8.1 (см. CONTEXT п. 45):
//  * AUMID совпал +60 — иначе у UWP все окна неразличимы (exe у них
//    ApplicationFrameHost.exe, класс ApplicationFrameWindow);
//  * «вето» по folder: если и в записи, и у живого окна папка известна и они
//    разные — пара не создаётся вовсе (папка — главный ключ окон Проводника);
//  * «вето» по AUMID по той же причине;
//  * адрес страницы (url): у браузеров адрес известен и у записи, и у живого окна.
//    Совпал — +60 и признак «url», разный — пара не создаётся вовсе: окно показывает
//    другую страницу, запись должна уйти в фазу B (CONTEXT п. 55, 60). Адрес у живого
//    окна заполняет restore перед матчингом (main.cpp), у записи он из layout.txt;
//  * неоднозначность: если у записи несколько окон с одинаковым максимальным баллом,
//    окна на разных десктопах и известные признаки их не различают — пара не
//    создаётся (MatchStats::ambiguous). Иначе выбор пары решал бы, какое окно куда
//    поедет, и окна одного приложения с разных столов перепутались бы (CONTEXT п. 61).
void MatchWindows(const Layout& layout, const std::vector<WindowInfo>& live,
                  const std::vector<DesktopTarget>& targets,
                  const std::vector<GUID>& liveDesktop,
                  const std::vector<DesktopInfo>& currentDesktops,
                  std::vector<WindowMatch>* matches, MatchStats* stats = nullptr);

// Перепроверка перед запуском (PLAN.md §8.3 п.0, CONTEXT п. 69). Пока restore работает,
// окна открывает не только он: Windows возвращает окна Проводника (`PersistBrowsers=1`),
// приложения из автозагрузки, браузер — сессию профиля. Снимок для фазы A делается один
// раз, и запись, чьё окно появилось позже, осталась бы без пары — фаза B запустила бы
// вторую копию (дубль). MatchRecord отвечает на вопрос «подошло ли этой записи какое-то
// из ещё не занятых живых окон».
//
// Правила те же, что у MatchWindows (таблица §8.1, вето по folder/aumid/url, порог
// принятия, правило неоднозначности): меняется только набор участников — одна запись и
// окна, которые пока ничьи (liveTaken[i] != 0 — окно уже занято сопоставлением фазы A,
// запуском или прошлой перепроверкой). Индексы в live — исходные, по всему вектору.
// true — пара найдена; match->saved = recordIndex, match->live = индекс в live.
bool MatchRecord(int recordIndex, const Layout& layout, const std::vector<WindowInfo>& live,
                 const std::vector<char>& liveTaken,
                 const std::vector<DesktopTarget>& targets,
                 const std::vector<GUID>& liveDesktop,
                 const std::vector<DesktopInfo>& currentDesktops, WindowMatch* match);

// Целевой прямоугольник окна (PLAN.md §8.4): mrel от монитора-якоря; если монитор
// отключён — абсолютные rect, если они попадают на живой монитор; иначе primary с
// сохранением относительной позиции. Учитывает смену DPI и не даёт окну оказаться
// целиком вне мониторов. note — что пришлось поправить (пусто = без поправок):
// «dpi», «primary», «size», «off-screen».
bool TargetRect(const LayoutWindow& window, const std::vector<LayoutMonitor>& savedMonitors,
                const std::vector<LayoutMonitor>& liveMonitors, RECT* out, std::wstring* note);

// «normal» | «maximized» | «minimized» из файла → showCmd для SetWindowPlacement.
int ShowCommandFor(const std::wstring& show);

}  // namespace sm
