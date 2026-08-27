CC = gcc
CFLAGS = -Wall -Wextra -g -pthread

OBJ = nimd.o game.o game_thread.o protocol.o

all: nimd test

nimd: $(OBJ)
	$(CC) $(CFLAGS) -o nimd $(OBJ)

test: test.o protocol.o game.o
	$(CC) $(CFLAGS) -o test test.o protocol.o game.o

nimd.o: nimd.c game.h game_thread.h protocol.h
game.o: game.c game.h
game_thread.o: game_thread.c game_thread.h game.h protocol.h
protocol.o: protocol.c protocol.h
test.o: test.c protocol.h game.h

clean:
	rm -f *.o nimd test
