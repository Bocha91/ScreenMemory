#include "storage.h"

#include <algorithm>
#include <utility>

#include "log.h"
#include "registry_desktops.h"

namespace sm {

namespace {

typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

// shcore.dll грузим динамически: нужен только GetDpiForMonitor, тянуть линковку
// ради него не хочется. Если экспорта нет — DPI считаем 96.
GetDpiForMonitorFn DpiForMonitorFn() {
    static GetDpiForMonitorFn fn = nullptr;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore != nullptr) {
            fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
        }
    }
    return fn;
}

BOOL CALLBACK MonitorProc(HMONITOR monitor, HDC /*dc*/, LPRECT /*rect*/, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<LayoutMonitor>*>(param);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return TRUE;
    }
    LayoutMonitor item;
    item.device = mi.szDevice;
    item.rect = mi.rcMonitor;
    item.work = mi.rcWork;
    item.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 ? 1 : 0;
    item.dpi = 96;
    if (GetDpiForMonitorFn fn = DpiForMonitorFn()) {
        UINT dpiX = 0;
        UINT dpiY = 0;
        // MDT_EFFECTIVE_DPI = 0: то, что пользователь выбрал в параметрах экрана.
        if (SUCCEEDED(fn(monitor, 0, &dpiX, &dpiY)) && dpiX > 0) {
            item.dpi = dpiX;
        }
    }
    out->push_back(std::move(item));
    return TRUE;
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ParseInt(const std::wstring& text, int* out) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const long long value = wcstoll(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != L'\0') {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

// Всё от первого неэкранированного «;» до конца строки — комментарий (PLAN.md §6).
// Экранированный «\;» остаётся частью значения.
std::wstring StripComment(const std::wstring& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const wchar_t c = value[i];
        if (c == L'\\') {
            ++i;  // экранированный символ пропускаем целиком
            continue;
        }
        if (c == L';') {
            return value.substr(0, i);
        }
    }
    return value;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& text, std::wstring* error) {
    const int textLength = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), textLength, nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) {
        if (error != nullptr) {
            *error = L"не удалось закодировать слепок в UTF-8";
        }
        return false;
    }
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), textLength, &utf8[0], bytes, nullptr, nullptr);

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = Win32ErrorText(GetLastError()) + L": " + path;
        }
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    const DWORD writeError = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (!ok || written != utf8.size()) {
        if (error != nullptr) {
            *error = Win32ErrorText(writeError) + L": " + path;
        }
        return false;
    }
    return true;
}

std::wstring FileNameOfPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool ReadUtf8File(const std::wstring& path, std::wstring* out, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = Win32ErrorText(GetLastError()) + L": " + path;
        }
        return false;
    }
    std::string utf8;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        utf8.append(buffer, read);
    }
    CloseHandle(file);

    int skip = 0;
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB &&
        static_cast<unsigned char>(utf8[2]) == 0xBF) {
        skip = 3;  // BOM при чтении допускаем, хотя сами пишем без него
    }
    const int size = static_cast<int>(utf8.size()) - skip;
    if (size <= 0) {
        out->clear();
        return true;
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.data() + skip, size, nullptr, 0);
    if (chars <= 0) {
        if (error != nullptr) {
            *error = L"файл не похож на UTF-8: " + path;
        }
        return false;
    }
    out->assign(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data() + skip, size, &(*out)[0], chars);
    return true;
}

// Метка из заголовка «# saved=2026-11-12T21:15:03» как есть — чтобы сравнивать слепки
// между собой (так restore понимает, что перед ним тот же самый снимок). false — заголовка
// нет или он битый.
bool HeaderSavedOf(const std::wstring& path, std::wstring* stamp) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    char head[512] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, head, sizeof(head) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    const std::string text(head, read);
    const size_t pos = text.find("# saved=");
    if (pos == std::string::npos || pos + 8 + 19 > text.size()) {
        return false;
    }
    const std::string raw = text.substr(pos + 8, 19);
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        bool separator = false;
        if (i == 4 || i == 7) {
            separator = c == '-';
        } else if (i == 10) {
            separator = c == 'T';
        } else if (i == 13 || i == 16) {
            separator = c == ':';
        }
        if (!separator && (c < '0' || c > '9')) {
            return false;
        }
    }
    stamp->assign(raw.begin(), raw.end());
    return true;
}

// Метка времени из заголовка «# saved=2026-11-12T21:15:03» — для имени файла
// ротации. Если заголовка нет или он битый — берём время изменения файла.
std::wstring SavedStampOf(const std::wstring& path) {
    std::wstring stamp;
    if (HeaderSavedOf(path, &stamp)) {
        std::replace(stamp.begin(), stamp.end(), L'T', L'_');
        std::replace(stamp.begin(), stamp.end(), L':', L'-');
        return stamp;
    }

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return FileTimestamp(data.ftLastWriteTime);
    }
    return L"unknown";
}

