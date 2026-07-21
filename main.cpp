// DrmLauncher - Win32 GUI для запуска N_m3u8DL-RE на удалённом сервере по SSH
//
// Использует libssh2 (exec-канал), как в KodiGui, вместо вызова внешнего ssh.exe.
//
// Юникодная (UTF-16) версия: все окна/контролы созданы через *W API, текст в
// элементах управления хранится как std::wstring. Поскольку SSH-команда и вывод
// удалённого процесса — это байты в кодировке UTF-8 (стандарт для POSIX shell
// и Linux/*BSD консоли), на границе "GUI <-> SSH" сделана явная конвертация
// UTF-16 <-> UTF-8 через WinAPI (Utf8ToWide / WideToUtf8).
//
// Сборка (MinGW):
//   g++ -O2 -std=c++17 main.cpp -o DrmLauncher.exe -lssh2 -lws2_32 -lwinhttp -mwindows -municode
//
// Сборка (MSVC, из "x64 Native Tools Command Prompt"):
//   cl /EHsc /utf-8 /std:c++17 main.cpp libssh2.lib ws2_32.lib winhttp.lib user32.lib gdi32.lib /Fe:DrmLauncher.exe
//
// Изменение громкости (Application.SetVolume) теперь выполняется НЕ через SSH,
// а прямым HTTP-запросом (WinHTTP) с Windows-клиента напрямую на Kodi JSON-RPC
// на Raspberry Pi. Нужна библиотека winhttp.lib (часть Windows SDK, отдельно
// ставить не нужно). Play/Stop-плеер и запуск/остановка загрузки по-прежнему
// работают через SSH, как и раньше.
// (флаг /utf-8 обязателен: файл сохранён в UTF-8, без него MSVC может
//  неверно интерпретировать кириллицу в строковых литералах)
//
// Важно: точка входа теперь wWinMain (юникодная), поэтому для MinGW обязателен
// флаг линковки -municode — иначе будет ошибка "undefined reference to WinMain"
// (линкер по умолчанию ищет WinMainCRTStartup, а не wWinMainCRTStartup).
// Для MSVC ничего дополнительно указывать не нужно — компилятор сам подставит
// нужный CRT-стартап под сигнатуру wWinMain.
//
// Нужна библиотека libssh2 (заголовки + .lib/.a). Проще всего поставить через vcpkg:
//   vcpkg install libssh2:x64-windows
// и указать компилятору include/lib пути vcpkg (см. README.txt рядом).

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include "resource.h"
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#include <winhttp.h>

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Константы подключения (заданы по условию задачи)
// ---------------------------------------------------------------------------
static const char* SSH_HOST = "192.168.8.45";
static const char* SSH_USER = "pi";
static const char* SSH_PASS = "639639";
static const int   SSH_PORT = 22;

// Kodi JSON-RPC (для кнопок "Воспроизвести" / "Остановить плеер").
// Запрос выполняется через SSH командой curl на самом Raspberry Pi (127.0.0.1),
// поэтому не зависит от того, доступен ли порт Kodi извне с Windows-клиента.
static const char* KODI_JSONRPC_HOST = "127.0.0.1";
static const int   KODI_JSONRPC_PORT = 8081;

// Для громкости (Application.SetVolume) запрос идёт напрямую с Windows-клиента
// по HTTP (WinHTTP), без SSH, поэтому нужен LAN-адрес Raspberry Pi, а не
// 127.0.0.1. Порт Kodi JSON-RPC (8081) должен быть доступен на этом адресе
// из локальной сети.
static const char* KODI_JSONRPC_LAN_HOST = SSH_HOST;

// ---------------------------------------------------------------------------
// Идентификаторы элементов управления
// ---------------------------------------------------------------------------
enum ControlId {
    ID_EDIT_ARG1 = 1001,   // $1  (например, ссылка на m3u8 / манифест)
    ID_EDIT_ARG2,          // $2  (ключ DRM)
    ID_BUTTON_RUN,
    ID_BUTTON_KILL,        // "Остановить" — killall -9 N_m3u8DL-RE на сервере
    ID_BUTTON_PLAY,        // "Воспроизвести" — Kodi JSON-RPC Player.Open (drm.aar.ts / drm.ts)
    ID_BUTTON_STOP_PLAY,   // "Остановить плеер" — Kodi JSON-RPC Player.Stop
    ID_EDIT_LOG,
    ID_STATIC_ARG1,
    ID_STATIC_ARG2,
    ID_SLIDER_VOLUME,      // Ползунок громкости — Kodi JSON-RPC Application.SetVolume
    ID_STATIC_VOLUME,
    ID_STATIC_VOLUME_VALUE
};

