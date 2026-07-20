// DrmLauncher - Win32 GUI äëÿ çàïóñêà N_m3u8DL-RE íà óäàë¸ííîì ñåðâåðå ïî SSH
//
// Èñïîëüçóåò libssh2 (exec-êàíàë), êàê â KodiGui, âìåñòî âûçîâà âíåøíåãî ssh.exe.
//
// Þíèêîäíàÿ (UTF-16) âåðñèÿ: âñå îêíà/êîíòðîëû ñîçäàíû ÷åðåç *W API, òåêñò â
// ýëåìåíòàõ óïðàâëåíèÿ õðàíèòñÿ êàê std::wstring. Ïîñêîëüêó SSH-êîìàíäà è âûâîä
// óäàë¸ííîãî ïðîöåññà — ýòî áàéòû â êîäèðîâêå UTF-8 (ñòàíäàðò äëÿ POSIX shell
// è Linux/*BSD êîíñîëè), íà ãðàíèöå "GUI <-> SSH" ñäåëàíà ÿâíàÿ êîíâåðòàöèÿ
// UTF-16 <-> UTF-8 ÷åðåç WinAPI (Utf8ToWide / WideToUtf8).
//
// Ñáîðêà (MinGW):
//   g++ -O2 -std=c++17 main.cpp -o DrmLauncher.exe -lssh2 -lws2_32 -mwindows -municode
//
// Ñáîðêà (MSVC, èç "x64 Native Tools Command Prompt"):
//   cl /EHsc /utf-8 /std:c++17 main.cpp libssh2.lib ws2_32.lib user32.lib gdi32.lib /Fe:DrmLauncher.exe
// (ôëàã /utf-8 îáÿçàòåëåí: ôàéë ñîõðàí¸í â UTF-8, áåç íåãî MSVC ìîæåò
//  íåâåðíî èíòåðïðåòèðîâàòü êèðèëëèöó â ñòðîêîâûõ ëèòåðàëàõ)
//
// Âàæíî: òî÷êà âõîäà òåïåðü wWinMain (þíèêîäíàÿ), ïîýòîìó äëÿ MinGW îáÿçàòåëåí
// ôëàã ëèíêîâêè -municode — èíà÷å áóäåò îøèáêà "undefined reference to WinMain"
// (ëèíêåð ïî óìîë÷àíèþ èùåò WinMainCRTStartup, à íå wWinMainCRTStartup).
// Äëÿ MSVC íè÷åãî äîïîëíèòåëüíî óêàçûâàòü íå íóæíî — êîìïèëÿòîð ñàì ïîäñòàâèò
// íóæíûé CRT-ñòàðòàï ïîä ñèãíàòóðó wWinMain.
//
// Íóæíà áèáëèîòåêà libssh2 (çàãîëîâêè + .lib/.a). Ïðîùå âñåãî ïîñòàâèòü ÷åðåç vcpkg:
//   vcpkg install libssh2:x64-windows
// è óêàçàòü êîìïèëÿòîðó include/lib ïóòè vcpkg (ñì. README.txt ðÿäîì).

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include "resource.h"
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <libssh2.h>

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wininet.lib")

// ---------------------------------------------------------------------------
// Êîíñòàíòû ïîäêëþ÷åíèÿ (çàäàíû ïî óñëîâèþ çàäà÷è)
// ---------------------------------------------------------------------------
static const char* SSH_HOST = "192.168.8.45";
static const char* SSH_USER = "pi";
static const char* SSH_PASS = "639639";
static const int   SSH_PORT = 22;

// Kodi JSON-RPC (HTTP), êàê â KodiGui (MainWindow::sendJsonRpc /
// MainWindow::postSetVolume, âåòêà tab_match_vers2) — òîò æå õîñò,
// îòäåëüíûé ïîðò âåá-èíòåðôåéñà Kodi.
static const char* KODI_JSONRPC_HOST = "192.168.8.45";
static const int   KODI_JSONRPC_PORT = 8081;
static const char* KODI_JSONRPC_PATH = "/jsonrpc";

