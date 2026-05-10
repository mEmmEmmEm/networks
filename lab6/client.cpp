#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <csignal>
#include <string>
#include <ctime>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cmath>
#include "messages.h"

#define SERVER_PORT 8080
#define SERVER_IP "127.0.0.1"
#define RECONNECT_DELAY 2
#define INPUT_TIMEOUT 1

#define ACK_TIMEOUT_MS 2000
#define MAX_RETRIES 3
#define PING_TIMEOUT_MS 2000

bool keepRunning = true;
bool connected = false;
int sock = -1;

std::string g_nickname;

pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;

std::atomic<uint32_t> client_msg_id{1};

struct PendingMsg {
    MessageEx msg;
    std::chrono::steady_clock::time_point send_time;
    int retries;
    bool acked;
};

struct PingStat {
    uint32_t msg_id;
    int seq;
    std::chrono::steady_clock::time_point send_time;
    bool answered;
    bool timeout;
    double rtt_ms;
    double jitter_ms;
};

std::vector<PendingMsg> pending_msgs;
std::vector<PingStat> ping_stats;

double last_successful_rtt = -1.0;

uint32_t next_msg_id() {
    return client_msg_id.fetch_add(1);
}

std::string msg_type_name(uint8_t type) {
    switch (type) {
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_ACK: return "MSG_ACK";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_BYE: return "MSG_BYE";
        default: return "MSG_" + std::to_string((int)type);
    }
}

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

bool send_all(int socket, const void* data, size_t len) {
    const char* ptr = (const char*)data;
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t sent = send(socket, ptr + sent_total, len - sent_total, 0);
        if (sent <= 0) return false;
        sent_total += sent;
    }

    return true;
}

bool recv_all(int socket, void* data, size_t len) {
    char* ptr = (char*)data;
    size_t received_total = 0;

    while (received_total < len) {
        ssize_t received = recv(socket, ptr + received_total, len - received_total, 0);
        if (received <= 0) return false;
        received_total += received;
    }

    return true;
}

bool send_message(int socket, const MessageEx& msg) {
    MessageEx net_msg = hton_message(msg);
    return send_all(socket, &net_msg, sizeof(MessageEx));
}

bool recv_message(int socket, MessageEx& msg) {
    if (!recv_all(socket, &msg, sizeof(MessageEx))) return false;
    msg = ntoh_message(msg);
    return true;
}

bool send_current_socket(const MessageEx& msg) {
    pthread_mutex_lock(&sock_mutex);
    int current_sock = sock;
    bool ok = current_sock >= 0 && send_message(current_sock, msg);
    pthread_mutex_unlock(&sock_mutex);
    return ok;
}

void add_pending(const MessageEx& msg) {
    PendingMsg p;
    p.msg = msg;
    p.send_time = std::chrono::steady_clock::now();
    p.retries = 0;
    p.acked = false;

    pthread_mutex_lock(&pending_mutex);
    pending_msgs.push_back(p);
    pthread_mutex_unlock(&pending_mutex);
}

void mark_acked(uint32_t msg_id) {
    pthread_mutex_lock(&pending_mutex);

    for (auto& p : pending_msgs) {
        if (p.msg.msg_id == msg_id) {
            p.acked = true;
            std::cout << "\n[Transport][RETRY] ACK received (id=" << msg_id << ")" << std::endl;
            break;
        }
    }

    pthread_mutex_unlock(&pending_mutex);
}

void cleanup_acked_pending() {
    pthread_mutex_lock(&pending_mutex);

    pending_msgs.erase(
        std::remove_if(pending_msgs.begin(), pending_msgs.end(), [](const PendingMsg& p) {
            return p.acked;
        }),
        pending_msgs.end()
    );

    pthread_mutex_unlock(&pending_mutex);
}

bool send_reliable(MessageEx& msg) {
    msg.msg_id = next_msg_id();
    msg.timestamp = time(nullptr);

    std::cout << "[Transport][RETRY] send " << msg_type_name(msg.type)
              << " (id=" << msg.msg_id << ")" << std::endl;

    bool ok = send_current_socket(msg);

    if (ok) {
        add_pending(msg);
    }

    return ok;
}

