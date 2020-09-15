#!/bin/bash

#This is a benchmark file for rdma_perf

#help function
max_size=268435456 # 256MB
min_size=1 # 1B
mult_int=4
loop_num=1000
log_file_name=`date +"%s"`
server_port=19875
hca="mlx5_0"
gid_idx=-1

help() {
    echo ""
    echo "Usage: $0 -M MAX_SIZE -m MIN_SIZE -p MULT_INT -l LOOP_NUM -n LOG_FILE_NAME -I SERVER_IP -P SERVER_PORT -d IB_DEV -g GID_IDX [-s]"
    echo "example-server: $0 -M $max_size -m $min_size -p $mult_int -l $loop_num -n $log_file_name -I 127.0.0.1 -P $server_port -d $hca -g $gid_idx -s"
    echo "example-client: $0 -M $max_size -m $min_size -p $mult_int -l $loop_num -n $log_file_name -I 127.0.0.1 -P $server_port -d $hca -g $gid_idx"
    echo "or all with default:"
    echo "example-server: $0 -s"
    echo "example-client: $0 -I 127.0.0.1"
    echo ""
    exit 1
}

is_server=0

while getopts "M:m:p:l:n:I:P:s?hd:g:" opt
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
        d ) hca=$OPTARG ;;
        g ) gid_idx=$OPTARG ;;
        h|? ) help ;;
    esac
done

# make sure it is made
make

dir="./log/$log_file_name"
if [ $is_server == 1 ]; then
    dir=$dir"-server/"
    mkdir -p $dir
else
    dir=$dir"-client/"
    mkdir -p $dir
fi

#from $min_size to $max_size, inteval * $mult_int
for ((size = $min_size; size <= $max_size; size = $size * $mult_int))
do
    if [ $is_server == 1 ]; then
        log_file="$dir/size-$size.txt"
        ./rdma_perf_log -s $size -l $loop_num -p $server_port -d $hca -g $gid_idx > $log_file
    else
        log_file="$dir/size-$size.txt"
        ./rdma_perf_log -s $size -l $loop_num -p $server_port -d $hca -g $gid_idx $server_ip > $log_file
    fi
    server_port=$[$server_port+1]
done

# do statistics
if [ $is_server == 1 ]; then
  python3 statistics.py $log_file_name"-server"
else
  python3 statistics.py $log_file_name"-client"
fi
