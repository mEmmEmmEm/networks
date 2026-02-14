#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int client_socket;
    char buffer[BUFFER_SIZE];
    client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_socket < 0) {
        perror("error creating socket");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    std::string message;
    std::cout << "input message: ";
    std::getline(std::cin, message);

    sendto(
        client_socket,
        message.c_str(),
        message.size(),
        0,
        (sockaddr*)&server_addr,
        sizeof(server_addr)
    );
    socklen_t server_len = sizeof(server_addr);
    ssize_t bytes_received = recvfrom(
        client_socket,
        buffer,
        BUFFER_SIZE,
        0,
        (sockaddr*)&server_addr,
        &server_len
    );

    if (bytes_received < 0) {
        perror("error recvfrom");
        close(client_socket);
        return 1;
    }
    buffer[bytes_received] = '\0';
    std::cout << "Server response: " << buffer << std::endl;
    close(client_socket);
    return 0;
}
