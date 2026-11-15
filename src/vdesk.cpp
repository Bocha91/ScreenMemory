#include "vdesk.h"

#include <dwmapi.h>
#include <objbase.h>
#include <shobjidl_core.h>

#include "log.h"

namespace {

// CLSID_VirtualDesktopManager — документированный класс (Windows 8+).
// В SDK он объявлен в shlguid.h и определён в uuid.lib; держим константу локально,
// чтобы не тащить ещё одну библиотеку ради одной GUID.
const GUID kClsidVirtualDesktopManager = {
    0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};

bool g_comReady = false;

// ---------------------------------------------------------------------------
// Перенос окна на другой десктоп. Документированный IVirtualDesktopManager
// отказывает с E_ACCESSDENIED, если окно принадлежит другому процессу: проверено
// экспериментом на этой машине (собственное окно переносится, окно Блокнота — нет,
// причём отказ приходит и при переносе «на текущий» десктоп). Поэтому основной путь —
// внутренний интерфейс оболочки: IApplicationView окна →
// IVirtualDesktopManagerInternal::MoveViewToDesktop; владелец окна ему не важен.
//
// GUID'ы и раскладка vtable взяты из VirtualDesktop (M. Scholtes) для Win10
// 1809..22H2. Перед первым использованием делается самопроверка: количество
// десктопов, текущий десктоп и поиск его же по GUID должны сходиться. Если сборка
// Windows другая и раскладка не совпала — внутренний путь отключается (иначе вызов
// по чужому слоту может уронить процесс), остаётся документированный API и запись
// в admin.log.
const GUID kClsidImmersiveShell = {
    0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
// Запрашивать надо именно IServiceProvider (IID ниже), а НЕ IID_IUnknown: объект,
// отданный как IUnknown, в этом процессе имеет другую раскладку vtable — вызов слота 3
// уходит в рекурсию и роняет процесс stack overflow (0xC00000FD) внутри combase.dll.
// Проверено на Win10 22H2: IUnknown — зависание/падение, IServiceProvider — S_OK.
const GUID kIidServiceProvider = {
    0x6d5140c1, 0x7436, 0x11ce, {0x80, 0x34, 0x00, 0xaa, 0x00, 0x60, 0x09, 0xfa}};
const GUID kSidDesktopManagerInternal = {
    0xc5e0cdca, 0x7b6e, 0x41b2, {0x9f, 0xc4, 0xd9, 0x39, 0x75, 0xcc, 0x46, 0x7b}};
const GUID kIidDesktopManagerInternal = {
    0xf31574d6, 0xb682, 0x4cdc, {0xbd, 0x56, 0x18, 0x27, 0x86, 0x0a, 0xbe, 0xc6}};
const GUID kIidApplicationViewCollection = {
    0x1841c6d7, 0x4f9d, 0x42c0, {0xaf, 0x41, 0x87, 0x47, 0x53, 0x8f, 0x10, 0xe5}};

// Вызовы по индексу vtable, а не через «фальшивые» объявления интерфейсов: при
// смене раскладки так видно, какой именно слот не совпал, и проще самопроверка.
using QueryServiceFn = HRESULT(STDMETHODCALLTYPE*)(void* self, REFGUID service, REFIID riid,
                                                   void** out);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void* self);
using GetCountFn = HRESULT(STDMETHODCALLTYPE*)(void* self, int* count);
using GetCurrentDesktopFn = HRESULT(STDMETHODCALLTYPE*)(void* self, void** desktop);
using FindDesktopFn = HRESULT(STDMETHODCALLTYPE*)(void* self, const GUID* id, void** desktop);
using MoveViewToDesktopFn = HRESULT(STDMETHODCALLTYPE*)(void* self, void* view, void* desktop);
using GetViewForHwndFn = HRESULT(STDMETHODCALLTYPE*)(void* self, HWND hwnd, void** view);
using GetDesktopIdFn = HRESULT(STDMETHODCALLTYPE*)(void* self, GUID* id);

template <typename Fn>
Fn VtableSlot(void* object, size_t index) {
    void** table = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(table[index]);
}

// Слоты считаются по фактической vtable COM-объекта: 0 QueryInterface, 1 AddRef, 2 Release.
// Поэтому первый метод любого внутреннего интерфейса идёт в слот 3 (в C#-объявлениях
// Scholtes нумерация начинается с 0 — оттуда и берётся путаница, из-за которой вызов
// «слота 0» попадал в QueryInterface и ронял процесс).
const size_t kSlotRelease = 2;
const size_t kSlotQueryService = 3;
// IApplicationViewCollection: 3 GetViews, 4 GetViewsByZOrder, 5 GetViewsByAppUserModelId,
// 6 GetViewForHwnd.
const size_t kSlotGetViewForHwnd = 6;
// IVirtualDesktopManagerInternal (Win10 1809+): 3 GetCount, 4 MoveViewToDesktop,
// 6 GetCurrentDesktop, 12 FindDesktop.
const size_t kSlotGetCount = 3;
const size_t kSlotMoveViewToDesktop = 4;
const size_t kSlotGetCurrentDesktop = 6;
const size_t kSlotFindDesktop = 12;
// IVirtualDesktop: 3 IsViewVisible, 4 GetId.
const size_t kSlotDesktopGetId = 4;

void ReleaseObject(void* object) {
    if (object != nullptr) {
        VtableSlot<ReleaseFn>(object, kSlotRelease)(object);
    }
}

// Внутренние объекты оболочки кэшируются на процесс: раскладка проверяется один раз.
struct InternalDesktopApi {
    void* provider = nullptr;  // IServiceProvider (CLSID_ImmersiveShell) — держим до конца
    void* manager = nullptr;   // IVirtualDesktopManagerInternal
    void* views = nullptr;     // IApplicationViewCollection
    bool checked = false;
    bool usable = false;
    std::wstring reason;       // почему недоступен — печатается один раз
};
InternalDesktopApi g_internal;

void ResetInternalApi() {
    ReleaseObject(g_internal.views);
    ReleaseObject(g_internal.manager);
    ReleaseObject(g_internal.provider);
    g_internal = InternalDesktopApi{};
}

// Инициализация внутреннего пути с самопроверкой раскладки vtable. false — путь
// недоступен (причина в g_internal.reason, печатается один раз).
bool EnsureInternalApi() {
    if (g_internal.checked) {
        return g_internal.usable;
    }
    g_internal.checked = true;

    // Отладочный ключ: позволяет проверить поведение «на чужой сборке Windows» без
    // реальной несовместимости. Используется в проверках (см. CONTEXT.md, п. 56).
    wchar_t forceOff[8] = {};
    if (GetEnvironmentVariableW(L"SCREENMEMORY_NO_INTERNAL_API", forceOff, 8) > 0 &&
        forceOff[0] == L'1') {
        g_internal.reason =
            L"внутренний путь отключён отладочным ключом SCREENMEMORY_NO_INTERNAL_API=1";
        return false;
    }

    if (!g_comReady) {
        g_internal.reason = L"COM не инициализирован (нет InitCom)";
        return false;
    }

    void* provider = nullptr;
    HRESULT hr = CoCreateInstance(kClsidImmersiveShell, nullptr, CLSCTX_ALL, kIidServiceProvider,
                                  &provider);
    if (FAILED(hr) || provider == nullptr) {
        g_internal.reason = L"CoCreateInstance(CLSID_ImmersiveShell): " + sm::HrText(hr);
        return false;
    }
    sm::Trace(L"внутренний путь: ImmersiveShell создан (%p)", provider);
    const QueryServiceFn query = VtableSlot<QueryServiceFn>(provider, kSlotQueryService);

    void* manager = nullptr;
    hr = query(provider, kSidDesktopManagerInternal, kIidDesktopManagerInternal, &manager);
    sm::Trace(L"внутренний путь: QueryService(manager) hr=%s ptr=%p", sm::HrText(hr).c_str(),
              manager);
    if (FAILED(hr) || manager == nullptr) {
        g_internal.reason = L"QueryService(IVirtualDesktopManagerInternal): " + sm::HrText(hr);
        ReleaseObject(provider);
        return false;
    }

    void* views = nullptr;
    hr = query(provider, kIidApplicationViewCollection, kIidApplicationViewCollection, &views);
    sm::Trace(L"внутренний путь: QueryService(views) hr=%s ptr=%p", sm::HrText(hr).c_str(), views);
    if (FAILED(hr) || views == nullptr) {
        g_internal.reason = L"QueryService(IApplicationViewCollection): " + sm::HrText(hr);
        ReleaseObject(manager);
        ReleaseObject(provider);
        return false;
    }

    // Самопроверка: количество десктопов правдоподобно, текущий десктоп получен и
    // поиск его же по GUID даёт тот же GUID. На чужой сборке Windows это не сойдётся.
    int count = 0;
    hr = VtableSlot<GetCountFn>(manager, kSlotGetCount)(manager, &count);
    sm::Trace(L"внутренний путь: GetCount hr=%s count=%d", sm::HrText(hr).c_str(), count);
    void* current = nullptr;
    if (SUCCEEDED(hr)) {
        hr = VtableSlot<GetCurrentDesktopFn>(manager, kSlotGetCurrentDesktop)(manager, &current);
        sm::Trace(L"внутренний путь: GetCurrentDesktop hr=%s ptr=%p", sm::HrText(hr).c_str(), current);
    }
    GUID currentId{};
    if (SUCCEEDED(hr) && current != nullptr) {
        hr = VtableSlot<GetDesktopIdFn>(current, kSlotDesktopGetId)(current, &currentId);
        sm::Trace(L"внутренний путь: GUID текущего %s", sm::GuidToString(currentId).c_str());
    }
    void* found = nullptr;
    if (SUCCEEDED(hr) && !sm::IsNullGuid(currentId)) {
        hr = VtableSlot<FindDesktopFn>(manager, kSlotFindDesktop)(manager, &currentId, &found);
        sm::Trace(L"внутренний путь: FindDesktop hr=%s ptr=%p", sm::HrText(hr).c_str(), found);
    }
    GUID foundId{};
    if (SUCCEEDED(hr) && found != nullptr) {
        hr = VtableSlot<GetDesktopIdFn>(found, kSlotDesktopGetId)(found, &foundId);
        sm::Trace(L"внутренний путь: GUID найденного %s", sm::GuidToString(foundId).c_str());
    }
    ReleaseObject(found);
    ReleaseObject(current);

    const bool ok = SUCCEEDED(hr) && count > 0 && count <= 64 && !sm::IsNullGuid(currentId) &&
                    !sm::IsNullGuid(foundId) && IsEqualGUID(currentId, foundId) != FALSE;
    if (!ok) {
        g_internal.reason = L"самопроверка внутреннего интерфейса не прошла (другая сборка Windows?)";
        ReleaseObject(views);
        ReleaseObject(manager);
        ReleaseObject(provider);
        return false;
    }

    g_internal.provider = provider;
    g_internal.manager = manager;
    g_internal.views = views;
    g_internal.usable = true;
    sm::Trace(L"перенос окон: внутренний интерфейс оболочки доступен (десктопов %d)", count);
    return true;
}

// Перенос окна любого процесса через IApplicationView. result — HRESULT последнего
// шага; E_NOTIMPL означает «внутренний путь недоступен».
bool MoveWindowToDesktopInternal(HWND hwnd, const GUID& desktop, HRESULT* result) {
    if (!EnsureInternalApi()) {
        if (result != nullptr) {
            *result = E_NOTIMPL;
        }
        return false;
    }

    void* view = nullptr;
    sm::Trace(L"внутренний путь: GetViewForHwnd(%p)…", static_cast<void*>(hwnd));
    HRESULT hr = VtableSlot<GetViewForHwndFn>(g_internal.views, kSlotGetViewForHwnd)(
        g_internal.views, hwnd, &view);
    if (FAILED(hr) || view == nullptr) {
        if (result != nullptr) {
            *result = FAILED(hr) ? hr : E_FAIL;
        }
        sm::Trace(L"GetViewForHwnd(%p): %s", static_cast<void*>(hwnd), sm::HrText(hr).c_str());
        return false;
    }

    void* target = nullptr;
    GUID id = desktop;
    sm::Trace(L"внутренний путь: FindDesktop(%s)…", sm::GuidToString(desktop).c_str());
    hr = VtableSlot<FindDesktopFn>(g_internal.manager, kSlotFindDesktop)(g_internal.manager, &id,
                                                                        &target);
    if (FAILED(hr) || target == nullptr) {
        if (result != nullptr) {
            *result = FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        sm::Trace(L"FindDesktop(%s): %s", sm::GuidToString(desktop).c_str(),
                  sm::HrText(hr).c_str());
        ReleaseObject(view);
        return false;
    }

    sm::Trace(L"внутренний путь: MoveViewToDesktop(view %p, desktop %p)…", view, target);
    hr = VtableSlot<MoveViewToDesktopFn>(g_internal.manager, kSlotMoveViewToDesktop)(
        g_internal.manager, view, target);
    sm::Trace(L"внутренний путь: MoveViewToDesktop hr=%s", sm::HrText(hr).c_str());
    ReleaseObject(target);
    ReleaseObject(view);

    if (result != nullptr) {
        *result = hr;
    }
    if (FAILED(hr)) {
        sm::Trace(L"MoveViewToDesktop(%p, %s): %s", static_cast<void*>(hwnd),
                  sm::GuidToString(desktop).c_str(), sm::HrText(hr).c_str());
        return false;
    }
    return true;
}

struct FallbackCollector {
    const sm::VirtualDesktopManager* vdm = nullptr;
    std::vector<GUID>* guids = nullptr;
};

// Резервный сбор списка: перебираем окна и запоминаем GUID десктопов в порядке появления.
BOOL CALLBACK CollectDesktopIdCallback(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<FallbackCollector*>(param);
    GUID guid{};
    if (!ctx->vdm->GetWindowDesktopId(hwnd, &guid, false) || sm::IsNullGuid(guid)) {
        return TRUE;
    }
    for (const GUID& known : *ctx->guids) {
        if (IsEqualGUID(known, guid)) {
            return TRUE;
        }
    }
    ctx->guids->push_back(guid);
    return TRUE;
}

// «Голосование» за текущий десктоп: сколько окон на нём живёт.
struct DesktopVote {
    GUID guid{};
    int count = 0;
};

struct VoteContext {
    const sm::VirtualDesktopManager* vdm = nullptr;
    // Текущий список десктопов. nullptr — не фильтровать (вызывающий его не знает).
    const std::vector<sm::DesktopInfo>* known = nullptr;
    std::vector<DesktopVote>* votes = nullptr;
    int visible = 0;       // видимых окон верхнего уровня
    int withDesktop = 0;   // из них с известным десктопом
    int ignored = 0;       // из них с GUID, которого нет в списке (маркер pinned-окон)
    int onCurrent = 0;     // из оставшихся «на текущем десктопе»
};

// DWM помечает cloaked окна, которые не показываются на активном десктопе
// (окна других виртуальных десктопов). Для нас это самый дешёвый фильтр.
bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (FAILED(hr)) {
        return false;  // DWM не ответил — окно считаем обычным
    }
    return cloaked != 0;
}

BOOL CALLBACK VoteCallback(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<VoteContext*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    if (IsCloaked(hwnd)) {
        return TRUE;
    }
    ++ctx->visible;

    GUID guid{};
    if (!ctx->vdm->GetWindowDesktopId(hwnd, &guid, false)) {
        return TRUE;
    }
    ++ctx->withDesktop;

    // Pinned-приложения («показывать на всех рабочих столах») отдают GUID, которого
    // нет в VirtualDesktopIDs: это не десктоп, а маркер «на всех сразу». Такой голос
    // не учитываем, иначе на десктопе с малым числом окон он перевесит настоящий.
    if (ctx->known != nullptr && sm::FindDesktopIndex(*ctx->known, guid) < 0) {
        ++ctx->ignored;
        return TRUE;
    }

    BOOL onCurrent = FALSE;
    if (!ctx->vdm->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent) || onCurrent == FALSE) {
        return TRUE;
    }
    ++ctx->onCurrent;

