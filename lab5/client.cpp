#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include <string>
#include "messages.h"

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define RECONNECT_DELAY 2
#define INPUT_TIMEOUT 1

bool keepRunning = true;
bool connected = false;
int sock = -1;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

bool read_input_with_timeout(char* buffer, int max_len, int timeout_seconds) {
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
    if (result > 0) {
        if (fgets(buffer, max_len, stdin) != NULL) {
            buffer[strcspn(buffer, "\n")] = 0;
            return true;
        }
    }
    return false;
}

std::string format_timestamp(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

void handleSignal(int) {
    keepRunning = false;
}

MessageEx ntoh_message(const MessageEx& net_msg) {
    MessageEx host_msg = net_msg;
    host_msg.length = ntohl(net_msg.length);
    host_msg.msg_id = ntohl(net_msg.msg_id);
    return host_msg;
}

MessageEx hton_message(const MessageEx& host_msg) {
    MessageEx net_msg = host_msg;
    net_msg.length = htonl(host_msg.length);
    net_msg.msg_id = htonl(host_msg.msg_id);
    return net_msg;
}

bool send_message(int socket, const MessageEx& msg) {
    MessageEx net_msg = hton_message(msg);
    if (send(socket, &net_msg, sizeof(MessageEx), 0) < 0) return false;
    return true;
}

bool recv_message(int socket, MessageEx& msg) {
    if (recv(socket, &msg, sizeof(MessageEx), 0) <= 0) return false;
    msg = ntoh_message(msg);
    return true;
}

void* receive_thread(void*) {
    MessageEx msg;
    while (keepRunning && connected) {
        pthread_mutex_lock(&sock_mutex);
        int current_sock = sock;
        pthread_mutex_unlock(&sock_mutex);
        if (current_sock < 0) break;
        if (!recv_message(current_sock, msg)) {
            std::cout << "\nConnection to server lost" << std::endl;
            pthread_mutex_lock(&sock_mutex);
            connected = false;
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            break;
        }
        switch (msg.type) {
            case MSG_WELCOME:
                std::cout << "\n*** " << msg.payload << " ***" << std::endl;
                break;
            case MSG_TEXT:
                std::cout << "[" << format_timestamp(msg.timestamp) << "]"
                          << "\n[id=" << msg.msg_id << "]"
                          << "[" << msg.sender << "]: " << msg.payload << std::endl;
                break;
            case MSG_PRIVATE:
                std::cout << "\n[PRIVATE]"
                          << "[" << format_timestamp(msg.timestamp) << "]"
                          << "[id=" << msg.msg_id << "]"
                          << "[" << msg.sender << "->" << msg.receiver << "]: "
                          << msg.payload << std::endl;
                break;
            case MSG_SERVER_INFO:
                std::cout << "\n[SERVER]: " << msg.payload << std::endl;
                break;
            case MSG_ERROR:
                std::cout << "\n[ERROR]: " << msg.payload << std::endl;
                break;
            case MSG_PONG:
                std::cout << "\npong" << std::endl;
                break;
            case MSG_BYE:
                std::cout << "\n*** Server closed connection ***" << std::endl;
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sock);
                sock = -1;
                pthread_mutex_unlock(&sock_mutex);
                break;
            case MSG_HISTORY_DATA:
                std::cout << "\n[HISTORY]:\n" << msg.payload << std::endl;
                break;
            default:
                std::cout << "\n*** Unknown message type: " << (int)msg.type << " ***" << std::endl;
        }
        std::cout << "> ";
        fflush(stdout);
    }
    return nullptr;
}

int connect_to_server() {
    int new_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (new_sock < 0) { perror("socket"); return -1; }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("inet_pton"); close(new_sock); return -1;
    }
    if (connect(new_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); close(new_sock); return -1;
    }
    return new_sock;
}