// ---------------------------------------------------------------------------
// Глобальные хендлы
// ---------------------------------------------------------------------------
HWND g_hArg1, g_hArg2, g_hLog, g_hRunBtn, g_hKillBtn, g_hPlayBtn, g_hStopBtn;
HWND g_hVolumeSlider, g_hVolumeValueLabel;
// Отдельные флаги "занятости": скачивание (Выполнить/Остановить) и плеер
// (Воспроизвести/Остановить плеер/громкость) теперь независимы — запуск
// скачивания больше не блокирует кнопки плеера.
std::atomic<bool> g_running{false};      // занят процесс скачивания (Выполнить/Остановить)
std::atomic<bool> g_playerBusy{false};   // занят Kodi-плеер (Воспроизвести/Остановить плеер)
std::atomic<bool> g_volumeBusy{false};   // выполняется запрос смены громкости
std::mutex g_logMutex;

// ---------------------------------------------------------------------------
// Конвертация UTF-8 <-> UTF-16 (WinAPI). SSH-команда и вывод удалённого шелла —
// UTF-8; нативные Win32 *W контролы — UTF-16.
// ---------------------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), needed);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

// Потокобезопасный вывод в лог (лог многострочный edit control)
void AppendLog(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

// Перегрузка для удобства логирования байтов, пришедших с удалённого хоста
// в UTF-8 (stdout/stderr процесса на Linux/*BSD).
void AppendLogUtf8(const std::string& utf8text) {
    AppendLog(Utf8ToWide(utf8text));
}

// Достаём текст из edit control в std::wstring
std::wstring GetEditText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::vector<wchar_t> buf(len + 1);
    GetWindowTextW(h, buf.data(), len + 1);
    return std::wstring(buf.data());
}

// ---------------------------------------------------------------------------
// Экранирование значения для одиночных кавычек в POSIX shell:  it's -> 'it'"'"'s'
// Работает с UTF-8 байтами: одинарная кавычка (0x27) не может встретиться
// как байт продолжения многобайтовой UTF-8 последовательности, поэтому
// побайтовый разбор безопасен.
// ---------------------------------------------------------------------------
std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\"'\"'";
        else out += c;
    }
    out += "'";
    return out;
}