    for (DesktopVote& vote : *ctx->votes) {
        if (IsEqualGUID(vote.guid, guid) != FALSE) {
            ++vote.count;
            return TRUE;
        }
    }
    DesktopVote vote;
    vote.guid = guid;
    vote.count = 1;
    ctx->votes->push_back(vote);
    return TRUE;
}

// Голосование по окнам: побеждает десктоп, на котором больше всего видимых
// и не «затемнённых» DWM окон, которые диспетчер считает текущими.
// При равенстве побеждает первое встреченное окно, а EnumWindows идёт сверху
// вниз по z-order — то есть самое верхнее окно на экране.
bool VoteCurrentDesktop(const sm::VirtualDesktopManager& vdm,
                        const std::vector<sm::DesktopInfo>* known, GUID* out, std::wstring* note,
                        bool traceCounters) {
    std::vector<DesktopVote> votes;
    VoteContext ctx;
    ctx.vdm = &vdm;
    ctx.known = known;
    ctx.votes = &votes;
    EnumWindows(VoteCallback, reinterpret_cast<LPARAM>(&ctx));

    const DesktopVote* best = nullptr;
    for (const DesktopVote& vote : votes) {
        if (best == nullptr || vote.count > best->count) {
            best = &vote;
        }
    }

    if (traceCounters) {
        sm::Trace(L"голосование по окнам: видимых %d, с десктопом %d (не из списка %d), "
                  L"на текущем %d, кандидатов %d",
                  ctx.visible, ctx.withDesktop, ctx.ignored, ctx.onCurrent,
                  static_cast<int>(votes.size()));
    }

    if (best == nullptr || best->count <= 0) {
        return false;
    }
    *out = best->guid;
    if (note != nullptr) {
        wchar_t buf[96];
        swprintf_s(buf, L"голосование по окнам (%d из %d)", best->count, ctx.onCurrent);
        *note = buf;
    }
    return true;
}