// layout-<время>.txt; если эта секунда занята (ручные копии) — layout-<время>-1.txt и т.д.
std::wstring RotatedPathFor(const std::wstring& dir, const std::wstring& stamp) {
    const std::wstring base = L"layout-" + stamp;
    std::wstring path = JoinPath(dir, base + L".txt");
    for (int i = 1; i < 100 && FileExists(path); ++i) {
        path = JoinPath(dir, base + L"-" + std::to_wstring(i) + L".txt");
    }
    return path;
}

// «layout-YYYY-MM-DD_HH-MM-SS.txt» (и «…-N.txt», если секунда была занята) — рабочая
// версия. Всё остальное из layout-*.txt — защищённые копии («вчерашний день», «прежняя
// конфигурация»): чистка их не трогает, иначе смысл защиты пропадает (PLAN.md §6.1).
bool IsVersionName(const std::wstring& name) {
    const std::wstring prefix = L"layout-";
    const std::wstring suffix = L".txt";
    if (name.size() < prefix.size() + 19 + suffix.size() ||
        name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    const std::wstring body =
        name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    for (size_t i = 0; i < 19; ++i) {
        const wchar_t c = body[i];
        bool separator = false;
        if (i == 4 || i == 7) {
            separator = c == L'-';
        } else if (i == 10) {
            separator = c == L'_';
        } else if (i == 13 || i == 16) {
            separator = c == L'-';
        }
        if (!separator && (c < L'0' || c > L'9')) {
            return false;
        }
    }
    if (body.size() == 19) {
        return true;
    }
    if (body[19] != L'-' || body.size() == 20) {
        return false;
    }
    for (size_t i = 20; i < body.size(); ++i) {
        if (body[i] < L'0' || body[i] > L'9') {
            return false;
        }
    }
    return true;
}

// Оставляем keep самых новых рабочих версий layout-<время>.txt, остальные удаляем
// (PLAN.md §6.1 п.4). Защищённые копии не считаются и не удаляются никогда.
int PruneOldVersions(const std::wstring& dir, int keep) {
    struct Entry {
        std::wstring path;
        FILETIME time{};
    };
    std::vector<Entry> entries;

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = JoinPath(dir, L"layout-*.txt");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!IsVersionName(data.cFileName)) {
            continue;  // защищённая копия: не наша, не считаем и не удаляем
        }
        Entry entry;
        entry.path = JoinPath(dir, data.cFileName);
        entry.time = data.ftLastWriteTime;
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return CompareFileTime(&a.time, &b.time) > 0;  // новые первыми
    });

    int pruned = 0;
    for (size_t i = static_cast<size_t>(keep); i < entries.size(); ++i) {
        if (DeleteFileW(entries[i].path.c_str())) {
            ++pruned;
            Trace(L"ротация: удалён %s", entries[i].path.c_str());
        } else {
            Warn(L"ротация: не удалось удалить %s (%s)", entries[i].path.c_str(),
                 Win32ErrorText(GetLastError()).c_str());
        }
    }
    return pruned;
}

}  // namespace

std::wstring NowTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t text[32] = {};
    swprintf_s(text, L"%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return text;
}

std::wstring FileTimestamp(const SYSTEMTIME& st) {
    wchar_t text[32] = {};
    swprintf_s(text, L"%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return text;
}

std::wstring FileTimestamp(const FILETIME& ft) {
    FILETIME local = {};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    return FileTimestamp(st);
}

bool CollectMonitors(std::vector<LayoutMonitor>* out) {
    out->clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(out));
    return !out->empty();
}

std::wstring EscapeValue(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (const wchar_t c : value) {
        switch (c) {
            case L'\\':
                escaped += L"\\\\";
                break;
            case L'\n':
                escaped += L"\\n";
                break;
            case L'\r':
                escaped += L"\\r";
                break;
            case L';':
                escaped += L"\\;";
                break;
            default:
                escaped += c;
                break;
        }
    }
    return escaped;
}

std::wstring UnescapeValue(const std::wstring& value) {
    std::wstring plain;
    plain.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != L'\\' || i + 1 >= value.size()) {
            plain += value[i];
            continue;
        }
        const wchar_t next = value[++i];
        switch (next) {
            case L'n':
                plain += L'\n';
                break;
            case L'r':
                plain += L'\r';
                break;
            default:
                plain += next;  // «\\», «\;» и любые другие экранированные символы
                break;
        }
    }
    return plain;
}

std::wstring RectToText(const RECT& rect) {
    wchar_t text[64] = {};
    swprintf_s(text, L"%ld,%ld,%ld,%ld", rect.left, rect.top, rect.right, rect.bottom);
    return text;
}

bool ParseRect(const std::wstring& text, RECT* out) {
    long long left = 0;
    long long top = 0;
    long long right = 0;
    long long bottom = 0;
    if (swscanf_s(text.c_str(), L"%lld,%lld,%lld,%lld", &left, &top, &right, &bottom) != 4) {
        return false;
    }
    out->left = static_cast<LONG>(left);
    out->top = static_cast<LONG>(top);
    out->right = static_cast<LONG>(right);
    out->bottom = static_cast<LONG>(bottom);
    return true;
}

