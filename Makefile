# Compiler and flags
CC = gcc
CFLAGS = -Wall
LDFLAGS = -lncurses

# Paths
SRC = src/nntm.c
OBJ = build/nntm.o
BIN = build/nntm

# Targets
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $(BIN)

$(OBJ): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)

clean:
	rm -rf build

install: $(BIN)
	install -Dm755 $(BIN) /usr/bin/nntm

.PHONY: all clean install