bool TryDesktopOfWindow(const sm::VirtualDesktopManager& vdm, HWND hwnd, GUID* out) {
    return hwnd != nullptr && vdm.GetWindowDesktopId(hwnd, out, false);
}

// Окна оболочки всегда на активном десктопе: рабочий стол (Progman) и панели задач.
bool TryDesktopOfShellWindows(const sm::VirtualDesktopManager& vdm, GUID* out) {
    HWND candidates[8];
    size_t count = 0;
    candidates[count++] = GetShellWindow();
    candidates[count++] = FindWindowW(L"Shell_TrayWnd", nullptr);

    HWND secondary = nullptr;
    while (count < _countof(candidates)) {
        secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr);
        if (secondary == nullptr) {
            break;
        }
        candidates[count++] = secondary;
    }

    for (size_t i = 0; i < count; ++i) {
        if (TryDesktopOfWindow(vdm, candidates[i], out)) {
            return true;
        }
    }
    return false;
}

bool AcceptCurrent(sm::CurrentDesktopInfo* out, const GUID& guid, const std::wstring& source) {
    out->guid = guid;
    out->known = true;
    out->source = source;
    return true;
}

}  // namespace

namespace sm {

bool InitCom(std::wstring* error) {
    if (g_comReady) {
        return true;
    }
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        if (error != nullptr) {
            *error = L"CoInitializeEx: " + HrText(hr);
        }
        return false;
    }
    g_comReady = true;
    Trace(L"COM: CoInitializeEx(STA) выполнен");
    return true;
}

