#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <iostream>
#include <condition_variable>
#include "packet_sender.h"
#include "wasapi_capture.h"
#include "discovery_responder.h"
using namespace lanaudio;
namespace {
std::atomic<bool> running{true};
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
int wmain(int argc, wchar_t** argv) {
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
            try { captureLoop(sender, running); }
            catch (const std::exception& e) {
                std::cerr << "Recovering capture: " << e.what() << '\n';
                for (int i = 0; i < 5 && running.load(); ++i) Sleep(100);
            }
        }
        if (control.joinable()) control.join();
        std::cout << sender.stats() << '\n';
    }
    CoUninitialize(); closesocket(socket); WSACleanup(); return 0;
}
