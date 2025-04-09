#include "libs.h"

int main() {

    initializeSockets();
//    std::signal(SIGINT, signal_handler);
//    std::signal(SIGTERM, signal_handler);

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        std::cerr << "Error creating socket" << std::endl;
        cleanupSockets();
        return EXIT_FAILURE;
    }
    std::thread udpThread(handleUDPServer);
    handleTCPServer(serverSocket);

    closeSockets(serverSocket, EXIT_SUCCESS);
    return EXIT_SUCCESS;
}