#include "libs.h"

void initializeSockets() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        exit(EXIT_FAILURE);
    }
#endif
}

void cleanupSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void sendMessage(int clientSocket, const std::string &message) {
    send(clientSocket, message.c_str(), message.size(), 0);
}

void handleEcho(int clientSocket, const std::string &command) {
    std::string response = command.substr(5) + '\n';
    sendMessage(clientSocket, response);
}

void handleTime(int clientSocket) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string timeStr = ctime(&now);
    timeStr.pop_back();
    timeStr += '\n';
    sendMessage(clientSocket, timeStr);
}

void handleExit(int clientSocket) {
    std::cout << "Closing connection." << std::endl;
#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}

void receiveFile(int clientSocket, const std::string &filename, int totalFileSize) {
    std::string save_path = "uploads/" + filename;

    // Создаем папку uploads, если её нет
#ifdef _WIN32
    _mkdir("uploads");
#else
    mkdir("uploads", 0777);
#endif

    // Если файл уже существует, получаем текущий размер (offset)
    int currentSize = 0;
    if (std::filesystem::exists(save_path)) {
        currentSize = std::filesystem::file_size(save_path);
    }
    // Отправляем клиенту сообщение с текущим offset
    sendMessage(clientSocket, "READY " + std::to_string(currentSize) + "\n");

    // Открываем файл в режиме добавления
    std::ofstream file(save_path, std::ios::binary | std::ios::app);
    if (!file) {
        std::cerr << "Error opening file for writing: " << save_path << std::endl;
        sendMessage(clientSocket, "ERROR: Could not open file\n");
        return;
    }

    char buffer[BUFFER_SIZE];
    int bytesReceived;
    int totalReceived = currentSize; // Начинаем с уже полученных байт

    while (totalReceived < totalFileSize && (bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0)) > 0) {
        file.write(buffer, bytesReceived);
        totalReceived += bytesReceived;
    }

    file.close();
    sendMessage(clientSocket, "File upload complete.\n");
}


void sendFile(int clientSocket, const std::string &filename, int offset = 0) {
    if (!std::filesystem::exists(filename)) {
        std::cerr << "File does not exist: " << filename << std::endl;
        sendMessage(clientSocket, "ERROR: File not found\n");
        return;
    }
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        sendMessage(clientSocket, "ERROR: Cannot open file\n");
        return;
    }
    file.seekg(0, std::ios::end);
    long fileSize = file.tellg();
    if (offset > fileSize) offset = fileSize;
    int remaining = fileSize - offset;
    file.seekg(offset, std::ios::beg);

    // Отправляем клиенту READY с оставшимся размером
    std::ostringstream header;
    header << "READY " << remaining << "\n";
    send(clientSocket, header.str().c_str(), header.str().size(), 0);
    std::cout << "READY " << remaining << " sent to client for download" << std::endl;

    char buffer[BUFFER_SIZE];
    long totalSent = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (!file.eof()) {
        file.read(buffer, BUFFER_SIZE);
        int bytesRead = file.gcount();
        if (bytesRead > 0) {
            send(clientSocket, buffer, bytesRead, 0);
            totalSent += bytesRead;
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsedTime = std::chrono::duration<double>(endTime - startTime).count();
    double speed = (totalSent / 1024.0) / elapsedTime; // KB/s

    std::cout << "File sent! Speed: " << speed << " KB/s" << std::endl;
    file.close();
}


void handleUpload(int clientSocket, std::string command) {
    std::cout << "Upload command received!" << std::endl;

    std::istringstream iss(command);
    std::string cmd, filename;
    int fileSize;

    if (!(iss >> cmd >> filename >> fileSize)) {
        const char errorMSG[] = "ERROR: usage: UPLOAD <filename> <filesize>\n";
        send(clientSocket, errorMSG, sizeof(errorMSG), 0);
        return;
    }

    std::cout << "Uploading file: " << filename << " (" << fileSize << " bytes)" << std::endl;
    receiveFile(clientSocket, filename, fileSize);
}

void handleDownload(int clientSocket, std::string command){
    std::istringstream iss(command);
    std::string cmd, filename;

    int offset = 0;

    if(!(iss >> cmd >> filename)) {
        const char errorMSG[] = "usage: DOWNLOAD <filename>\n";
        send(clientSocket, errorMSG, sizeof(errorMSG), 0);
        return;
    }

    if (!(iss >> offset)) {
        offset = 0;
    }

    std::cout << "Download requested: " << filename << std::endl;
    //TODO: Make absolute path for files (or idk)
    std::string serverFilePath = "uploads/" + filename;
    sendFile(clientSocket, serverFilePath, offset);
}

void handleClient(int clientSocket) {
    char buffer[BUFFER_SIZE];
    std::string receivedData;
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived <= 0) {
            std::cout << "Client disconnected." << std::endl;
            break;
        }

        receivedData.append(buffer, bytesReceived);
        size_t pos;
        while ((pos = receivedData.find('\n')) != std::string::npos) {
            std::string command = receivedData.substr(0, pos);
            receivedData.erase(0, pos + 1);

            if (!command.empty() && command.back() == '\r') {
                command.pop_back();
            }
            std::cout << "Received: " << command << std::endl;

            if (command.rfind("ECHO ", 0) == 0) {
                handleEcho(clientSocket, command);
            } else if (command == "TIME") {
                handleTime(clientSocket);
            } else if (command.rfind("UPLOAD", 0) == 0) {
                handleUpload(clientSocket, command);
            } else if (command.rfind("DOWNLOAD", 0) == 0) {
                handleDownload(clientSocket, command);
            } else if (command == "CLOSE" || command == "EXIT" || command == "QUIT") {
                handleExit(clientSocket);
                return;
            } else {
                sendMessage(clientSocket, "Unknown command\n");
            }
        }
    }
}