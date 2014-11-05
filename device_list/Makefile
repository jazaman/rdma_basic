# This make file si to be used as the base for the 
# other projects

CFLAGS=-c -Wall -Werror -pedantic -std=c++0x
NO_WARNING=-Wno-unused-variable -Wno-unused-but-set-variable
LIBS=-libverbs -lrdmacm -lpthread

APPS := server_rdma client_rdma

ifndef $(CC)
	CC=g++
else ifeq ($(TOOL),clang)
	CC=clang++
endif

ifndef LIBS
	LIBS=
endif

ifeq ($(DEBUG),1)
	DEBUG_FLAG=-g
else
	DEBUG_FLAG=
endif
	
ifdef LIB_DIR
        override LIBDIR_FLAG=-L
else
	LIBDIR_FLAG=
endif

.cpp.o:
	$(CC) $(CFLAGS) $(DEBUG_FLAG) $(NO_WARNING) $^
	
client_rdma: client_rdma.o
	$(CC) $^ $(LIB_DIR) $(LIBS) -o $@
server_rdma: server_rdma.o
	$(CC) $^ $(LIB_DIR) $(LIBS) -o $@ 

#objects = metaprogramming.o 
#all_objs:$(objects)

#$(objects): %.o: %.cpp
#	$(CC) $(CFLAGS) $< -o $@ 

all:${APPS}

clean:
	rm -rf *.o ${APPS}
