#include "config.h"

#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#include "log.h"

namespace sm {
namespace {

const wchar_t* const kFileName = L"Config.ini";

// Файл настроек — это несколько строк. Больше мегабайта читать не будем: если рядом с exe
// случайно оказался чужой файл с таким именем, программа не должна его вычитывать.
constexpr ULONGLONG kMaxConfigBytes = 1024 * 1024;
constexpr int kMaxTimeoutSec = 24 * 60 * 60;

// Автозапуск (PLAN.md §11): интервал AutoSave в минутах и задержка AutoRestore в секундах.
constexpr int kDefaultAutoSaveMinutes = 10;
constexpr int kMaxAutoSaveMinutes = 24 * 60;
constexpr int kDefaultAutoRestoreDelaySec = 30;
constexpr int kMaxAutoRestoreDelaySec = 60 * 60;

// Сколько рабочих версий слепка держать в bin (PLAN.md §6.1, ключ Layouts). Значения те же,
// что kDefaultLayoutVersions/kMaxLayoutVersions в storage.h: там они — для записи файла.
constexpr int kDefaultLayoutVersions = 3;
constexpr int kMaxLayoutVersions = 20;

// Ожидание «успокоения» окон перед запуском приложений (ключ settle-wait, PLAN.md §8.3 п.0).
constexpr int kDefaultSettleWaitSec = 10;
constexpr int kMaxSettleWaitSec = 10 * 60;

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return text;
}

bool ReadWholeFile(const std::wstring& path, std::string* bytes, DWORD* errorCode) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *errorCode = GetLastError();
        return false;
    }

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE) {
        *errorCode = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0 || static_cast<ULONGLONG>(size.QuadPart) > kMaxConfigBytes) {
        *errorCode = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return false;
    }

    bytes->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = TRUE;
    if (!bytes->empty()) {
        ok = ReadFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &read, nullptr);
    }
    const DWORD readError = (ok == FALSE) ? GetLastError() : 0;
    CloseHandle(file);
    if (ok == FALSE) {
        *errorCode = readError;
        return false;
    }
    bytes->resize(read);
    return true;
}

int DecodeWithCodePage(const char* data, size_t size, UINT codePage, DWORD flags,
                       std::wstring* out) {
    const int length = static_cast<int>(size);
    const int need = MultiByteToWideChar(codePage, flags, data, length, nullptr, 0);
    if (need <= 0) {
        return 0;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(need));
    const int got = MultiByteToWideChar(codePage, flags, data, length, buffer.data(), need);
    if (got <= 0) {
        return 0;
    }
    out->assign(buffer.data(), static_cast<size_t>(got));
    return got;
}

// Кодировка: UTF-16LE с BOM, UTF-8 с BOM или без. Если байты не складываются в UTF-8,
// считаем, что файл сохранён старым «Блокнотом» в системной ANSI: путь с «кракозябрами»
// всё равно полезнее, чем никакой настройки (в Trace это видно).
bool DecodeText(const std::string& bytes, std::wstring* out) {
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t offset = 0;
    bool utf16Le = false;
    if (bytes.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        offset = 3;  // BOM UTF-8: сам себя не выдаёт, но и не мешает
    } else if (bytes.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        offset = 2;
        utf16Le = true;
    }

    const size_t rest = bytes.size() - offset;
    out->clear();
    if (rest == 0) {
        return true;
    }

    if (utf16Le) {
        if (rest % 2 != 0) {
            return false;  // обрезанный UTF-16 — файл повреждён
        }
        const unsigned char* data = raw + offset;
        out->reserve(rest / 2);
        for (size_t i = 0; i + 1 < rest; i += 2) {
            const wchar_t ch = static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
            out->push_back(ch);
        }
        return true;
    }

    const char* data = bytes.data() + offset;
    if (DecodeWithCodePage(data, rest, CP_UTF8, MB_ERR_INVALID_CHARS, out) > 0) {
        return true;
    }
    return DecodeWithCodePage(data, rest, CP_ACP, 0, out) > 0;
}

std::wstring ExpandEnv(const std::wstring& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const DWORD need = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (need == 0) {
        return text;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(need));
    const DWORD got = ExpandEnvironmentStringsW(text.c_str(), buffer.data(), need);
    if (got == 0 || got > need) {
        return text;
    }
    return std::wstring(buffer.data(), static_cast<size_t>(got - 1));
}

// Путь из конфига: пробелы по краям и обрамляющие кавычки убираем — строку удобно
// вставлять из «Свойства → Объект» ярлыка или из Проводника («Копировать как путь»).
std::wstring ValuePath(const std::wstring& value) {
    std::wstring text = Trim(value);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
        text = Trim(text.substr(1, text.size() - 2));
    }
    return ExpandEnv(text);
}

