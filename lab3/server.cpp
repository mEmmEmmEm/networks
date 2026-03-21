#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "messages.h"

#define PORT 54000
#define THREAD_COUNT 10

struct Client {
    int sock;
    std::string nick;
};

std::queue<int> client_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::vector<Client> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void push_client(int client) {
    pthread_mutex_lock(&queue_mutex);
    client_queue.push(client);
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}

int pop_client() {
    pthread_mutex_lock(&queue_mutex);

    while (client_queue.empty()) {
        pthread_cond_wait(&queue_cond, &queue_mutex);
    }

    int client = client_queue.front();
    client_queue.pop();

    pthread_mutex_unlock(&queue_mutex);
    return client;
}

void add_client(int sock, const std::string& nick) {
    pthread_mutex_lock(&clients_mutex);
    clients.push_back({sock, nick});
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(int sock) {
    pthread_mutex_lock(&clients_mutex);

    clients.erase(std::remove_if(clients.begin(), clients.end(),
        [sock](const Client& c) { return c.sock == sock; }),
        clients.end());

    pthread_mutex_unlock(&clients_mutex);
}

void broadcast(Message &msg, int sender) {
    pthread_mutex_lock(&clients_mutex);

    std::string sender_name = "Server";

    for (auto &c : clients) {
        if (c.sock == sender) {
            sender_name = c.nick;
            break;
        }
    }

    std::string full;

    if (msg.type == MSG_TEXT && sender != -1) {
        full = "[" + sender_name + "]: " + msg.payload;
    } else {
        full = msg.payload;
    }

    Message new_msg{};
    prepareMessage(new_msg, MSG_TEXT, full);

    for (auto &c : clients) {
        if (c.sock != sender) {
            send(c.sock, &new_msg, sizeof(new_msg), 0);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

void* worker(void*) {
    while (true) {
        int client = pop_client();

        Message msg{};

        int bytes = recv(client, &msg, sizeof(msg), 0);
        if (bytes <= 0) {
            close(client);
            continue;
        }

        std::string nick;

        if (msg.type == MSG_HELLO) {
            nick = msg.payload;

            std::cout << "Client connected: " << nick << std::endl;

            Message reply{};
            prepareMessage(reply, MSG_WELCOME, "Welcome!");
            send(client, &reply, sizeof(reply), 0);

            add_client(client, nick);

            Message join_msg{};
            prepareMessage(join_msg, MSG_TEXT, nick + " joined the chat");
            broadcast(join_msg, -1);
        }

        while (true) {
            bytes = recv(client, &msg, sizeof(msg), 0);

            if (bytes <= 0) break;

            if (msg.type == MSG_TEXT) {
                std::cout << "[" << nick << "]: " << msg.payload << std::endl;
                broadcast(msg, client);
            }
            else if (msg.type == MSG_PING) {
                Message pong{};
                prepareMessage(pong, MSG_PONG, "");
                send(client, &pong, sizeof(pong), 0);
            }
            else if (msg.type == MSG_BYE) {
                break;
            }
        }

        Message leave_msg{};
        prepareMessage(leave_msg, MSG_TEXT, nick + " left the chat");
        broadcast(leave_msg, -1);

        remove_client(client);
        close(client);

        std::cout << "Client disconnected: " << nick << std::endl;
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    listen(server_fd, 10);
    std::cout << "Server started on port " << PORT << std::endl;

    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], nullptr, worker, nullptr);
        pthread_detach(threads[i]);
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int client = accept(server_fd, (sockaddr *)&client_addr, &addrlen);
        if (client < 0) {
            perror("accept");
            continue;
        }
        push_client(client);
    }

    close(server_fd);
    return 0;
}