// ---------------------------------------------------------------------------
// Прямой HTTP POST к Kodi JSON-RPC через WinHTTP (без SSH и без внешнего curl.exe).
// Используется только для смены громкости — остальные Kodi-команды (Play/Stop)
// по-прежнему идут через SSH+curl на самом сервере, см. RunSshExec ниже.
// ---------------------------------------------------------------------------
bool KodiJsonRpcHttpPost(const std::string& host, int port, const std::string& jsonBody, std::wstring& outError) {
    std::wstring hostW = Utf8ToWide(host);
    bool ok = false;

    HINTERNET hSession = WinHttpOpen(L"DrmLauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        outError = L"WinHttpOpen не выполнен (код " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    // Таймауты, чтобы зависший/недоступный Kodi не подвешивал поток надолго.
    WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostW.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        outError = L"WinHttpConnect не выполнен (код " + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/jsonrpc",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        outError = L"WinHttpOpenRequest не выполнен (код " + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json";
    BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
        (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(), (DWORD)jsonBody.size(), 0);

    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        ok = (statusCode >= 200 && statusCode < 300);
        if (!ok) outError = L"Сервер вернул HTTP-статус " + std::to_wstring(statusCode);
    } else {
        outError = L"Не удалось отправить HTTP-запрос (код " + std::to_wstring(GetLastError()) + L")";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Если в поле DRM-ключа введено значение вида "значение1:значение2", извлекаем
// значение2 (то, что идёт после ':'). Если ':' в строке нет, возвращаем строку как есть.
std::string ExtractKeyAfterColon(const std::string& s) {
    size_t pos = s.find(':');
    if (pos == std::string::npos) return s;
    return s.substr(pos + 1);
}

// ---------------------------------------------------------------------------
// Собственно работа с SSH: подключение, аутентификация по паролю, exec, стрим вывода.
// Общее ядро для любой команды на сервере — используется и кнопкой "Выполнить",
// и кнопкой "Остановить" (killall), чтобы не дублировать connect/auth/read-цикл.
// ---------------------------------------------------------------------------
void RunSshExec(const std::string& cmd, std::atomic<bool>& busyFlag, HWND btn1, HWND btn2 = nullptr) {
    const std::string host = SSH_HOST;

    busyFlag = true;
    EnableWindow(btn1, FALSE);
    if (btn2) EnableWindow(btn2, FALSE);

    auto fail = [&](const std::wstring& msg) {
        AppendLog(L"[ОШИБКА] " + msg + L"\r\n");
        busyFlag = false;
        EnableWindow(btn1, TRUE);
        if (btn2) EnableWindow(btn2, TRUE);
    };

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fail(L"WSAStartup не выполнен");
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fail(L"Не удалось создать сокет");
        WSACleanup();
        return;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(SSH_PORT);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        fail(L"Не удалось разрешить адрес сервера: " + Utf8ToWide(host));
        closesocket(sock);
        WSACleanup();
        return;
    }

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fail(L"Не удалось подключиться к " + Utf8ToWide(host) + L":" + Utf8ToWide(portStr));
        freeaddrinfo(res);
        closesocket(sock);
        WSACleanup();
        return;
    }
    freeaddrinfo(res);

    if (libssh2_init(0) != 0) {
        fail(L"libssh2_init не выполнен");
        closesocket(sock);
        WSACleanup();
        return;
    }

    LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session) {
        fail(L"Не удалось создать SSH-сессию");
        closesocket(sock);
        WSACleanup();
        return;
    }

    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, sock) != 0) {
        fail(L"SSH handshake не выполнен");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    AppendLog(L"[ИНФО] Соединение установлено, авторизация...\r\n");

    if (libssh2_userauth_password(session, SSH_USER, SSH_PASS) != 0) {
        char* errmsg = nullptr;
        int errlen = 0;
        libssh2_session_last_error(session, &errmsg, &errlen, 0);
        fail(L"Ошибка авторизации: " + Utf8ToWide(errmsg ? errmsg : "неизвестно"));
        libssh2_session_disconnect(session, "auth failed");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session);
    if (!channel) {
        fail(L"Не удалось открыть канал");
        libssh2_session_disconnect(session, "channel failed");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    AppendLog(L"[ИНФО] Выполняется команда:\r\n" + Utf8ToWide(cmd) + L"\r\n\r\n");

    if (libssh2_channel_exec(channel, cmd.c_str()) != 0) {
        fail(L"Не удалось выполнить команду на сервере");
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "exec failed");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    // Читаем stdout и stderr удалённого процесса (байты в UTF-8) и стримим в лог,
    // конвертируя в UTF-16 для отображения в EDIT-контроле.
    char buf[4096];
    ssize_t n;
    for (;;) {
        n = libssh2_channel_read(channel, buf, sizeof(buf) - 1);
        if (n > 0) {
            AppendLogUtf8(std::string(buf, n));
        }
        ssize_t nErr = libssh2_channel_read_stderr(channel, buf, sizeof(buf) - 1);
        if (nErr > 0) {
            AppendLogUtf8(std::string(buf, nErr));
        }
        if (libssh2_channel_eof(channel) && n <= 0 && nErr <= 0) break;
        if (n == LIBSSH2_ERROR_EAGAIN || nErr == LIBSSH2_ERROR_EAGAIN) {
            Sleep(50);
            continue;
        }
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) break;
    }

    int exitCode = libssh2_channel_get_exit_status(channel);
    AppendLog(L"\r\n[ИНФО] Процесс завершён, код возврата: " + std::to_wstring(exitCode) + L"\r\n");

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    libssh2_session_disconnect(session, "done");
    libssh2_session_free(session);
    closesocket(sock);
    libssh2_exit();
    WSACleanup();

    busyFlag = false;
    EnableWindow(btn1, TRUE);
    if (btn2) EnableWindow(btn2, TRUE);
}

