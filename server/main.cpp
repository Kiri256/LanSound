#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <iostream>
#include <condition_variable>
#include <string>
#include "packet_sender.h"
#include "wasapi_capture.h"
#include "discovery_responder.h"
using namespace lanaudio;
namespace {
std::atomic<bool> running{true};
std::atomic<bool> paused{false};
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT TrayPause = 1001;
constexpr UINT TrayAutostart = 1002;
constexpr UINT TrayExit = 1003;

bool autostartEnabled() {
    HKEY key = nullptr; DWORD type = 0, size = 0;
    const auto result = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key);
    if (result != ERROR_SUCCESS) return false;
    const bool present = RegQueryValueExW(key, L"LanAudioServer", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key); return present;
}

void setAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t path[MAX_PATH]{}; const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length && length < MAX_PATH) {
            std::wstring value = L"\"" + std::wstring(path, length) + L"\"";
            RegSetValueExW(key, L"LanAudioServer", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
        }
    } else RegDeleteValueW(key, L"LanAudioServer");
    RegCloseKey(key);
}

void addFirewallRules(int port, int discoveryPort) {
    std::wstring commandLine = L"netsh.exe advfirewall firewall add rule name=\"LanAudio Server\" dir=in action=allow protocol=UDP localport=" +
        std::to_wstring(port) + L"," + std::to_wstring(discoveryPort) + L" profile=any enable=yes";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        WaitForSingleObject(process.hProcess, 3000);
        TerminateProcess(process.hProcess, 0);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
}

LRESULT CALLBACK trayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == TrayMessage && lParam == WM_RBUTTONUP) {
        POINT point{}; GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (paused.load() ? MF_CHECKED : 0), TrayPause, paused.load() ? L"继续播放" : L"暂停播放");
        AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), TrayAutostart, L"开机自启动");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, TrayExit, L"退出");
        SetForegroundWindow(window);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu); PostMessageW(window, WM_NULL, 0, 0); return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == TrayPause) paused.store(!paused.load());
        else if (LOWORD(wParam) == TrayAutostart) setAutostart(!autostartEnabled());
        else if (LOWORD(wParam) == TrayExit) { running.store(false); PostMessageW(window, WM_CLOSE, 0, 0); }
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { Shell_NotifyIconW(NIM_DELETE, reinterpret_cast<NOTIFYICONDATAW*>(GetWindowLongPtrW(window, GWLP_USERDATA))); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

void trayLoop() {
    const wchar_t className[] = L"LanAudioServerTray";
    WNDCLASSW klass{}; klass.lpfnWndProc = trayWindowProc; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = className;
    RegisterClassW(&klass);
    HWND window = CreateWindowExW(0, className, L"LanAudio Server", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, klass.hInstance, nullptr);
    if (!window) return;
    NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon); icon.hWnd = window; icon.uID = 1; icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = TrayMessage; icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wcscpy_s(icon.szTip, L"LanAudio 服务端");
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&icon)); Shell_NotifyIconW(NIM_ADD, &icon);
    while (running.load()) {
        MSG message; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(100);
    }
    if (IsWindow(window)) DestroyWindow(window);
    UnregisterClassW(className, klass.hInstance);
}
template<class Receive> void receiveBatch(SOCKET socket, int limit, Receive receive) {
    // The control thread is the sole receiver. FIONREAD allows bounded draining
    // without making the socket's concurrent audio sends nonblocking.
    for (int i = 0; i < limit; ++i) {
        receive();
        u_long available = 0;
        if (ioctlsocket(socket, FIONREAD, &available) == SOCKET_ERROR || !available) break;
    }
}
BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) { running.store(false); return TRUE; }
    return FALSE;
}
}
int runServer(int argc, wchar_t** argv) {
    if (argc > 3) { std::wcerr << L"Usage: lan-audio-server.exe [audio-port] [discovery-port]\n"; return 2; }
    const int port = argc >= 2 ? _wtoi(argv[1]) : 30010;
    const int discoveryPort = argc == 3 ? _wtoi(argv[2]) : DiscoveryPort;
    if (port < 1 || port > 65535 || discoveryPort < 1 || discoveryPort > 65535 || port == discoveryPort) return 2;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa)) return 1;
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) { WSACleanup(); return 1; }
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(uint16_t(port));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        std::cerr << "UDP bind failed: " << WSAGetLastError() << '\n'; closesocket(socket); WSACleanup(); return 1;
    }
    DWORD receiveTimeout = 200, sendTimeout = 10;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    addFirewallRules(port, discoveryPort);
    std::thread tray(trayLoop);
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { closesocket(socket); WSACleanup(); return 1; }
    std::cout << "LanAudio server v" << LANAUDIO_VERSION_STRING << "; audio UDP 0.0.0.0:" << port
              << "; waiting for protocol-v" << unsigned(Version) << " Android client. Ctrl+C to stop.\n";
    {
        DiscoveryResponder discovery{uint16_t(discoveryPort), uint16_t(port)};
        if (discovery.socket() != INVALID_SOCKET) std::cout << "Discovery UDP 0.0.0.0:" << discoveryPort << " (all IPv4 interfaces)" << std::endl;
        PacketSender sender(socket);
        bool connected = false;
        std::mutex connectionMutex;
        std::condition_variable connectionReady;
        std::thread control([&] {
            std::array<uint8_t, 32> bytes{};
            while (running.load()) {
                fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable);
                if (discovery.socket() != INVALID_SOCKET) FD_SET(discovery.socket(), &readable);
                timeval timeout{0, 200000};
                if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
                if (FD_ISSET(socket, &readable)) {
                    receiveBatch(socket, 16, [&] {
                        sockaddr_in from{}; int size = sizeof(from);
                        const int count = recvfrom(socket, reinterpret_cast<char*>(bytes.data()), int(bytes.size()), 0, reinterpret_cast<sockaddr*>(&from), &size);
                        if (count == 5 && bytes[0]=='L' && bytes[1]=='A' && bytes[2]=='H' && bytes[3]=='I' && bytes[4]==Version) {
                            sender.setTarget(from);
                            const uint8_t ack[] = {'L','A','O','K',Version};
                            sendto(socket, reinterpret_cast<const char*>(ack), sizeof(ack), 0, reinterpret_cast<sockaddr*>(&from), size);
                            { std::lock_guard<std::mutex> lock(connectionMutex); connected = true; }
                            connectionReady.notify_one();
                        }
                    });
                }
                if (discovery.socket() != INVALID_SOCKET && FD_ISSET(discovery.socket(), &readable))
                    receiveBatch(discovery.socket(), 8, [&] { discovery.respond(); });
            }
        });
        {
            std::unique_lock<std::mutex> lock(connectionMutex);
            while (running.load() && !connected)
                connectionReady.wait_for(lock, std::chrono::milliseconds(200));
        }
        while (running.load()) {
            if (paused.load()) { Sleep(100); continue; }
            try { captureLoop(sender, running, paused); }
            catch (const std::exception& e) {
                std::cerr << "Recovering capture: " << e.what() << '\n';
                for (int i = 0; i < 5 && running.load(); ++i) Sleep(100);
            }
        }
        if (control.joinable()) control.join();
        std::cout << sender.stats() << '\n';
    }
    running.store(false); if (tray.joinable()) tray.join();
    restoreDefaultOutput();
    CoUninitialize(); closesocket(socket); WSACleanup(); return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return runServer(__argc, __wargv); }
