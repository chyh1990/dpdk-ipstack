OBJS:=directsock.o
TARGET:=libdirectsock.so

CFLAGS:=-O2 -fPIC

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ -shared $^ -ldl

clean:
	rm -f $(TARGET) *.o
