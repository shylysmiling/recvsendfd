#define _POSIX_C_SOURCE 2

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))

struct ModeOptions {
	char const *socketPath;
	char const *file;
	char **childArgv;
	int descriptor;
	int isPersistent;
};


sig_atomic_t static volatile isRunning;

static void SignalHandler(int sig) {
	if(sig == SIGINT || sig == SIGTERM) {
		isRunning = 0;
	} else {
		raise(sig);
	}
}

static void InitSignalHandlers() {
	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);
}


static void LogError(char const *msg) {
	write(STDERR_FILENO, msg, strlen(msg));
	write(STDERR_FILENO, "\n", 1);
}

static void LogErrno() {
	LogError(strerror(errno));
}

static _Noreturn void Die(char const *msg) {
	LogError(msg);
	exit(EXIT_FAILURE);
}


static int MatchBaseName(char const *path, char const *name) {
	char const *lastSeparator = strrchr(path, '/');
	char const *basePart = (lastSeparator != NULL ? (lastSeparator + 1) : path);
	return strcmp(basePart, name) == 0;
}


static int Connect(char const *path) {
	int sock = socket(AF_UNIX, SOCK_DGRAM, 0);

	if(sock != -1) {
		struct sockaddr_un address;

		address.sun_family = AF_UNIX;
		strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);

		if(connect(sock, (struct sockaddr *)&address, sizeof(address)) == -1) {
			LogErrno();
			close(sock);
			sock = -1;
		}
	}

	return sock;
}

static int CreateSock(char const *path) {
	unlink(path);

	int sock = socket(AF_UNIX, SOCK_DGRAM, 0);

	if(sock != -1) {
		struct sockaddr_un address;

		address.sun_family = AF_UNIX;
		strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);

		if(bind(sock, (struct sockaddr *)&address, sizeof(address)) == -1) {
			LogErrno();
			close(sock);
			sock = -1;
		}
	} else {
		LogErrno();
	}

	return sock;
}

int ReceiveDescriptor(int sock) {
	int descriptor = -1;

	char unusedBuffer[sizeof(int)];

	memset(&unusedBuffer, 0, sizeof(unusedBuffer));

	struct iovec buffers[1] = {
		{.iov_base = unusedBuffer, .iov_len = sizeof(unusedBuffer)},
	};

	struct msghdr messageHeader;

	memset(&messageHeader, 0, sizeof(messageHeader));

	messageHeader.msg_iov = buffers;
	messageHeader.msg_iovlen = ARRAY_LENGTH(buffers);

	char controlMessagesBuffer[CMSG_SPACE(sizeof(descriptor))];

	messageHeader.msg_control = controlMessagesBuffer;
	messageHeader.msg_controllen = sizeof(controlMessagesBuffer);

	if(recvmsg(sock, &messageHeader, 0) < 0) {
		LogErrno();
		return -1;
	}

	struct cmsghdr *controlMessage = CMSG_FIRSTHDR(&messageHeader);
	int *controlMessageData = (int *)CMSG_DATA(controlMessage);

	descriptor = (controlMessageData != NULL) ? *controlMessageData : -1;

	return descriptor;
}

int SendDescriptor(int sock, int descriptor) {
	struct msghdr messageHeader = {0};

	memset(&messageHeader, 0, sizeof(messageHeader));

	char controlMessageBuffer[CMSG_SPACE(sizeof(descriptor))];

	memset(&controlMessageBuffer, 0, sizeof(controlMessageBuffer));

	struct iovec buffers[1] = {
		{.iov_base = "\0", .iov_len = 1},
	};

	messageHeader.msg_iov = buffers;
	messageHeader.msg_iovlen = ARRAY_LENGTH(buffers);
	messageHeader.msg_control = controlMessageBuffer;
	messageHeader.msg_controllen = sizeof(controlMessageBuffer);

	struct cmsghdr *controlMessage = CMSG_FIRSTHDR(&messageHeader);

	controlMessage->cmsg_level = SOL_SOCKET;
	controlMessage->cmsg_type = SCM_RIGHTS;
	controlMessage->cmsg_len = CMSG_LEN(sizeof(descriptor));

	int *controlMessageData = (int *)CMSG_DATA(controlMessage);
	*controlMessageData = descriptor;

	if(sendmsg(sock, &messageHeader, 0) < 0) {
		LogErrno();
		return -1;
	}

	return 0;
}


