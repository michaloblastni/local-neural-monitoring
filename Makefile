# Makefile for building local_neural_monitoring.exe with MinGW

CC=mingw32-gcc
CFLAGS=-mconsole -lgdi32
TARGET=local_neural_monitoring.exe
SRC=serial.c recording.c local_neural_monitoring.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) -o $(TARGET) $(SRC) $(CFLAGS)

clean:
	del /Q $(TARGET)
