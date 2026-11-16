#include "matching.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

#include "browser_url.h"
#include "log.h"

namespace sm {

namespace {

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Элемент актуального списка по полю index (позиция в векторе и индекс совпадают
// только у нашего ReadDesktops, полагаться на это не будем).
const DesktopInfo* DesktopByIndex(const std::vector<DesktopInfo>& desktops, int index) {
    for (const DesktopInfo& desktop : desktops) {
        if (desktop.index == index) {
            return &desktop;
        }
    }
    return nullptr;
}

const LayoutMonitor* MonitorByDevice(const std::vector<LayoutMonitor>& monitors,
                                     const std::wstring& device) {
    if (device.empty()) {
        return nullptr;
    }
    for (const LayoutMonitor& monitor : monitors) {
        if (EqualsNoCase(monitor.device, device)) {
            return &monitor;
        }
    }
    return nullptr;
}

const LayoutMonitor* PrimaryMonitor(const std::vector<LayoutMonitor>& monitors) {
    for (const LayoutMonitor& monitor : monitors) {
        if (monitor.primary != 0) {
            return &monitor;
        }
    }
    return monitors.empty() ? nullptr : &monitors.front();
}

long long OverlapArea(const RECT& a, const RECT& b) {
    const LONG left = (std::max)(a.left, b.left);
    const LONG top = (std::max)(a.top, b.top);
    const LONG right = (std::min)(a.right, b.right);
    const LONG bottom = (std::min)(a.bottom, b.bottom);
    if (right <= left || bottom <= top) {
        return 0;
    }
    return static_cast<long long>(right - left) * static_cast<long long>(bottom - top);
}

long long AreaOf(const RECT& rect) {
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return static_cast<long long>(width) * static_cast<long long>(height);
}

// Пересечение занимает не меньше половины меньшего прямоугольника — «rect близок».
bool MostlyOverlaps(const RECT& a, const RECT& b) {
    const long long base = (std::min)(AreaOf(a), AreaOf(b));
    if (base <= 0) {
        return false;
    }
    return OverlapArea(a, b) * 2 >= base;
}

}  // namespace

const wchar_t* DesktopMatchKindText(DesktopMatchKind kind) {
    switch (kind) {
        case DesktopMatchKind::Guid:
            return L"GUID";
        case DesktopMatchKind::Name:
            return L"имя";
        case DesktopMatchKind::Index:
            return L"индекс";
        default:
            return L"fallback";
    }
}

int DesktopSlot(const Layout& layout, int desktopIndex) {
    for (size_t i = 0; i < layout.desktops.size(); ++i) {
        if (layout.desktops[i].index == desktopIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool MapDesktops(const Layout& layout, const std::vector<DesktopInfo>& current,
                 std::vector<DesktopTarget>* targets, std::wstring* error) {
    if (targets == nullptr) {
        return false;
    }
    targets->clear();
    if (current.empty()) {
        if (error != nullptr) {
            *error = L"актуальный список десктопов пуст";
        }
        return false;
    }

    targets->reserve(layout.desktops.size());
    for (const LayoutDesktop& saved : layout.desktops) {
        DesktopTarget target;

        const std::wstring savedName = Trim(saved.name);

        // 1. Имя — главный признак (CONTEXT п. 57). Имя дают, чтобы обозначить, что
        //    на десктопе открыто, поэтому оно важнее и GUID, и номера: десктоп могли
        //    удалить и создать заново — GUID тогда другой, а имя то же. Если
        //    имён-дублей несколько — берём ближайший по индексу и отмечаем это.
        if (!savedName.empty()) {
            int best = -1;
            int bestDelta = 0;
            int sameNames = 0;
            for (const DesktopInfo& desktop : current) {
                if (!EqualsNoCase(Trim(desktop.name), savedName)) {
                    continue;
                }
                ++sameNames;
                const int delta = std::abs(desktop.index - saved.index);
                if (best < 0 || delta < bestDelta) {
                    best = desktop.index;
                    bestDelta = delta;
                }
            }
            if (best >= 0) {
                target.kind = DesktopMatchKind::Name;
                target.currentIndex = best;
                target.ambiguousName = sameNames > 1;
            }
        }

        // 2. Номер (индекс) — второй по важности: пользователь, переставляя десктопы
        //    в Task View, обычно помнит «третий слева», а не GUID.
        if (target.currentIndex < 0 && DesktopByIndex(current, saved.index) != nullptr) {
            target.kind = DesktopMatchKind::Index;
            target.currentIndex = saved.index;
        }

        // 3. GUID — последний: он переживает переименование, но ничего не говорит о
        //    том, что на десктопе было открыто.
        if (target.currentIndex < 0) {
            const int byGuid = FindDesktopIndex(current, saved.guid);
            if (byGuid >= 0) {
                target.kind = DesktopMatchKind::Guid;
                target.currentIndex = byGuid;
            }
        }

        // 4. Ничего не нашли — на десктоп 0 (требование 15): имя не хардкодится,
        //    «нулевой» — первый в актуальном списке.
        if (target.currentIndex < 0) {
            target.kind = DesktopMatchKind::Fallback;
            target.currentIndex = current.front().index;
        }

        const DesktopInfo* found = DesktopByIndex(current, target.currentIndex);
        if (found == nullptr) {
            found = &current.front();
            target.currentIndex = found->index;
        }
        target.guid = found->guid;
        target.currentName = found->name;
        target.renamed = target.kind == DesktopMatchKind::Guid && !savedName.empty() &&
                         !EqualsNoCase(savedName, Trim(found->name));
        target.nameNotFound = !savedName.empty() && !target.renamed &&
                              !EqualsNoCase(savedName, Trim(found->name));
        targets->push_back(target);
    }
    return true;
}

void MatchWindows(const Layout& layout, const std::vector<WindowInfo>& live,
                  const std::vector<DesktopTarget>& targets,
                  const std::vector<GUID>& liveDesktop,
                  const std::vector<DesktopInfo>& currentDesktops,
                  std::vector<WindowMatch>* matches, MatchStats* stats) {
    if (matches == nullptr) {
        return;
    }
    matches->clear();
    if (stats != nullptr) {
        stats->ambiguous = 0;
    }

    // Кандидаты «запись ↔ окно» с баллами (PLAN.md §8.1).
    struct Candidate {
        int saved = -1;
        int live = -1;
        int score = 0;
        std::wstring why;
    };
    std::vector<Candidate> candidates;

    for (size_t s = 0; s < layout.windows.size(); ++s) {
        const LayoutWindow& saved = layout.windows[s];
        const int slot = DesktopSlot(layout, saved.desktop);
        const DesktopTarget* target =
            (slot >= 0 && slot < static_cast<int>(targets.size())) ? &targets[static_cast<size_t>(slot)]
                                                                  : nullptr;

        for (size_t l = 0; l < live.size(); ++l) {
            const WindowInfo& info = live[l];
            int score = 0;
            std::wstring why;

            const bool exeMatch = !saved.exe.empty() && EqualsNoCase(saved.exe, info.exePath);
            if (exeMatch) {
                score += 100;
                why += L"exe ";
            }
            if (!saved.cls.empty() && saved.cls == info.cls) {
                score += 50;
                why += L"class ";
            }
            if (!saved.title.empty() && saved.title == info.title) {
                score += 40;
                why += L"title ";
            }
            const bool aumidMatch = !saved.aumid.empty() && EqualsNoCase(saved.aumid, info.aumid);
            if (aumidMatch) {
                score += 60;
                why += L"aumid ";
            }
            const bool folderMatch = !saved.folder.empty() && EqualsNoCase(saved.folder, info.folder);
            if (folderMatch) {
                score += 40;
                why += L"folder ";
            }

            // Адрес страницы (PLAN.md §8.1): у браузера это единственный признак
            // содержимого окна — заголовок живой вкладки меняется от любого перехода.
            const bool urlKnown = !saved.url.empty() && !info.url.empty();
            const bool urlMatch = urlKnown && SamePage(saved.url, info.url);
            if (urlMatch) {
                score += 60;
                why += L"url ";
            }

            // «Вето»: у Проводника папка — главный ключ (PLAN.md §9.2), у UWP — AUMID.
            // Если оба значения известны и различны, это другое окно: лучше не
            // восстановить, чем утащить чужое окно на место потерянного.
            if (!saved.folder.empty() && !info.folder.empty() && !folderMatch) {
                continue;
            }
            if (!saved.aumid.empty() && !info.aumid.empty() && !aumidMatch) {
                continue;
            }
            // Вето по адресу страницы: адрес известен у обоих и разный — окно браузера
            // показывает другую страницу (CONTEXT п. 55). Запись не сопоставляем: она
            // уйдёт в фазу B и откроет нужную страницу отдельным окном, а чужое окно
            // останется на месте — это лучше, чем утащить его на чужой десктоп.
            if (urlKnown && !urlMatch) {
                continue;
            }

            // Десктоп — только разрешение ничьих, «угадывать окно» по нему нельзя.
            const GUID liveGuid = liveDesktop[l];
            if (target != nullptr && !IsNullGuid(liveGuid)) {
                if (IsEqualGUID(liveGuid, target->guid) != FALSE) {
                    score += 25;
                    why += L"desktop ";
                } else {
                    const int liveIndex = FindDesktopIndex(currentDesktops, liveGuid);
                    if (liveIndex >= 0 && !Trim(saved.desktopName).empty() &&
                        EqualsNoCase(Trim(saved.desktopName),
                                     Trim(currentDesktops[static_cast<size_t>(liveIndex)].name))) {
                        score += 10;
                        why += L"desktop-name ";
                    }
                }
            }

            if (info.rectOk && MostlyOverlaps(saved.rect, info.rect)) {
                score += 20;
                why += L"rect ";
            }

            // Порог принятия: 100 (exe совпал) либо 90 (Проводник: class + folder).
            const bool explorerPair = !saved.folder.empty() && folderMatch;
            if (score < 100 && !(explorerPair && score >= 90)) {
                continue;
            }
            candidates.push_back(Candidate{static_cast<int>(s), static_cast<int>(l), score, why});
        }
    }

    // Неоднозначность (CONTEXT п. 61). Требование пользователя: окна одного и того же
    // приложения, открытые на разных десктопах, не должны перепутываться. Если у записи
    // несколько окон с одинаковым максимальным баллом, эти окна сидят на разных столах,
    // а известные признаки (exe, класс, заголовок, папка, AUMID, адрес) их не различают,
    // то выбор пары решает, какое окно куда поедет — при нехватке данных это лотерея.
    // Такие кандидаты отклоняем: запись уйдёт в фазу B и откроет своё окно, а живые окна
    // останутся на своих столах. Если окна на одном столе, перепутывание безвредно
    // (переносов не будет), поэтому правило не срабатывает.
    if (candidates.size() > 1) {
        auto contentKey = [&live](int liveIndex) {
            const WindowInfo& info = live[static_cast<size_t>(liveIndex)];
            std::wstring key = info.exePath;
            key += L'\n';
            key += info.cls;
            key += L'\n';
            key += info.title;
            key += L'\n';
            key += info.folder;
            key += L'\n';
            key += info.aumid;
            key += L'\n';
            key += info.url;
            return key;
        };

        std::vector<char> drop(candidates.size(), 0);
        std::map<int, std::vector<size_t>> bySaved;
        for (size_t i = 0; i < candidates.size(); ++i) {
            bySaved[candidates[i].saved].push_back(i);
        }
        for (const auto& group : bySaved) {
            const std::vector<size_t>& list = group.second;
            if (list.size() < 2) {
                continue;
            }
            int best = -1;
            for (const size_t index : list) {
                if (candidates[index].score > best) {
                    best = candidates[index].score;
                }
            }
            std::vector<size_t> ties;
            for (const size_t index : list) {
                if (candidates[index].score == best) {
                    ties.push_back(index);
                }
            }
            if (ties.size() < 2) {
                continue;
            }
            const std::wstring firstKey = contentKey(candidates[ties[0]].live);
            bool windowsDiffer = false;
            for (size_t i = 1; i < ties.size() && !windowsDiffer; ++i) {
                windowsDiffer = contentKey(candidates[ties[i]].live) != firstKey;
            }
            if (!windowsDiffer) {
                continue;  // окна неотличимы: перепутывание ничего не меняет
            }
            const GUID& firstGuid = liveDesktop[static_cast<size_t>(candidates[ties[0]].live)];
            bool desktopsDiffer = false;
            for (size_t i = 1; i < ties.size() && !desktopsDiffer; ++i) {
                const GUID& guid = liveDesktop[static_cast<size_t>(candidates[ties[i]].live)];
                desktopsDiffer = IsEqualGUID(guid, firstGuid) == FALSE;
            }
            if (!desktopsDiffer) {
                continue;  // один стол: окна и так не переместятся
            }
            for (const size_t index : ties) {
                drop[index] = 1;
            }
            if (stats != nullptr) {
                ++stats->ambiguous;
            }
            Trace(L"  неоднозначно: запись «%s» — %d окон с баллом %d на разных десктопах",
                  layout.windows[static_cast<size_t>(group.first)].title.c_str(),
                  static_cast<int>(ties.size()), best);
        }
        if (std::find(drop.begin(), drop.end(), 1) != drop.end()) {
            std::vector<Candidate> kept;
            kept.reserve(candidates.size());
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (drop[i] == 0) {
                    kept.push_back(std::move(candidates[i]));
                }
            }
            candidates.swap(kept);
        }
    }

    // Жадно: лучшие пары забирают окна первыми, каждое окно и каждая запись — один раз.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  if (a.saved != b.saved) {
                      return a.saved < b.saved;
                  }
                  return a.live < b.live;
              });

