#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>

#define PORT 8080
#define BUFFER_SIZE 1024

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

void handleEcho(int clientSocket, std::string command){
    std::string response = command.substr(5) + '\n';
    send(clientSocket, response.c_str(), response.size(), 0);
}

void handleTime(int clientSocket){
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string timeStr = ctime(&now);
    timeStr.pop_back();
    timeStr += '\n';
    send(clientSocket, timeStr.c_str(), timeStr.length(), 0);
}

void handleExit(int clientSocket){
    std::cout << "Closing connection." << std::endl;
#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}

void receiveFile(int clientSocket, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error opening file for writing" << std::endl;
        return;
    }

    char buffer[BUFFER_SIZE];
    int bytesReceived;
    while ((bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0)) > 0) {
        file.write(buffer, bytesReceived);
    }
    file.close();
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
                std::string filename = command.substr(7);
                receiveFile(clientSocket, filename);
            } else if (command.rfind("DOWNLOAD", 0) == 0) {

            } else if (command == "CLOSE" || command == "EXIT" || command == "QUIT") {
                handleExit(clientSocket);
            } else {
                char errorMsg[] = {"Unknown command\n"};
                send(clientSocket, errorMsg, sizeof(errorMsg), 0);
            }
        }
    }
}


int main() {
    initializeSockets();

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        std::cerr << "Error creating socket" << std::endl;
        cleanupSockets();
        return EXIT_FAILURE;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "Bind failed" << std::endl;
#ifdef _WIN32
        closesocket(serverSocket);
#else
        close(serverSocket);
#endif
        cleanupSockets();
        return EXIT_FAILURE;
    }

    if (listen(serverSocket, 5) == -1) {
        std::cerr << "Listen failed" << std::endl;
#ifdef _WIN32
        closesocket(serverSocket);
#else
        close(serverSocket);
#endif
        cleanupSockets();
        return EXIT_FAILURE;
    }

    std::cout << "Server listening on port " << PORT << "..." << std::endl;

    while (true) {
        sockaddr_in clientAddr;
        socklen_t clientSize = sizeof(clientAddr);
        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientSize);
        if (clientSocket == -1) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        std::cout << "Client connected from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
        handleClient(clientSocket);
    }

#ifdef _WIN32
    closesocket(serverSocket);
#else
    close(serverSocket);
#endif
    cleanupSockets();
    return EXIT_SUCCESS;
}