void UninitCom() {
    if (!g_comReady) {
        return;
    }
    // Внутренние объекты оболочки держат ссылки на COM — освобождаем до CoUninitialize.
    ResetInternalApi();
    CoUninitialize();
    g_comReady = false;
}

VirtualDesktopManager::~VirtualDesktopManager() {
    Reset();
}

void VirtualDesktopManager::Reset() {
    if (mgr_ != nullptr) {
        mgr_->Release();
        mgr_ = nullptr;
    }
}

bool VirtualDesktopManager::Init(std::wstring* error) {
    Reset();
    if (!g_comReady) {
        if (error != nullptr) {
            *error = L"COM не инициализирован (нет InitCom)";
        }
        return false;
    }

    IVirtualDesktopManager* mgr = nullptr;
    const HRESULT hr = CoCreateInstance(kClsidVirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(IVirtualDesktopManager),
                                        reinterpret_cast<void**>(&mgr));
    if (FAILED(hr) || mgr == nullptr) {
        if (error != nullptr) {
            *error = L"CoCreateInstance(IVirtualDesktopManager): " + HrText(hr);
        }
        return false;
    }

    mgr_ = mgr;
    Trace(L"IVirtualDesktopManager: создан");
    return true;
}

bool VirtualDesktopManager::GetWindowDesktopId(HWND hwnd, GUID* out, bool traceFailures) const {
    if (mgr_ == nullptr || hwnd == nullptr || out == nullptr) {
        return false;
    }
    GUID guid{};
    const HRESULT hr = mgr_->GetWindowDesktopId(hwnd, &guid);
    if (FAILED(hr) || IsNullGuid(guid)) {
        if (traceFailures) {
            Trace(L"GetWindowDesktopId(%p): %s", static_cast<void*>(hwnd), HrText(hr).c_str());
        }
        return false;
    }
    *out = guid;
    return true;
}

