#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <vector>
#include <queue>
#include <algorithm>
#include <random>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>
#include <ctime>
#include "messages.h"

#define PORT 8080
#define THREAD_POOL_SIZE 10
#define MAX_QUEUE_SIZE 100
#define MAX_HISTORY 100
#define LOG_FILE "messages_log.json"

bool keepRunning = true;
std::atomic<uint32_t> id{1};

int sim_delay_ms = 0;
double sim_drop_rate = 0.0;
double sim_corrupt_rate = 0.0;

std::mt19937 rng((unsigned)time(nullptr));
pthread_mutex_t rng_mutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<ClientInfo> clients;
std::vector<OfflineMsg> offline;

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t json_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
std::queue<int> client_queue;

thread_local bool simulated_drop_happened = false;

void log_layer_name(const std::string& layer, const std::string& message) {
    std::cout << "[" << layer << "] " << message << std::endl;
}

void log_layer(int layer, const std::string& message) {
    switch (layer) {
        case 1:
            log_layer_name("Network Access", message);
            break;
        case 2:
            log_layer_name("Internet", message);
            break;
        case 3:
            log_layer_name("Transport", message);
            break;
        case 4:
            log_layer_name("Application", message);
            break;
        default:
            log_layer_name("Unknown", message);
            break;
    }
}

std::string msg_type_name(uint8_t type) {
    switch (type) {
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_ACK: return "MSG_ACK";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_BYE: return "MSG_BYE";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        default: return "MSG_" + std::to_string((int)type);
    }
}

void handleSignal(int) {
    keepRunning = false;
    pthread_cond_broadcast(&queue_cond);
}

bool random_event(double probability) {
    if (probability <= 0.0) return false;

    pthread_mutex_lock(&rng_mutex);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    bool result = dist(rng) < probability;
    pthread_mutex_unlock(&rng_mutex);

    return result;
}

int random_payload_index(int max_len) {
    pthread_mutex_lock(&rng_mutex);
    std::uniform_int_distribution<int> dist(0, max_len - 1);
    int result = dist(rng);
    pthread_mutex_unlock(&rng_mutex);

    return result;
}

bool apply_network_simulation(MessageEx& msg) {
    if (sim_delay_ms > 0) {
        usleep(sim_delay_ms * 1000);

        std::cout << "[Transport][SIM] DELAY applied: "
                  << sim_delay_ms << " ms" << std::endl;
    }

    if (random_event(sim_drop_rate)) {
        std::cout << "[Transport][SIM] DROP (id="
                  << msg.msg_id << ", rate="
                  << sim_drop_rate << ")" << std::endl;

        return false;
    }

    if (random_event(sim_corrupt_rate) && msg.length > 1) {
        int payload_len = std::min((int)strlen(msg.payload), MAX_PAYLOAD - 1);

        if (payload_len > 0) {
            int pos = random_payload_index(payload_len);
            msg.payload[pos] = msg.payload[pos] ^ 0x01;

            std::cout << "[Transport][SIM] CORRUPT payload (id="
                      << msg.msg_id << ")" << std::endl;
        }
    }

    return true;
}

std::string unescape_json(const std::string& s) {
    std::string result;

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                default:
                    result += s[i + 1];
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

    if (start == std::string::npos) {
        return "";
    }

    start += pattern.length();

    size_t i = start;

    while (i < line.size()) {
        if (line[i] == '"' && line[i - 1] != '\\') {
            break;
        }

        i++;
    }

    return unescape_json(line.substr(start, i - start));
}

long extract_number(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";

    size_t start = line.find(pattern);

    if (start == std::string::npos) {
        return 0;
    }

    start += pattern.length();

    size_t end = line.find_first_of(",}", start);

    return std::stol(line.substr(start, end - start));
}

bool extract_bool(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";

    size_t start = line.find(pattern);

    if (start == std::string::npos) {
        return false;
    }

    start += pattern.length();

    return line.substr(start, 4) == "true";
}

