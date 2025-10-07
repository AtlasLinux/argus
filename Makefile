CC = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Iinclude
LDFLAGS = -ldrm -lwayland-server -linput -ludev -lm
SRCS = src/main.c src/drm_simple.c src/wayland.c src/input.c
OBJS = $(SRCS:.c=.o)
TARGET = argus

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o build/$@ $(OBJS) $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)
