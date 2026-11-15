#include "process_info.h"

#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl_core.h>

#include <map>
#include <utility>
#include <vector>

#include "log.h"

namespace {

// NtQueryInformationProcess(ProcessCommandLineInformation) — единственный способ
// получить командную строку чужого процесса. Класс информации 60 (Windows 8.1+).
// Своя копия структуры — чтобы не зависеть от версии winternl.h в установленном
// SDK; ntdll.lib не линкуется, функция берётся через GetProcAddress.
constexpr ULONG kProcessCommandLineInformation = 60;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004u);
constexpr LONG kStatusBufferTooSmall = static_cast<LONG>(0xC0000023u);

struct NtUnicodeString {
    USHORT Length = 0;
    USHORT MaximumLength = 0;
    PWSTR Buffer = nullptr;
};

struct ProcessCommandLine {
    NtUnicodeString CommandLine;
};

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationProcessFn NtQueryInformationProcessPtr() {
    static NtQueryInformationProcessFn fn = []() -> NtQueryInformationProcessFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");  // ntdll загружен всегда
        if (ntdll == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }();
    return fn;
}

// Указатель CommandLine.Buffer ведёт внутрь того же буфера, который заполнил
// ntdll, поэтому строку копируем сразу, до освобождения буфера.
std::wstring ReadCommandLine(HANDLE process) {
    const NtQueryInformationProcessFn query = NtQueryInformationProcessPtr();
    if (query == nullptr) {
        return std::wstring();
    }

    std::vector<unsigned char> buffer(1024);
    ULONG required = 0;
    LONG status = query(process, kProcessCommandLineInformation, buffer.data(),
                        static_cast<ULONG>(buffer.size()), &required);
    if ((status == kStatusInfoLengthMismatch || status == kStatusBufferTooSmall) &&
        required > buffer.size()) {
        buffer.assign(required, 0);
        status = query(process, kProcessCommandLineInformation, buffer.data(),
                       static_cast<ULONG>(buffer.size()), &required);
    }
    if (status != 0 || buffer.size() < sizeof(ProcessCommandLine)) {
        sm::Trace(L"командная строка: NtQueryInformationProcess = 0x%08lX",
                  static_cast<unsigned long>(status));
        return std::wstring();
    }

    const auto* info = reinterpret_cast<const ProcessCommandLine*>(buffer.data());
    if (info->CommandLine.Buffer == nullptr || info->CommandLine.Length == 0) {
        return std::wstring();
    }
    return std::wstring(info->CommandLine.Buffer,
                        info->CommandLine.Length / sizeof(wchar_t));
}

std::map<DWORD, sm::ProcessInfo>& Cache() {
    static std::map<DWORD, sm::ProcessInfo> cache;
    return cache;
}

}  // namespace

namespace sm {

bool ReadProcessInfo(DWORD pid, ProcessInfo* out) {
    if (out == nullptr) {
        return false;
    }
    *out = ProcessInfo{};
    if (pid == 0) {
        out->error = ERROR_INVALID_PARAMETER;
        return false;
    }

    // PROCESS_QUERY_LIMITED_INFORMATION достаточно и для пути, и для командной
    // строки; для процессов с более высокими правами OpenProcess не пройдёт.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        out->error = GetLastError();
        Trace(L"процесс %lu: OpenProcess: %s", static_cast<unsigned long>(pid),
              Win32ErrorText(out->error).c_str());
        return false;
    }

    // 32768 символов — с запасом: MAX_PATH давно не предел.
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE) {
        path.resize(size);
        out->exePath = path;
        out->exeName = FileNameOf(out->exePath);
    } else {
        out->error = GetLastError();
    }
    out->cmdline = ReadCommandLine(process);

    CloseHandle(process);
    return !out->exePath.empty();
}

const ProcessInfo& ProcessInfoFor(DWORD pid) {
    auto& cache = Cache();
    const auto found = cache.find(pid);
    if (found != cache.end()) {
        return found->second;
    }

    ProcessInfo info;
    ReadProcessInfo(pid, &info);
    if (IsVerbose()) {
        Trace(L"процесс %lu: exe=%s | cmdline=%s", static_cast<unsigned long>(pid),
              info.exePath.empty() ? L"—" : info.exePath.c_str(),
              info.cmdline.empty() ? L"—" : info.cmdline.c_str());
    }
    return cache.emplace(pid, std::move(info)).first->second;
}

void ClearProcessCache() {
    Cache().clear();
}

std::wstring WindowAppUserModelId(HWND hwnd) {
    if (hwnd == nullptr) {
        return std::wstring();
    }

    IPropertyStore* store = nullptr;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store));
    if (FAILED(hr) || store == nullptr) {
        Trace(L"AUMID: SHGetPropertyStoreForWindow(%p): %s", static_cast<void*>(hwnd),
              HrText(hr).c_str());
        return std::wstring();
    }

    std::wstring aumid;
    PROPVARIANT value;
    PropVariantInit(&value);
    hr = store->GetValue(PKEY_AppUserModel_ID, &value);
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        aumid = value.pwszVal;
    }
    PropVariantClear(&value);
    store->Release();
    return aumid;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

}  // namespace sm
