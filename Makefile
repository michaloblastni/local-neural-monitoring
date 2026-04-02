# Makefile for building local_neural_monitoring.exe with MinGW

CC=gcc
WINDRES=windres
CFLAGS=-mconsole -lgdi32 -lkernel32 -lshell32 -luser32 -lsetupapi
TARGET=local_neural_monitoring.exe
SRC=serial.c recording.c settings.c hw_detect.c local_neural_monitoring.c
RESOBJ=resources.o

all: $(TARGET)

$(RESOBJ): resources.rc resource.h
	$(WINDRES) resources.rc -o $(RESOBJ) -I.

$(TARGET): $(SRC) $(RESOBJ)
	$(CC) -o $(TARGET) $(SRC) $(RESOBJ) $(CFLAGS)

clean:
	del /Q $(TARGET) $(RESOBJ)
