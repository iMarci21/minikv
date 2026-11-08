CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE -g
SANITIZEFLAGS ?= -fsanitize=address,undefined

SRC = $(wildcard src/*.c)
# Replaces all *.c to *.o automatically
OBJ = $(SRC:.c=.o)
TARGET = minikv

.PHONY: all clean sanitize format

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Append the sanitize-flags and rebuild all *.o
sanitize: CFLAGS += $(SANITIZEFLAGS)
sanitize: clean $(TARGET)

clean:
	rm -f src/*.o $(TARGET)
