#ifndef TCP_SERVER_LIBS_H
#define TCP_SERVER_LIBS_H


#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <direct.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <csignal>
#include "ThreadPool.cpp"

#define TCP_PORT 8080
#define UDP_PORT 8081
#define BUFFER_SIZE 1024

//functions
void initializeSockets();
void cleanupSockets();
void handleClient(int clientSocket);
void handleUDPServer();
int handleTCPServer(SOCKET serverSocket);
int closeSockets(SOCKET serverSocket, int exitCode);
void signal_handler(int);

#endif//TCP_SERVER_LIBS_H