namespace {

void AppendLine(std::wstring* text, const std::wstring& line) {
    text->append(line);
    text->append(L"\r\n");
}

void AppendKey(std::wstring* text, const wchar_t* key, const std::wstring& value) {
    text->append(key);
    text->append(L"=");
    text->append(EscapeValue(value));
    text->append(L"\r\n");
}

void AppendInt(std::wstring* text, const wchar_t* key, int value) {
    text->append(key);
    text->append(L"=");
    text->append(std::to_wstring(value));
    text->append(L"\r\n");
}

void AppendRect(std::wstring* text, const wchar_t* key, const RECT& rect) {
    AppendKey(text, key, RectToText(rect));
}

std::wstring CountText(size_t value) {
    return std::to_wstring(value);
}

}  // namespace

std::wstring SerializeLayout(const Layout& layout) {
    std::wstring text;
    text.reserve(1024 + layout.windows.size() * 320);

    AppendLine(&text, L"# ScreenMemory layout v1");
    AppendLine(&text, L"# saved=" + layout.saved + L"  screen=" + std::to_wstring(layout.screenW) +
                          L"x" + std::to_wstring(layout.screenH) +
                          L"  monitors=" + CountText(layout.monitors.size()) +
                          L"  desktops=" + CountText(layout.desktops.size()) +
                          L"  windows=" + CountText(layout.windows.size()));

    for (const LayoutDesktop& desktop : layout.desktops) {
        AppendLine(&text, L"[desktop." + std::to_wstring(desktop.index) + L"]");
        AppendKey(&text, L"guid", GuidToString(desktop.guid));
        AppendKey(&text, L"name", desktop.name);
    }

    text.append(L"\r\n");
    for (size_t i = 0; i < layout.monitors.size(); ++i) {
        const LayoutMonitor& monitor = layout.monitors[i];
        AppendLine(&text, L"[monitor." + CountText(i) + L"]");
        AppendKey(&text, L"device", monitor.device);
        AppendRect(&text, L"rect", monitor.rect);
        AppendRect(&text, L"work", monitor.work);
        AppendInt(&text, L"dpi", static_cast<int>(monitor.dpi));
        AppendInt(&text, L"primary", monitor.primary);
    }

    text.append(L"\r\n");
    for (size_t i = 0; i < layout.windows.size(); ++i) {
        const LayoutWindow& window = layout.windows[i];
        AppendLine(&text, L"[window." + CountText(i) + L"]");
        AppendInt(&text, L"desktop", window.desktop);
        AppendKey(&text, L"desktop_name", window.desktopName);
        AppendInt(&text, L"pinned", window.pinned);
        AppendInt(&text, L"zg", window.zg);
        AppendKey(&text, L"exe", window.exe);
        if (!window.cmdline.empty()) {
            AppendKey(&text, L"cmdline", window.cmdline);
        }
        if (!window.url.empty()) {
            AppendKey(&text, L"url", window.url);
        }
        AppendKey(&text, L"class", window.cls);
        if (!window.folder.empty()) {
            AppendKey(&text, L"folder", window.folder);
        }
        AppendKey(&text, L"title", window.title);
        if (!window.aumid.empty()) {
            AppendKey(&text, L"aumid", window.aumid);
        }
        AppendKey(&text, L"monitor", window.monitor);
        AppendRect(&text, L"mrel", window.mrel);
        AppendRect(&text, L"rect", window.rect);
        if (window.frameOk) {
            AppendRect(&text, L"frame", window.frame);
        }
        AppendKey(&text, L"show", window.show);
        AppendInt(&text, L"uwp", window.uwp);
        // Ключ пишется только у окон, которые были открыты от администратора (§9.3):
        // у обычных приложений его нет, и слепок остаётся прежнего вида.
        if (window.elevated != 0) {
            AppendInt(&text, L"elevated", 1);
        }
    }

    return text;
}

