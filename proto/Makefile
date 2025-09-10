# Makefile for compiling socket communication program
# compilor and flags
CXX = g++
CXXFLAGS = -lpthread

# source files and target executable
SRC = socket_comm.cpp
TARGET = socket_comm

# default target
all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) -o $@ $^ $(CXXFLAGS)

# clean target to remove the executable
clean:
	rm -f $(TARGET)

.PHONY: all clean