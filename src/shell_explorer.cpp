#include "shell_explorer.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <map>

#include "log.h"

namespace {

// PIDL -> строка. Сначала файловый путь (годится для explorer.exe), затем
// «parsing name» для виртуальных папок — с префиксом shell:.
bool NameFromPidl(PCIDLIST_ABSOLUTE pidl, std::wstring* out) {
    PWSTR text = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &text)) && text != nullptr) {
        *out = text;
        CoTaskMemFree(text);
        return true;
    }
    text = nullptr;
    if (FAILED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &text)) ||
        text == nullptr) {
        return false;
    }
    *out = L"shell:" + std::wstring(text);
    CoTaskMemFree(text);
    return true;
}

// Активное представление браузера -> его папка -> PIDL -> строка.
//
// IFolderView спрашиваем у самого представления: GetItemObject(SVGIO_BACKGROUND)
// на Win10 DefView отвечает E_NOINTERFACE (0x80004002).
bool FolderFromBrowser(IShellBrowser* browser, std::wstring* folder) {
    IShellView* view = nullptr;
    HRESULT hr = browser->QueryActiveShellView(&view);
    if (FAILED(hr) || view == nullptr) {
        sm::Trace(L"Проводник: QueryActiveShellView: %s", sm::HrText(hr).c_str());
        return false;
    }

    bool ok = false;
    IFolderView2* folderView = nullptr;
    hr = view->QueryInterface(IID_PPV_ARGS(&folderView));
    if (SUCCEEDED(hr) && folderView != nullptr) {
        IPersistFolder2* persist = nullptr;
        hr = folderView->GetFolder(IID_PPV_ARGS(&persist));
        if (SUCCEEDED(hr) && persist != nullptr) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            hr = persist->GetCurFolder(&pidl);
            if (SUCCEEDED(hr) && pidl != nullptr) {
                ok = NameFromPidl(pidl, folder);
                CoTaskMemFree(pidl);
            } else {
                sm::Trace(L"Проводник: GetCurFolder: %s", sm::HrText(hr).c_str());
            }
            persist->Release();
        } else {
            sm::Trace(L"Проводник: IFolderView::GetFolder: %s", sm::HrText(hr).c_str());
        }
        folderView->Release();
    } else {
        sm::Trace(L"Проводник: QueryInterface(IFolderView2): %s", sm::HrText(hr).c_str());
    }
    view->Release();
    return ok;
}


// HWND окна в элементе коллекции: сначала автоматизационный IWebBrowserApp::HWND
// (ровно то, что отдаёт PowerShell через Shell.Application.Windows().HWND),
// запасной — IShellBrowser::GetWindow (иногда это окно представления, не рамочное).
HWND ElementWindow(IDispatch* item, IShellBrowser* const browsers[2]) {
    HWND window = nullptr;
    IWebBrowserApp* webApp = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&webApp))) && webApp != nullptr) {
        SHANDLE_PTR value = 0;
        if (SUCCEEDED(webApp->get_HWND(&value))) {
            window = reinterpret_cast<HWND>(value);
        }
        webApp->Release();
    }
    for (int i = 0; i < 2 && window == nullptr; ++i) {
        if (browsers[i] != nullptr) {
            browsers[i]->GetWindow(&window);
        }
    }
    return window;
}

// HWND -> папка для окон активного стола. Заполняется одним проходом по
// IShellWindows: иначе на каждое окно приходилось бы по 15 RPC-вызовов к explorer.
std::map<HWND, std::wstring>& FolderCache() {
    static std::map<HWND, std::wstring> cache;
    return cache;
}

bool& FolderCacheBuilt() {
    static bool built = false;
    return built;
}

void BuildFolderCache() {
    if (FolderCacheBuilt()) {
        return;
    }
    FolderCacheBuilt() = true;

    IShellWindows* shellWindows = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr) || shellWindows == nullptr) {
        sm::Trace(L"Проводник: CoCreateInstance(ShellWindows): %s", sm::HrText(hr).c_str());
        return;
    }

    long count = 0;
    shellWindows->get_Count(&count);
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        IDispatch* item = nullptr;
        if (FAILED(shellWindows->Item(index, &item)) || item == nullptr) {
            continue;
        }

        // Два кандидата на браузер: сам элемент коллекции и его TopLevelBrowser из
        // IServiceProvider. На Win10 22H2 это разные объекты, и активное
        // представление отдаёт только один из них — пробуем оба.
        IShellBrowser* browsers[2] = {nullptr, nullptr};
        if (FAILED(item->QueryInterface(IID_PPV_ARGS(&browsers[0])))) {
            browsers[0] = nullptr;
        }
        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&provider))) && provider != nullptr) {
            provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browsers[1]));
            provider->Release();
        }

        const HWND window = ElementWindow(item, browsers);
        if (window != nullptr) {
            std::wstring folder;
            for (IShellBrowser* candidate : browsers) {
                if (candidate != nullptr && FolderFromBrowser(candidate, &folder)) {
                    break;
                }
            }
            if (!folder.empty()) {
                FolderCache()[window] = folder;
                const HWND root = GetAncestor(window, GA_ROOT);
                if (root != nullptr && root != window) {
                    FolderCache()[root] = folder;
                }
            }
        }

        for (IShellBrowser* candidate : browsers) {
            if (candidate != nullptr) {
                candidate->Release();
            }
        }
        item->Release();
    }
    shellWindows->Release();

    sm::Trace(L"Проводник: IShellWindows — окон %ld, папки прочитаны у %d", count,
              static_cast<int>(FolderCache().size()));
}

bool FolderByCom(HWND hwnd, std::wstring* folder) {
    BuildFolderCache();
    const auto it = FolderCache().find(hwnd);
    if (it == FolderCache().end()) {
        return false;
    }
    *folder = it->second;
    return true;
}

// Запасной путь для окон на неактивных виртуальных столах: IShellWindows
// перечисляет только окна активного стола (проверено на Win10 22H2 — 15 элементов
// при 34 открытых окнах Проводника). Заголовок окна Проводника — это путь его
// папки, поэтому принимаем его, но только если по нему есть существующий каталог:
// у виртуальных папок («Быстрый доступ») и у папок библиотек заголовок — просто имя.
std::wstring FolderFromTitle(const std::wstring& title) {
    if (title.size() < 3) {  // «C:\» — минимальный путь
        return std::wstring();
    }
    if (title.find(L':') == std::wstring::npos && title.find(L'\\') == std::wstring::npos) {
        return std::wstring();
    }
    const DWORD attrs = GetFileAttributesW(title.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::wstring();
    }
    return title;
}

}  // namespace

namespace sm {

bool IsExplorerWindowClass(const std::wstring& cls) {
    return _wcsicmp(cls.c_str(), L"CabinetWClass") == 0 ||
           _wcsicmp(cls.c_str(), L"ExploreWClass") == 0;
}

bool GetExplorerFolder(HWND hwnd, const std::wstring& title, std::wstring* folder) {
    if (folder == nullptr || hwnd == nullptr) {
        return false;
    }
    folder->clear();

    if (FolderByCom(hwnd, folder)) {
        return true;
    }

    *folder = FolderFromTitle(title);
    if (folder->empty()) {
        return false;
    }
    Trace(L"Проводник: окна %p нет в IShellWindows (неактивный стол), папка по заголовку: %s",
          static_cast<void*>(hwnd), folder->c_str());
    return true;
}

}  // namespace sm

