CPP      = g++.exe
CC       = gcc.exe
OBJ      = gather.o
LINKOBJ  = gather.o
LIBS     = -lws2_32 libuv.dll sqlite3.dll lua5.1.dll -s
INCS     = -I"libuv" -I"libsqlite" -I"liblua"
BIN      = gather.exe
CFLAGS   = $(INCS) -Os
RM       = rm.exe -f

.PHONY: all all-before all-after clean clean-custom server client

all: all-before $(BIN) all-after server client

clean: clean-custom
	${RM} $(OBJ) $(BIN) server.exe client.exe server.o client.o

$(BIN): $(OBJ)
	$(CPP) $(LINKOBJ) -o $(BIN) $(LIBS)

gather.o: gather.cpp
	$(CPP) -c gather.cpp -o gather.o $(CFLAGS)


server: server.exe

server.exe: server.o
	$(CPP) server.o -o server.exe $(LIBS)

server.o: server.c
	$(CPP) -c server.c -o server.o $(CFLAGS)

client: client.exe

client.exe: client.o
	$(CPP) client.o -o client.exe $(LIBS)

client.o: client.c
	$(CPP) -c client.c -o client.o $(CFLAGS)






## https://www.cnblogs.com/skyfsm/p/6934246.html
## https://www.jianshu.com/p/f35a915f5e8c
## https://blog.csdn.net/qq_28351609/article/details/84630535
## https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/
## https://blog.csdn.net/u010659887/article/details/88687073

