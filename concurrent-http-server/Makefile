# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -pthread -lrt

# Directories
SRC_DIR = src
OBJ_DIR = obj
TEST_DIR = tests

# Test binaries
TEST_CONCURRENT = $(TEST_DIR)/test_concurrent

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

# Executable
TARGET = server
TARGET_TSAN = server_tsan

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Thread Sanitizer version (para detectar race conditions)
$(TARGET_TSAN): CFLAGS += -fsanitize=thread -O1
$(TARGET_TSAN): LDFLAGS += -fsanitize=thread
$(TARGET_TSAN): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET_TSAN) $(LDFLAGS)

# Ensure obj directory exists
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Compile each .c into obj/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C concurrent test
$(TEST_CONCURRENT): $(TEST_DIR)/test_concurrent.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TARGET) $(TARGET_TSAN)
	rm -f $(TEST_CONCURRENT)

run: all
	./server

.PHONY: all clean run test testFull tsan

# Testes normais (essenciais)
testSimple: $(TARGET) $(TEST_CONCURRENT)
	@$(TEST_DIR)/run_with_server.sh ./$(TARGET) normal

# Testes completos (incluindo sync e stress)
testFull: $(TARGET) $(TEST_CONCURRENT)
	@$(TEST_DIR)/run_with_server.sh ./$(TARGET) full

# Build com Thread Sanitizer
tsan: $(TARGET_TSAN)
	@echo "Servidor compilado com Thread Sanitizer: $(TARGET_TSAN)"
	@echo "Execute: ./$(TARGET_TSAN)"
	@echo "Ou use: SERVER_BIN=./$(TARGET_TSAN) make test"
