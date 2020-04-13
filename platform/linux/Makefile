CPP      = g++
CC       = gcc
OBJ      = gather.o
LINKOBJ  = gather.o
LIBS     = -Wl,-rpath='.' -L. -luv -lsqlite3 -llua -lpthread -ldl -s
#LIBS     = libuv.a libsqlite3.a liblua.a -lpthread -ldl -s
INCS     = -I"libuv" -I"libsqlite" -I"liblua"
BIN      = gather
CFLAGS   = $(INCS) -Os
RM       = rm -f

.PHONY: all all-before all-after clean clean-custom server client

all: all-before $(BIN) all-after test

clean: clean-custom
	${RM} $(OBJ) $(BIN) server client server.o client.o

$(BIN): $(OBJ)
	$(CPP) $(LINKOBJ) -o $(BIN) $(LIBS)

gather.o: gather.cpp
	$(CPP) -c gather.cpp -o gather.o $(CFLAGS)


test: server client

server: server.o
	$(CPP) server.o -o server $(LIBS)

server.o: server.c
	$(CPP) -c server.c -o server.o $(CFLAGS)

client: client.o
	$(CPP) client.o -o client $(LIBS)

client.o: client.c
	$(CPP) -c client.c -o client.o $(CFLAGS)






## https://www.cnblogs.com/skyfsm/p/6934246.html
## https://www.jianshu.com/p/f35a915f5e8c
## https://blog.csdn.net/qq_28351609/article/details/84630535
## https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/
## https://blog.csdn.net/u010659887/article/details/88687073