std::string escape_json(const std::string& s) {
    std::string result;

    for (char c : s) {
        switch (c) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
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
        uint32_t current_id = extract_number(line, "msg_id");

        if (current_id == msg_id) {
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

    for (auto& l : lines) {
        out << l << "\n";
    }

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
        uint32_t current_id = extract_number(line, "msg_id");

        if (current_id > max_id) {
            max_id = current_id;
        }
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
        if (line.empty()) {
            continue;
        }

        HistoryMessage msg;

        msg.msg_id = extract_number(line, "msg_id");
        msg.timestamp = extract_number(line, "timestamp");
        msg.sender = extract_string(line, "sender");
        msg.receiver = extract_string(line, "receiver");
        msg.type = extract_string(line, "type");
        msg.text = extract_string(line, "text");
        msg.is_offline = extract_bool(line, "is_offline");

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
        bool is_for_requester = (
            msg.receiver == requesting_user ||
            msg.sender == requesting_user
        );

        result << "[" << time_str << "]";
        result << "[id=" << msg.msg_id << "]";

        if (msg.is_offline) {
            if (is_for_requester) {
                result << "[OFFLINE]["
                       << msg.sender << " -> " << msg.receiver
                       << "]: " << msg.text;
            } else {
                result << "[OFFLINE]["
                       << msg.sender << " -> " << msg.receiver
                       << "]: " << std::string(msg.text.length(), '*');
            }
        }
        else if (is_private) {
            if (is_for_requester) {
                result << "[PRIVATE]["
                       << msg.sender << " -> " << msg.receiver
                       << "]: " << msg.text;
            } else {
                result << "[PRIVATE]["
                       << msg.sender << " -> " << msg.receiver
                       << "]: " << std::string(msg.text.length(), '*');
            }
        } else {
            result << "[" << msg.sender << "]: " << msg.text;
        }

        result << "\n";
    }

    if (result.str().empty()) {
        return "No messages in history.\n";
    }

    return result.str();
}

MessageEx ntoh_message(const MessageEx& net_msg) {
    log_layer(4, "deserialize Message network->host");

    MessageEx host_msg = net_msg;

    host_msg.length = ntohl(net_msg.length);
    host_msg.msg_id = ntohl(net_msg.msg_id);

    return host_msg;
}

MessageEx hton_message(const MessageEx& host_msg) {
    log_layer(4, "serialize Message host->network");

    MessageEx net_msg = host_msg;

    net_msg.length = htonl(host_msg.length);
    net_msg.msg_id = htonl(host_msg.msg_id);

    return net_msg;
}

bool send_all(int socket, const void* data, size_t len) {
    const char* ptr = (const char*)data;
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t sent = send(socket, ptr + sent_total, len - sent_total, 0);

        if (sent <= 0) {
            return false;
        }

        sent_total += sent;
    }

    return true;
}

bool recv_all(int socket, void* data, size_t len) {
    char* ptr = (char*)data;
    size_t received_total = 0;

    while (received_total < len) {
        ssize_t received = recv(socket, ptr + received_total, len - received_total, 0);

        if (received <= 0) {
            return false;
        }

        received_total += received;
    }

    return true;
}

bool send_message(int socket, const MessageEx& msg) {
    log_layer(3, "send() - transmitting data");

    MessageEx net_msg = hton_message(msg);

    return send_all(socket, &net_msg, sizeof(MessageEx));
}

bool recv_message_raw(int socket, MessageEx& msg) {
    log_layer(3, "recv() - receiving data");

    if (!recv_all(socket, &msg, sizeof(MessageEx))) {
        return false;
    }

    msg = ntoh_message(msg);

    return true;
}

bool recv_message_simulated(int socket, MessageEx& msg) {
    simulated_drop_happened = false;

    if (!recv_message_raw(socket, msg)) {
        return false;
    }

    if (!apply_network_simulation(msg)) {
        simulated_drop_happened = true;
        return false;
    }

    return true;
}

ClientInfo* find_client_by_socket_unlocked(int socket) {
    for (auto& client : clients) {
        if (client.socket == socket) {
            return &client;
        }
    }

    return nullptr;
}

ClientInfo* find_client_by_nickname_unlocked(const std::string& nickname) {
    for (auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) {
            return &client;
        }
    }

    return nullptr;
}

