.PHONY : all
all : recvsendfd recvfd sendfd

recvsendfd : recvsendfd.c
	${CC} -o recvsendfd -O ${CFLAGS} recvsendfd.c

recvfd : recvsendfd
	ln -s recvfd recvsendfd

sendfd : recvsendfd
	ln -s sendfd recvsendfd