int ParseSeconds(const std::wstring& value) {
    if (value.empty()) {
        return 0;
    }
    wchar_t* stop = nullptr;
    const long parsed = wcstol(value.c_str(), &stop, 10);
    if (stop == value.c_str() || parsed < 0) {
        Trace(L"Config.ini: prelaunch-timeout=«%s» — не число секунд, жду без ограничения",
              value.c_str());
        return 0;
    }
    while (*stop != L'\0' && iswspace(*stop) != 0) {
        ++stop;
    }
    if (*stop != L'\0') {
        Trace(L"Config.ini: prelaunch-timeout=«%s» — лишние символы, жду без ограничения",
              value.c_str());
        return 0;
    }
    if (parsed > kMaxTimeoutSec) {
        return kMaxTimeoutSec;
    }
    return static_cast<int>(parsed);
}

// Число для ключей автозапуска: пусто/не число/лишние символы — значение по умолчанию,
// иначе зажимаем в [minValue, maxValue]. Всё нестандартное попадает в лог (--verbose).
int ParseBounded(const std::wstring& value, const wchar_t* keyName, int fallback, int minValue,
                 int maxValue) {
    if (value.empty()) {
        return fallback;
    }
    wchar_t* stop = nullptr;
    const long parsed = wcstol(value.c_str(), &stop, 10);
    if (stop == value.c_str()) {
        Trace(L"Config.ini: %s=«%s» — не число, беру %d", keyName, value.c_str(), fallback);
        return fallback;
    }
    while (*stop != L'\0' && iswspace(*stop) != 0) {
        ++stop;
    }
    if (*stop != L'\0') {
        Trace(L"Config.ini: %s=«%s» — лишние символы, беру %d", keyName, value.c_str(), fallback);
        return fallback;
    }
    if (parsed < minValue) {
        Trace(L"Config.ini: %s=%ld — меньше %d, беру %d", keyName, parsed, minValue, minValue);
        return minValue;
    }
    if (parsed > maxValue) {
        Trace(L"Config.ini: %s=%ld — больше %d, беру %d", keyName, parsed, maxValue, maxValue);
        return maxValue;
    }
    return static_cast<int>(parsed);
}

}  // namespace

bool LoadConfig(Config* config, std::wstring* error) {
    if (config == nullptr) {
        return false;
    }
    *config = Config();
    config->path = JoinPath(ExeDir(), kFileName);
    if (error != nullptr) {
        error->clear();
    }

    std::string bytes;
    DWORD code = 0;
    if (ReadWholeFile(config->path, &bytes, &code) == false) {
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;  // файла нет — работаем со значениями по умолчанию
        }
        if (error != nullptr) {
            *error = Win32ErrorText(code);
        }
        return false;
    }

    std::wstring text;
    if (DecodeText(bytes, &text) == false) {
        if (error != nullptr) {
            *error = L"не удалось прочитать текст (кодировка или обрезанный UTF-16)";
        }
        return false;
    }
    config->exists = true;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t newline = text.find(L'\n', pos);
        const std::wstring line =
            Trim(text.substr(pos, (newline == std::wstring::npos) ? std::wstring::npos
                                                                  : newline - pos));
        pos = (newline == std::wstring::npos) ? text.size() + 1 : newline + 1;

        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;  // комментарий
        }
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            continue;  // секция: на разбор не влияет, нужна только для порядка в файле
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            continue;  // строка не «ключ=значение» — молча пропускаем
        }

        const std::wstring key = Lower(Trim(line.substr(0, equals)));
        const std::wstring value = Trim(line.substr(equals + 1));
        if (key == L"prelaunch") {
            config->preLaunch = ValuePath(value);
        } else if (key == L"prelaunch-args") {
            config->preLaunchArgs = ExpandEnv(value);
        } else if (key == L"prelaunch-timeout") {
            config->preLaunchTimeoutSec = ParseSeconds(value);
        } else if (key == L"autosave") {
            config->autoSaveMinutes = ParseBounded(value, L"AutoSave", kDefaultAutoSaveMinutes, 1,
                                                   kMaxAutoSaveMinutes);
            config->autoSaveSet = !value.empty();
        } else if (key == L"autorestoredelay") {
            config->autoRestoreDelaySec =
                ParseBounded(value, L"AutoRestoreDelay", kDefaultAutoRestoreDelaySec, 0,
                             kMaxAutoRestoreDelaySec);
            config->autoRestoreDelaySet = !value.empty();
        } else if (key == L"layouts") {
            config->layoutVersions = ParseBounded(value, L"Layouts", kDefaultLayoutVersions, 1,
                                                  kMaxLayoutVersions);
            config->layoutVersionsSet = !value.empty();
        } else if (key == L"settle-wait") {
            config->settleWaitSec = ParseBounded(value, L"settle-wait", kDefaultSettleWaitSec, 0,
                                                 kMaxSettleWaitSec);
            config->settleWaitSet = !value.empty();
        } else if (!key.empty()) {
            config->unknownKeys.push_back(key);
        }
    }
    return true;
}

}  // namespace sm
