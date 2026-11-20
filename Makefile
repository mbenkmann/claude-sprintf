CXX := g++
OPTIMIZE := -O2
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter 

CFLAGS := $(OPTIMIZE) $(WARNFLAGS) -fno-common -ggdb3
CXXFLAGS := $(CFLAGS) -std=gnu++2a -fno-rtti -fno-threadsafe-statics
EMBEDFLAGS := -fno-exceptions -fno-unwind-tables -fsingle-precision-constant -fno-stack-protector

all: sprintf.o

test: sprintf_test
	./sprintf_test fmt.txt

exhaustive: sprintf_test
	./sprintf_test --exhaustive fmt.txt


%.o: %.cpp
	$(CXX) -o $@ -c $(CXXFLAGS) $(EMBEDFLAGS) $^

sprintf_test: sprintf_test.cpp
	$(CXX) -o $@ $(CXXFLAGS) $(EMBEDFLAGS) $^

clean:
	rm -f sprintf_test sprintf.o
