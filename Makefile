CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -MMD -MP \
          $(shell pkg-config --cflags gtk4 json-glib-1.0)
LDFLAGS = $(shell pkg-config --libs gtk4 json-glib-1.0) -lm

SRCS    = main.c dialogs.c quectel.c nmea_status.c
OBJS    = $(SRCS:.c=.o)
DEPS    = $(OBJS:.o=.d)
TARGET  = QueConfig

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

.PHONY: all clean