bool VirtualDesktopManager::IsWindowOnCurrentVirtualDesktop(HWND hwnd, BOOL* out) const {
    if (mgr_ == nullptr || hwnd == nullptr || out == nullptr) {
        return false;
    }
    BOOL onCurrent = FALSE;
    const HRESULT hr = mgr_->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent);
    if (FAILED(hr)) {
        Trace(L"IsWindowOnCurrentVirtualDesktop(%p): %s", static_cast<void*>(hwnd),
              HrText(hr).c_str());
        return false;
    }
    *out = onCurrent;
    return true;
}

bool VirtualDesktopManager::MoveWindowToDesktop(HWND hwnd, const GUID& desktop,
                                                HRESULT* result) const {
    if (hwnd == nullptr) {
        if (result != nullptr) {
            *result = E_INVALIDARG;
        }
        return false;
    }

    // 1. Внутренний путь: работает для окон любого процесса (PLAN.md §8.2 п.3–4).
    HRESULT internal = E_NOTIMPL;
    if (MoveWindowToDesktopInternal(hwnd, desktop, &internal)) {
        if (result != nullptr) {
            *result = S_OK;
        }
        return true;
    }

    // 2. Документированный API: переносит только окна собственного процесса, но пусть
    //    будет — вдруг внутренний интерфейс недоступен, а окно наше.
    if (mgr_ != nullptr) {
        const HRESULT hr = mgr_->MoveWindowToDesktop(hwnd, desktop);
        if (SUCCEEDED(hr)) {
            if (result != nullptr) {
                *result = hr;
            }
            return true;
        }
        if (result != nullptr) {
            *result = hr;
        }
        Trace(L"MoveWindowToDesktop(%p, %s): %s", static_cast<void*>(hwnd),
              GuidToString(desktop).c_str(), HrText(hr).c_str());
        return false;
    }

    if (result != nullptr) {
        *result = internal;
    }
    if (internal == E_NOTIMPL) {
        Trace(L"перенос окон недоступен: %s", g_internal.reason.c_str());
    }
    return false;
}

