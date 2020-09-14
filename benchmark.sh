#!/bin/bash

#This is a benchmark file for rdma_perf

#help function
max_size=33554432 # 32MB
min_size=1024 # 1KB
mult_int=4
loop_num=1000
log_file_name=`date +"%s"`
server_port=19875

help() {
    echo ""
    echo "Usage: $0 -M MAX_SIZE -m MIN_SIZE -p MULT_INT -l LOOP_NUM -n LOG_FILE_NAME -I SERVER_IP -P SERVER_PORT [-s]"
    echo "example-server: $0 -M $max_size -m $min_size -p $mult_int -l $loop_num -n $log_file_name -I 127.0.0.1 -P $server -s"
    echo "example-client: $0 -M $max_size -m $min_size -p $mult_int -l $loop_num -n $log_file_name -I 127.0.0.1 -P $server"
    echo "or all with default:"
    echo "example-server: $0 -I 127.0.0.1 -s"
    echo "example-client: $0 -I 127.0.0.1"
    echo ""
    exit 1
}

is_server=0

while getopts "M:m:p:l:n:I:P:s?h" opt
do
    case "$opt" in
        M ) max_size=$OPTARG ;;
        m ) min_size=$OPTARG ;;
        p ) mult_int=$OPTARG ;;
        l ) loop_num=$OPTARG ;;
        n ) log_file_name=$OPTARG ;;
        I ) server_ip="$OPTARG" ;;
        P ) server_port=$OPTARG ;;
        s ) is_server=1 ;;
        h|? ) help ;;
    esac
done

dir="./log/$log_file_name"
if [ $is_server == 1 ]; then
    dir=$dir"-server/"
    mkdir -p $dir
else
    dir=$dir"-client/"
    mkdir -p $dir
fi

#from 1B to 1GB 2 ^ 30, inteval * 4
for ((size = $min_size; size < $max_size; size = $size * $mult_int))
do
    if [ $is_server == 1 ]; then
        log_file="$dir/size-$size.txt"
        ./rdma_perf_log -s $size -l $loop_num -p $server_port > $log_file
    else
        log_file="$dir/size-$size.txt"
        ./rdma_perf_log -s $size -l $loop_num -p $server_port $server_ip > $log_file
    fi
    server_port=$[$server_port+1]
done