bool is_duplicate_and_store(int client_sock, uint32_t msg_id) {
    pthread_mutex_lock(&clients_mutex);

    ClientInfo* client = find_client_by_socket_unlocked(client_sock);

    if (!client) {
        pthread_mutex_unlock(&clients_mutex);
        return true;
    }

    for (uint32_t last_id : client->last_ids) {
        if (last_id == msg_id && msg_id != 0) {
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }

    client->last_ids[client->last_id_pos] = msg_id;
    client->last_id_pos = (client->last_id_pos + 1) % 32;

    pthread_mutex_unlock(&clients_mutex);

    return false;
}

void send_ack(int client_sock, uint32_t msg_id) {
    MessageEx ack;
    memset(&ack, 0, sizeof(MessageEx));

    ack.type = MSG_ACK;
    ack.msg_id = msg_id;
    ack.timestamp = time(nullptr);

    strcpy(ack.payload, "ack");

    ack.length = strlen(ack.payload) + 1;

    std::cout << "[Transport][ACK] send MSG_ACK (id="
              << msg_id << ")" << std::endl;

    send_message(client_sock, ack);
}

std::string get_online_users_list() {
    std::string result = "[SERVER]: Online users\n";

    pthread_mutex_lock(&clients_mutex);

    for (const auto& client : clients) {
        if (client.authenticated && client.active && strlen(client.nickname) > 0) {
            result += "  " + std::string(client.nickname) + "\n";
        }
    }

    pthread_mutex_unlock(&clients_mutex);

    return result;
}

int parse_history_param(const char* payload) {
    if (payload == nullptr || payload[0] == '\0') {
        return -1;
    }

    char* endptr;
    long val = strtol(payload, &endptr, 10);

    if (endptr == payload || *endptr != '\0' || val <= 0) {
        return 0;
    }

    if (val > MAX_HISTORY) {
        val = MAX_HISTORY;
    }

    return (int)val;
}

bool is_nickname_unique_unlocked(const std::string& nickname) {
    for (const auto& client : clients) {
        if (client.authenticated && std::string(client.nickname) == nickname) {
            return false;
        }
    }

    return true;
}

void remove_client(int socket) {
    log_layer(4, "remove client");

    pthread_mutex_lock(&clients_mutex);

    auto it = std::find_if(
        clients.begin(),
        clients.end(),
        [socket](const ClientInfo& c) {
            return c.socket == socket;
        }
    );

    if (it != clients.end()) {
        if (it->authenticated) {
            MessageEx info_msg;
            memset(&info_msg, 0, sizeof(MessageEx));

            info_msg.type = MSG_SERVER_INFO;

            snprintf(
                info_msg.payload,
                MAX_PAYLOAD,
                "User [%s] disconnected",
                it->nickname
            );

            info_msg.length = strlen(info_msg.payload) + 1;

            for (const auto& client : clients) {
                if (client.socket != socket && client.authenticated) {
                    send_message(client.socket, info_msg);
                }
            }

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

    ClientInfo* sender = find_client_by_socket_unlocked(sender_socket);

    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    for (const auto& client : clients) {
        if (client.socket != sender_socket && client.authenticated) {
            send_message(client.socket, msg);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

void send_private_message(
    const std::string& target_nick,
    const std::string& message,
    int sender_socket,
    uint32_t original_msg_id
) {
    log_layer(4, "private message started");

    pthread_mutex_lock(&clients_mutex);

    ClientInfo* sender = find_client_by_socket_unlocked(sender_socket);

    if (!sender || !sender->authenticated) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    std::string sender_name = sender->nickname;

    ClientInfo* target = find_client_by_nickname_unlocked(target_nick);

    int target_socket = target ? target->socket : -1;

    pthread_mutex_unlock(&clients_mutex);

    MessageEx private_msg;
    memset(&private_msg, 0, sizeof(MessageEx));

    snprintf(private_msg.payload, MAX_PAYLOAD, "%s", message.c_str());

    private_msg.length = strlen(private_msg.payload) + 1;
    private_msg.type = MSG_PRIVATE;
    private_msg.msg_id = original_msg_id;

    snprintf(private_msg.sender, MAX_NAME, "%s", sender_name.c_str());
    snprintf(private_msg.receiver, MAX_NAME, "%s", target_nick.c_str());

    private_msg.timestamp = time(nullptr);

    if (target_socket < 0) {
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
    else if (!send_message(target_socket, private_msg)) {
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));

        snprintf(
            error_msg.payload,
            MAX_PAYLOAD,
            "Failed to send message to '%s'",
            target_nick.c_str()
        );

        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;

        send_message(sender_socket, error_msg);
    } else {
        std::cout << "[PRIVATE] "
                  << sender_name << " -> "
                  << target_nick << ": "
                  << message << std::endl;

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

    pthread_mutex_lock(&clients_mutex);

    if (!is_nickname_unique_unlocked(nickname)) {
        pthread_mutex_unlock(&clients_mutex);

        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));

        snprintf(
            error_msg.payload,
            MAX_PAYLOAD,
            "Nickname '%s' is already taken",
            nickname.c_str()
        );

        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;

        send_message(client_sock, error_msg);

        return false;
    }

    ClientInfo* client = find_client_by_socket_unlocked(client_sock);

    if (!client) {
        pthread_mutex_unlock(&clients_mutex);
        return false;
    }

    strncpy(client->nickname, nickname.c_str(), MAX_NAME - 1);
    client->nickname[MAX_NAME - 1] = '\0';
    client->authenticated = true;

    pthread_mutex_unlock(&clients_mutex);

    log_layer(4, "authentication success: " + nickname);

    MessageEx welcome_msg;
    memset(&welcome_msg, 0, sizeof(MessageEx));

    snprintf(
        welcome_msg.payload,
        MAX_PAYLOAD,
        "Welcome %s",
        nickname.c_str()
    );

    welcome_msg.length = strlen(welcome_msg.payload) + 1;
    welcome_msg.type = MSG_WELCOME;

    send_message(client_sock, welcome_msg);

    MessageEx info_msg;
    memset(&info_msg, 0, sizeof(MessageEx));

    snprintf(
        info_msg.payload,
        MAX_PAYLOAD,
        "User [%s] connected",
        nickname.c_str()
    );

    info_msg.length = strlen(info_msg.payload) + 1;
    info_msg.type = MSG_SERVER_INFO;

    pthread_mutex_lock(&clients_mutex);

    for (const auto& c : clients) {
        if (c.socket != client_sock && c.authenticated) {
            send_message(c.socket, info_msg);
        }
    }

    pthread_mutex_unlock(&clients_mutex);

    std::cout << "User [" << nickname << "] connected" << std::endl;

    return true;
}

void send_offline_messages(int client_sock) {
    pthread_mutex_lock(&offline_mutex);
    pthread_mutex_lock(&clients_mutex);

    ClientInfo* auth_client = find_client_by_socket_unlocked(client_sock);

    if (!auth_client) {
        pthread_mutex_unlock(&clients_mutex);
        pthread_mutex_unlock(&offline_mutex);
        return;
    }

    std::string nickname = auth_client->nickname;
    int target_socket = auth_client->socket;

    pthread_mutex_unlock(&clients_mutex);

    for (auto it = offline.begin(); it != offline.end();) {
        if (strcmp(it->receiver, nickname.c_str()) == 0) {
            MessageEx off_msg;
            memset(&off_msg, 0, sizeof(MessageEx));

            snprintf(off_msg.payload, MAX_PAYLOAD, "[OFFLINE]%s", it->text);

            off_msg.length = strlen(off_msg.payload) + 1;
            off_msg.type = MSG_PRIVATE;

            snprintf(off_msg.sender, MAX_NAME, "%s", it->sender);
            snprintf(off_msg.receiver, MAX_NAME, "%s", it->receiver);

            off_msg.timestamp = it->timestamp;
            off_msg.msg_id = it->msg_id;

            if (send_message(target_socket, off_msg)) {
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
}

void handle_client(int client_sock) {
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

    if (!recv_message_raw(client_sock, msg) || msg.type != MSG_AUTH) {
        MessageEx error_msg;
        memset(&error_msg, 0, sizeof(MessageEx));

        strcpy(error_msg.payload, "Authentication required");

        error_msg.length = strlen(error_msg.payload) + 1;
        error_msg.type = MSG_ERROR;

        send_message(client_sock, error_msg);
        remove_client(client_sock);

        return;
    }

    log_layer(4, "received MSG_AUTH, payload: " + std::string(msg.payload));

    if (!authenticate_client(client_sock, msg.payload)) {
        remove_client(client_sock);
        return;
    }

    send_offline_messages(client_sock);

    while (keepRunning) {
        bool received = recv_message_simulated(client_sock, msg);

        if (!received) {
            if (!keepRunning) {
                break;
            }

            if (simulated_drop_happened) {
                continue;
            }

            log_layer(4, "client disconnected");
            break;
        }

        pthread_mutex_lock(&clients_mutex);

        ClientInfo* sender_ptr = find_client_by_socket_unlocked(client_sock);

        if (!sender_ptr) {
            pthread_mutex_unlock(&clients_mutex);
            break;
        }

        std::string sender_nickname = sender_ptr->nickname;

        pthread_mutex_unlock(&clients_mutex);

        log_layer(4, "processing message type: " + msg_type_name(msg.type));

        if (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE || msg.type == MSG_PING) {
            if (is_duplicate_and_store(client_sock, msg.msg_id)) {
                std::cout << "[Application][DEDUP] duplicate ignored (id="
                          << msg.msg_id << ")" << std::endl;

                send_ack(client_sock, msg.msg_id);

                continue;
            }
        }

        std::string target = msg.receiver;
        std::string message = msg.payload;

        switch (msg.type) {
            case MSG_TEXT: {
                std::cout << "[Application][ACK] process MSG_TEXT (id="
                          << msg.msg_id << ")" << std::endl;

                std::cout << "[" << sender_nickname << "]: "
                          << msg.payload << std::endl;

                MessageEx broadcast_msg;
                memset(&broadcast_msg, 0, sizeof(MessageEx));

                snprintf(broadcast_msg.payload, MAX_PAYLOAD, "%s", message.c_str());

                broadcast_msg.length = strlen(broadcast_msg.payload) + 1;
                broadcast_msg.type = MSG_TEXT;

                snprintf(broadcast_msg.sender, MAX_NAME, "%s", sender_nickname.c_str());

                broadcast_msg.receiver[0] = '\0';
                broadcast_msg.timestamp = time(nullptr);
                broadcast_msg.msg_id = msg.msg_id;

                broadcast_message(broadcast_msg, client_sock);
                log_message_to_json(broadcast_msg, false);
                send_ack(client_sock, msg.msg_id);

                break;
            }

            case MSG_PRIVATE: {
                std::cout << "[Application][ACK] process MSG_PRIVATE (id="
                          << msg.msg_id << ")" << std::endl;

                send_private_message(target, message, client_sock, msg.msg_id);
                send_ack(client_sock, msg.msg_id);

                break;
            }

            case MSG_LIST: {
                log_layer(4, "processing user list");

                MessageEx info_msg;
                memset(&info_msg, 0, sizeof(MessageEx));

                std::string user_list = get_online_users_list();

                snprintf(info_msg.payload, MAX_PAYLOAD, "%s", user_list.c_str());

                info_msg.length = strlen(info_msg.payload) + 1;
                info_msg.type = MSG_SERVER_INFO;

                send_message(client_sock, info_msg);

                break;
            }

            case MSG_HISTORY: {
                log_layer(4, "processing message history");

                int n = parse_history_param(msg.payload);

                if (n == 0) {
                    MessageEx error_msg;
                    memset(&error_msg, 0, sizeof(MessageEx));

                    snprintf(
                        error_msg.payload,
                        MAX_PAYLOAD,
                        "Invalid parameter. Use /history or /history <positive_number>"
                    );

                    error_msg.length = strlen(error_msg.payload) + 1;
                    error_msg.type = MSG_ERROR;

                    send_message(client_sock, error_msg);

                    break;
                }

                MessageEx history_msg;
                memset(&history_msg, 0, sizeof(MessageEx));

                std::string history = get_history(n, sender_nickname);

                snprintf(history_msg.payload, MAX_PAYLOAD, "%s", history.c_str());

                history_msg.length = strlen(history_msg.payload) + 1;
                history_msg.type = MSG_HISTORY_DATA;

                send_message(client_sock, history_msg);

                break;
            }

            case MSG_PING: {
                std::cout << "[Transport][PING] recv MSG_PING (id="
                          << msg.msg_id << ")" << std::endl;

                MessageEx pong_msg;
                memset(&pong_msg, 0, sizeof(MessageEx));

                strcpy(pong_msg.payload, "pong");

                pong_msg.length = strlen(pong_msg.payload) + 1;
                pong_msg.type = MSG_PONG;
                pong_msg.msg_id = msg.msg_id;
                pong_msg.timestamp = time(nullptr);

                std::cout << "[Transport][PING] send MSG_PONG (id="
                          << msg.msg_id << ")" << std::endl;

                send_message(client_sock, pong_msg);
                send_ack(client_sock, msg.msg_id);

                break;
            }

            case MSG_BYE: {
                log_layer(4, "client requested disconnect");

                remove_client(client_sock);

                return;
            }

            default: {
                log_layer(4, "unknown message type: " + std::to_string(msg.type));

                MessageEx error_msg;
                memset(&error_msg, 0, sizeof(MessageEx));

                snprintf(error_msg.payload, MAX_PAYLOAD, "%s", "unknown message type");

                error_msg.length = strlen(error_msg.payload) + 1;
                error_msg.type = MSG_ERROR;

                send_message(client_sock, error_msg);

                break;
            }
        }
    }

    remove_client(client_sock);
}

void* worker_thread(void*) {
    while (keepRunning) {
        pthread_mutex_lock(&queue_mutex);

        while (client_queue.empty() && keepRunning) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }

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

void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--delay=", 8) == 0) {
            sim_delay_ms = atoi(argv[i] + 8);
        }
        else if (strncmp(argv[i], "--drop=", 7) == 0) {
            sim_drop_rate = atof(argv[i] + 7);

            if (sim_drop_rate < 0.0) {
                sim_drop_rate = 0.0;
            }

            if (sim_drop_rate > 1.0) {
                sim_drop_rate = 1.0;
            }
        }
        else if (strncmp(argv[i], "--corrupt=", 10) == 0) {
            sim_corrupt_rate = atof(argv[i] + 10);

            if (sim_corrupt_rate < 0.0) {
                sim_corrupt_rate = 0.0;
            }

            if (sim_corrupt_rate > 1.0) {
                sim_corrupt_rate = 1.0;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    parse_args(argc, argv);

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    int server_sock;
    struct sockaddr_in server_addr;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;

    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        exit(1);
    }

    if (listen(server_sock, 10) < 0) {
        perror("listen");
        close(server_sock);
        exit(1);
    }

    id.store(load_max_id() + 1);

    std::cout << "Server listening on port " << PORT << std::endl;
    std::cout << "TCP/IP Layer visualization enabled" << std::endl;
    std::cout << "Simulation: delay=" << sim_delay_ms
              << "ms, drop=" << sim_drop_rate
              << ", corrupt=" << sim_corrupt_rate << std::endl;
    std::cout << "==================================" << std::endl;

    pthread_t thread_pool[THREAD_POOL_SIZE];

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }

        pthread_detach(thread_pool[i]);
    }

    while (keepRunning) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(
            server_sock,
            (struct sockaddr*)&client_addr,
            &client_len
        );

        if (client_sock < 0) {
            if (keepRunning) {
                perror("accept");
            }

            continue;
        }

        char client_ip[INET_ADDRSTRLEN];

        inet_ntop(
            AF_INET,
            &client_addr.sin_addr,
            client_ip,
            INET_ADDRSTRLEN
        );

        log_layer(
            2,
            "New connection from " +
            std::string(client_ip) +
            ":" +
            std::to_string(ntohs(client_addr.sin_port))
        );

        std::cout << "New connection from "
                  << client_ip << ":"
                  << ntohs(client_addr.sin_port)
                  << std::endl;

        add_to_queue(client_sock);
    }

    close(server_sock);

    pthread_mutex_lock(&clients_mutex);

    for (auto& client : clients) {
        close(client.socket);
    }

    clients.clear();

    pthread_mutex_unlock(&clients_mutex);

    std::cout << "Server shutting down" << std::endl;

    return 0;
}