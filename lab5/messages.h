#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32

struct MessageEx {
    uint32_t length;
    uint8_t type;
    uint32_t msg_id;

    char sender[MAX_NAME];
    char receiver[MAX_NAME];

    time_t timestamp;
    char payload[MAX_PAYLOAD];
};

enum {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,

    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,

    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14
};

struct HistoryMessage {
    uint32_t msg_id;
    time_t timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool delivered;
    bool is_offline;
};

typedef struct {  
    char sender[MAX_NAME];  
    char receiver[MAX_NAME];  
    char text[MAX_PAYLOAD];  
    time_t timestamp;  
    uint32_t msg_id;  
} OfflineMsg;

struct ClientInfo {
    int socket;
    struct sockaddr_in address;
    char nickname[MAX_NAME];
    bool authenticated;
    bool active;
    
    ClientInfo() : socket(-1), authenticated(false), active(false) {
        memset(&address, 0, sizeof(address));
        memset(nickname, 0, sizeof(nickname));
    }
};