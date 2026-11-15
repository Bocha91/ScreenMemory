#include "registry_desktops.h"

#include <objbase.h>
#include <shlobj.h>  // SHGetKnownFolderPath

#include <cstring>

#include "log.h"

namespace {

const wchar_t* const kVirtualDesktopsKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
const wchar_t* const kPinnedAppsKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\PinnedApps";

// Обёртка над HKEY: закрывает ключ в деструкторе, копировать нельзя.
class RegKey {
public:
    RegKey() = default;
    ~RegKey() { Close(); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    bool Open(HKEY parent, const std::wstring& sub, std::wstring* error) {
        Close();
        const LSTATUS st = RegOpenKeyExW(parent, sub.c_str(), 0, KEY_READ, &key_);
        if (st != ERROR_SUCCESS) {
            if (key_ != nullptr) {
                RegCloseKey(key_);
                key_ = nullptr;
            }
            if (error != nullptr) {
                *error = L"RegOpenKeyEx(" + sub + L"): " + sm::Win32ErrorText(static_cast<DWORD>(st));
            }
            return false;
        }
        return true;
    }

    HKEY get() const { return key_; }

    void Close() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
            key_ = nullptr;
        }
    }

private:
    HKEY key_ = nullptr;
};

// Имя значения по индексу. Буфер растёт, если имени не хватает места
// (пути к exe бывают длинными, особенно при longPathAware).
bool EnumValueAt(HKEY key, DWORD index, std::wstring* name, DWORD* type, DWORD* data) {
    for (DWORD cap = 256; cap <= 32768; cap *= 2) {
        std::wstring buf(cap, L'\0');
        DWORD nameLen = cap;
        DWORD valueType = 0;
        BYTE dataBuf[1024] = {};
        DWORD dataLen = static_cast<DWORD>(sizeof(dataBuf));

        const LSTATUS st = RegEnumValueW(key, index, &buf[0], &nameLen, nullptr, &valueType,
                                         dataBuf, &dataLen);
        if (st == ERROR_SUCCESS) {
            buf.resize(nameLen);
            *name = buf;
            *type = valueType;
            *data = 0;
            if (valueType == REG_DWORD && dataLen >= sizeof(DWORD)) {
                std::memcpy(data, dataBuf, sizeof(DWORD));
            }
            return true;
        }
        if (st != ERROR_MORE_DATA) {
            return false;  // сюда же попадает ERROR_NO_MORE_ITEMS
        }
    }
    return false;
}

// "{6D809377-...}\Относительный\путь" → полный путь; пусто, если папка не разрешилась.
std::wstring ResolveKnownFolderPath(const std::wstring& raw) {
    const size_t close = raw.find(L'}');
    if (close == std::wstring::npos || close <= 1) {
        return std::wstring();
    }

    GUID folder{};
    if (!sm::ParseGuid(raw.substr(0, close + 1), &folder)) {
        return std::wstring();
    }

    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &base)) || base == nullptr) {
        if (base != nullptr) {
            CoTaskMemFree(base);
        }
        return std::wstring();
    }

    std::wstring rel = raw.substr(close + 1);
    while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/')) {
        rel.erase(0, 1);
    }

    std::wstring full;
    if (!rel.empty()) {
        full = sm::JoinPath(base, rel);
    }
    CoTaskMemFree(base);
    return full;
}

}  // namespace

namespace sm {

std::wstring GuidToString(const GUID& guid) {
    wchar_t buf[64] = {};
    if (StringFromGUID2(guid, buf, static_cast<int>(_countof(buf))) > 0) {
        return buf;
    }
    wchar_t alt[64];
    swprintf_s(alt, L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
               guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
               guid.Data4[6], guid.Data4[7]);
    return alt;
}

bool ParseGuid(const std::wstring& text, GUID* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }
    std::wstring s = text;
    if (s.front() != L'{') {
        s = L"{" + s + L"}";
    }
    return SUCCEEDED(CLSIDFromString(s.c_str(), out));
}

bool IsNullGuid(const GUID& guid) {
    static const GUID kNull = {};
    return std::memcmp(&guid, &kNull, sizeof(GUID)) == 0;
}

