CC      = clang
CCFLAGS = -g

all: allocator

test: allocator
	./allocator

allocator: allocator.c
	$(CC) $(CCFLAGS) allocator.c -o allocator

clean:
	rm allocator