namespace {

// Значения в файле читаются через Trim (storage.cpp, ParseLayoutText): пробелы по краям
// не переживают запись-чтение. Сравнение обязано видеть те же значения, иначе слепок
// «вечно меняется» из-за пробела в конце cmdline и файл переписывается каждый раз.
std::wstring Trimmed(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

void AppendTrimmed(std::wstring* text, const wchar_t* key, const std::wstring& value) {
    AppendKey(text, key, Trimmed(value));
}

// Ключ окна для сравнения слепков: всё, кроме zg (см. SameLayoutContent).
std::wstring WindowContentKey(const LayoutWindow& window) {
    std::wstring text;
    AppendInt(&text, L"desktop", window.desktop);
    AppendTrimmed(&text, L"desktop_name", window.desktopName);
    AppendInt(&text, L"pinned", window.pinned);
    AppendTrimmed(&text, L"exe", window.exe);
    AppendTrimmed(&text, L"cmdline", window.cmdline);
    AppendTrimmed(&text, L"url", window.url);
    AppendTrimmed(&text, L"class", window.cls);
    AppendTrimmed(&text, L"folder", window.folder);
    AppendTrimmed(&text, L"title", window.title);
    AppendTrimmed(&text, L"aumid", window.aumid);
    AppendTrimmed(&text, L"monitor", window.monitor);
    AppendRect(&text, L"mrel", window.mrel);
    AppendRect(&text, L"rect", window.rect);
    AppendInt(&text, L"frame", window.frameOk ? 1 : 0);
    AppendRect(&text, L"frame_rect", window.frame);
    AppendTrimmed(&text, L"show", window.show);
    AppendInt(&text, L"uwp", window.uwp);
    // Права окна — часть содержимого: если приложение перестало (или стало) запускаться
    // от администратора, файл должен перезаписаться, иначе restore пойдёт по старым правам.
    AppendInt(&text, L"elevated", window.elevated != 0 ? 1 : 0);
    return text;
}

struct NamedWindowKey {
    std::wstring key;
    std::wstring title;
};

std::vector<NamedWindowKey> SortedKeys(const Layout& layout) {
    std::vector<NamedWindowKey> keys;
    keys.reserve(layout.windows.size());
    for (const LayoutWindow& window : layout.windows) {
        keys.push_back(NamedWindowKey{WindowContentKey(window), window.title});
    }
    std::sort(keys.begin(), keys.end(),
              [](const NamedWindowKey& a, const NamedWindowKey& b) { return a.key < b.key; });
    return keys;
}

bool ContainsKey(const std::vector<NamedWindowKey>& keys, const std::wstring& key) {
    const auto it = std::lower_bound(
        keys.begin(), keys.end(), key,
        [](const NamedWindowKey& item, const std::wstring& value) { return item.key < value; });
    return it != keys.end() && it->key == key;
}

std::wstring WindowLabel(const std::wstring& title) {
    return title.empty() ? std::wstring(L"(без заголовка)") : L"«" + title + L"»";
}

}  // namespace

std::wstring LayoutDiffReason(const Layout& a, const Layout& b) {
    if (a.screenW != b.screenW || a.screenH != b.screenH) {
        return L"другой размер экрана";
    }
    if (a.desktops.size() != b.desktops.size()) {
        return L"другое число десктопов: было " + std::to_wstring(a.desktops.size()) + L", стало " +
               std::to_wstring(b.desktops.size());
    }
    for (size_t i = 0; i < a.desktops.size(); ++i) {
        const LayoutDesktop& left = a.desktops[i];
        const LayoutDesktop& right = b.desktops[i];
        if (left.index != right.index) {
            return L"десктопы идут в другом порядке";
        }
        if (!IsEqualGUID(left.guid, right.guid) || left.name != right.name) {
            return L"десктоп " + std::to_wstring(left.index) + L" стал другим";
        }
    }
    if (a.monitors.size() != b.monitors.size()) {
        return L"другое число мониторов";
    }
    for (size_t i = 0; i < a.monitors.size(); ++i) {
        const LayoutMonitor& left = a.monitors[i];
        const LayoutMonitor& right = b.monitors[i];
        const std::wstring name =
            left.device.empty() ? std::wstring(L"монитор") : L"монитор " + left.device;
        if (!EqualRect(&left.rect, &right.rect)) {
            return name + L": другие границы";
        }
        if (!EqualRect(&left.work, &right.work)) {
            return name + L": другая рабочая область";
        }
        if (left.dpi != right.dpi) {
            return name + L": другой DPI";
        }
        if (left.primary != right.primary) {
            return name + L": сменился основной";
        }
        if (left.device != right.device) {
            return L"другой набор мониторов";
        }
    }
    if (a.windows.size() != b.windows.size()) {
        return L"другое число окон: было " + std::to_wstring(a.windows.size()) + L", стало " +
               std::to_wstring(b.windows.size());
    }
    const std::vector<NamedWindowKey> left = SortedKeys(a);
    const std::vector<NamedWindowKey> right = SortedKeys(b);
    for (const NamedWindowKey& item : left) {
        if (!ContainsKey(right, item.key)) {
            return L"окно " + WindowLabel(item.title) + L": в прежнем слепке было другим";
        }
    }
    for (const NamedWindowKey& item : right) {
        if (!ContainsKey(left, item.key)) {
            return L"окно " + WindowLabel(item.title) + L": новое";
        }
    }
    return std::wstring();
}

bool SameLayoutContent(const Layout& a, const Layout& b) {
    return LayoutDiffReason(a, b).empty();
}

namespace {

// «# saved=2026-11-12T21:15:03  screen=7680x2160  ...» — читаем две части,
// счётчики в конце строки не проверяем: истина важнее заголовка.
void ParseHeaderLine(const std::wstring& line, Layout* out) {
    const size_t savedPos = line.find(L"saved=");
    if (savedPos != std::wstring::npos) {
        const size_t begin = savedPos + 6;
        const size_t end = line.find_first_of(L" \t", begin);
        out->saved = line.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
    }
    const size_t screenPos = line.find(L"screen=");
    if (screenPos != std::wstring::npos) {
        const size_t begin = screenPos + 7;
        const size_t end = line.find_first_of(L" \t", begin);
        const std::wstring value =
            line.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        const size_t separator = value.find(L'x');
        if (separator != std::wstring::npos) {
            int width = 0;
            int height = 0;
            if (ParseInt(value.substr(0, separator), &width) &&
                ParseInt(value.substr(separator + 1), &height)) {
                out->screenW = width;
                out->screenH = height;
            }
        }
    }
}

}  // namespace

bool ParseLayout(const std::wstring& text, Layout* out, std::wstring* error) {
    *out = Layout();
    bool headerSeen = false;

    enum class Section { None, Desktop, Monitor, Window };
    Section section = Section::None;
    int sectionIndex = -1;

    std::wstring line;
    size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        const size_t end = text.find(L'\n', pos);
        line.assign(text, pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        pos = (end == std::wstring::npos) ? text.size() + 1 : end + 1;

        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (first && !line.empty() && line[0] == L'\xFEFF') {
            line.erase(0, 1);  // BOM
        }
        first = false;

        const std::wstring trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed[0] == L'#') {
            if (trimmed.find(L"# ScreenMemory layout") == 0) {
                headerSeen = true;
            } else if (trimmed.find(L"# saved=") == 0) {
                ParseHeaderLine(trimmed, out);
            }
            continue;
        }

        if (trimmed[0] == L'[') {
            const size_t close = trimmed.find(L']');
            if (close == std::wstring::npos) {
                continue;
            }
            const std::wstring name = Trim(trimmed.substr(1, close - 1));
            section = Section::None;
            sectionIndex = -1;
            int number = -1;
            if (name.find(L"desktop.") == 0 && ParseInt(name.substr(8), &number) && number >= 0) {
                section = Section::Desktop;
            } else if (name.find(L"monitor.") == 0 && ParseInt(name.substr(8), &number) &&
                       number >= 0) {
                section = Section::Monitor;
            } else if (name.find(L"window.") == 0 && ParseInt(name.substr(7), &number) &&
                       number >= 0) {
                section = Section::Window;
            } else {
                continue;  // неизвестная секция — игнорируем целиком
            }
            sectionIndex = number;
            if (section == Section::Desktop) {
                if (static_cast<int>(out->desktops.size()) <= number) {
                    out->desktops.resize(static_cast<size_t>(number) + 1);
                }
                out->desktops[static_cast<size_t>(number)].index = number;
            } else if (section == Section::Monitor) {
                if (static_cast<int>(out->monitors.size()) <= number) {
                    out->monitors.resize(static_cast<size_t>(number) + 1);
                }
            } else {
                if (static_cast<int>(out->windows.size()) <= number) {
                    out->windows.resize(static_cast<size_t>(number) + 1);
                }
            }
            continue;
        }

        if (section == Section::None || sectionIndex < 0) {
            continue;
        }

        const size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        const std::wstring key = Trim(trimmed.substr(0, eq));
        const std::wstring value = UnescapeValue(Trim(StripComment(trimmed.substr(eq + 1))));

        int number = 0;
        RECT rect{};
        if (section == Section::Desktop) {
            LayoutDesktop& desktop = out->desktops[static_cast<size_t>(sectionIndex)];
            if (key == L"guid") {
                ParseGuid(value, &desktop.guid);
            } else if (key == L"name") {
                desktop.name = value;
            }
        } else if (section == Section::Monitor) {
            LayoutMonitor& monitor = out->monitors[static_cast<size_t>(sectionIndex)];
            if (key == L"device") {
                monitor.device = value;
            } else if (key == L"rect" && ParseRect(value, &rect)) {
                monitor.rect = rect;
            } else if (key == L"work" && ParseRect(value, &rect)) {
                monitor.work = rect;
            } else if (key == L"dpi" && ParseInt(value, &number)) {
                monitor.dpi = number > 0 ? static_cast<UINT>(number) : 96U;
            } else if (key == L"primary" && ParseInt(value, &number)) {
                monitor.primary = number != 0 ? 1 : 0;
            }
        } else {
            LayoutWindow& window = out->windows[static_cast<size_t>(sectionIndex)];
            if (key == L"desktop" && ParseInt(value, &number)) {
                window.desktop = number;
            } else if (key == L"desktop_name") {
                window.desktopName = value;
            } else if (key == L"pinned" && ParseInt(value, &number)) {
                window.pinned = number != 0 ? 1 : 0;
            } else if (key == L"zg" && ParseInt(value, &number)) {
                window.zg = number;
            } else if (key == L"exe") {
                window.exe = value;
            } else if (key == L"cmdline") {
                window.cmdline = value;
            } else if (key == L"url") {
                window.url = value;
            } else if (key == L"class") {
                window.cls = value;
            } else if (key == L"folder") {
                window.folder = value;
            } else if (key == L"title") {
                window.title = value;
            } else if (key == L"aumid") {
                window.aumid = value;
            } else if (key == L"monitor") {
                window.monitor = value;
            } else if (key == L"mrel" && ParseRect(value, &rect)) {
                window.mrel = rect;
            } else if (key == L"rect" && ParseRect(value, &rect)) {
                window.rect = rect;
            } else if (key == L"frame" && ParseRect(value, &rect)) {
                window.frame = rect;
                window.frameOk = true;
            } else if (key == L"show") {
                window.show = value;
            } else if (key == L"uwp" && ParseInt(value, &number)) {
                window.uwp = number != 0 ? 1 : 0;
            } else if (key == L"elevated" && ParseInt(value, &number)) {
                window.elevated = number != 0 ? 1 : 0;
            }
        }
    }

