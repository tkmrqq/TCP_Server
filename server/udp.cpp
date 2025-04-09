#include "../libs.h"
#include <filesystem>

struct UDPPacket {
    uint32_t seq;
    uint16_t length;
    char data[BUFFER_SIZE];
};

void sendUDPMessage(SOCKET sock, const sockaddr_in &addr, const std::string &msg) {
    sendto(sock, msg.c_str(), msg.size(), 0,
           (sockaddr *) &addr, sizeof(addr));
}

void handleUDPDownload(SOCKET sock, const sockaddr_in &clientAddr, const std::string &filename) {
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
            sendto(sock, (char *) &packet, sizeof(packet.seq) + sizeof(packet.length) + chunkSize,
                   0, (sockaddr *) &clientAddr, sizeof(clientAddr));

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            timeval timeout{2, 0};

            if (select(0, &readSet, nullptr, nullptr, &timeout) > 0) {
                char ackBuffer[BUFFER_SIZE];
                sockaddr_in ackAddr;
                socklen_t addrLen = sizeof(ackAddr);
                int bytes = recvfrom(sock, ackBuffer, BUFFER_SIZE, 0, (sockaddr *) &ackAddr, &addrLen);

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
    sendto(sock, (char *) &endPacket, sizeof(endPacket.seq) + sizeof(endPacket.length),
           0, (sockaddr *) &clientAddr, sizeof(clientAddr));

    file.close();
}

void handleUDPTime(int sock, sockaddr_in addr) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string timeStr = ctime(&now);
    timeStr.pop_back();
    timeStr += '\n';
    sendUDPMessage(sock, addr, timeStr);
}

void handleUDPUpload(SOCKET sock, const sockaddr_in &clientAddr, const std::string &filename, const std::string &sizeStr) {
    uint32_t fileSize;
    try {
        fileSize = std::stoul(sizeStr);
    } catch (...) {
        sendUDPMessage(sock, clientAddr, "ERROR: Invalid file size\n");
        return;
    }

    // Проверяем доступное место
    std::error_code ec;
    auto space = std::filesystem::space("uploads", ec);
    if (ec || space.available < fileSize) {
        sendUDPMessage(sock, clientAddr, "ERROR: Not enough disk space\n");
        return;
    }

    // Отправляем подтверждение
    sendUDPMessage(sock, clientAddr, "READY\n");

    // Открываем файл для записи
    std::ofstream file("uploads/" + filename, std::ios::binary);
    if (!file) {
        sendUDPMessage(sock, clientAddr, "ERROR: Can't create file\n");
        return;
    }

    uint32_t expectedSeq = 0;
    uint32_t totalReceived = 0;
    bool transferComplete = false;
    auto transferStart = std::chrono::steady_clock::now();

    while (!transferComplete && totalReceived < fileSize) {
        char packetBuffer[BUFFER_SIZE];
        sockaddr_in senderAddr;
        socklen_t senderLen = sizeof(senderAddr);

        // Получаем пакет с таймаутом
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout{10, 0};// 10 секунд таймаут

        if (select(sock + 1, &readSet, nullptr, nullptr, &timeout) <= 0) {
            std::cerr << "Timeout waiting for packet " << expectedSeq << std::endl;
            break;
        }

        int bytes = recvfrom(sock, packetBuffer, sizeof(packetBuffer), 0,
                             (sockaddr *) &senderAddr, &senderLen);

        // Проверяем что пакет от нужного клиента
        if (senderAddr.sin_addr.s_addr != clientAddr.sin_addr.s_addr ||
            senderAddr.sin_port != clientAddr.sin_port) {
            continue;
        }

        // Проверяем минимальный размер пакета
        if (bytes < 6) continue;

        // Разбираем заголовок пакета
        uint32_t packetSeq = ntohl(*reinterpret_cast<uint32_t *>(packetBuffer));
        uint16_t chunkSize = ntohs(*reinterpret_cast<uint16_t *>(packetBuffer + 4));

        // Проверяем пакет завершения
        if (packetSeq == UINT32_MAX) {
            transferComplete = true;
            sendUDPMessage(sock, clientAddr, "ACK " + std::to_string(packetSeq) + "\n");
            break;
        }

        // Проверяем порядковый номер
        if (packetSeq == expectedSeq) {
            // Проверяем корректность размера данных
            if (bytes - 6 != chunkSize || totalReceived + chunkSize > fileSize) {
                std::cerr << "Invalid chunk size: " << chunkSize << std::endl;
                continue;
            }

            // Записываем данные в файл
            file.write(packetBuffer + 6, chunkSize);
            totalReceived += chunkSize;
            expectedSeq++;

            // Выводим прогресс
            std::cout << "\rReceived: " << totalReceived << "/" << fileSize
                      << " (" << (totalReceived * 100 / fileSize) << "%)" << std::flush;
        }

        // Отправляем подтверждение (ACK)
        sendUDPMessage(sock, clientAddr, "ACK " + std::to_string(packetSeq) + "\n");
    }

    file.close();

    // Проверяем успешность завершения
    if (transferComplete && totalReceived == fileSize) {
        auto duration = std::chrono::steady_clock::now() - transferStart;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        double speed = (totalReceived / 1024.0 / 1024.0) / (ms / 1000.0);// MB/s

        std::cout << "\nUpload complete: " << filename
                  << " (" << totalReceived << " bytes)"
                  << " in " << ms << " ms (" << speed << " MB/s)" << std::endl;
    } else {
        std::cerr << "\nUpload failed or incomplete: " << totalReceived << "/" << fileSize << std::endl;
        std::filesystem::remove("uploads/" + filename);
    }
}

void handleUDPCommand(SOCKET sock) {
    sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    char buffer[BUFFER_SIZE];

    while (true) {
        int bytes = recvfrom(sock, buffer, BUFFER_SIZE, 0,
                             (sockaddr *) &clientAddr, &addrLen);
        if (bytes <= 0) continue;

        std::string command(buffer, bytes);
        std::cout << "[UDP]:" << command << std::endl;

        if (command.find("UDP_DOWNLOAD ") == 0) {
            std::string filename = command.substr(13);
            filename.erase(filename.find_last_not_of(" \n\r\t") + 1);// Trim whitespace
            handleUDPDownload(sock, clientAddr, filename);
        } else if (command.find("UDP_UPLOAD ") == 0) {
            std::string args = command.substr(11);

            size_t last_space = args.rfind(' ');
            if (last_space == std::string::npos) {
                sendUDPMessage(sock, clientAddr, "ERROR: Invalid format. Use: UDP_UPLOAD <filename> <size>\n");
                return;
            }

            std::string filename = args.substr(0, last_space);
            std::string sizeStr = args.substr(last_space + 1);

            filename.erase(filename.find_last_not_of(" \n\r\t") + 1);
            sizeStr.erase(sizeStr.find_last_not_of(" \n\r\t") + 1);

            handleUDPUpload(sock, clientAddr, filename, sizeStr);
        } else if (command.find("UDP_TIME ") == 0) {
            handleUDPTime(sock, clientAddr);
        } else {
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

    if (bind(sock, (sockaddr *) &serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed" << std::endl;
        closesocket(sock);
        return;
    }

    std::cout << "[UDP] Server started on port " << UDP_PORT << std::endl;
    handleUDPCommand(sock);
    closesocket(sock);
}