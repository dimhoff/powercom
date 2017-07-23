all: powercom_send apc_ups_logger

powercom_send: powercom_send.c
	gcc -g -std=c11 -g -Wall -o $@ $^ -lrt -lpthread

apc_ups_logger: apc_ups_logger.c
	gcc -g -std=c11 -g -Wall -o $@ $^ -lrt

clean:
	rm -f powercom_send apc_ups_logger
