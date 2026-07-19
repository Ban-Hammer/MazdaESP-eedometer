CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../src
SRC = ../src/ota_protocol.cpp
TESTS = test_crc32 test_frame test_state_machine

all: run

run: $(TESTS)
	@for t in $(TESTS); do echo "== running $$t =="; ./$$t || exit 1; done

test_%: test_%.cpp $(SRC) ../src/ota_protocol.h check.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)

clean:
	rm -f $(TESTS)
.PHONY: all run clean
