.PHONY: clean

CFLAGS  := -Wall -g
LDFLAGS := ${LDFLAGS} -libverbs -lrdmacm -pthread

APPS    := server client

all: ${APPS}


clean:
	rm -f ${APPS}