    // Секции [window.N] раскладываются по номеру, поэтому дырка в нумерации (файл правили
    // руками и секцию удалили) оставляла пустую запись: она уходила в фазу B строкой
    // «не запускаем: команду запуска собрать не удалось», писала мусорную команду в
    // admin.log и делала код возврата 1 (CONTEXT п. 72). У настоящей записи всегда есть
    // чем опознать окно — путь к exe, AUMID, заголовок или класс, — так что пустые
    // выбрасываем: индексы дальше пересчитываются сами, наружу они не выходят.
    out->windows.erase(std::remove_if(out->windows.begin(), out->windows.end(),
                                      [](const LayoutWindow& window) {
                                          return window.exe.empty() && window.title.empty() &&
                                                 window.cls.empty() && window.aumid.empty() &&
                                                 window.cmdline.empty() && window.folder.empty() &&
                                                 window.url.empty();
                                      }),
                       out->windows.end());

    if (!headerSeen) {
        if (error != nullptr) {
            *error = L"нет заголовка «# ScreenMemory layout v1»: файл не похож на раскладку";
        }
        return false;
    }
    return true;
}

namespace {

// Продвинуть цепочку защищённых копий: последняя удаляется, каждая предыдущая
// переименовывается на шаг вперёд, на освободившееся первое место копируется source.
// previous — метка слепка, который лежал в первой копии до сдвига (для отчёта).
bool PushChain(const std::wstring& dir, const wchar_t* const* names, int count,
               const std::wstring& source, std::wstring* previous, std::wstring* error) {
    if (previous != nullptr) {
        previous->clear();
        std::wstring stamp;
        if (HeaderSavedOf(JoinPath(dir, names[0]), &stamp)) {
            *previous = stamp;
        }
    }

    if (count < 1) {
        return true;
    }
    const std::wstring last = JoinPath(dir, names[count - 1]);
    if (FileExists(last) && !DeleteFileW(last.c_str())) {
        if (error != nullptr) {
            *error = L"не удалось удалить " + last + L": " + Win32ErrorText(GetLastError());
        }
        return false;
    }
    for (int i = count - 1; i >= 1; --i) {
        const std::wstring from = JoinPath(dir, names[i - 1]);
        if (!FileExists(from)) {
            continue;
        }
        const std::wstring to = JoinPath(dir, names[i]);
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (error != nullptr) {
                *error = L"не удалось переименовать " + from + L": " +
                         Win32ErrorText(GetLastError());
            }
            return false;
        }
        Trace(L"цепочка: %s -> %s", from.c_str(), to.c_str());
    }