void save_netdiag_json() {
    pthread_mutex_lock(&ping_mutex);

    int sent = ping_stats.size();
    int received = 0;

    double rtt_sum = 0.0;
    double jitter_sum = 0.0;
    int jitter_count = 0;

    for (const auto& p : ping_stats) {
        if (p.answered) {
            received++;
            rtt_sum += p.rtt_ms;

            if (p.jitter_ms >= 0.0) {
                jitter_sum += p.jitter_ms;
                jitter_count++;
            }
        }
    }

    double rtt_avg = received > 0 ? rtt_sum / received : 0.0;
    double jitter_avg = jitter_count > 0 ? jitter_sum / jitter_count : 0.0;
    double loss = sent > 0 ? ((double)(sent - received) / sent) * 100.0 : 0.0;

    std::string filename = "net_diag_" + g_nickname + ".json";
    std::ofstream out(filename);

    if (out.is_open()) {
        out << "{\n";
        out << "  \"nickname\": \"" << g_nickname << "\",\n";
        out << "  \"sent\": " << sent << ",\n";
        out << "  \"received\": " << received << ",\n";
        out << "  \"rtt_avg_ms\": " << rtt_avg << ",\n";
        out << "  \"jitter_avg_ms\": " << jitter_avg << ",\n";
        out << "  \"loss_percent\": " << loss << ",\n";
        out << "  \"packets\": [\n";

        for (size_t i = 0; i < ping_stats.size(); i++) {
            const auto& p = ping_stats[i];

            out << "    {";
            out << "\"seq\": " << p.seq << ", ";
            out << "\"msg_id\": " << p.msg_id << ", ";
            out << "\"answered\": " << (p.answered ? "true" : "false") << ", ";
            out << "\"rtt_ms\": " << p.rtt_ms << ", ";
            out << "\"jitter_ms\": " << p.jitter_ms;
            out << "}";

            if (i + 1 < ping_stats.size()) out << ",";
            out << "\n";
        }

        out << "  ]\n";
        out << "}\n";
    }

    pthread_mutex_unlock(&ping_mutex);
}

void print_netdiag() {
    pthread_mutex_lock(&ping_mutex);

    int sent = ping_stats.size();
    int received = 0;

    double rtt_sum = 0.0;
    double jitter_sum = 0.0;
    int jitter_count = 0;

    for (const auto& p : ping_stats) {
        if (p.answered) {
            received++;
            rtt_sum += p.rtt_ms;

            if (p.jitter_ms >= 0.0) {
                jitter_sum += p.jitter_ms;
                jitter_count++;
            }
        }
    }

    double rtt_avg = received > 0 ? rtt_sum / received : 0.0;
    double jitter_avg = jitter_count > 0 ? jitter_sum / jitter_count : 0.0;
    double loss = sent > 0 ? ((double)(sent - received) / sent) * 100.0 : 0.0;

    std::cout << "\nRTT avg : " << rtt_avg << " ms" << std::endl;
    std::cout << "Jitter  : " << jitter_avg << " ms" << std::endl;
    std::cout << "Loss    : " << loss << " %" << std::endl;

    pthread_mutex_unlock(&ping_mutex);

    save_netdiag_json();

    std::cout << "Saved: net_diag_" << g_nickname << ".json" << std::endl;
}

void handle_pong(const MessageEx& msg) {
    auto receive_time = std::chrono::steady_clock::now();

    pthread_mutex_lock(&ping_mutex);

    for (auto& p : ping_stats) {
        if (p.msg_id == msg.msg_id && !p.answered && !p.timeout) {
            p.answered = true;

            p.rtt_ms = std::chrono::duration<double, std::milli>(
                receive_time - p.send_time
            ).count();

            if (last_successful_rtt >= 0.0) {
                p.jitter_ms = std::abs(p.rtt_ms - last_successful_rtt);
            } else {
                p.jitter_ms = -1.0;
            }

            last_successful_rtt = p.rtt_ms;

            std::cout << "\nPING " << p.seq << " -> RTT=" << p.rtt_ms << "ms";

            if (p.jitter_ms >= 0.0) {
                std::cout << " | Jitter=" << p.jitter_ms << "ms";
            }

            std::cout << std::endl;
            break;
        }
    }

    pthread_mutex_unlock(&ping_mutex);
}

