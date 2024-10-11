What is it?
============================================================================

The pair of programs (in single executable) demonstrating `AF_UNIX` socket
feature of sharing file descriptors.

Examples
============================================================================

- Transmit file content.

	Start receiver on `/tmp/socket`:

	```
	recvfd /tmp/socket
	```

	Send file:

	```
	sendfd -f input.txt /tmp/socket
	```

- Send specific descriptor.

	Start receiver on `/tmp/socket`:

	```
	recvfd /tmp/socket
	```

	Open descriptor using `exec` ([manpage](https://man7.org/linux/man-pages/man1/exec.1p.html#EXAMPLES)):

	```
	exec 3< input.txt
	```

	Send it:

	```
	sendfd -d 3 /tmp/socket
	```

- Resend descriptor to the other program.

	Start receiver on `/tmp/socket` with `-e` switch and provide command to start child:

	```
	recvfd -e -s /tmp/socket -- python3 -c 'print("first line:", input())'
	```

	Send something:

	```
	echo "hello python" | sendfd /tmp/socket
	```
