# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -pthread -I./include

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj

# Files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
EXEC = sim_q3

# Default target
all: $(OBJ_DIR) $(EXEC)

# Create object directory if it doesn't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Link object files to create the executable
$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile source files into object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files (useful before pushing to GitHub)
clean:
	rm -rf $(OBJ_DIR) $(EXEC)

# Generate the tar.gz file for submission
tar: clean
	cd .. && tar -czvf group_<number>.tar.gz group_<number>

.PHONY: all clean tar