    std::vector<char> savedUsed(layout.windows.size(), 0);
    std::vector<char> liveUsed(live.size(), 0);
    for (const Candidate& candidate : candidates) {
        if (savedUsed[static_cast<size_t>(candidate.saved)] != 0 ||
            liveUsed[static_cast<size_t>(candidate.live)] != 0) {
            continue;
        }
        savedUsed[static_cast<size_t>(candidate.saved)] = 1;
        liveUsed[static_cast<size_t>(candidate.live)] = 1;
        WindowMatch match;
        match.saved = candidate.saved;
        match.live = candidate.live;
        match.score = candidate.score;
        match.why = candidate.why;
        matches->push_back(std::move(match));
    }
}

// Перепроверка перед запуском (PLAN.md §8.3 п.0, CONTEXT п. 69): одна запись против окон,
// которые ещё ничьи. Участники урезаются до подмножества, а правила остаются прежними —
// поэтому решение совпадает с тем, что приняла бы фаза A, если бы окно существовало тогда.
bool MatchRecord(int recordIndex, const Layout& layout, const std::vector<WindowInfo>& live,
                 const std::vector<char>& liveTaken,
                 const std::vector<DesktopTarget>& targets,
                 const std::vector<GUID>& liveDesktop,
                 const std::vector<DesktopInfo>& currentDesktops, WindowMatch* match) {
    if (match == nullptr || recordIndex < 0 ||
        recordIndex >= static_cast<int>(layout.windows.size())) {
        return false;
    }

    // Подмножество записей: только эта. Десктопы и мониторы — как в слепке целиком:
    // DesktopSlot ищет десктоп записи именно по ним.
    Layout subset;
    subset.saved = layout.saved;
    subset.screenW = layout.screenW;
    subset.screenH = layout.screenH;
    subset.desktops = layout.desktops;
    subset.monitors = layout.monitors;
    subset.windows.push_back(layout.windows[static_cast<size_t>(recordIndex)]);

    // Подмножество окон: всё, что ещё не занято. Индексы переводим обратно, чтобы
    // вызывающий работал с исходным вектором живых окон.
    std::vector<WindowInfo> liveSubset;
    std::vector<GUID> liveDesktopSubset;
    std::vector<int> liveIndex;
    for (size_t i = 0; i < live.size(); ++i) {
        if (i < liveTaken.size() && liveTaken[i] != 0) {
            continue;
        }
        liveSubset.push_back(live[i]);
        liveDesktopSubset.push_back(i < liveDesktop.size() ? liveDesktop[i] : GUID{});
        liveIndex.push_back(static_cast<int>(i));
    }
    if (liveSubset.empty()) {
        return false;
    }

    std::vector<WindowMatch> found;
    MatchWindows(subset, liveSubset, targets, liveDesktopSubset, currentDesktops, &found, nullptr);
    if (found.empty()) {
        return false;
    }
    const WindowMatch& best = found.front();
    match->saved = recordIndex;
    match->live = liveIndex[static_cast<size_t>(best.live)];
    match->score = best.score;
    match->why = best.why;
    return true;
}

namespace {

void AddNote(std::wstring* note, const wchar_t* what) {
    if (note == nullptr) {
        return;
    }
    if (!note->empty()) {
        note->append(L", ");
    }
    note->append(what);
}

}  // namespace

bool TargetRect(const LayoutWindow& window, const std::vector<LayoutMonitor>& savedMonitors,
                const std::vector<LayoutMonitor>& liveMonitors, RECT* out, std::wstring* note) {
    if (out == nullptr || liveMonitors.empty()) {
        return false;
    }
    if (note != nullptr) {
        note->clear();
    }

    const LONG savedWidth = window.mrel.right - window.mrel.left;
    const LONG savedHeight = window.mrel.bottom - window.mrel.top;
    const LayoutMonitor* live = MonitorByDevice(liveMonitors, window.monitor);
    const LayoutMonitor* savedMonitor = MonitorByDevice(savedMonitors, window.monitor);

    RECT target{};
    bool have = false;

    if (live != nullptr) {
        double scale = 1.0;
        // Кросс-мониторные окна не масштабируем, только сдвигаем (PLAN.md §8.4 п.5).
        const bool crossMonitor =
            savedMonitor != nullptr &&
            (window.mrel.left < -8 || window.mrel.top < -8 ||
             window.mrel.right > (savedMonitor->rect.right - savedMonitor->rect.left) + 8 ||
             window.mrel.bottom > (savedMonitor->rect.bottom - savedMonitor->rect.top) + 8);
        if (!crossMonitor && savedMonitor != nullptr && savedMonitor->dpi != 0 &&
            live->dpi != savedMonitor->dpi) {
            scale = static_cast<double>(live->dpi) / static_cast<double>(savedMonitor->dpi);
            AddNote(note, L"dpi");
        }
        target.left = live->rect.left + static_cast<LONG>(std::lround(window.mrel.left * scale));
        target.top = live->rect.top + static_cast<LONG>(std::lround(window.mrel.top * scale));
        target.right = target.left + static_cast<LONG>(std::lround(savedWidth * scale));
        target.bottom = target.top + static_cast<LONG>(std::lround(savedHeight * scale));
        have = true;
    } else if (window.monitor.empty()) {
        // monitor пуст — в mrel записаны абсолютные координаты (PLAN.md §8.4 п.2).
        target = window.rect;
        have = true;
    } else if (AreaOf(window.rect) > 0) {
        // Монитор-якорь отключён: абсолютные координаты годятся, только если они
        // попадают на живой монитор.
        for (const LayoutMonitor& monitor : liveMonitors) {
            if (OverlapArea(window.rect, monitor.rect) > 0) {
                target = window.rect;
                have = true;
                AddNote(note, L"abs");
                break;
            }
        }
    }

    if (!have) {
        // Всё пропало — на primary, сохраняя относительную позицию и размер.
        const LayoutMonitor* primary = PrimaryMonitor(liveMonitors);
        if (primary == nullptr) {
            return false;
        }
        target.left = primary->rect.left + window.mrel.left;
        target.top = primary->rect.top + window.mrel.top;
        target.right = target.left + savedWidth;
        target.bottom = target.top + savedHeight;
        AddNote(note, L"primary");
    }

    LONG width = target.right - target.left;
    LONG height = target.bottom - target.top;
    if (width < 100) {
        width = 100;
        AddNote(note, L"size");
    }
    if (height < 50) {
        height = 50;
        AddNote(note, L"size");
    }

    // Монитор, на котором окно занимает больше всего места: по нему и ограничиваем
    // размер, чтобы окно не залезало под панель задач.
    const LayoutMonitor* home = nullptr;
    long long bestArea = -1;
    for (const LayoutMonitor& monitor : liveMonitors) {
        const long long area = OverlapArea(target, monitor.rect);
        if (area > bestArea) {
            bestArea = area;
            home = &monitor;
        }
    }
    if (home == nullptr) {
        home = PrimaryMonitor(liveMonitors);
    }
    if (home == nullptr) {
        return false;
    }
    const LONG workWidth = home->work.right - home->work.left;
    const LONG workHeight = home->work.bottom - home->work.top;
    if (workWidth > 0 && width > workWidth) {
        width = workWidth;
        AddNote(note, L"size");
    }
    if (workHeight > 0 && height > workHeight) {
        height = workHeight;
        AddNote(note, L"size");
    }
    if (bestArea <= 0) {
        // Окно оказалось целиком вне всех мониторов — прижимаем в рабочую область.
        target.left = home->work.left;
        target.top = home->work.top;
        AddNote(note, L"off-screen");
    }

    target.right = target.left + width;
    target.bottom = target.top + height;
    *out = target;
    return true;
}

int ShowCommandFor(const std::wstring& show) {
    if (EqualsNoCase(show, L"maximized")) {
        return SW_SHOWMAXIMIZED;
    }
    if (EqualsNoCase(show, L"minimized")) {
        return SW_SHOWMINIMIZED;
    }
    return SW_SHOWNORMAL;
}

}  // namespace sm