bool DetectCurrentDesktop(const VirtualDesktopManager& vdm, CurrentDesktopInfo* out,
                          const std::vector<DesktopInfo>* knownDesktops) {
    if (out == nullptr) {
        return false;
    }
    *out = CurrentDesktopInfo{};
    out->source = L"не определён";

    // В --verbose прогоняем все способы, чтобы в логе было видно, что дал каждый:
    // на чужой машине это единственный способ понять, почему десктоп не найден.
    const bool verbose = IsVerbose();

    bool focusOk = false;
    bool consoleOk = false;
    bool voteOk = false;
    bool shellOk = false;
    bool registryOk = false;
    GUID focusGuid{};
    GUID consoleGuid{};
    GUID voteGuid{};
    GUID shellGuid{};
    GUID registryGuid{};
    std::wstring voteNote;

    if (vdm.ok()) {
        // 1. Окно в фокусе: активное окно по определению на активном десктопе.
        //    Осторожно с терминалами: под ConEmu фокус достаётся служебному окну
        //    VirtualConsoleClass, у которого может не быть записи в диспетчере
        //    десктопов (0x8002802B, TYPE_E_ELEMENTNOTFOUND), поэтому это только
        //    первая попытка, а не единственная.
        HWND foreground = GetForegroundWindow();
        if (verbose) {
            wchar_t cls[128] = L"?";
            if (foreground != nullptr) {
                GetClassNameW(foreground, cls, _countof(cls));
            }
            Trace(L"кандидат «окно в фокусе»: %p класс \"%s\"", static_cast<void*>(foreground), cls);
        }
        focusOk = TryDesktopOfWindow(vdm, GetAncestor(foreground, GA_ROOTOWNER), &focusGuid) ||
                  TryDesktopOfWindow(vdm, GetAncestor(foreground, GA_ROOT), &focusGuid) ||
                  TryDesktopOfWindow(vdm, foreground, &focusGuid);

        // 2. Консольное окно: в conhost (обычный cmd, Windows Terminal) тоже на
        //    активном десктопе.
        if (!focusOk || verbose) {
            consoleOk = TryDesktopOfWindow(vdm, GetConsoleWindow(), &consoleGuid);
        }

        // 3. Голосование по окнам — не зависит от того, чьё окно в фокусе.
        if (!focusOk || verbose) {
            voteOk = VoteCurrentDesktop(vdm, knownDesktops, &voteGuid, &voteNote, verbose);
        }

        // 4. Окна оболочки (рабочий стол, панели задач) — на активном десктопе.
        if (!focusOk && !consoleOk && !voteOk) {
            shellOk = TryDesktopOfShellWindows(vdm, &shellGuid);
        }
    }

    // 5. Реестр: на Win10 22H2 значения CurrentVirtualDesktop нет, но на других
    //    сборках оно встречается — пусть будет последней попыткой.
    if (!focusOk && !consoleOk && !voteOk && !shellOk) {
        registryOk = ReadCurrentDesktopFromRegistry(&registryGuid) && !IsNullGuid(registryGuid);
    }

    if (verbose) {
        auto text = [](bool ok, const GUID& guid) {
            return ok ? GuidToString(guid) : std::wstring(L"—");
        };
        Trace(L"диагностика десктопа: фокус=%s, консоль=%s, голосование=%s, оболочка=%s, реестр=%s",
              text(focusOk, focusGuid).c_str(), text(consoleOk, consoleGuid).c_str(),
              text(voteOk, voteGuid).c_str(), text(shellOk, shellGuid).c_str(),
              text(registryOk, registryGuid).c_str());
    }

    // Приоритет: фокус (самый прямой признак) → консоль → голосование по окнам
    // → окна оболочки → реестр.
    if (focusOk) {
        return AcceptCurrent(out, focusGuid, L"окно в фокусе");
    }
    if (consoleOk) {
        return AcceptCurrent(out, consoleGuid, L"консольное окно");
    }
    if (voteOk) {
        return AcceptCurrent(out, voteGuid, voteNote);
    }
    if (shellOk) {
        return AcceptCurrent(out, shellGuid, L"окно оболочки");
    }
    if (registryOk) {
        return AcceptCurrent(out, registryGuid, L"реестр (CurrentVirtualDesktop)");
    }

    Trace(L"текущий десктоп: ни один способ не сработал");
    return false;
}

