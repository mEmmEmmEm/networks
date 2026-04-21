#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <vector>
#include <queue>
#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include "messages.h"

#define PORT 8080
#define THREAD_POOL_SIZE 10
#define MAX_QUEUE_SIZE 100
#define MAX_CLIENTS 100
#define MAX_HISTORY 100
#define LOG_FILE "messages_log.json"

bool keepRunning = true;
std::atomic<uint32_t> id{0};

std::vector<ClientInfo> clients;
std::vector<OfflineMsg> offline;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t json_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
std::queue<int> client_queue;

void log_layer(int layer, const std::string& message) {
    std::cout << "[Layer " << layer << " - ";
    switch(layer) {
        case 2: std::cout << "Internet"; break;
        case 3: std::cout << "Transport"; break;
        case 4: std::cout << "Application"; break;
        default: std::cout << "Unknown";
    }
    std::cout << "] " << message << std::endl;
}

void handleSignal(int) {
    keepRunning = false;
    pthread_cond_broadcast(&queue_cond);
}

std::string unescape_json(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += s[i + 1];
            }
            i++;
        } else {
            result += s[i];
        }
    }
    return result;
}

std::string extract_string(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":\"";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return "";
    start += pattern.length();
    size_t i = start;
    while (i < line.size()) {
        if (line[i] == '"' && line[i - 1] != '\\') break;
        i++;
    }
    return unescape_json(line.substr(start, i - start));
}

long extract_number(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    size_t start = line.find(pattern);
    if (start == std::string::npos) return 0;
    start += pattern.length();
    size_t end = line.find_first_of(",}", start);
    return std::stol(line.substr(start, end - start));
}

std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

bool update_delivered(uint32_t msg_id) {
    pthread_mutex_lock(&json_mutex);
    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    bool updated = false;
    while (std::getline(file, line)) {
        uint32_t id = extract_number(line, "msg_id");
        if (id == msg_id) {
            size_t pos = line.find("\"delivered\":false");
            if (pos != std::string::npos) {
                line.replace(pos, strlen("\"delivered\":false"), "\"delivered\":true");
                updated = true;
            }
        }
        lines.push_back(line);
    }
    file.close();
    std::ofstream out(LOG_FILE, std::ios::trunc);
    for (auto& l : lines) out << l << "\n";
    out.close();
    pthread_mutex_unlock(&json_mutex);
    return updated;
}

uint32_t load_max_id() {
    pthread_mutex_lock(&json_mutex);
    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return 0;
    }
    uint32_t max_id = 0;
    std::string line;
    while (std::getline(file, line)) {
        uint32_t id = extract_number(line, "msg_id");
        if (id > max_id) max_id = id;
    }
    file.close();
    pthread_mutex_unlock(&json_mutex);
    return max_id;
}