int FindDesktopIndex(const std::vector<DesktopInfo>& desktops, const GUID& guid) {
    for (size_t i = 0; i < desktops.size(); ++i) {
        if (IsEqualGUID(desktops[i].guid, guid)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring ReadDesktopName(const GUID& guid) {
    RegKey base;
    if (!base.Open(HKEY_CURRENT_USER, kVirtualDesktopsKey, nullptr)) {
        return std::wstring();
    }
    RegKey key;
    if (!key.Open(base.get(), L"Desktops\\" + GuidToString(guid), nullptr)) {
        return std::wstring();
    }

    for (DWORD cap = 256; cap <= 32768; cap *= 2) {
        std::wstring buf(cap, L'\0');
        DWORD size = static_cast<DWORD>(cap * sizeof(wchar_t));
        DWORD type = 0;
        const LSTATUS st = RegQueryValueExW(key.get(), L"Name", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(&buf[0]), &size);
        if (st == ERROR_SUCCESS) {
            if (type != REG_SZ && type != REG_EXPAND_SZ) {
                return std::wstring();
            }
            buf.resize(size / sizeof(wchar_t));
            while (!buf.empty() && buf.back() == L'\0') {
                buf.pop_back();
            }
            return buf;
        }
        if (st != ERROR_MORE_DATA) {
            return std::wstring();
        }
    }
    return std::wstring();
}


bool ReadDesktops(std::vector<DesktopInfo>* out, std::wstring* error) {
    if (out == nullptr) {
        return false;
    }
    out->clear();

    RegKey key;
    if (!key.Open(HKEY_CURRENT_USER, kVirtualDesktopsKey, error)) {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    LSTATUS st = RegQueryValueExW(key.get(), L"VirtualDesktopIDs", nullptr, &type, nullptr, &size);
    if (st != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"значение VirtualDesktopIDs: " + Win32ErrorText(static_cast<DWORD>(st));
        }
        return false;
    }
    if (type != REG_BINARY || size == 0 || size % 16 != 0) {
        if (error != nullptr) {
            wchar_t buf[128];
            swprintf_s(buf, L"значение VirtualDesktopIDs: неожиданный формат (type=%lu, size=%lu)",
                       static_cast<unsigned long>(type), static_cast<unsigned long>(size));
            *error = buf;
        }
        return false;
    }

    std::vector<BYTE> raw(size, 0);
    DWORD got = size;
    st = RegQueryValueExW(key.get(), L"VirtualDesktopIDs", nullptr, &type, raw.data(), &got);
    if (st != ERROR_SUCCESS || got < 16) {
        if (error != nullptr) {
            *error = L"чтение VirtualDesktopIDs: " + Win32ErrorText(static_cast<DWORD>(st));
        }
        return false;
    }

    // Формат совпадает с Win32 GUID: Data1/Data2/Data3 в little-endian, Data4 — как есть.
    const int count = static_cast<int>(got / 16);
    out->reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        DesktopInfo info;
        std::memcpy(&info.guid, raw.data() + static_cast<size_t>(i) * 16, 16);
        info.index = i;
        info.name = ReadDesktopName(info.guid);
        Trace(L"десктоп [%d] %s \"%s\"", info.index, GuidToString(info.guid).c_str(),
              info.name.c_str());
        out->push_back(info);
    }

    Trace(L"десктопов прочитано: %d", count);
    return true;
}

bool ReadPinnedApps(std::vector<PinnedApp>* out, std::wstring* error) {
    if (out == nullptr) {
        return false;
    }
    out->clear();

    HKEY key = nullptr;
    const LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kPinnedAppsKey, 0, KEY_READ, &key);
    if (st == ERROR_FILE_NOT_FOUND) {
        Trace(L"ключ PinnedApps отсутствует: ни одно приложение не закреплено");
        return true;  // норма: ни одно приложение не помечено «показывать на всех десктопах»
    }
    if (st != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = L"RegOpenKeyEx(PinnedApps): " + Win32ErrorText(static_cast<DWORD>(st));
        }
        return false;
    }

    for (DWORD i = 0;; ++i) {
        std::wstring name;
        DWORD type = 0;
        DWORD data = 0;
        if (!EnumValueAt(key, i, &name, &type, &data)) {
            break;
        }
        if (name.empty()) {
            continue;
        }

        PinnedApp app;
        app.raw = name;
        app.type = type;
        app.data = data;
        ExplainPinnedName(name, &app.kind, &app.resolved);
        Trace(L"pinned[%lu] %s (%s) type=%lu data=0x%08lX", static_cast<unsigned long>(i),
              app.raw.c_str(), app.kind.c_str(), static_cast<unsigned long>(app.type),
              static_cast<unsigned long>(app.data));
        out->push_back(app);
    }

    RegCloseKey(key);
    Trace(L"pinned-приложений прочитано: %d", static_cast<int>(out->size()));
    return true;
}

bool ReadCurrentDesktopFromRegistry(GUID* out) {
    if (out == nullptr) {
        return false;
    }
    RegKey key;
    if (!key.Open(HKEY_CURRENT_USER, kVirtualDesktopsKey, nullptr)) {
        return false;
    }

    DWORD type = 0;
    wchar_t buf[64] = {};
    DWORD size = static_cast<DWORD>(sizeof(buf));
    const LSTATUS st = RegQueryValueExW(key.get(), L"CurrentVirtualDesktop", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(buf), &size);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    return ParseGuid(buf, out);
}

void ExplainPinnedName(const std::wstring& raw, std::wstring* kind, std::wstring* resolved) {
    if (kind == nullptr || resolved == nullptr) {
        return;
    }
    resolved->clear();

    if (!raw.empty() && raw.front() == L'{') {
        *kind = L"known";
        *resolved = ResolveKnownFolderPath(raw);  // может остаться пустым
        return;
    }

    const bool looksLikePath = raw.find(L'\\') != std::wstring::npos ||
                               raw.find(L'/') != std::wstring::npos;
    *kind = looksLikePath ? L"путь" : L"aumid";
}

bool PinnedMatches(const PinnedApp& pinned, const std::wstring& exePath,
                   const std::wstring& aumid) {
    if (!aumid.empty() && _wcsicmp(pinned.raw.c_str(), aumid.c_str()) == 0) {
        return true;
    }
    if (!exePath.empty()) {
        if (_wcsicmp(pinned.raw.c_str(), exePath.c_str()) == 0) {
            return true;
        }
        if (pinned.kind == L"known" && !pinned.resolved.empty() &&
            _wcsicmp(pinned.resolved.c_str(), exePath.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace sm
