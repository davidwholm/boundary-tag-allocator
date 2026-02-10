CC      ?= cc
CFLAGS  = -Wall -Wextra -Wpedantic -g -O2

TARGET  = allocator
SRC     = allocator.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all test clean