void log_message_to_json(const MessageEx& msg, bool is_offline) {
    pthread_mutex_lock(&json_mutex);
    std::ofstream file(LOG_FILE, std::ios::app);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return;
    }
    file << "{";
    file << "\"msg_id\":" << msg.msg_id << ",";
    file << "\"timestamp\":" << msg.timestamp << ",";
    file << "\"sender\":\"" << escape_json(msg.sender) << "\",";
    file << "\"receiver\":\"" << escape_json(msg.receiver) << "\",";
    file << "\"type\":\"" << (msg.type == MSG_PRIVATE ? "MSG_PRIVATE" : "MSG_TEXT") << "\",";
    file << "\"text\":\"" << escape_json(msg.payload) << "\",";
    file << "\"delivered\":" << (is_offline ? "false" : "true") << ",";
    file << "\"is_offline\":" << (is_offline ? "true" : "false");
    file << "}\n";
    file.close();
    pthread_mutex_unlock(&json_mutex);
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::string format_timestamp(time_t timestamp) {
    struct tm* tm_info = localtime(&timestamp);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

std::string get_history(int n, const std::string& requesting_user) {
    std::vector<HistoryMessage> all_messages;
    pthread_mutex_lock(&json_mutex);
    std::ifstream file(LOG_FILE);
    if (!file.is_open()) {
        pthread_mutex_unlock(&json_mutex);
        return "No history available.\n";
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        HistoryMessage msg;
        msg.msg_id = extract_number(line, "msg_id");
        msg.timestamp = extract_number(line, "timestamp");
        msg.sender = extract_string(line, "sender");
        msg.receiver = extract_string(line, "receiver");
        msg.type = extract_string(line, "type");
        msg.text = extract_string(line, "text");
        std::string str_bool = extract_string(line, "is_offline");
        std::transform(str_bool.begin(), str_bool.end(), str_bool.begin(), ::tolower);
        msg.is_offline = (str_bool == "true");
        all_messages.push_back(msg);
    }
    file.close();
    pthread_mutex_unlock(&json_mutex);
    std::ostringstream result;
    int total = all_messages.size();
    int start = (n > 0 && n < total) ? total - n : 0;
    for (int i = start; i < total; i++) {
        const auto& msg = all_messages[i];
        std::string time_str = format_timestamp(msg.timestamp);
        bool is_private = (msg.type == "MSG_PRIVATE");
        bool is_for_requester = (msg.receiver == requesting_user || msg.sender == requesting_user);
        result << "[" << time_str << "]";
        result << "[id=" << msg.msg_id << "]";
        if (msg.is_offline) {
            if (is_for_requester)
                result << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: " << msg.text;
            else
                result << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: " << std::string(msg.text.length(), '*');
        }
        else if (is_private) {
            if (is_for_requester)
                result << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: " << msg.text;
            else
                result << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: " << std::string(msg.text.length(), '*');
        } else {
            result << "[" << msg.sender << "]: " << msg.text;
        }
        result << "\n";
    }
    if (result.str().empty()) return "No messages in history.\n";
    return result.str();
}

MessageEx ntoh_message(const MessageEx& net_msg) {
    log_layer(4, "deserialize Message (network to host)");
    MessageEx host_msg = net_msg;
    host_msg.length = ntohl(net_msg.length);
    host_msg.msg_id = ntohl(net_msg.msg_id);
    return host_msg;
}

MessageEx hton_message(const MessageEx& host_msg) {
    log_layer(4, "serialize Message (host to network)");
    MessageEx net_msg = host_msg;
    net_msg.length = htonl(host_msg.length);
    net_msg.msg_id = htonl(host_msg.msg_id);
    return net_msg;
}

bool send_message(int socket, const MessageEx& msg) {
    log_layer(3, "send() - transmitting data");
    MessageEx net_msg = hton_message(msg);
    if (send(socket, &net_msg, sizeof(MessageEx), 0) < 0) return false;
    return true;
}

bool recv_message(int socket, MessageEx& msg) {
    log_layer(3, "recv() - receiving data");
    if (recv(socket, &msg, sizeof(MessageEx), 0) <= 0) return false;
    msg = ntoh_message(msg);
    return true;
}

ClientInfo* find_client_by_socket(int socket) {
    for (auto& client : clients)
        if (client.socket == socket) return &client;
    return nullptr;
}

ClientInfo* find_client_by_nickname(const std::string& nickname) {
    for (auto& client : clients)
        if (client.authenticated && std::string(client.nickname) == nickname) return &client;
    return nullptr;
}

std::string get_online_users_list() {
    std::string result = "[SERVER]: Online users\n";
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients)
        if (client.authenticated && client.active && strlen(client.nickname) > 0)
            result += "  " + std::string(client.nickname) + "\n";
    pthread_mutex_unlock(&clients_mutex);
    return result;
}

int parse_history_param(const char* payload) {
    if (payload == nullptr || payload[0] == '\0') return -1;
    char* endptr;
    long val = strtol(payload, &endptr, 10);
    if (endptr == payload || *endptr != '\0' || val <= 0) return 0;
    if (val > MAX_HISTORY) val = MAX_HISTORY;
    return (int)val;
}

bool is_nickname_unique(const std::string& nickname) {
    for (const auto& client : clients)
        if (client.authenticated && std::string(client.nickname) == nickname) return false;
    return true;
}