// Поток кнопки "Выполнить" — собирает команду запуска N_m3u8DL-RE из аргументов
// и передаёт её в общее ядро RunSshExec.
void RunSshCommandThread(std::wstring arg1W, std::wstring arg2W) {
    const std::string arg1 = WideToUtf8(arg1W);
    std::string arg2 = WideToUtf8(arg2W);
    arg2 = ExtractKeyAfterColon(arg2); // берём только значение после ':' (сам ключ DRM)

    // Формируем команду:
    // N_m3u8DL-RE $1 -M format=mp4 --key $2 -sv worst --save-name "drm"
    //   --save-dir $HOME --live-pipe-mux --select-audio id="audio_aar=128000"
    std::string cmd =
        "rm -rf \"$HOME/drm\" && "
        "N_m3u8DL-RE " + ShellQuote(arg1) +
        " -M format=mp4 --key " + ShellQuote(arg2) +
        " -sv worst --save-name \"drm\" --save-dir \"$HOME\""
        " --live-pipe-mux --select-audio id=\"audio_aar=128000\"";

    RunSshExec(cmd, g_running, g_hRunBtn, g_hKillBtn);
}

// Поток кнопки "Остановить" — принудительно убивает N_m3u8DL-RE на сервере.
void KillCommandThread() {
    RunSshExec("killall -9 N_m3u8DL-RE", g_running, g_hRunBtn, g_hKillBtn);
}

