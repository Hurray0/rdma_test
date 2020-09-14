all:
	gcc rdma_perf.c -o rdma_perf -g  -libverbs

clean:
	rm service