void* retry_thread(void*) {
    while (keepRunning) {
        usleep(200 * 1000);

        if (!connected) continue;

        std::vector<MessageEx> to_resend;
        auto now = std::chrono::steady_clock::now();

        pthread_mutex_lock(&pending_mutex);

        for (auto& p : pending_msgs) {
            if (p.acked) continue;

            double elapsed = std::chrono::duration<double, std::milli>(
                now - p.send_time
            ).count();

            if (elapsed >= ACK_TIMEOUT_MS) {
                std::cout << "\n[Transport][RETRY] wait ACK timeout" << std::endl;

                if (p.retries >= MAX_RETRIES) {
                    std::cout << "[Transport][RETRY] delivery failed (id="
                              << p.msg.msg_id << ")" << std::endl;
                    p.acked = true;
                } else {
                    p.retries++;
                    p.send_time = now;
                    to_resend.push_back(p.msg);

                    std::cout << "[Transport][RETRY] resend "
                              << p.retries << "/" << MAX_RETRIES
                              << " (id=" << p.msg.msg_id << ")" << std::endl;
                }
            }
        }

        pthread_mutex_unlock(&pending_mutex);

        for (const auto& msg : to_resend) {
            send_current_socket(msg);
        }

        cleanup_acked_pending();
    }

    return nullptr;
}

void* ping_timeout_thread(void*) {
    while (keepRunning) {
        usleep(100 * 1000);

        auto now = std::chrono::steady_clock::now();

        pthread_mutex_lock(&ping_mutex);

        for (auto& p : ping_stats) {
            if (!p.answered && !p.timeout) {
                double elapsed = std::chrono::duration<double, std::milli>(
                    now - p.send_time
                ).count();

                if (elapsed >= PING_TIMEOUT_MS) {
                    p.timeout = true;
                    std::cout << "\nPING " << p.seq << " -> timeout" << std::endl;
                }
            }
        }

        pthread_mutex_unlock(&ping_mutex);
    }

    return nullptr;
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
            if (sock >= 0) close(sock);
            sock = -1;
            pthread_mutex_unlock(&sock_mutex);

            break;
        }

        switch (msg.type) {
            case MSG_ACK:
                mark_acked(msg.msg_id);
                break;

            case MSG_WELCOME:
                std::cout << "\n*** " << msg.payload << " ***" << std::endl;
                break;

            case MSG_TEXT:
                std::cout << "[" << format_timestamp(msg.timestamp) << "]"
                          << "\n[id=" << msg.msg_id << "]"
                          << "[" << msg.sender << "]: "
                          << msg.payload << std::endl;
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
                handle_pong(msg);
                break;

            case MSG_BYE:
                std::cout << "\n*** Server closed connection ***" << std::endl;

                pthread_mutex_lock(&sock_mutex);
                connected = false;
                if (sock >= 0) close(sock);
                sock = -1;
                pthread_mutex_unlock(&sock_mutex);

                break;

            case MSG_HISTORY_DATA:
                std::cout << "\n[HISTORY]:\n" << msg.payload << std::endl;
                break;

            default:
                std::cout << "\n*** Unknown message type: "
                          << (int)msg.type << " ***" << std::endl;
        }

        std::cout << "> ";
        fflush(stdout);
    }

    return nullptr;
}