bool authenticate(int socket, const std::string& nickname) {
    MessageEx auth_msg;
    memset(&auth_msg, 0, sizeof(MessageEx));
    strncpy(auth_msg.payload, nickname.c_str(), MAX_NAME - 1);
    auth_msg.payload[MAX_NAME-1] = '\0';
    auth_msg.length = strlen(auth_msg.payload) + 1;
    auth_msg.type = MSG_AUTH;
    if (!send_message(socket, auth_msg)) return false;
    MessageEx response;
    if (!recv_message(socket, response)) return false;
    if (response.type == MSG_ERROR) {
        std::cout << "Authentication failed: " << response.payload << std::endl;
        return false;
    }
    if (response.type == MSG_WELCOME) {
        std::cout << response.payload << std::endl;
        return true;
    }
    return false;
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    pthread_t recv_thread;
    std::string nickname;
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    while (nickname.empty()) {
        std::cout << "Nickname cannot be empty. Enter your nickname: ";
        std::getline(std::cin, nickname);
    }
    while (keepRunning) {
        std::cout << "Connecting to " << SERVER_IP << ":" << SERVER_PORT << "..." << std::endl;
        pthread_mutex_lock(&sock_mutex);
        sock = connect_to_server();
        pthread_mutex_unlock(&sock_mutex);
        if (sock < 0) {
            std::cout << "Connection failed. Retrying in " << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
            continue;
        }
        if (!authenticate(sock, nickname)) {
            std::cout << "Authentication failed" << std::endl;
            pthread_mutex_lock(&sock_mutex);
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);
            sleep(RECONNECT_DELAY);
            keepRunning = false;
            break;
        }
        connected = true;
        std::cout << "Connected to server. Type messages:" << std::endl;
        if (pthread_create(&recv_thread, NULL, receive_thread, NULL) != 0) {
            perror("pthread_create"); break;
        }
        char input[MAX_PAYLOAD];
        std::cout << "> " << std::flush;
        while (keepRunning && connected) {
            if (read_input_with_timeout(input, MAX_PAYLOAD, INPUT_TIMEOUT)) {
                if (strlen(input) == 0) continue;
                MessageEx msg;
                memset(&msg, 0, sizeof(MessageEx));
                if (strcmp(input, "/quit") == 0) {
                    strcpy(msg.payload, "bye");
                    msg.length = strlen(msg.payload) + 1;
                    msg.type = MSG_BYE;
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) send_message(sock, msg);
                    pthread_mutex_unlock(&sock_mutex);
                    connected = false;
                    keepRunning = false;
                    break;
                }
                else if (strcmp(input, "/ping") == 0) {
                    strcpy(msg.payload, "ping");
                    msg.length = strlen(msg.payload) + 1;
                    msg.type = MSG_PING;
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) send_message(sock, msg);
                    pthread_mutex_unlock(&sock_mutex);
                }
                else if (strncmp(input, "/w ", 3) == 0) {
                    char* target = strtok(input + 3, " ");
                    char* message = strtok(NULL, "");
                    if (target && message && strlen(message) > 0) {
                        snprintf(msg.payload, MAX_PAYLOAD, "%s", message);
                        msg.length = strlen(msg.payload) + 1;
                        msg.type = MSG_PRIVATE;
                        snprintf(msg.receiver, MAX_NAME, "%s", target);
                        pthread_mutex_lock(&sock_mutex);
                        if (sock >= 0) send_message(sock, msg);
                        pthread_mutex_unlock(&sock_mutex);
                    } else {
                        std::cout << "Usage: /w <nickname> <message>" << std::endl;
                    }
                }
                else if (strcmp(input, "/help") == 0) {
                    std::cout << "  /help                              - Show this help message     \n";
                    std::cout << "  /list                              - Show online users list     \n";
                    std::cout << "  /history                           - Show all last messages     \n";
                    std::cout << "  /history N                         - Show last N messages       \n";
                    std::cout << "  /quit                              - Disconnect from server     \n";
                    std::cout << "  /w <nick> <message>                - Send private message       \n";
                    std::cout << "  /ping                              - Receive pong from server   \n";
                }
                else if (strcmp(input, "/list") == 0) {
                    strcpy(msg.payload, "list");
                    msg.length = strlen(msg.payload) + 1;
                    msg.type = MSG_LIST;
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) send_message(sock, msg);
                    pthread_mutex_unlock(&sock_mutex);
                }
                else if (strncmp(input, "/history", 8) == 0) {
                    if (strlen(input) == 8) {
                        strcpy(msg.payload, "");
                        msg.length = 1;
                        msg.type = MSG_HISTORY;
                        pthread_mutex_lock(&sock_mutex);
                        if (sock >= 0) send_message(sock, msg);
                        pthread_mutex_unlock(&sock_mutex);
                    }
                    else if (strlen(input) > 8 && input[8] == ' ') {
                        const char* param = input + 9;
                        bool valid = true;
                        for (size_t i = 0; param[i] != '\0'; i++) {
                            if (!std::isdigit(param[i])) {
                                valid = false;
                                break;
                            }
                        }
                        if (valid && strlen(param) > 0) {
                            int n = std::stoi(param);
                            if (n > 0) {
                                strcpy(msg.payload, param);
                                msg.length = strlen(msg.payload) + 1;
                                msg.type = MSG_HISTORY;
                                pthread_mutex_lock(&sock_mutex);
                                if (sock >= 0) send_message(sock, msg);
                                pthread_mutex_unlock(&sock_mutex);
                            } else {
                                std::cout << "Error: N must be a positive number." << std::endl;
                                std::cout << "> " << std::flush;
                                continue;
                            }
                        } else {
                            std::cout << "Error: Invalid parameter. Use /history or /history <positive_number>" << std::endl;
                            std::cout << "> " << std::flush;
                            continue;
                        }
                    } else if (input[0] == '/') {
                        std::cout << "Unknown command\n";
                        continue;
                    }
                    else {
                        std::cout << "Error: Use /history or /history <positive_number>" << std::endl;
                        std::cout << "> " << std::flush;
                        continue;
                    }
                }
                else {
                    strncpy(msg.payload, input, MAX_PAYLOAD - 1);
                    msg.payload[MAX_PAYLOAD-1] = '\0';
                    msg.length = strlen(msg.payload) + 1;
                    msg.type = MSG_TEXT;
                    pthread_mutex_lock(&sock_mutex);
                    if (sock >= 0) send_message(sock, msg);
                    pthread_mutex_unlock(&sock_mutex);
                }
                std::cout << "> " << std::flush;
            }
        }
        pthread_join(recv_thread, NULL);
        if (keepRunning && !connected) {
            std::cout << "Attempting to reconnect in " << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
        }
    }
    pthread_mutex_lock(&sock_mutex);
    if (sock >= 0) close(sock);
    pthread_mutex_unlock(&sock_mutex);
    std::cout << "Client shutdown" << std::endl;
    return 0;
}