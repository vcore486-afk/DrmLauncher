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
//   g++ -O2 -std=c++17 main.cpp -o DrmLauncher.exe -lssh2 -lws2_32 -mwindows -municode
//
// Сборка (MSVC, из "x64 Native Tools Command Prompt"):
//   cl /EHsc /utf-8 /std:c++17 main.cpp libssh2.lib ws2_32.lib user32.lib gdi32.lib /Fe:DrmLauncher.exe
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

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Константы подключения (заданы по условию задачи)
// ---------------------------------------------------------------------------
static const char* SSH_HOST = "192.168.8.45";
static const char* SSH_USER = "pi";
static const char* SSH_PASS = "639639";
static const int   SSH_PORT = 22;

// ---------------------------------------------------------------------------
// Идентификаторы элементов управления
// ---------------------------------------------------------------------------
enum ControlId {
    ID_EDIT_ARG1 = 1001,   // $1  (например, ссылка на m3u8 / манифест)
    ID_EDIT_ARG2,          // $2  (ключ DRM)
    ID_BUTTON_RUN,
    ID_BUTTON_KILL,        // "Остановить" — killall -9 N_m3u8DL-RE на сервере
    ID_EDIT_LOG,
    ID_STATIC_ARG1,
    ID_STATIC_ARG2
};

// ---------------------------------------------------------------------------
// Глобальные хендлы
// ---------------------------------------------------------------------------
HWND g_hArg1, g_hArg2, g_hLog, g_hRunBtn, g_hKillBtn;
std::atomic<bool> g_running{false};
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
// Собственно работа с SSH: подключение, аутентификация по паролю, exec, стрим вывода.
// Общее ядро для любой команды на сервере — используется и кнопкой "Выполнить",
// и кнопкой "Остановить" (killall), чтобы не дублировать connect/auth/read-цикл.
// ---------------------------------------------------------------------------
void RunSshExec(const std::string& cmd) {
    const std::string host = SSH_HOST;

    g_running = true;
    EnableWindow(g_hRunBtn, FALSE);
    EnableWindow(g_hKillBtn, FALSE);

    auto fail = [&](const std::wstring& msg) {
        AppendLog(L"[ОШИБКА] " + msg + L"\r\n");
        g_running = false;
        EnableWindow(g_hRunBtn, TRUE);
        EnableWindow(g_hKillBtn, TRUE);
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

    g_running = false;
    EnableWindow(g_hRunBtn, TRUE);
    EnableWindow(g_hKillBtn, TRUE);
}

// Поток кнопки "Выполнить" — собирает команду запуска N_m3u8DL-RE из аргументов
// и передаёт её в общее ядро RunSshExec.
void RunSshCommandThread(std::wstring arg1W, std::wstring arg2W) {
    const std::string arg1 = WideToUtf8(arg1W);
    const std::string arg2 = WideToUtf8(arg2W);

    // Формируем команду:
    // N_m3u8DL-RE $1 -M format=mp4 --key $2 -sv worst --save-name "drm"
    //   --save-dir $HOME --live-pipe-mux --select-audio id="audio_aar=128000"
    std::string cmd =
        "rm -rf \"$HOME/drm\" && "
        "N_m3u8DL-RE " + ShellQuote(arg1) +
        " -M format=mp4 --key " + ShellQuote(arg2) +
        " -sv worst --save-name \"drm\" --save-dir \"$HOME\""
        " --live-pipe-mux --select-audio id=\"audio_aar=128000\"";

    RunSshExec(cmd);
}

// Поток кнопки "Остановить" — принудительно убивает N_m3u8DL-RE на сервере.
void KillCommandThread() {
    RunSshExec("killall -9 N_m3u8DL-RE");
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
        return 0;

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
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 430,
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