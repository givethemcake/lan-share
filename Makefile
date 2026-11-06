CXX      = g++
CXXFLAGS = -std=c++17 -Wall -O2

lan-share: lan-share.cpp queue.h
	$(CXX) $(CXXFLAGS) -o lan-share lan-share.cpp -pthread

clean:
	rm -f lan-share