// Кнопка "Воспроизвести": на сервере определяется, какой файл реально появился после N_m3u8DL-RE (drm.aar.ts, если аудио замьюксить в mp4 не удалось, иначе drm.ts), и через Kodi JSON-RPC (Player.Open) запускается его воспроизведение.
void PlayCommandThread() {
    std::string url = std::string("http://") + KODI_JSONRPC_HOST + ":" + std::to_string(KODI_JSONRPC_PORT) + "/jsonrpc";

    std::string cmd = std::string(R"CMD(FILE="$HOME/drm.aar.ts"
if [ ! -f "$FILE" ]; then FILE="$HOME/drm.ts"; fi
if [ ! -f "$FILE" ]; then echo ')CMD") + std::string((const char*)"Файл drm.aar.ts или drm.ts не найден в $HOME на сервере") + R"CMD(' >&2; exit 1; fi
curl -s -X POST -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":1,"method":"Player.Open","params":{"item":{"file":"'"$FILE"'"}}}' )CMD" + url;

    RunSshExec(cmd, g_playerBusy, g_hPlayBtn, g_hStopBtn);
}

// Кнопка "Остановить плеер": Kodi JSON-RPC Player.Stop останавливает текущее воспроизведение (не трогая сам процесс N_m3u8DL-RE).
void StopPlayCommandThread() {
    std::string url = std::string("http://") + KODI_JSONRPC_HOST + ":" + std::to_string(KODI_JSONRPC_PORT) + "/jsonrpc";

    std::string cmd = std::string(R"CMD(curl -s -X POST -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":1,"method":"Player.Stop","params":{"playerid":1}}' )CMD") + url;

    RunSshExec(cmd, g_playerBusy, g_hPlayBtn, g_hStopBtn);
}

// Ползунок громкости: Kodi JSON-RPC Application.SetVolume (0-100). В отличие
// от Play/Stop-плеера, здесь SSH не используется — запрос уходит напрямую
// с Windows-клиента на Kodi по HTTP (WinHTTP), это быстрее (не тратится время
// на SSH-хендшейк и авторизацию на каждое движение ползунка).
void SetVolumeThread(int volume) {
    g_volumeBusy = true;
    EnableWindow(g_hVolumeSlider, FALSE);

    AppendLog(L"[ИНФО] Установка громкости: " + std::to_wstring(volume) +
        L"% (прямой HTTP-запрос к Kodi JSON-RPC, без SSH)\r\n");

    std::string jsonBody =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Application.SetVolume\",\"params\":{\"volume\":"
        + std::to_string(volume) + "}}";

    std::wstring err;
    bool ok = KodiJsonRpcHttpPost(KODI_JSONRPC_LAN_HOST, KODI_JSONRPC_PORT, jsonBody, err);

    if (ok) {
        AppendLog(L"[ИНФО] Громкость установлена: " + std::to_wstring(volume) + L"%\r\n");
    } else {
        AppendLog(L"[ОШИБКА] " + err + L"\r\n");
    }

    g_volumeBusy = false;
    EnableWindow(g_hVolumeSlider, TRUE);
}


// ---------------------------------------------------------------------------
// Обработчик окна
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        auto mkLabel = [&](const wchar_t* text, int x, int y, int w, int id) {
            HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                x, y, w, 20, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
            return h;
        };
        auto mkEdit = [&](int x, int y, int w, int id, const wchar_t* placeholder = L"") {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", placeholder,
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                x, y, w, 24, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
            return h;
        };

        int labelW = 150, editX = 170, editW = 400, y = 15, rowH = 34;

        mkLabel(L"Аргумент $1 (URL/манифест):", 15, y, labelW, ID_STATIC_ARG1);
        g_hArg1 = mkEdit(editX, y - 2, editW, ID_EDIT_ARG1);
        y += rowH;

        mkLabel(L"Аргумент $2 (ключ DRM):", 15, y, labelW, ID_STATIC_ARG2);
        g_hArg2 = mkEdit(editX, y - 2, editW, ID_EDIT_ARG2);
        y += rowH;

        g_hRunBtn = CreateWindowW(L"BUTTON", L"Выполнить",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            editX, y, 150, 30, hwnd, (HMENU)ID_BUTTON_RUN, nullptr, nullptr);
        SendMessageW(g_hRunBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hKillBtn = CreateWindowW(L"BUTTON", L"Остановить",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            editX + 160, y, 150, 30, hwnd, (HMENU)ID_BUTTON_KILL, nullptr, nullptr);
        SendMessageW(g_hKillBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 40;

        g_hPlayBtn = CreateWindowW(L"BUTTON", L"Воспроизвести",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            editX, y, 150, 30, hwnd, (HMENU)ID_BUTTON_PLAY, nullptr, nullptr);
        SendMessageW(g_hPlayBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hStopBtn = CreateWindowW(L"BUTTON", L"Остановить плеер",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            editX + 160, y, 150, 30, hwnd, (HMENU)ID_BUTTON_STOP_PLAY, nullptr, nullptr);
        SendMessageW(g_hStopBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 40;

        mkLabel(L"Громкость плеера:", 15, y + 5, labelW, ID_STATIC_VOLUME);

        g_hVolumeSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            editX, y, editW - 60, 30, hwnd, (HMENU)ID_SLIDER_VOLUME, nullptr, nullptr);
        SendMessageW(g_hVolumeSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(g_hVolumeSlider, TBM_SETTICFREQ, 10, 0);
        SendMessageW(g_hVolumeSlider, TBM_SETPOS, TRUE, 50);

        g_hVolumeValueLabel = CreateWindowW(L"STATIC", L"50%",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            editX + editW - 50, y + 5, 50, 20, hwnd, (HMENU)ID_STATIC_VOLUME_VALUE, nullptr, nullptr);
        SendMessageW(g_hVolumeValueLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 45;

        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            15, y, editX + editW - 15, 260, hwnd, (HMENU)ID_EDIT_LOG, nullptr, nullptr);
        SendMessageW(g_hLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_BUTTON_RUN && !g_running) {
            std::wstring arg1 = GetEditText(g_hArg1);
            std::wstring arg2 = GetEditText(g_hArg2);

            SetWindowTextW(g_hLog, L"");
            std::thread(RunSshCommandThread, arg1, arg2).detach();
        }
        else if (LOWORD(wParam) == ID_BUTTON_KILL && !g_running) {
            SetWindowTextW(g_hLog, L"");
            std::thread(KillCommandThread).detach();
        }
        else if (LOWORD(wParam) == ID_BUTTON_PLAY && !g_playerBusy) {
            SetWindowTextW(g_hLog, L"");
            std::thread(PlayCommandThread).detach();
        }
        else if (LOWORD(wParam) == ID_BUTTON_STOP_PLAY && !g_playerBusy) {
            SetWindowTextW(g_hLog, L"");
            std::thread(StopPlayCommandThread).detach();
        }
        return 0;

    case WM_HSCROLL: {
        if ((HWND)lParam == g_hVolumeSlider) {
            int pos = (int)SendMessageW(g_hVolumeSlider, TBM_GETPOS, 0, 0);
            SetWindowTextW(g_hVolumeValueLabel, (std::to_wstring(pos) + L"%").c_str());

            // Отправляем на Kodi не на каждое промежуточное событие перетаскивания
            // (SB_THUMBTRACK), а только когда значение зафиксировано — это
            // избавляет от лавины SSH-подключений при перетаскивании ползунка.
            WORD code = LOWORD(wParam);
            if (code != SB_THUMBTRACK && !g_volumeBusy) {
                SetWindowTextW(g_hLog, L"");
                std::thread(SetVolumeThread, pos).detach();
            }
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    InitCommonControls();

    const wchar_t* CLASS_NAME = L"DrmLauncherWnd";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"N_m3u8DL-RE — запуск по SSH (192.168.8.45)",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 515,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
