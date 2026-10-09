#pragma once
#include <winsock2.h>
#include <windows.h>
#include "../shared/discovery_protocol.h"
#include <iostream>

namespace lanaudio {
class DiscoveryResponder {
public:
    DiscoveryResponder(uint16_t listenPort, uint16_t audioPort) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            std::cerr << "Discovery socket failed: " << WSAGetLastError() << "; manual connection remains available.\n";
            return;
        }
        DWORD sendTimeout = 10;
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
        BOOL exclusive = TRUE;
        setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(listenPort);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            std::cerr << "Discovery bind failed: " << WSAGetLastError() << "; manual connection remains available.\n";
            closesocket(socket_); socket_ = INVALID_SOCKET;
        }
        std::array<char, DiscoveryNameBytes> utf8Name{};
        size_t nameLength = 0;
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD count = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(name, &count)) {
            const auto length = WideCharToMultiByte(CP_UTF8, 0, name, int(count), utf8Name.data(), int(utf8Name.size()), nullptr, nullptr);
            if (length > 0) nameLength = size_t(length);
        }
        replyLength_ = writeDiscoveryReply(reply_.data(), 0, audioPort, std::string_view(utf8Name.data(), nameLength));
    }
    ~DiscoveryResponder() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }
    SOCKET socket() const { return socket_; }
    void respond() {
        std::array<uint8_t, DiscoveryQueryBytes + 1> query{};
        sockaddr_in from{}; int addressBytes = sizeof(from);
        const int count = recvfrom(socket_, reinterpret_cast<char*>(query.data()), int(query.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &addressBytes);
        uint32_t nonce;
        if (count <= 0 || !parseDiscoveryQuery(query.data(), size_t(count), nonce)) return;
        put32(reply_.data() + 5, nonce);
        sendto(socket_, reinterpret_cast<const char*>(reply_.data()), int(replyLength_), 0, reinterpret_cast<sockaddr*>(&from), addressBytes);
    }
private:
    SOCKET socket_ = INVALID_SOCKET;
    std::array<uint8_t, DiscoveryHeaderBytes + DiscoveryNameBytes> reply_{};
    size_t replyLength_ = 0;
};
}
