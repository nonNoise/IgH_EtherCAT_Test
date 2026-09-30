CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -I/usr/local/include
LDFLAGS := -L/usr/local/lib -Wl,-rpath,/usr/local/lib
LDLIBS  := -lethercat -lm

TARGET  := ec_jitter
SRC     := ec_jitter.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	sudo ./$(TARGET)

clean:
	rm -f $(TARGET)