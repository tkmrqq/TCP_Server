#include "../libs.h"

volatile std::sig_atomic_t g_shutdown = 0;
std::mutex accept_mutex;
std::mutex log_mutex;

// В структуре или глобально (лучше использовать класс)
std::unordered_map<int, bool> clientUploadMode; // key: clientSocket, value: in upload mode
std::unordered_map<int, int> clientFileSizes;   // Остаток байт для приёма
std::unordered_map<int, std::string> clientFileName;
static std::unordered_map<int, int> bytesReceivedMap;

void signal_handler(int) {
    g_shutdown = 1;
    std::cout << "Detected SIGINT, closing server...";
}

void log_message(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "[LOG] " << msg << std::endl;
}

int closeSockets(SOCKET serverSocket, int exitCode){
#ifdef _WIN32
    closesocket(serverSocket);
#else
    close(serverSocket);
#endif
    cleanupSockets();
    return exitCode;
}

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

int handleTCPServer(SOCKET serverSocket){
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(TCP_PORT);

    ThreadPool pool(4);

    if (bind(serverSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "Bind failed" << std::endl;
        closeSockets(serverSocket, EXIT_FAILURE);
    }

    if (listen(serverSocket, 5) == -1) {
        std::cerr << "Listen failed" << std::endl;
        closeSockets(serverSocket, EXIT_FAILURE);
    }

    std::cout << "[TCP] Server listening on port " << TCP_PORT << "..." << std::endl;

    while (!g_shutdown) {
        sockaddr_in clientAddr;
        socklen_t clientSize = sizeof(clientAddr);
        std::lock_guard<std::mutex> lock(accept_mutex);
        int clientSocket = accept(serverSocket, (struct sockaddr *) &clientAddr, &clientSize);
        if (clientSocket == -1) {
            //            if (errno == EWOULDBLOCK || errno == EAGAIN) {
            //                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            //                continue;
            //            }
            std::cerr << "Accept failed" << std::endl;
            break;
        }

        pool.enqueue([clientSocket] {
            handleClient(clientSocket);
        });

        log_message("Client connected: " + std::string(inet_ntoa(clientAddr.sin_addr)));

        //        std::thread clientThread(handleClient, clientSocket);
        //        clientThread.detach();
    }
    return EXIT_SUCCESS;
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
    std::istringstream iss(command);
    std::string cmd, filename;
    int fileSize;
    if (!(iss >> cmd >> filename >> fileSize)) {
        sendMessage(clientSocket, "ERROR: Invalid UPLOAD command\n");
        return;
    }

    // Создаем папку uploads, если её нет
    if (!std::filesystem::exists("uploads")) {
        std::filesystem::create_directory("uploads");
    }

    std::string filepath = "uploads/" + filename;
    clientUploadMode[clientSocket] = true;
    clientFileSizes[clientSocket] = fileSize;
    clientFileName[clientSocket] = filename;

    // Проверяем текущий размер файла
    int currentSize = 0;
    if (std::filesystem::exists(filepath)) {
        currentSize = std::filesystem::file_size(filepath);
    }

    sendMessage(clientSocket, "READY " + std::to_string(currentSize) + "\n");
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

void receiveFileData(int clientSocket, const char* data, int size) {
    std::string filepath = "uploads/" + clientFileName[clientSocket];
    std::ofstream file(filepath, std::ios::binary | std::ios::app);

    if (!file) {
        std::cerr << "Error opening file: " << filepath << std::endl;
        sendMessage(clientSocket, "ERROR: Could not open file\n");
        return;
    }

    file.write(data, size);
    bytesReceivedMap[clientSocket] += size;

    if (bytesReceivedMap[clientSocket] >= clientFileSizes[clientSocket]) {
        clientUploadMode[clientSocket] = false;
        sendMessage(clientSocket, "File upload complete.\n");
        bytesReceivedMap.erase(clientSocket);
    }
}

void handleClient(int clientSocket) {
    char buffer[BUFFER_SIZE];
    std::string receivedData;
    clientUploadMode[clientSocket] = false;
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived <= 0) break;

        if (clientUploadMode[clientSocket]) {
            receiveFileData(clientSocket, buffer, bytesReceived);
            continue;
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
    clientUploadMode.erase(clientSocket);
    clientFileSizes.erase(clientSocket);
    clientFileName.erase(clientSocket);
}