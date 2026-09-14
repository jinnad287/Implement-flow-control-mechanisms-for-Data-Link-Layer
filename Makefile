CXX      = g++
CXXFLAGS = -std=c++17 -Wall -O2
COMMON   = common.cpp common.h

TARGETS = stop_wait_sender stop_wait_receiver \
          gbn_sender gbn_receiver \
          sr_sender sr_receiver

all: $(TARGETS)

stop_wait_sender: stop_wait_sender.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ stop_wait_sender.cpp common.cpp

stop_wait_receiver: stop_wait_receiver.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ stop_wait_receiver.cpp common.cpp

gbn_sender: gbn_sender.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ gbn_sender.cpp common.cpp

gbn_receiver: gbn_receiver.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ gbn_receiver.cpp common.cpp

sr_sender: sr_sender.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ sr_sender.cpp common.cpp

sr_receiver: sr_receiver.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) -o $@ sr_receiver.cpp common.cpp

clean:
	rm -f $(TARGETS) *.o received_output.txt

.PHONY: all clean