    const std::wstring first = JoinPath(dir, names[0]);
    // CopyFile переносит на копию дату исходника: «вчерашний день» помечен временем
    // слепка, а не временем сдвига — по нему и решается, что уже сдвинуто.
    if (!CopyFileW(source.c_str(), first.c_str(), FALSE)) {
        if (error != nullptr) {
            *error = L"не удалось скопировать " + source + L" в " + first + L": " +
                     Win32ErrorText(GetLastError());
        }
        return false;
    }
    Trace(L"цепочка: %s -> %s", source.c_str(), first.c_str());
    return true;
}

}  // namespace

bool WriteLayoutFile(const std::wstring& dir, const Layout& layout, const Layout* previous,
                     int versions, LayoutSaveResult* result, std::wstring* error) {
    const std::wstring finalPath = JoinPath(dir, L"layout.txt");
    const std::wstring tempPath = JoinPath(dir, L"layout.new");
    if (result != nullptr) {
        *result = LayoutSaveResult();
        result->path = finalPath;
    }
    if (versions < 1) {
        versions = kDefaultLayoutVersions;
    }
    if (versions > kMaxLayoutVersions) {
        versions = kMaxLayoutVersions;
    }

    // 1. Пишем во временный файл: даже если что-то упадёт между шагами,
    //    старый layout.txt останется целым.
    if (!WriteUtf8File(tempPath, SerializeLayout(layout), error)) {
        return false;
    }

    // 2. Изменились мониторы или набор десктопов — прежний слепок в защищённую копию.
    //    Именно save замечает смену конфигурации (restore за это не отвечает): при
    //    следующем запуске restore, когда мониторы уже починены, «как было при четырёх
    //    мониторах» иначе было бы не восстановить. Копию делаем ДО ротации: после неё
    //    layout.txt — это уже новый слепок. Три места в цепочке страхуют от случая, когда
    //    «прежним» оказался слепок, снятый уже после сбоя.
    if (FileExists(finalPath) && previous != nullptr && !SameConfigSignature(*previous, layout)) {
        std::wstring copied;
        std::wstring shiftError;
        if (PushChain(dir, kConfigChainNames, kConfigChainLength, finalPath, &copied,
                      &shiftError)) {
            if (result != nullptr) {
                result->configCopy = kConfigChainNames[0];
            }
            Trace(L"конфигурация изменилась: прежний слепок — в %s", kConfigChainNames[0]);
        } else {
            // Не фатально: свежий слепок важнее архивного.
            Warn(L"копия «прежняя конфигурация» не создана: %s", shiftError.c_str());
        }
    }

    // 3. Предыдущий слепок — в файл ротации. Имя берём из его же заголовка
    //    «# saved=», чтобы имя отражало время снимка, а не время переименования.
    if (FileExists(finalPath)) {
        const std::wstring target = RotatedPathFor(dir, SavedStampOf(finalPath));
        if (MoveFileExW(finalPath.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (result != nullptr) {
                result->rotated = FileNameOfPath(target);
            }
            Trace(L"ротация: %s -> %s", finalPath.c_str(), target.c_str());
        } else {
            // Не фатально: свежий слепок важнее старого, ниже файл просто заменится.
            Warn(L"ротация не удалась (%s), layout.txt будет перезаписан",
                 Win32ErrorText(GetLastError()).c_str());
        }
    }

    // 4. Атомарная замена: на NTFS MoveFileEx с REPLACE_EXISTING не теряет файл.
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error != nullptr) {
            *error = L"не удалось заменить layout.txt: " + Win32ErrorText(GetLastError());
        }
        DeleteFileW(tempPath.c_str());
        return false;
    }

    // 5. Держим versions рабочих версий; защищённые копии не считаются (PLAN.md §6.1).
    const int pruned = PruneOldVersions(dir, versions);
    if (result != nullptr) {
        result->pruned = pruned;
    }
    return true;
}

