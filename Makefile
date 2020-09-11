all:
	gcc service.c -o service -g  -libverbs

clean:
	rm service
