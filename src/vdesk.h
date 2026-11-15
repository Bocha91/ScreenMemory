#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "registry_desktops.h"

// COM-интерфейсы объявляем в .cpp (shobjidl_core.h тянет слишком много заголовков).
struct IVirtualDesktopManager;

// IVirtualDesktopManager — документированный интерфейс Win8+ (PLAN.md §9.1):
// узнать десктоп окна, проверить «окно на текущем десктопе», перенести окно.
// Список десктопов он не отдаёт, поэтому порядок/имена берём из реестра.
namespace sm {

// COM как STA на весь процесс. InitCom/UninitCom можно вызывать вложенно:
// реальная инициализация выполняется один раз.
bool InitCom(std::wstring* error);
void UninitCom();

class VirtualDesktopManager {
public:
    VirtualDesktopManager() = default;
    ~VirtualDesktopManager();
    VirtualDesktopManager(const VirtualDesktopManager&) = delete;
    VirtualDesktopManager& operator=(const VirtualDesktopManager&) = delete;

    bool Init(std::wstring* error);
    void Reset();
    bool ok() const { return mgr_ != nullptr; }

    // traceFailures = false — не писать в лог ожидаемые отказы: при переборе сотен
    // окон (поиск текущего десктопа) часть из них заведомо без записи в диспетчере.
    bool GetWindowDesktopId(HWND hwnd, GUID* out, bool traceFailures = true) const;
    bool IsWindowOnCurrentVirtualDesktop(HWND hwnd, BOOL* out) const;

    // Перенос окна на десктоп по GUID. Пути по порядку:
    //  1) недокументированный IVirtualDesktopManagerInternal::MoveViewToDesktop —
    //     единственный, который на Win10 22H2 переносит окна *других* процессов;
    //  2) документированный IVirtualDesktopManager::MoveWindowToDesktop — он отвечает
    //     E_ACCESSDENIED для чужого окна и оставлен резервом для окон своего процесса.
    // Сам факт переноса вызывающий должен перепроверить через GetWindowDesktopId.
    bool MoveWindowToDesktop(HWND hwnd, const GUID& desktop, HRESULT* result = nullptr) const;

private:
    IVirtualDesktopManager* mgr_ = nullptr;
};

struct CurrentDesktopInfo {
    GUID guid{};
    bool known = false;
    // Способ, которым десктоп найден: «окно в фокусе», «консольное окно»,
    // «голосование по окнам (N из M)», «окно оболочки», «реестр».
    std::wstring source;
};

// Текущий десктоп. Порядок попыток:
//   1) окно в фокусе; 2) консольное окно; 3) голосование по окнам (видимые,
//   не cloaked, «на текущем десктопе»); 4) окна оболочки (рабочий стол, панель
//   задач); 5) значение CurrentVirtualDesktop в реестре (на Win10 22H2 его нет).
// Голосование нужно для терминалов вроде ConEmu: их служебные окна не заведены
// в диспетчере десктопов, поэтому первые две попытки там не работают.
//
// knownDesktops (необязательный) — текущий список десктопов: при голосовании GUID,
// которого в нём нет, игнорируется. Это важно из-за pinned-приложений: они отдают
// собственный GUID-маркер, отсутствующий в VirtualDesktopIDs, и без фильтра на
// десктопе с малым числом окон «побеждал» бы этот маркер.
bool DetectCurrentDesktop(const VirtualDesktopManager& vdm, CurrentDesktopInfo* out,
                          const std::vector<DesktopInfo>* knownDesktops = nullptr);

// Внутренний (недокументированный) путь переноса окон между десктопами:
// самопроверка раскладки COM-объектов оболочки (PLAN.md §9.1). false — путь
// недоступен (другая сборка Windows), причина — в InternalApiReason().
//
// Спрашивать нужно ДО того, как что-то менять: документированный
// IVirtualDesktopManager::MoveWindowToDesktop переносит только окна своего
// процесса и для чужих возвращает E_ACCESSDENIED, поэтому «тихий» переход на
// него приводит к тому, что окна остаются не на своих столах.
bool EnsureInternalApiUsable();
std::wstring InternalApiReason();

// Резервный сбор списка десктопов, если VirtualDesktopIDs недоступен:
// GUID берём из окон, порядок — порядок первого появления, имена — из реестра.
// Порядок при этом приблизительный, о чём вызывающий обязан предупредить пользователя.
bool CollectDesktopsFallback(const VirtualDesktopManager& vdm, std::vector<DesktopInfo>* out,
                             std::wstring* error);

}  // namespace sm