// ---------------------------------------------------------------------------
// Èäåíòèôèêàòîðû ýëåìåíòîâ óïðàâëåíèÿ
// ---------------------------------------------------------------------------
enum ControlId {
    ID_EDIT_ARG1 = 1001,   // $1  (íàïðèìåð, ññûëêà íà m3u8 / ìàíèôåñò)
    ID_EDIT_ARG2,          // $2  (êëþ÷ DRM)
    ID_BUTTON_RUN,
    ID_BUTTON_KILL,        // "Îñòàíîâèòü" — killall -9 N_m3u8DL-RE íà ñåðâåðå
    ID_EDIT_LOG,
    ID_STATIC_ARG1,
    ID_STATIC_ARG2,
    ID_SLIDER_VOLUME,      // ïîëçóíîê ãðîìêîñòè (Kodi Application.SetVolume)
    ID_STATIC_VOLUME
};

// ---------------------------------------------------------------------------
// Ãëîáàëüíûå õåíäëû
// ---------------------------------------------------------------------------
HWND g_hArg1, g_hArg2, g_hLog, g_hRunBtn, g_hKillBtn;
HWND g_hVolSlider, g_hVolLabel;
std::atomic<bool> g_running{false};
std::mutex g_logMutex;

// ---------------------------------------------------------------------------
// Êîíâåðòàöèÿ UTF-8 <-> UTF-16 (WinAPI). SSH-êîìàíäà è âûâîä óäàë¸ííîãî øåëëà —
// UTF-8; íàòèâíûå Win32 *W êîíòðîëû — UTF-16.
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

// Ïîòîêîáåçîïàñíûé âûâîä â ëîã (ëîã ìíîãîñòðî÷íûé edit control)
void AppendLog(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

// Ïåðåãðóçêà äëÿ óäîáñòâà ëîãèðîâàíèÿ áàéòîâ, ïðèøåäøèõ ñ óäàë¸ííîãî õîñòà
// â UTF-8 (stdout/stderr ïðîöåññà íà Linux/*BSD).
void AppendLogUtf8(const std::string& utf8text) {
    AppendLog(Utf8ToWide(utf8text));
}

// Äîñòà¸ì òåêñò èç edit control â std::wstring
std::wstring GetEditText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::vector<wchar_t> buf(len + 1);
    GetWindowTextW(h, buf.data(), len + 1);
    return std::wstring(buf.data());
}

// ---------------------------------------------------------------------------
// Ýêðàíèðîâàíèå çíà÷åíèÿ äëÿ îäèíî÷íûõ êàâû÷åê â POSIX shell:  it's -> 'it'"'"'s'
// Ðàáîòàåò ñ UTF-8 áàéòàìè: îäèíàðíàÿ êàâû÷êà (0x27) íå ìîæåò âñòðåòèòüñÿ
// êàê áàéò ïðîäîëæåíèÿ ìíîãîáàéòîâîé UTF-8 ïîñëåäîâàòåëüíîñòè, ïîýòîìó
// ïîáàéòîâûé ðàçáîð áåçîïàñåí.
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
// Ñîáñòâåííî ðàáîòà ñ SSH: ïîäêëþ÷åíèå, àóòåíòèôèêàöèÿ ïî ïàðîëþ, exec, ñòðèì âûâîäà.
// Îáùåå ÿäðî äëÿ ëþáîé êîìàíäû íà ñåðâåðå — èñïîëüçóåòñÿ è êíîïêîé "Âûïîëíèòü",
// è êíîïêîé "Îñòàíîâèòü" (killall), ÷òîáû íå äóáëèðîâàòü connect/auth/read-öèêë.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Set Kodi volume (Application.SetVolume) via HTTP JSON-RPC.
// Same call as KodiGui's MainWindow::sendJsonRpc / postSetVolume
// (branch tab_match_vers2), but done with WinINet instead of Qt's
// QNetworkAccessManager, since this app has no Qt. Kodi's web server
// with JSON-RPC must be enabled in Kodi settings.
// ---------------------------------------------------------------------------
void PostSetVolume(int volume) {
    std::string body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Application.SetVolume\","
        "\"params\":{\"volume\":" + std::to_string(volume) + "}}";

    HINTERNET hInet = InternetOpenA("DrmLauncher", INTERNET_OPEN_TYPE_DIRECT,
                                     nullptr, nullptr, 0);
    if (!hInet) {
        AppendLogUtf8("[volume] InternetOpen failed\r\n");
        return;
    }

    HINTERNET hConn = InternetConnectA(hInet, KODI_JSONRPC_HOST, (INTERNET_PORT)KODI_JSONRPC_PORT,
                                        nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        AppendLogUtf8("[volume] could not connect to Kodi JSON-RPC\r\n");
        InternetCloseHandle(hInet);
        return;
    }

    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", KODI_JSONRPC_PATH, nullptr, nullptr,
                                       nullptr, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hReq) {
        AppendLogUtf8("[volume] HttpOpenRequest failed\r\n");
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);
        return;
    }

    static const char* headers = "Content-Type: application/json\r\n";
    BOOL ok = HttpSendRequestA(hReq, headers, (DWORD)strlen(headers),
                                (LPVOID)body.data(), (DWORD)body.size());
    if (!ok) {
        AppendLogUtf8("[volume] Application.SetVolume request failed\r\n");
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hInet);
}

