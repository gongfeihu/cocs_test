CC = gcc
CFLAGS = -g -Wall -std=c99
LIBS = -lssl -lcrypto

# 目标文件
CLIENT_OBJ = client.o fastcdc.o
SERVER1_OBJ = server1.o
SERVER2_OBJ = server2.o
SERVER3_OBJ = server3.o
SERVER4_OBJ = server4.o

# 可执行文件
CLIENT = client
SERVER1 = server1
SERVER2 = server2
SERVER3 = server3
SERVER4 = server4

# 默认目标
all: $(CLIENT) $(SERVER1) $(SERVER2) $(SERVER3) $(SERVER4)

# 客户端
$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CLIENT_OBJ) -o $(CLIENT) $(LIBS)

client.o: client.c
	$(CC) $(CFLAGS) -c client.c

fastcdc.o: fastcdc.c
	$(CC) $(CFLAGS) -c fastcdc.c

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

# 服务端3
$(SERVER3): $(SERVER3_OBJ)
	$(CC) $(SERVER3_OBJ) -o $(SERVER3) $(LIBS)

server3.o: server3.c
	$(CC) $(CFLAGS) -c server3.c

# 服务端4
$(SERVER4): $(SERVER4_OBJ)
	$(CC) $(SERVER4_OBJ) -o $(SERVER4) $(LIBS)

server4.o: server4.c
	$(CC) $(CFLAGS) -c server4.c

# 便捷目标
client: $(CLIENT)
server1: $(SERVER1)
server2: $(SERVER2)
server3: $(SERVER3)
server4: $(SERVER4)

# 清理
clean:
	rm -f $(CLIENT) $(SERVER1) $(SERVER2) $(SERVER3) $(SERVER4) *.o

# 伪目标
.PHONY: all clean client server1 server2 server3 server4
