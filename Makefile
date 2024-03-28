CC=gcc
CFLAGS=-O3

motion-transfer: motion-transfer.c
	$(CC) $(CFLAGS) $< -lavformat -lavcodec -lswscale -lavutil -o $@
