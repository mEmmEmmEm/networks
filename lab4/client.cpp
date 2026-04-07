#include <iostream>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "messages.h"

#define PORT 54000
#define SERVER_IP "127.0.0.1"

int sock;
void* receiver(void*) {
    Message msg{};

    while (true) {
        int bytes = recv(sock, &msg, sizeof(msg), 0);

        if (bytes <= 0) {
            std::cout << "Disconnected\n";
            exit(0);
        }

        if (msg.type == MSG_TEXT) {
            std::cout << msg.payload << std::endl;
        }
        else if (msg.type == MSG_PRIVATE) {
            std::cout << msg.payload << std::endl;
        }
        else if (msg.type == MSG_PONG) {
            std::cout << "PONG\n";
        }
        else if (msg.type == MSG_WELCOME) {
            std::cout << msg.payload << std::endl;
        }
        else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << msg.payload << std::endl;
        }
        else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << std::endl;
        }
    }
}

void connect_to_server() {
    while (true) {
        sock = socket(AF_INET, SOCK_STREAM, 0);

        sockaddr_in serv{};
        serv.sin_family = AF_INET;
        serv.sin_port = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);

        if (connect(sock, (sockaddr*)&serv, sizeof(serv)) == 0) {
            std::cout << "Connected\n";
            break;
        }

        std::cout << "Retry...\n";
        sleep(2);
    }
}

int main() {
    connect_to_server();

    std::string nick;
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nick);

    Message msg{};
    prepareMessage(msg, MSG_AUTH, nick);
    send(sock, &msg, sizeof(msg), 0);

    pthread_t t;
    pthread_create(&t, nullptr, receiver, nullptr);
    pthread_detach(t);

    while (true) {
        std::string input;
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "/quit") {
            prepareMessage(msg, MSG_BYE, "");
            send(sock, &msg, sizeof(msg), 0);
            break;
        }
        else if (input == "/ping") {
            prepareMessage(msg, MSG_PING, "");
        }
        else if (input.rfind("/w ", 0) == 0) {
            size_t pos = input.find(' ', 3);
            if (pos == std::string::npos) continue;

            std::string target = input.substr(3, pos - 3);
            std::string text = input.substr(pos + 1);

            prepareMessage(msg, MSG_PRIVATE, target + ":" + text);
        }
        else {
            prepareMessage(msg, MSG_TEXT, input);
        }

        send(sock, &msg, sizeof(msg), 0);
    }

    close(sock);
    return 0;
}