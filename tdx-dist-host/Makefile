CC := cc
CFLAGS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -std=c11 -O2 -Wall -Wextra -Iinclude
LDLIBS := -libverbs -lrdmacm
BIN_DIR := bin
SRC_DIR := src

COMMON_SRCS := \
	$(SRC_DIR)/common.c \
	$(SRC_DIR)/config.c \
	$(SRC_DIR)/layout.c \
	$(SRC_DIR)/transport_rdma.c

all: $(BIN_DIR)/host

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/host: $(COMMON_SRCS) $(SRC_DIR)/host_main.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(COMMON_SRCS) $(SRC_DIR)/host_main.c $(LDLIBS)

clean:
	rm -f $(BIN_DIR)/host

.PHONY: all clean