// Runs PostSetVolume() on a worker thread so the slider never blocks the UI.
void SetVolumeThread(int volume) {
    PostSetVolume(volume);
}

void RunSshExec(const std::string& cmd) {
    const std::string host = SSH_HOST;

    g_running = true;
    EnableWindow(g_hRunBtn, FALSE);
    EnableWindow(g_hKillBtn, FALSE);

    auto fail = [&](const std::wstring& msg) {
        AppendLog(L"[ÎØÈÁÊÀ] " + msg + L"\r\n");
        g_running = false;
        EnableWindow(g_hRunBtn, TRUE);
        EnableWindow(g_hKillBtn, TRUE);
    };

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fail(L"WSAStartup íå âûïîëíåí");
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fail(L"Íå óäàëîñü ñîçäàòü ñîêåò");
        WSACleanup();
        return;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(SSH_PORT);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        fail(L"Íå óäàëîñü ðàçðåøèòü àäðåñ ñåðâåðà: " + Utf8ToWide(host));
        closesocket(sock);
        WSACleanup();
        return;
    }

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fail(L"Íå óäàëîñü ïîäêëþ÷èòüñÿ ê " + Utf8ToWide(host) + L":" + Utf8ToWide(portStr));
        freeaddrinfo(res);
        closesocket(sock);
        WSACleanup();
        return;
    }
    freeaddrinfo(res);

    if (libssh2_init(0) != 0) {
        fail(L"libssh2_init íå âûïîëíåí");
        closesocket(sock);
        WSACleanup();
        return;
    }

    LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session) {
        fail(L"Íå óäàëîñü ñîçäàòü SSH-ñåññèþ");
        closesocket(sock);
        WSACleanup();
        return;
    }

    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, sock) != 0) {
        fail(L"SSH handshake íå âûïîëíåí");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    AppendLog(L"[ÈÍÔÎ] Ñîåäèíåíèå óñòàíîâëåíî, àâòîðèçàöèÿ...\r\n");

    if (libssh2_userauth_password(session, SSH_USER, SSH_PASS) != 0) {
        char* errmsg = nullptr;
        int errlen = 0;
        libssh2_session_last_error(session, &errmsg, &errlen, 0);
        fail(L"Îøèáêà àâòîðèçàöèè: " + Utf8ToWide(errmsg ? errmsg : "íåèçâåñòíî"));
        libssh2_session_disconnect(session, "auth failed");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session);
    if (!channel) {
        fail(L"Íå óäàëîñü îòêðûòü êàíàë");
        libssh2_session_disconnect(session, "channel failed");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    AppendLog(L"[ÈÍÔÎ] Âûïîëíÿåòñÿ êîìàíäà:\r\n" + Utf8ToWide(cmd) + L"\r\n\r\n");

    if (libssh2_channel_exec(channel, cmd.c_str()) != 0) {
        fail(L"Íå óäàëîñü âûïîëíèòü êîìàíäó íà ñåðâåðå");
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "exec failed");
        libssh2_session_free(session);
        closesocket(sock);
        WSACleanup();
        return;
    }

    // ×èòàåì stdout è stderr óäàë¸ííîãî ïðîöåññà (áàéòû â UTF-8) è ñòðèìèì â ëîã,
    // êîíâåðòèðóÿ â UTF-16 äëÿ îòîáðàæåíèÿ â EDIT-êîíòðîëå.
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
    AppendLog(L"\r\n[ÈÍÔÎ] Ïðîöåññ çàâåðø¸í, êîä âîçâðàòà: " + std::to_wstring(exitCode) + L"\r\n");

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

// Ïîòîê êíîïêè "Âûïîëíèòü" — ñîáèðàåò êîìàíäó çàïóñêà N_m3u8DL-RE èç àðãóìåíòîâ
// è ïåðåäà¸ò å¸ â îáùåå ÿäðî RunSshExec.
void RunSshCommandThread(std::wstring arg1W, std::wstring arg2W) {
    const std::string arg1 = WideToUtf8(arg1W);
    const std::string arg2 = WideToUtf8(arg2W);

    // Ôîðìèðóåì êîìàíäó:
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

// Ïîòîê êíîïêè "Îñòàíîâèòü" — ïðèíóäèòåëüíî óáèâàåò N_m3u8DL-RE íà ñåðâåðå.
void KillCommandThread() {
    RunSshExec("killall -9 N_m3u8DL-RE");
}

// ---------------------------------------------------------------------------
// Îáðàáîò÷èê îêíà
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

        mkLabel(L"Àðãóìåíò $1 (URL/ìàíèôåñò):", 15, y, labelW, ID_STATIC_ARG1);
        g_hArg1 = mkEdit(editX, y - 2, editW, ID_EDIT_ARG1);
        y += rowH;

        mkLabel(L"Àðãóìåíò $2 (êëþ÷ DRM):", 15, y, labelW, ID_STATIC_ARG2);
        g_hArg2 = mkEdit(editX, y - 2, editW, ID_EDIT_ARG2);
        y += rowH;

        g_hRunBtn = CreateWindowW(L"BUTTON", L"Âûïîëíèòü",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            editX, y, 150, 30, hwnd, (HMENU)ID_BUTTON_RUN, nullptr, nullptr);
        SendMessageW(g_hRunBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hKillBtn = CreateWindowW(L"BUTTON", L"Îñòàíîâèòü",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            editX + 160, y, 150, 30, hwnd, (HMENU)ID_BUTTON_KILL, nullptr, nullptr);
        SendMessageW(g_hKillBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 45;

        // Ïîëçóíîê ãðîìêîñòè Kodi (Application.SetVolume ÷åðåç JSON-RPC),
        // ëîãèêà êàê â KodiGui (MainWindow::postSetVolume, âåòêà tab_match_vers2),
        // ñì. PostSetVolume()/SetVolumeThread() âûøå.
        mkLabel(L"Ãðîìêîñòü Kodi:", 15, y, labelW, ID_STATIC_VOLUME);
        g_hVolSlider = CreateWindowExW(0, TRACKBAR_CLASS, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            editX, y - 2, 260, 30, hwnd, (HMENU)(INT_PTR)ID_SLIDER_VOLUME, nullptr, nullptr);
        SendMessageW(g_hVolSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(g_hVolSlider, TBM_SETTICFREQ, 10, 0);
        SendMessageW(g_hVolSlider, TBM_SETPOS, TRUE, 50);

        g_hVolLabel = mkLabel(L"50%", editX + 270, y, 50, 0);
        y += rowH;


        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            15, y, editX + editW - 15, 260, hwnd, (HMENU)ID_EDIT_LOG, nullptr, nullptr);
        SendMessageW(g_hLog, WM_SETFONT, (WPARAM)hFont, TRUE);

        return 0;
    }

    case WM_HSCROLL: {
        if ((HWND)lParam == g_hVolSlider) {
            int pos = (int)SendMessageW(g_hVolSlider, TBM_GETPOS, 0, 0);
            SetWindowTextW(g_hVolLabel, (std::to_wstring(pos) + L"%").c_str());
            std::thread(SetVolumeThread, pos).detach();
        }
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

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"N_m3u8DL-RE — çàïóñê ïî SSH (192.168.8.45)",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 470,
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
