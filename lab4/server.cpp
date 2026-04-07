#include <iostream>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "messages.h"

#define PORT 54000

struct Client {
    int sock;
    std::string nick;
    bool authenticated;
};

std::vector<Client> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_recv(uint8_t type) {
    std::cout << "[Layer 4 - Transport] recv()\n";
    std::cout << "[Layer 6 - Presentation] deserialize Message\n";
    std::cout << "[Layer 5 - Session] check session\n";
    std::cout << "[Layer 7 - Application] handle type: " << (int)type << "\n";
}

void log_send() {
    std::cout << "[Layer 7 - Application] prepare response\n";
    std::cout << "[Layer 6 - Presentation] serialize Message\n";
    std::cout << "[Layer 4 - Transport] send()\n";
}

bool is_nick_taken(const std::string& nick) {
    for (auto& c : clients) {
        if (c.nick == nick) return true;
    }
    return false;
}

Client* find_client(const std::string& nick) {
    for (auto& c : clients) {
        if (c.nick == nick) return &c;
    }
    return nullptr;
}

void broadcast(const std::string& text, int sender = -1) {
    Message msg{};
    prepareMessage(msg, MSG_TEXT, text);

    pthread_mutex_lock(&clients_mutex);
    for (auto& c : clients) {
        if (c.sock != sender) {
            log_send();
            send(c.sock, &msg, sizeof(msg), 0);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void* handle_client(void* arg) {
    int client = *(int*)arg;
    delete (int*)arg;

    Message msg{};

    int bytes = recv(client, &msg, sizeof(msg), 0);
    if (bytes <= 0) {
        close(client);
        return nullptr;
    }

    log_recv(msg.type);

    if (msg.type != MSG_AUTH) {
        Message err{};
        prepareMessage(err, MSG_ERROR, "Auth required");
        log_send();
        send(client, &err, sizeof(err), 0);
        close(client);
        return nullptr;
    }

    std::string nick = msg.payload;

    pthread_mutex_lock(&clients_mutex);

    if (nick.empty() || is_nick_taken(nick)) {
        pthread_mutex_unlock(&clients_mutex);

        Message err{};
        prepareMessage(err, MSG_ERROR, "Invalid or taken nickname");
        log_send();
        send(client, &err, sizeof(err), 0);
        close(client);
        return nullptr;
    }

    clients.push_back({client, nick, true});
    pthread_mutex_unlock(&clients_mutex);
    Message welcome{};
    prepareMessage(welcome, MSG_WELCOME, "Welcome " + nick);
    log_send();
    send(client, &welcome, sizeof(welcome), 0);

    broadcast("User [" + nick + "] connected");

    std::cout << "Client connected: " << nick << std::endl;

    while (true) {
        bytes = recv(client, &msg, sizeof(msg), 0);
        if (bytes <= 0) break;

        log_recv(msg.type);

        if (msg.type == MSG_TEXT) {
            std::string full = "[" + nick + "]: " + msg.payload;
            std::cout << full << std::endl;
            broadcast(full, client);
        }

        else if (msg.type == MSG_PRIVATE) {
            std::string data = msg.payload;
            auto pos = data.find(':');
            if (pos == std::string::npos) continue;

            std::string target = data.substr(0, pos);
            std::string text = data.substr(pos + 1);

            pthread_mutex_lock(&clients_mutex);
            Client* receiver = find_client(target);
            pthread_mutex_unlock(&clients_mutex);

            if (!receiver) {
                Message err{};
                prepareMessage(err, MSG_ERROR, "User not found");
                log_send();
                send(client, &err, sizeof(err), 0);
                continue;
            }

            std::string full = "[PRIVATE][" + nick + "]: " + text;

            Message out{};
            prepareMessage(out, MSG_TEXT, full);

            log_send();
            send(receiver->sock, &out, sizeof(out), 0);
        }

        else if (msg.type == MSG_PING) {
            Message pong{};
            prepareMessage(pong, MSG_PONG, "");
            log_send();
            send(client, &pong, sizeof(pong), 0);
        }

        else if (msg.type == MSG_BYE) {
            break;
        }
    }

    pthread_mutex_lock(&clients_mutex);
    clients.erase(std::remove_if(clients.begin(), clients.end(),
        [client](const Client& c) { return c.sock == client; }),
        clients.end());
    pthread_mutex_unlock(&clients_mutex);

    broadcast("User [" + nick + "] disconnected");

    close(client);
    std::cout << "Client disconnected: " << nick << std::endl;

    return nullptr;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    std::cout << "Server started on port " << PORT << std::endl;

    while (true) {
        int client = accept(server_fd, nullptr, nullptr);

        int* pclient = new int(client);
        pthread_t tid;
        pthread_create(&tid, nullptr, handle_client, pclient);
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}