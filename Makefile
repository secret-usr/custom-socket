# ==================================================================
# Makefile for compiling socket communication program
# ==================================================================

# compilor and flags
CXX = g++
CXXFLAGS = -lpthread

# NOTE: Default build adds no explicit log-level macro.
# Program source must provide its own default behavior when no -DDEBUG/-DINFO/-DWARN/-DERROR is given.

# source files and target executable
SRC = socket_comm.cpp
TARGET = socket_comm
TESTER_SRC = test/test_client.cpp
TESTER_TARGET = tester

# default target
all: clean $(TARGET)

$(TARGET): $(SRC)
	$(CXX) -o $@ $^ $(CXXFLAGS)

# clean target to remove the executable
clean:
	rm -f $(TARGET)

cleantester:
	rm -f $(TESTER_TARGET)

# log level build targets (force rebuild via clean first)
# Each target appends a compile-time macro to enable logging scope.
debug: CXXFLAGS += -DDEBUG
debug: clean $(TARGET)

info: CXXFLAGS += -DINFO
info: clean $(TARGET)

warning: CXXFLAGS += -DWARN
warning: clean $(TARGET)

error: CXXFLAGS += -DERROR
error: clean $(TARGET)

# build alias equals error log level
build: error

# run target
run: clean $(TARGET)
	./$(TARGET)

# generate function call graph for socket_comm.cpp
# Requires: python3, graphviz (dot)
callgraph: $(SRC)
	python3 utils/func-call-analyzer/func-call-analyzer.py $(SRC) --dot callgraph.dot --max-depth 6
	dot -Tpng callgraph.dot -o callgraph.png
	rm -f callgraph.dot

# tester target
tester: cleantester $(TESTER_SRC)
	$(CXX) -o $(TESTER_TARGET) $(TESTER_SRC) $(CXXFLAGS)

.PHONY: all clean debug info warning error build callgraph run tester cleantester