void remove_client(int socket) {
    log_layer(4, "remove client");
    pthread_mutex_lock(&clients_mutex);
    auto it = std::find_if(clients.begin(), clients.end(), [socket](const ClientInfo& c) { return c.socket == socket; });
    if (it != clients.end()) {
        if (it->authenticated) {
            MessageEx info_msg;
            memset(&info_msg, 0, sizeof(MessageEx));
            info_msg.type = MSG_SERVER_INFO;
            snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] disconnected", it->nickname);
            info_msg.length = strlen(info_msg.payload) + 1;
            for (const auto& client : clients)
                if (client.socket != socket && client.authenticated)
                    send_message(client.socket, info_msg);
            std::cout << "User [" << it->nickname << "] disconnected" << std::endl;
        }
        close(it->socket);
        clients.erase(it);
    }
    pthread_mutex_unlock(&clients_mutex);
}

void broadcast_message(const MessageEx& msg, int sender_socket) {
    log_layer(4, "broadcast message");
    pthread_mutex_lock(&clients_mutex);
    ClientInfo* sender = find_client_by_socket(sender_socket);
    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    for (const auto& client : clients)
        if (client.socket != sender_socket && client.authenticated)
            send_message(client.socket, msg);
    pthread_mutex_unlock(&clients_mutex);
}

void send_private_message(const std::string& target_nick, const std::string& message, int sender_socket) {
    log_layer(4, "private message started");
    pthread_mutex_lock(&clients_mutex);
    ClientInfo* sender = find_client_by_socket(sender_socket);
    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    ClientInfo* target = find_client_by_nickname(target_nick);
    pthread_mutex_unlock(&clients_mutex);
    MessageEx private_msg;
    memset(&private_msg, 0, sizeof(MessageEx));
    snprintf(private_msg.payload, MAX_PAYLOAD, "%s", message.c_str());
    private_msg.length = strlen(private_msg.payload) + 1;
    private_msg.type = MSG_PRIVATE;
    private_msg.msg_id = id.fetch_add(1);
    snprintf(private_msg.sender, MAX_NAME, "%s", sender->nickname);
    snprintf(private_msg.receiver, MAX_NAME, "%s", target_nick.c_str());
    private_msg.timestamp = time(nullptr);
    if (!target) {
        log_layer(4, "offline private message save");
        OfflineMsg offline_msg;
        memset(&offline_msg, 0, sizeof(OfflineMsg));
        snprintf(offline_msg.text, MAX_PAYLOAD, "%s", private_msg.payload);
        snprintf(offline_msg.sender, MAX_NAME, "%s", private_msg.sender);
        snprintf(offline_msg.receiver, MAX_NAME, "%s", private_msg.receiver);
        offline_msg.timestamp = private_msg.timestamp;
        offline_msg.msg_id = private_msg.msg_id;
        pthread_mutex_lock(&offline_mutex);
        offline.push_back(offline_msg);
        pthread_mutex_unlock(&offline_mutex);
        log_message_to_json(private_msg, true);
    }
    else if (!send_message(target->socket, private_msg)) {
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        snprintf(error_msg.payload, MAX_PAYLOAD, "Failed to send message to '%s'", target_nick.c_str());
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(sender_socket, error_msg);
    } else {
        std::cout << "[PRIVATE] " << sender->nickname << " -> " << target_nick << ": " << message << std::endl;
        log_message_to_json(private_msg, false);
    }
}

bool authenticate_client(int client_sock, const std::string& nickname) {
    log_layer(4, "authentication process started");
    if (nickname.empty()) {
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        strcpy(error_msg.payload, "Nickname cannot be empty");
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(client_sock, error_msg);
        return false;
    }
    if (!is_nickname_unique(nickname)) {
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        snprintf(error_msg.payload, MAX_PAYLOAD, "Nickname '%s' is already taken", nickname.c_str());
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(client_sock, error_msg);
        return false;
    }
    ClientInfo* client = find_client_by_socket(client_sock);
    if (!client) return false;
    strncpy(client->nickname, nickname.c_str(), MAX_NAME - 1);
    client->nickname[MAX_NAME - 1] = '\0';
    client->authenticated = true;
    log_layer(4, "authentication success: " + nickname);
    MessageEx welcome_msg;
    memset(&welcome_msg, 0, sizeof(MessageEx));
    snprintf(welcome_msg.payload, MAX_PAYLOAD, "Welcome %s", nickname.c_str());
    welcome_msg.length = strlen(welcome_msg.payload) + 1;
    welcome_msg.type = MSG_WELCOME;
    send_message(client_sock, welcome_msg);
    MessageEx info_msg;
    memset(&info_msg, 0, sizeof(MessageEx));
    snprintf(info_msg.payload, MAX_PAYLOAD, "User [%s] connected", nickname.c_str());
    info_msg.length = strlen(info_msg.payload) + 1;
    info_msg.type = MSG_SERVER_INFO;
    for (const auto& c : clients)
        if (c.socket != client_sock && c.authenticated)
            send_message(c.socket, info_msg);
    std::cout << "User [" << nickname << "] connected" << std::endl;
    return true;
}

