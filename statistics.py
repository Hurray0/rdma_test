#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Author: Hurray(zhuhongrui@{megvii.com,ncsg.ac.cn})
# Date: 2020.09.15 11:30:25

import os, sys
from matplotlib import pyplot as plt
from multiprocessing import Pool

MAX_PROC = 8
ibv_name_list = ["ibv_get_device_list", "ibv_open_device", "ibv_alloc_pd", "ibv_create_cq", "ibv_reg_mr", "ibv_create_qp", "ibv_modify_qp(init)", "ibv_post_recv", "ibv_modify_qp(rtr)", "ibv_modify_qp(rts)", "ibv_post_send", "ibv_poll_cq", "ibv_destroy_qp", "ibv_dereg_mr", "ibv_destroy_cq", "ibv_dealloc_pd", "ibv_close_device"]
first_file = True

def DEBUG(msg: str):
    print(msg)

def _handle_file(filename: str) -> dict:
    global first_file
    global ibv_name_list
    data = {}
    with open(filename, 'r') as f:
        for line in f.readlines():
            # format analysis
            if line[0:4] != "ibv_": continue
            t = line.split(' ')
            ibv_name = t[0]
            ibv_time = int(t[1]) # usecond

            # insert to data
            data.setdefault(ibv_name, [])
            data[ibv_name].append(ibv_time)
    return data

def _draw_data(data: dict, size: int, dirname: str):
    # mkdir
    if not os.path.isdir(dirname):
        os.mkdir(dirname)

    # for every ibverb(every size), draw histgram
    for ibv_name in data:
        plt.figure(dpi=300)
        time_list = data[ibv_name]
        plt.hist(time_list, bins=50)
        plt.title("ibverb " + ibv_name + " latency distribution")
        plt.xlabel("time: usecond")
        plt.ylabel("frequency")
        fname = "size-" + str(size) + "-" + ibv_name + ".png"
        fname = os.path.join(dirname, fname)
        plt.savefig(fname)
        plt.close()
        print("save: " + fname)

def _do_summary(datas: list, dirname: str, uid: str):
    "datas[size][ibv_name] = [value1 value2 value3 ...]"
    def avg(d: list) -> float:
        return sum(d) * 1.0 / len(d)

    def trans_bytes(byte: int) -> str:
        if byte < 1024: return "%dB" % byte
        if byte < 1024 * 1024: return "%dKB" % (byte/1024)
        if byte < 1024 * 1024 * 1024: return "%dMB" % (byte/1024/1024)
        else: return "%dGB" % (byte/1024/1024/1024)

    # mkdir
    if not os.path.isdir(dirname):
        os.mkdir(dirname)

    logs = {} # logs[ibv_name] = []
    sizes = list(datas.keys()) # k: size v: data
    sizes.sort()
    for size in sizes:
        for ibv_name in datas[size]:
            logs.setdefault(ibv_name, [])
            logs[ibv_name].append( avg(datas[size][ibv_name]) )

    DEBUG(dirname)
    ibv_names = ibv_name_list


    sums = [0]*len(sizes)
    for ibv_name in ibv_names:
        try:
            sums = list(map(sum, zip(*[sums, logs[ibv_name]])))
        except:
            # ibv_post_recv or ibv_post_send
            pass


    plt.figure(dpi=300)
    sizes_str = list(map(trans_bytes, sizes))
    for ibv_name in ibv_names:
        plt.bar(sizes_str, sums, label=ibv_name)
        try:
            sums = list(map(lambda x: x[0]-x[1], zip(*[sums, logs[ibv_name]])))
        except:
            # ibv_post_recv or ibv_post_send
            pass
    plt.legend(fontsize=8)
    plt.ylabel('latency: uSecond')
    plt.xlabel('packet size')
    plt.xticks(rotation=45)
    plt.title("ibverbs latency for different transfer size @ " + uid)
    fname = "all_bar.png"
    fname = os.path.join(dirname, fname)
    plt.savefig(fname)
    plt.close()
    print("save: " + fname)

    #####################################################
    # csv
    fname = "avg_latency.csv"
    fname = os.path.join(dirname, fname)

    with open(fname, 'w') as f:
        # title
        f.write("-,")
        for size in sizes_str:
            f.write("%s," % size)
        f.write("\n")

        # line
        for ibv_name in ibv_names:
            if ibv_name not in logs: continue;
            f.write(ibv_name + ",")
            for item in logs[ibv_name]:
                f.write("%.2f," % item)
            f.write("\n")

def work(filename: str, size: int, dirname: str) -> dict:
    # handle data
    data = _handle_file(filename)
    # draw
    _draw_data(data, size, dirname + '_img')
    return data

def handle_log_folder(logdirname: str="./log/"):
    pool = Pool(processes=MAX_PROC)
    m_datas = {}
    for d in os.listdir(logdirname):
        dname = os.path.join(logdirname, d)
        if os.path.isfile(dname): continue
        if "_img" in d: continue
        datas = {}
        m_datas[dname] = datas
        for f in os.listdir(dname):
            if 'size-' not in f or ".txt" not in f: continue
            DEBUG("handle: " + f)
            size: int = int(f.split('-')[1].split('.')[0])
            fname = os.path.join(dname, f)
            datas[size] = pool.apply_async(work, (fname, size, dname))
    pool.close()
    pool.join()

    for d in os.listdir(logdirname):
        dname = os.path.join(logdirname, d)
        if os.path.isfile(dname): continue
        if "_img" in d: continue
        datas = m_datas[dname]
        for t in datas:
            datas[t] = datas[t].get()
        _do_summary(datas, dname + "_img", d)

def handle_folder(foldername: str):
    pool = Pool(processes=MAX_PROC)
    datas = {}
    dname = os.path.join("./log/", foldername)
    for f in os.listdir(dname):
        if 'size-' not in f or ".txt" not in f: continue
        DEBUG("handle: " + f)
        size: int = int(f.split('-')[1].split('.')[0])
        fname = os.path.join(dname, f)
        datas[size] = pool.apply_async(work, (fname, size, dname))
    pool.close()
    pool.join()

    for t in datas:
        datas[t] = datas[t].get()
    _do_summary(datas, dname + "_img", foldername)

if __name__ == '__main__':
    if len(sys.argv) <= 1:
        handle_log_folder()
    else:
        handle_folder(sys.argv[1])
