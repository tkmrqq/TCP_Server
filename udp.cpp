#include "libs.h"
#include <filesystem>

struct UDPPacket {
    uint32_t seq;
    uint16_t length;
    char data[BUFFER_SIZE];
};

void sendUDPMessage(SOCKET sock, const sockaddr_in& addr, const std::string& msg) {
    sendto(sock, msg.c_str(), msg.size(), 0,
           (sockaddr*)&addr, sizeof(addr));
}

void handleUDPDownload(SOCKET sock, const sockaddr_in& clientAddr, const std::string& filename) {
    std::ifstream file("uploads/" + filename, std::ios::binary | std::ios::ate);
    if (!file) {
        sendUDPMessage(sock, clientAddr, "ERROR: File not found\n");
        return;
    }

    uint32_t fileSize = file.tellg();
    file.seekg(0);
    sendUDPMessage(sock, clientAddr, "READY " + std::to_string(fileSize) + "\n");

    char buffer[BUFFER_SIZE];
    const int maxRetries = 5;
    uint32_t expectedSeq = 0;

    while (file.read(buffer, BUFFER_SIZE) || file.gcount() > 0) {
        int chunkSize = file.gcount();
        UDPPacket packet;
        packet.seq = htonl(expectedSeq);
        packet.length = htons(static_cast<uint16_t>(chunkSize));
        memcpy(packet.data, buffer, chunkSize);

        bool ackReceived = false;
        for (int attempt = 0; attempt < maxRetries && !ackReceived; ++attempt) {
            sendto(sock, (char*)&packet, sizeof(packet.seq) + sizeof(packet.length) + chunkSize,
                   0, (sockaddr*)&clientAddr, sizeof(clientAddr));

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            timeval timeout{2, 0};

            if (select(0, &readSet, nullptr, nullptr, &timeout) > 0) {
                char ackBuffer[BUFFER_SIZE];
                sockaddr_in ackAddr;
                socklen_t addrLen = sizeof(ackAddr);
                int bytes = recvfrom(sock, ackBuffer, BUFFER_SIZE, 0, (sockaddr*)&ackAddr, &addrLen);

                if (bytes > 0 && ackAddr.sin_addr.s_addr == clientAddr.sin_addr.s_addr && ackAddr.sin_port == clientAddr.sin_port) {
                    std::string ack(ackBuffer, bytes);
                    ack = ack.substr(0, ack.find_last_not_of("\r\n") + 1);

                    if (ack.substr(0, 4) == "ACK ") {
                        try {
                            uint32_t ackSeq = std::stoul(ack.substr(4));
                            if (ackSeq == expectedSeq) {
                                ackReceived = true;
                                expectedSeq++;
                            }
                        } catch (...) {
                            // Ошибка парсинга номера ACK
                        }
                    }
                }
            }
        }

        if (!ackReceived) {
            std::cerr << "Max retries reached for packet " << expectedSeq << std::endl;
            break;
        }
    }

    // Отправка конечного пакета
    UDPPacket endPacket;
    endPacket.seq = htonl(UINT32_MAX);
    endPacket.length = htons(0);
    sendto(sock, (char*)&endPacket, sizeof(endPacket.seq) + sizeof(endPacket.length),
           0, (sockaddr*)&clientAddr, sizeof(clientAddr));

    file.close();
}

void handleUDPCommand(SOCKET sock) {
    sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    char buffer[BUFFER_SIZE];

    while (true) {
        int bytes = recvfrom(sock, buffer, BUFFER_SIZE, 0,
                             (sockaddr*)&clientAddr, &addrLen);
        if (bytes <= 0) continue;

        std::string command(buffer, bytes);
        std::cout << "[UDP]:" << command << std::endl;

        if (command.find("UDP_DOWNLOAD ") == 0) {
            std::string filename = command.substr(13);
            filename.erase(filename.find_last_not_of(" \n\r\t") + 1); // Trim whitespace
            handleUDPDownload(sock, clientAddr, filename);
        }
        else {
            sendUDPMessage(sock, clientAddr, "UNKNOWN COMMAND\n");
        }
    }
}

void handleUDPServer() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed" << std::endl;
        return;
    }

    // Создаем папку uploads если ее нет
    std::filesystem::create_directories("uploads");

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(UDP_PORT);

    if (bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed" << std::endl;
        closesocket(sock);
        return;
    }

    std::cout << "[UDP] Server started on port " << UDP_PORT << std::endl;
    handleUDPCommand(sock);
    closesocket(sock);
}