void* handle_client(int client_sock) {
    log_layer(4, "new client connection handler started");
    MessageEx msg;
    ClientInfo client;
    client.socket = client_sock;
    client.authenticated = false;
    client.active = true;
    socklen_t addr_len = sizeof(client.address);
    getpeername(client_sock, (struct sockaddr*)&client.address, &addr_len);
    pthread_mutex_lock(&clients_mutex);
    clients.push_back(client);
    pthread_mutex_unlock(&clients_mutex);
    if (!recv_message(client_sock, msg) || msg.type != MSG_AUTH) {
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));
        strcpy(error_msg.payload, "Authentication required");
        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;
        send_message(client_sock, error_msg);
        remove_client(client_sock);
        return nullptr;
    }
    log_layer(4, "received MSG_AUTH, payload: " + std::string(msg.payload));
    if (!authenticate_client(client_sock, msg.payload)) {
        remove_client(client_sock);
        return nullptr;
    }
    pthread_mutex_lock(&offline_mutex);
    ClientInfo* auth_client = find_client_by_socket(client_sock);
    if (!auth_client) {
        pthread_mutex_unlock(&offline_mutex);
        remove_client(client_sock);
        return nullptr;
    }
    for (auto it = offline.begin(); it != offline.end(); ) {
        if (strcmp(it->receiver, auth_client->nickname) == 0) {
            MessageEx off_msg;
            memset(&off_msg, 0, sizeof(MessageEx));
            snprintf(off_msg.payload, MAX_PAYLOAD, "[OFFLINE]%s", it->text);
            off_msg.length = strlen(off_msg.payload) + 1;
            off_msg.type = MSG_PRIVATE;
            snprintf(off_msg.sender, MAX_NAME, "%s", it->sender);
            snprintf(off_msg.receiver, MAX_NAME, "%s", it->receiver);
            off_msg.timestamp = it->timestamp;
            off_msg.msg_id = it->msg_id;
            if (send_message(auth_client->socket, off_msg)) {
                update_delivered(it->msg_id);
                it = offline.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
    pthread_mutex_unlock(&offline_mutex);
    while (keepRunning && client.active) {
        if (!recv_message(client_sock, msg)) {
            log_layer(4, "client disconnected");
            break;
        }
        log_layer(4, "processing message type: " + std::to_string(msg.type));
        ClientInfo* sender = find_client_by_socket(client_sock);
        if (!sender) break;
        std::string sender_nickname = sender->nickname;
        std::string target = msg.receiver;
        std::string message = msg.payload;
        std::string user_list = get_online_users_list();
        switch (msg.type) {
            case MSG_TEXT: {
                log_layer(4, "broadcasting text message: " + std::string(msg.payload));
                std::cout << "[" << sender_nickname << "]: " << msg.payload << std::endl;
                MessageEx broadcast_msg;
                memset(&broadcast_msg, 0, sizeof(MessageEx));
                snprintf(broadcast_msg.payload, MAX_PAYLOAD, "%s", message.c_str());
                broadcast_msg.length = strlen(broadcast_msg.payload) + 1;
                broadcast_msg.type = MSG_TEXT;
                snprintf(broadcast_msg.sender, MAX_NAME, "%s", sender_nickname.c_str());
                broadcast_msg.receiver[0] = '\0';
                broadcast_msg.timestamp = time(nullptr);
                broadcast_msg.msg_id = id.fetch_add(1);
                broadcast_message(broadcast_msg, client_sock);
                log_message_to_json(broadcast_msg, false);
                break;
            }
            case MSG_PRIVATE:
                log_layer(4, "processing private message");
                send_private_message(target, message, client_sock);
                break;
            case MSG_LIST:
                log_layer(4, "processing message list");
                {
                    MessageEx info_msg;
                    memset(&info_msg, 0, sizeof(MessageEx));
                    snprintf(info_msg.payload, MAX_PAYLOAD, "%s", user_list.c_str());
                    info_msg.length = strlen(info_msg.payload) + 1;
                    info_msg.type = MSG_SERVER_INFO;
                    send_message(client_sock, info_msg);
                }
                break;
            case MSG_HISTORY:
                log_layer(4, "processing message history");
                {
                    int n = parse_history_param(msg.payload);
                    if (n == 0) {
                        MessageEx error_msg;
                        memset(&error_msg, 0, sizeof(MessageEx));
                        snprintf(error_msg.payload, MAX_PAYLOAD, "Invalid parameter. Use /history or /history <positive_number>");
                        error_msg.length = strlen(error_msg.payload) + 1;
                        error_msg.type = MSG_ERROR;
                        send_message(client_sock, error_msg);
                        break;
                    }
                    MessageEx history_msg;
                    memset(&history_msg, 0, sizeof(MessageEx));
                    snprintf(history_msg.payload, MAX_PAYLOAD, "%s", get_history(n, sender_nickname).c_str());
                    history_msg.length = strlen(history_msg.payload) + 1;
                    history_msg.type = MSG_HISTORY_DATA;
                    send_message(client_sock, history_msg);
                }
                break;
            case MSG_PING:
                log_layer(4, "responding to PING with PONG");
                {
                    MessageEx pong_msg;
                    memset(&pong_msg, 0, sizeof(MessageEx));
                    strcpy(pong_msg.payload, "pong");
                    pong_msg.length = strlen(pong_msg.payload) + 1;
                    pong_msg.type = MSG_PONG;
                    send_message(client_sock, pong_msg);
                }
                break;
            case MSG_BYE:
                log_layer(4, "client requested disconnect");
                client.active = false;
                break;
            default:
                log_layer(4, "unknown message type: " + std::to_string(msg.type));
                {
                    MessageEx error_msg;
                    memset(&error_msg, 0, sizeof(MessageEx));
                    snprintf(error_msg.payload, MAX_PAYLOAD, "%s", "unknown message type");
                    error_msg.length = strlen(error_msg.payload) + 1;
                    error_msg.type = MSG_ERROR;
                    send_message(client_sock, error_msg);
                }
        }
    }
    remove_client(client_sock);
    return nullptr;
}

void* worker_thread(void*) {
    while (keepRunning) {
        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty() && keepRunning)
            pthread_cond_wait(&queue_cond, &queue_mutex);
        if (!keepRunning || client_queue.empty()) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        int client_sock = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);
        handle_client(client_sock);
    }
    return nullptr;
}

