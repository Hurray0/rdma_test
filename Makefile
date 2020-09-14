all:
	gcc rdma_perf.c -o rdma_perf -g  -libverbs
	gcc -D LOG_TO_FILE rdma_perf.c -o rdma_perf_log -g  -libverbs

clean:
	rm rdma_perf_log rdma_perf