bool CollectDesktopsFallback(const VirtualDesktopManager& vdm, std::vector<DesktopInfo>* out,
                             std::wstring* error) {
    if (out == nullptr) {
        return false;
    }
    out->clear();

    if (!vdm.ok()) {
        if (error != nullptr) {
            *error = L"IVirtualDesktopManager недоступен";
        }
        return false;
    }

    std::vector<GUID> guids;
    FallbackCollector ctx;
    ctx.vdm = &vdm;
    ctx.guids = &guids;
    EnumWindows(CollectDesktopIdCallback, reinterpret_cast<LPARAM>(&ctx));

    // Десктоп без окон тоже должен попасть в список — добавляем активный.
    CurrentDesktopInfo current;
    if (DetectCurrentDesktop(vdm, &current) && current.known) {
        bool present = false;
        for (const GUID& known : guids) {
            if (IsEqualGUID(known, current.guid)) {
                present = true;
                break;
            }
        }
        if (!present) {
            guids.push_back(current.guid);
        }
    }

    if (guids.empty()) {
        if (error != nullptr) {
            *error = L"ни одного десктопа определить не удалось (нет ни одного окна)";
        }
        return false;
    }

    out->reserve(guids.size());
    for (size_t i = 0; i < guids.size(); ++i) {
        DesktopInfo info;
        info.guid = guids[i];
        info.index = static_cast<int>(i);
        info.name = ReadDesktopName(info.guid);
        Trace(L"десктоп (резервный список) [%d] %s \"%s\"", info.index,
              GuidToString(info.guid).c_str(), info.name.c_str());
        out->push_back(info);
    }

    Warn(L"десктопов найдено резервным способом: %d (порядок может отличаться от Task View)",
         static_cast<int>(out->size()));
    return true;
}

bool EnsureInternalApiUsable() { return EnsureInternalApi(); }

std::wstring InternalApiReason() {
    EnsureInternalApi();
    if (g_internal.usable) {
        return std::wstring();
    }
    return g_internal.reason.empty() ? std::wstring(L"причина неизвестна") : g_internal.reason;
}

}  // namespace sm
