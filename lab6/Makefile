CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

all: server client

server: server.cpp messages.h
	$(CXX) $(CXXFLAGS) server.cpp -o server

client: client.cpp messages.h
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client