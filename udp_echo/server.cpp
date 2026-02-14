#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int server_socket;
    char buffer[BUFFER_SIZE];
    server_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_socket < 0) {
        perror("error creating socket");
        return 1;
    }
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("error bind");
        close(server_socket);
        return 1;
    }

    std::cout << "UDP server running on port " << PORT << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        ssize_t bytes_received = recvfrom(
            server_socket,
            buffer,
            BUFFER_SIZE,
            0,
            (sockaddr*)&client_addr,
            &client_len
        );

        if (bytes_received < 0) {
            perror("error recvfrom");
            continue;
        }

        buffer[bytes_received] = '\0';

        std::cout << "Received from "
                  << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port)
                  << " -> " << buffer << std::endl;
        sendto(
            server_socket,
            buffer,
            bytes_received,
            0,
            (sockaddr*)&client_addr,
            client_len
        );
    }
    close(server_socket);
    return 0;
}