namespace {

// Путь из ключа --from: «C:\…», «\\сервер\…», «\каталог» — как есть, иначе рядом с exe.
std::wstring ResolveFromPath(const std::wstring& dir, const std::wstring& file) {
    if (file.size() >= 2 && (file[1] == L':' || (file[0] == L'\\' && file[1] == L'\\'))) {
        return file;
    }
    if (!file.empty() && file[0] == L'\\') {
        return file;
    }
    return JoinPath(dir, file);
}

// Начало текущей загрузки системы в местном времени. NtQuerySystemInformation
// (SystemTimeOfDayInformation, класс 3) отдаёт точное время загрузки — оно не зависит ни
// от сна, ни от перевода часов; «сейчас минус GetTickCount64» — запасной путь.
std::wstring BootStampForSession() {
    typedef LONG(NTAPI * NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);
    struct TimeOfDayInfo {
        LARGE_INTEGER bootTime;  // системное время (UTC) в 100-нс единицах
        LARGE_INTEGER currentTime;
        LARGE_INTEGER timeZoneBias;
        ULONG timeZoneId;
        ULONG reserved;
        ULONGLONG bootTimeBias;
        ULONGLONG sleepTimeBias;
    };

    LARGE_INTEGER boot{};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");  // ntdll загружен всегда
    if (ntdll != nullptr) {
        const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
        TimeOfDayInfo info{};
        if (query != nullptr &&
            query(3 /* SystemTimeOfDayInformation */, &info, sizeof(info), nullptr) >= 0) {
            boot = info.bootTime;
        }
    }
    if (boot.QuadPart <= 0) {
        FILETIME nowFileTime{};
        GetSystemTimeAsFileTime(&nowFileTime);
        ULARGE_INTEGER now{};
        now.LowPart = nowFileTime.dwLowDateTime;
        now.HighPart = nowFileTime.dwHighDateTime;
        const ULONGLONG worked = GetTickCount64() * 10000ULL;  // мс → 100-нс
        if (now.QuadPart <= worked) {
            return std::wstring();
        }
        boot.QuadPart = static_cast<LONGLONG>(now.QuadPart - worked);
    }

    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(static_cast<ULONGLONG>(boot.QuadPart) & 0xFFFFFFFFULL);
    utc.dwHighDateTime = static_cast<DWORD>(static_cast<ULONGLONG>(boot.QuadPart) >> 32);
    SYSTEMTIME utcTime{};
    if (!FileTimeToSystemTime(&utc, &utcTime)) {
        return std::wstring();
    }
    SYSTEMTIME localTime{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime)) {
        localTime = utcTime;
    }
    wchar_t text[32] = {};
    swprintf_s(text, L"%04d-%02d-%02dT%02d:%02d:%02d", localTime.wYear, localTime.wMonth,
               localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond);
    return text;
}

// GUID в текст: для сравнения набора десктопов годится любой стабильный вид.
std::wstring GuidText(const GUID& guid) {
    wchar_t text[64] = {};
    swprintf_s(text, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
               guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
               guid.Data4[6], guid.Data4[7]);
    return text;
}