void add_to_queue(int client_sock) {
    pthread_mutex_lock(&queue_mutex);
    if (client_queue.size() < MAX_QUEUE_SIZE) {
        client_queue.push(client_sock);
        pthread_cond_signal(&queue_cond);
    } else {
        std::cout << "Queue is full, rejecting client" << std::endl;
        close(client_sock);
    }
    pthread_mutex_unlock(&queue_mutex);
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    int server_sock;
    struct sockaddr_in server_addr;
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_sock); exit(1);
    }
    if (listen(server_sock, 10) < 0) {
        perror("listen"); close(server_sock); exit(1);
    }
    id.store(load_max_id() + 1);
    std::cout << "Server listening on port " << PORT << std::endl;
    std::cout << "TCP/IP Layer visualization enabled" << std::endl;
    std::cout << "==================================" << std::endl;
    pthread_t thread_pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create"); exit(1);
        }
        pthread_detach(thread_pool[i]);
    }
    while (keepRunning) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            if (keepRunning) perror("accept");
            continue;
        }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        log_layer(2, "New connection from " + std::string(client_ip) + ":" + std::to_string(ntohs(client_addr.sin_port)));
        std::cout << "New connection from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;
        add_to_queue(client_sock);
    }
    close(server_sock);
    pthread_mutex_lock(&clients_mutex);
    for (auto& client : clients) close(client.socket);
    clients.clear();
    pthread_mutex_unlock(&clients_mutex);
    std::cout << "Server shutting down" << std::endl;
    return 0;
}