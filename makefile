CC = gcc
CFLAGS = -g -Wall -std=c99
LIBS = -lssl -lcrypto

# 目标文件
CLIENT_OBJ = client.o
SERVER1_OBJ = server1.o
SERVER2_OBJ = server2.o

# 可执行文件
CLIENT = client
SERVER1 = server1
SERVER2 = server2

# 默认目标
all: $(CLIENT) $(SERVER1) $(SERVER2)

# 客户端
$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CLIENT_OBJ) -o $(CLIENT) $(LIBS)

client.o: client.c
	$(CC) $(CFLAGS) -c client.c

# 服务端1
$(SERVER1): $(SERVER1_OBJ)
	$(CC) $(SERVER1_OBJ) -o $(SERVER1) $(LIBS)

server1.o: server1.c
	$(CC) $(CFLAGS) -c server1.c

# 服务端2
$(SERVER2): $(SERVER2_OBJ)
	$(CC) $(SERVER2_OBJ) -o $(SERVER2) $(LIBS)

server2.o: server2.c
	$(CC) $(CFLAGS) -c server2.c

# 便捷目标
client: $(CLIENT)
server1: $(SERVER1)
server2: $(SERVER2)

# 清理
clean:
	rm -f $(CLIENT) $(SERVER1) $(SERVER2) *.o

# 伪目标
.PHONY: all clean client server1 server2