// Порядок мониторов у EnumDisplayMonitors не гарантирован — сравниваем как множество.
bool MonitorLess(const LayoutMonitor& a, const LayoutMonitor& b) {
    if (a.device != b.device) {
        return a.device < b.device;
    }
    if (a.rect.left != b.rect.left) {
        return a.rect.left < b.rect.left;
    }
    if (a.rect.top != b.rect.top) {
        return a.rect.top < b.rect.top;
    }
    if (a.rect.right != b.rect.right) {
        return a.rect.right < b.rect.right;
    }
    if (a.rect.bottom != b.rect.bottom) {
        return a.rect.bottom < b.rect.bottom;
    }
    return a.primary < b.primary;
}

}  // namespace

std::wstring SessionStartStamp() {
    return BootStampForSession();
}

const wchar_t* const kDayChainNames[kDayChainLength] = {
    L"layout-вчерашний-день.txt",
    L"layout-позавчерашний-день.txt",
    L"layout-позапозавчерашний-день.txt",
};

const wchar_t* const kConfigChainNames[kConfigChainLength] = {
    L"layout-прежняя-конфигурация.txt",
    L"layout-прежняя-конфигурация-2.txt",
    L"layout-прежняя-конфигурация-3.txt",
};

bool ReadLayoutFromFile(const std::wstring& dir, const std::wstring& file, Layout* out,
                        std::wstring* path, std::wstring* error) {
    const std::wstring resolved = ResolveFromPath(dir, file);
    if (path != nullptr) {
        *path = resolved;
    }
    if (!FileExists(resolved)) {
        if (error != nullptr) {
            *error = L"файл раскладки не найден: " + resolved;
        }
        return false;
    }
    std::wstring text;
    if (!ReadUtf8File(resolved, &text, error)) {
        return false;
    }
    return ParseLayout(text, out, error);
}

bool ReadLayoutFile(const std::wstring& dir, Layout* out, std::wstring* path,
                    std::wstring* error) {
    return ReadLayoutFromFile(dir, L"layout.txt", out, path, error);
}

bool SameConfigSignature(const Layout& a, const Layout& b) {
    if (a.monitors.size() != b.monitors.size() || a.desktops.size() != b.desktops.size()) {
        return false;
    }

    std::vector<LayoutMonitor> left = a.monitors;
    std::vector<LayoutMonitor> right = b.monitors;
    std::sort(left.begin(), left.end(), MonitorLess);
    std::sort(right.begin(), right.end(), MonitorLess);
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].device != right[i].device || left[i].primary != right[i].primary ||
            left[i].rect.left != right[i].rect.left || left[i].rect.top != right[i].rect.top ||
            left[i].rect.right != right[i].rect.right ||
            left[i].rect.bottom != right[i].rect.bottom) {
            return false;
        }
    }

    // Десктопы: важен набор GUID. Порядок и имена — нет: десктоп можно переименовать,
    // и это не повод считать конфигурацию другой.
    std::vector<std::wstring> leftGuids;
    std::vector<std::wstring> rightGuids;
    leftGuids.reserve(a.desktops.size());
    rightGuids.reserve(b.desktops.size());
    for (const LayoutDesktop& item : a.desktops) {
        leftGuids.push_back(GuidText(item.guid));
    }
    for (const LayoutDesktop& item : b.desktops) {
        rightGuids.push_back(GuidText(item.guid));
    }
    std::sort(leftGuids.begin(), leftGuids.end());
    std::sort(rightGuids.begin(), rightGuids.end());
    return leftGuids == rightGuids;
}

bool ShiftDayChain(const std::wstring& dir, const Layout& layout, DayChainResult* result,
                   std::wstring* error) {
    if (result != nullptr) {
        *result = DayChainResult();
        result->yesterday = kDayChainNames[0];
    }
    const std::wstring source = JoinPath(dir, L"layout.txt");
    if (!FileExists(source)) {
        if (result != nullptr) {
            result->reason = L"нет layout.txt";
        }
        return true;
    }

    // Тот же слепок уже стоит «вчерашним» — сдвигать нечего. Без этой проверки каждый
    // ручной restore съедал бы по одному звену цепочки, и от истории не осталось бы
    // ничего за три запуска.
    std::wstring yesterdayStamp;
    if (HeaderSavedOf(JoinPath(dir, kDayChainNames[0]), &yesterdayStamp) &&
        !layout.saved.empty() && yesterdayStamp == layout.saved) {
        if (result != nullptr) {
            result->reason = L"слепок уже стоит во «вчерашнем дне»";
        }
        return true;
    }

    // Слепок снят после загрузки системы — значит, перезагрузки не было и это середина
    // текущей сессии: цепочку дней двигать не за что (PLAN.md §6.1 п.5).
    const std::wstring sessionStart = SessionStartStamp();
    if (!sessionStart.empty() && !layout.saved.empty() && layout.saved >= sessionStart) {
        if (result != nullptr) {
            result->reason = L"слепок снят в текущей сессии (" + layout.saved +
                             L" не раньше загрузки " + sessionStart + L")";
        }
        return true;
    }

    std::wstring previous;
    if (!PushChain(dir, kDayChainNames, kDayChainLength, source, &previous, error)) {
        return false;
    }
    if (result != nullptr) {
        result->shifted = true;
        result->previous = previous;
    }
    return true;
}

}  // namespace sm