static void CopyStream(int outputStream, int inputStream, size_t bufferSize) {
	char buffer[bufferSize];
	int bytesReceived = -1;

	while((bytesReceived = read(inputStream, buffer, bufferSize)) > 0) {
		write(outputStream, buffer, bytesReceived);
	}

	if(bytesReceived < 0) {
		LogErrno();
	}
}


int ReceiverMode(struct ModeOptions const *options) {
	if(options->socketPath == NULL) {
		return 0;
	}

	int sock = CreateSock(options->socketPath);

	if(sock == -1) {
		return 0;
	}

	isRunning = 1;

	InitSignalHandlers();

	while(isRunning) {
		int receivedDescriptor = ReceiveDescriptor(sock);

		if(receivedDescriptor != -1) {
			if(options->childArgv != NULL) {
				int pid = fork();

				switch(pid) {
					case -1:
						LogErrno();
						break;
					case 0:
						if(dup2(receivedDescriptor, options->descriptor) != -1) {
							if(receivedDescriptor != options->descriptor) {
								close(receivedDescriptor);
							}

							execvp(options->childArgv[0], options->childArgv);
						}

						LogErrno();
						Die("Child startup failed.");
						break;
				}
			} else {
				CopyStream(STDOUT_FILENO, receivedDescriptor, 4096);
				close(receivedDescriptor);
			}
		}

		if(!options->isPersistent) {
			isRunning = 0;
		}
	}

	return 1;
}

int SenderMode(struct ModeOptions const *options) {
	int descriptor = options->descriptor;

	if(options->file != NULL) {
		descriptor = open(options->file, O_RDONLY);

		if(descriptor == -1) {
			LogErrno();
			return 0;
		}
	}

	int sock = Connect(options->socketPath);

	if(sock == -1) {
		return 0;
	}

	if(SendDescriptor(sock, descriptor) == -1) {
		return 0;
	}

	return 1;
}

int main(int argc, char *argv[]) {
	int (*mode)(struct ModeOptions const *options) = NULL;
	struct ModeOptions options;
	int isExecPresent = 0;
	int opt = -1;

	memset(&options, 0, sizeof(options));

	while((opt = getopt(argc, argv, "d:es:f:pwr")) != -1) {
		switch (opt) {
		case 'w':
			mode = SenderMode;
			break;
		case 'r':
			mode = ReceiverMode;
			break;
		case 'f':
			options.file = optarg;
			break;
		case 's':
			options.socketPath = optarg;
			break;
		case 'e':
			isExecPresent = 1;
			break;
		case 'p':
			options.isPersistent = 1;
			break;
		case 'd':
			if(sscanf(optarg, "%d", &options.descriptor) != 1) {
				Die("Can't scan descriptor.");
			}
			break;
		default:
			Die("Unknown option.");
		}
	}

	if(isExecPresent) {
		if(optind >= argc) {
			Die("Missing command.");
		}

		options.childArgv = &argv[optind];
	} else if(options.socketPath == NULL) {
		options.socketPath = argv[optind];
	}

	if(MatchBaseName(argv[0], "sendfd")) {
		mode = SenderMode;
	} else if(MatchBaseName(argv[0], "recvfd")) {
		mode = ReceiverMode;
	}

	if(mode == NULL) {
		Die("Unknown mode.");
	}

	return mode(&options) ? EXIT_SUCCESS : EXIT_FAILURE;
}
