# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -pthread -lrt

# Directories
SRC_DIR = src
OBJ_DIR = obj

# Source files
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/master.c \
       $(SRC_DIR)/worker.c \
       $(SRC_DIR)/thread_pool.c \
       $(SRC_DIR)/cache.c \
       $(SRC_DIR)/config.c \
       $(SRC_DIR)/http.c \
       $(SRC_DIR)/logger.c \
       $(SRC_DIR)/stats.c

# Object files
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Executable (must be in project root)
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Ensure obj directory exists
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Compile each .c into obj/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TARGET)

run: all
	./server

.PHONY: all clean run