int connect_to_server() {
    int new_sock = socket(AF_INET, SOCK_STREAM, 0);

    if (new_sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(new_sock);
        return -1;
    }

    if (connect(new_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(new_sock);
        return -1;
    }

    return new_sock;
}

bool authenticate(int socket, const std::string& nickname) {
    MessageEx auth_msg;
    memset(&auth_msg, 0, sizeof(MessageEx));

    strncpy(auth_msg.payload, nickname.c_str(), MAX_NAME - 1);
    auth_msg.payload[MAX_NAME - 1] = '\0';

    auth_msg.length = strlen(auth_msg.payload) + 1;
    auth_msg.type = MSG_AUTH;
    auth_msg.msg_id = next_msg_id();
    auth_msg.timestamp = time(nullptr);

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

int parse_ping_count(const char* input) {
    if (strcmp(input, "/ping") == 0) return 10;

    if (strncmp(input, "/ping ", 6) != 0) return -1;

    const char* n_str = input + 6;

    if (*n_str == '\0') return -1;

    for (int i = 0; n_str[i]; i++) {
        if (!std::isdigit((unsigned char)n_str[i])) return -1;
    }

    int n = atoi(n_str);

    if (n <= 0) return -1;

    return n;
}

void start_ping_series(int n) {
    pthread_mutex_lock(&ping_mutex);
    ping_stats.clear();
    last_successful_rtt = -1.0;
    pthread_mutex_unlock(&ping_mutex);

    for (int i = 1; i <= n; i++) {
        MessageEx msg;
        memset(&msg, 0, sizeof(MessageEx));

        strcpy(msg.payload, "ping");
        msg.length = strlen(msg.payload) + 1;
        msg.type = MSG_PING;
        msg.msg_id = next_msg_id();
        msg.timestamp = time(nullptr);

        PingStat stat;
        stat.msg_id = msg.msg_id;
        stat.seq = i;
        stat.send_time = std::chrono::steady_clock::now();
        stat.answered = false;
        stat.timeout = false;
        stat.rtt_ms = 0.0;
        stat.jitter_ms = -1.0;

        pthread_mutex_lock(&ping_mutex);
        ping_stats.push_back(stat);
        pthread_mutex_unlock(&ping_mutex);

        std::cout << "[Transport][PING] send MSG_PING (id="
                  << msg.msg_id << ")" << std::endl;

        send_current_socket(msg);
        add_pending(msg);

        usleep(100 * 1000);
    }
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    pthread_t recv_thread_id;
    pthread_t retry_thread_id;
    pthread_t ping_timeout_thread_id;

    std::cout << "Enter your nickname: ";
    std::getline(std::cin, g_nickname);

    while (g_nickname.empty()) {
        std::cout << "Nickname cannot be empty. Enter your nickname: ";
        std::getline(std::cin, g_nickname);
    }

    pthread_create(&retry_thread_id, NULL, retry_thread, NULL);
    pthread_detach(retry_thread_id);

    pthread_create(&ping_timeout_thread_id, NULL, ping_timeout_thread, NULL);
    pthread_detach(ping_timeout_thread_id);

    while (keepRunning) {
        std::cout << "Connecting to " << SERVER_IP << ":" << SERVER_PORT << "..." << std::endl;

        pthread_mutex_lock(&sock_mutex);
        sock = connect_to_server();
        pthread_mutex_unlock(&sock_mutex);

        if (sock < 0) {
            std::cout << "Connection failed. Retrying in "
                      << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
            continue;
        }

        if (!authenticate(sock, g_nickname)) {
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

        if (pthread_create(&recv_thread_id, NULL, receive_thread, NULL) != 0) {
            perror("pthread_create");
            break;
        }

        char input[MAX_PAYLOAD];

        std::cout << "> " << std::flush;

        while (keepRunning && connected) {
            if (!read_input_with_timeout(input, MAX_PAYLOAD, INPUT_TIMEOUT)) {
                continue;
            }

            if (strlen(input) == 0) {
                continue;
            }

            MessageEx msg;
            memset(&msg, 0, sizeof(MessageEx));

            if (strcmp(input, "/quit") == 0) {
                strcpy(msg.payload, "bye");
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_BYE;
                msg.msg_id = next_msg_id();
                msg.timestamp = time(nullptr);

                send_current_socket(msg);

                connected = false;
                keepRunning = false;
                break;
            }
            else if (strncmp(input, "/ping", 5) == 0) {
                int n = parse_ping_count(input);

                if (n <= 0) {
                    std::cout << "Usage: /ping or /ping N" << std::endl;
                } else {
                    start_ping_series(n);
                }
            }
            else if (strcmp(input, "/netdiag") == 0) {
                print_netdiag();
            }
            else if (strncmp(input, "/w ", 3) == 0) {
                char* target = strtok(input + 3, " ");
                char* message = strtok(NULL, "");

                if (target && message && strlen(message) > 0) {
                    snprintf(msg.payload, MAX_PAYLOAD, "%s", message);
                    msg.length = strlen(msg.payload) + 1;
                    msg.type = MSG_PRIVATE;
                    snprintf(msg.receiver, MAX_NAME, "%s", target);

                    send_reliable(msg);
                } else {
                    std::cout << "Usage: /w <nickname> <message>" << std::endl;
                }
            }
            else if (strcmp(input, "/help") == 0) {
                std::cout << "  /help                              - Show this help message\n";
                std::cout << "  /list                              - Show online users list\n";
                std::cout << "  /history                           - Show all last messages\n";
                std::cout << "  /history N                         - Show last N messages\n";
                std::cout << "  /quit                              - Disconnect from server\n";
                std::cout << "  /w <nick> <message>                - Send private message\n";
                std::cout << "  /ping                              - Send 10 ping requests\n";
                std::cout << "  /ping N                            - Send N ping requests\n";
                std::cout << "  /netdiag                           - Show RTT/jitter/loss\n";
            }
            else if (strcmp(input, "/list") == 0) {
                strcpy(msg.payload, "list");
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_LIST;
                msg.msg_id = next_msg_id();
                msg.timestamp = time(nullptr);

                send_current_socket(msg);
            }
            else if (strncmp(input, "/history", 8) == 0) {
                if (strlen(input) == 8) {
                    strcpy(msg.payload, "");
                    msg.length = 1;
                    msg.type = MSG_HISTORY;
                    msg.msg_id = next_msg_id();
                    msg.timestamp = time(nullptr);

                    send_current_socket(msg);
                }
                else if (strlen(input) > 8 && input[8] == ' ') {
                    const char* param = input + 9;
                    bool valid = true;

                    for (size_t i = 0; param[i] != '\0'; i++) {
                        if (!std::isdigit((unsigned char)param[i])) {
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
                            msg.msg_id = next_msg_id();
                            msg.timestamp = time(nullptr);

                            send_current_socket(msg);
                        } else {
                            std::cout << "Error: N must be a positive number." << std::endl;
                        }
                    } else {
                        std::cout << "Error: Invalid parameter. Use /history or /history <positive_number>" << std::endl;
                    }
                } else {
                    std::cout << "Error: Use /history or /history <positive_number>" << std::endl;
                }
            }
            else if (input[0] == '/') {
                std::cout << "Unknown command" << std::endl;
            }
            else {
                strncpy(msg.payload, input, MAX_PAYLOAD - 1);
                msg.payload[MAX_PAYLOAD - 1] = '\0';
                msg.length = strlen(msg.payload) + 1;
                msg.type = MSG_TEXT;

                send_reliable(msg);
            }

            std::cout << "> " << std::flush;
        }

        pthread_join(recv_thread_id, NULL);

        if (keepRunning && !connected) {
            std::cout << "Attempting to reconnect in "
                      << RECONNECT_DELAY << " seconds..." << std::endl;
            sleep(RECONNECT_DELAY);
        }
    }

    pthread_mutex_lock(&sock_mutex);

    if (sock >= 0) {
        close(sock);
    }

    pthread_mutex_unlock(&sock_mutex);

    std::cout << "Client shutdown" << std::endl;

    return 0;
}