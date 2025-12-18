CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread -O2

all: server

server: server.cpp ThreadPool.h Protocol.h Connection.h
	$(CXX) $(CXXFLAGS) -o server server.cpp

clean:
	rm -f server
