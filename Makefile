CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
TARGET := astorb2bin

all: $(TARGET) astorb.bin

$(TARGET): astorb2bin.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

astorb.bin: $(TARGET) astorb.dat
	./$(TARGET) astorb.dat astorb.bin

clean:
	rm -f $(TARGET)

.PHONY: all clean
