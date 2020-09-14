/*
* BUILD COMMAND:
* gcc -Wall -I/usr/local/ofed/include -O2 -o RDMA_RC_example
-L/usr/local/ofed/lib64 -L/usr/local/ofed/lib - libverbs RDMA_RC_example.c
*
*/
/******************************************************************************
 *
 * RDMA Aware Networks Programming Example
 *
 * This code demonstrates how to perform the following operations using the *
 *VPI Verbs API:
 *
 * Send
 * Receive
 * RDMA Read
 * RDMA Write
 *
 *****************************************************************************/
#include "rdma_perf.h"

//#define MSG_SIZE (strlen(MSG) + 1)
//#define MSG_SIZE 1024 * 1024 * 1024 // 1GB

size_t MSG_SIZE = 1024 * 1024;
size_t LOOP = 1;

struct config_t config = {NULL,  /* dev_name */
                          NULL,  /* server_name */
                          19875, /* tcp_port */
                          1,     /* ib_port */
                          -1 /* gid_idx */};

static int sock_connect(const char *servername, int port) {
  struct addrinfo *resolved_addr = NULL;
  struct addrinfo *iterator;
  char service[6];
  int sockfd = -1;
  int listenfd = 0;
  int tmp;
  struct addrinfo hints = {
      .ai_flags = AI_PASSIVE, .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  if (sprintf(service, "%d", port) < 0)
    goto sock_connect_exit;
  /* Resolve DNS address, use sockfd as temp storage */
  sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
  if (sockfd < 0) {
    PRINT_ERR("%s for %s:%d\n", gai_strerror(sockfd), servername, port);
    goto sock_connect_exit;
  }
  /* Search through results and find the one we want */
  for (iterator = resolved_addr; iterator; iterator = iterator->ai_next) {
    sockfd = socket(iterator->ai_family, iterator->ai_socktype,
                    iterator->ai_protocol);
    if (sockfd >= 0) {
      if (servername) {
        /* Client mode. Initiate connection to remote */
        if ((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen))) {
          PRINT("failed connect \n");
          close(sockfd);
          sockfd = -1;
        }
      } else {
        /* Server mode. Set up listening socket an accept a connection */
        listenfd = sockfd;
        sockfd = -1;
        if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
          goto sock_connect_exit;
        listen(listenfd, 1);
        sockfd = accept(listenfd, NULL, 0);
      }
    }
  }
sock_connect_exit:
  if (listenfd)
    close(listenfd);
  if (resolved_addr)
    freeaddrinfo(resolved_addr);
  if (sockfd < 0) {
    if (servername)
      PRINT_ERR( "Couldn't connect to %s:%d\n", servername, port);
    else {
      perror("server accept");
      PRINT_ERR( "accept() failed\n");
    }
  }
  return sockfd;
}

int sock_sync_data(int sock, int xfer_size, char *local_data,
                   char *remote_data) {
  int rc;
  int read_bytes = 0;
  int total_read_bytes = 0;
  rc = write(sock, local_data, xfer_size);
  if (rc < xfer_size)
    PRINT_ERR( "Failed writing data during sock_sync_data\n");
  else
    rc = 0;
  while (!rc && total_read_bytes < xfer_size) {
    read_bytes = read(sock, remote_data, xfer_size);
    if (read_bytes > 0)
      total_read_bytes += read_bytes;
    else
      rc = read_bytes;
  }
  return rc;
}

static int poll_completion(struct resources *res) {
  struct ibv_wc wc;
  unsigned long start_time_msec;
  unsigned long cur_time_msec;
  struct timeval cur_time;
  int poll_result;
  int rc = 0;

  /* poll the completion for a while before giving up of doing it .. */
  gettimeofday(&cur_time, NULL);
  start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
  size_t t0 = get_timestamp();
  do {
    poll_result = ibv_poll_cq(res->cq, 1, &wc);

    gettimeofday(&cur_time, NULL);
    cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
  } while ((poll_result == 0) &&
           ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
  size_t t1 = get_timestamp();
  PRINT_TIME("ibv_poll_cq", t1 - t0);


  if (poll_result < 0) {
    /* poll CQ failed */
    PRINT_ERR( "poll CQ failed\n");
    rc = 1;
  } else if (poll_result == 0) { /* the CQ is empty */
    PRINT_ERR( "completion wasn't found in the CQ after timeout\n");
    rc = 1;
  } else {
    /* CQE found */
    PRINT("completion was found in CQ with status 0x%x\n", wc.status);
    /* check the completion status (here we don't care about the completion
     * opcode */
    if (wc.status != IBV_WC_SUCCESS) {
      PRINT_ERR(
              "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n",
              wc.status, wc.vendor_err);
      rc = 1;
    }
  }
  return rc;
}

static int post_send(struct resources *res, int opcode) {
  struct ibv_send_wr sr;
  struct ibv_sge sge;
  struct ibv_send_wr *bad_wr = NULL;
  int rc;
  /* prepare the scatter/gather entry */
  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)res->buf;
  sge.length = MSG_SIZE;
  sge.lkey = res->mr->lkey;
  /* prepare the send work request */
  memset(&sr, 0, sizeof(sr));
  sr.next = NULL;
  sr.wr_id = 0;
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = opcode;
  sr.send_flags = IBV_SEND_SIGNALED;
  if (opcode != IBV_WR_SEND) {
    sr.wr.rdma.remote_addr = res->remote_props.addr;
    sr.wr.rdma.rkey = res->remote_props.rkey;
  }
  /* there is a Receive Request in the responder side, so we won't get any into
   * RNR flow */
  LOG_TIME_CHECK(rc = ibv_post_send(res->qp, &sr, &bad_wr),
          "ibv_post_send", rc == 0);

  if (!rc) {
    switch (opcode) {
    case IBV_WR_SEND:
      PRINT("Send Request was posted\n");
      break;
    case IBV_WR_RDMA_READ:
      PRINT("RDMA Read Request was posted\n");
      break;
    case IBV_WR_RDMA_WRITE:
      PRINT("RDMA Write Request was posted\n");
      break;
    default:
      PRINT("Unknown Request was posted\n");
      break;
    }
  }
  return rc;
}
static int post_receive(struct resources *res) {
  struct ibv_recv_wr rr;
  struct ibv_sge sge;
  struct ibv_recv_wr *bad_wr;
  int rc;
  /* prepare the scatter/gather entry */
  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)res->buf;
  sge.length = MSG_SIZE;
  sge.lkey = res->mr->lkey;
  /* prepare the receive work request */
  memset(&rr, 0, sizeof(rr));
  rr.next = NULL;
  rr.wr_id = 0;
  rr.sg_list = &sge;
  rr.num_sge = 1;
  /* post the Receive Request to the RQ */
  LOG_TIME_CHECK(rc = ibv_post_recv(res->qp, &rr, &bad_wr),
          "ibv_post_recv", rc == 0);

  return rc;
}
static void resources_init(struct resources *res) {
  memset(res, 0, sizeof *res);
  res->sock = -1;
}
static int sock_create(struct resources *res) {
  int rc = 0;
  /* if client side */
  if (config.server_name) {
    res->sock = sock_connect(config.server_name, config.tcp_port);
    if (res->sock < 0) {
      PRINT_ERR(
              "failed to establish TCP connection to server %s, port %d\n",
              config.server_name, config.tcp_port);
      rc = -1;
      goto sock_create_exit;
    }
  } else {
    PRINT("waiting on port %d for TCP connection\n", config.tcp_port);
    res->sock = sock_connect(NULL, config.tcp_port);
    if (res->sock < 0) {
      PRINT_ERR(
              "failed to establish TCP connection with client on port %d\n",
              config.tcp_port);
      rc = -1;
      goto sock_create_exit;
    }
  }
  PRINT("TCP connection was established\n");
  return rc;
sock_create_exit:
  if (res->sock >= 0) {
    if (close(res->sock))
      PRINT_ERR( "failed to close socket\n");
    res->sock = -1;
  }
  return rc;
}
static int resources_create(struct resources *res) {
  struct ibv_device **dev_list = NULL;
  struct ibv_qp_init_attr qp_init_attr;
  struct ibv_device *ib_dev = NULL;
  size_t size;
  int i;
  int mr_flags = 0;
  int cq_size = 0;
  int num_devices;
  int rc = 0;

  PRINT("searching for IB devices in host\n");
  /* get device names in the system */
  LOG_TIME_CHECK(dev_list = ibv_get_device_list(&num_devices),
          "ibv_get_device_list", dev_list);

  if (!dev_list) {
    rc = 1;
    goto resources_create_exit;
  }
  /* if there isn't any IB device in host */
  if (!num_devices) {
    PRINT_ERR( "found %d device(s)\n", num_devices);
    rc = 1;
    goto resources_create_exit;
  }
  PRINT("found %d device(s)\n", num_devices);
  /* search for the specific device we want to work with */
  for (i = 0; i < num_devices; i++) {
    if (!config.dev_name) {
      config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
      PRINT("device not specified, using first one found: %s\n",
              config.dev_name);
    }
    if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name)) {
      ib_dev = dev_list[i];
      break;
    }
  }
  /* if the device wasn't found in host */
  if (!ib_dev) {
    PRINT_ERR( "IB device %s wasn't found\n", config.dev_name);
    rc = 1;
    goto resources_create_exit;
  }
  /* get device handle */
  LOG_TIME(res->ib_ctx = ibv_open_device(ib_dev), "ibv_open_device");

  if (!res->ib_ctx) {
    PRINT_ERR( "failed to open device %s\n", config.dev_name);
    rc = 1;
    goto resources_create_exit;
  }
  /* We are now done with device list, free it */
  ibv_free_device_list(dev_list);
  dev_list = NULL;
  ib_dev = NULL;
  /* query port properties */
  if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
    PRINT_ERR( "ibv_query_port on port %u failed\n", config.ib_port);
    rc = 1;
    goto resources_create_exit;
  }
  /* allocate Protection Domain */
  LOG_TIME(res->pd = ibv_alloc_pd(res->ib_ctx), "ibv_alloc_pd");

  if (!res->pd) {
    PRINT_ERR( "ibv_alloc_pd failed\n");
    rc = 1;
    goto resources_create_exit;
  }
  /* each side will send only one WR, so Completion Queue with 1 entry is enough
   */
  cq_size = 1;
  LOG_TIME(res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0),
          "ibv_create_cq");
  if (!res->cq) {
    PRINT_ERR( "failed to create CQ with %u entries\n", cq_size);
    rc = 1;
    goto resources_create_exit;
  }
  /* allocate the memory buffer that will hold the data */
  size = MSG_SIZE;
  res->buf = (char *)malloc(size);
  PRINT("MSG_SIZE: %zu\n", MSG_SIZE);
  if (!res->buf) {
    PRINT_ERR( "failed to malloc %Zu bytes to memory buffer\n", size);
    rc = 1;
    goto resources_create_exit;
  }
  memset(res->buf, 0, size);
  /* only in the server side put the message in the memory buffer */
  // if (!config.server_name) {
  //  strcpy(res->buf, MSG);
  //  PRINT("going to send the message: '%s'\n", res->buf);
  //}

  /* register the memory buffer */
  mr_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  LOG_TIME(res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags),
          "ibv_reg_mr");

  if (!res->mr) {
    PRINT_ERR( "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
    rc = 1;
    goto resources_create_exit;
  }
  PRINT(
          "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
          res->buf, res->mr->lkey, res->mr->rkey, mr_flags);
  /* create the Queue Pair */
  memset(&qp_init_attr, 0, sizeof(qp_init_attr));
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.sq_sig_all = 1;
  qp_init_attr.send_cq = res->cq;
  qp_init_attr.recv_cq = res->cq;
  qp_init_attr.cap.max_send_wr = 1;
  qp_init_attr.cap.max_recv_wr = 1;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;

  LOG_TIME(res->qp = ibv_create_qp(res->pd, &qp_init_attr), "ibv_create_qp");

  if (!res->qp) {
    PRINT_ERR( "failed to create QP\n");
    rc = 1;
    goto resources_create_exit;
  }
  PRINT("QP was created, QP number=0x%x\n", res->qp->qp_num);
resources_create_exit:
  if (rc) {
    /* Error encountered, cleanup */
    if (res->qp) {
      ibv_destroy_qp(res->qp);
      res->qp = NULL;
    }
    if (res->mr) {
      ibv_dereg_mr(res->mr);
      res->mr = NULL;
    }
    if (res->buf) {
      free(res->buf);
      res->buf = NULL;
    }
    if (res->cq) {
      ibv_destroy_cq(res->cq);
      res->cq = NULL;
    }
    if (res->pd) {
      ibv_dealloc_pd(res->pd);
      res->pd = NULL;
    }
    if (res->ib_ctx) {
      ibv_close_device(res->ib_ctx);
      res->ib_ctx = NULL;
    }
    if (dev_list) {
      ibv_free_device_list(dev_list);
      dev_list = NULL;
    }
  }
  return rc;
}
static int modify_qp_to_init(struct ibv_qp *qp) {
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = config.ib_port;
  attr.pkey_index = 0;
  attr.qp_access_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  LOG_TIME_CHECK(rc = ibv_modify_qp(qp, &attr, flags),
          "ibv_modify_qp(init)", rc == 0);
  return rc;
}
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn,
                            uint16_t dlid, uint8_t *dgid) {
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_256;
  attr.dest_qp_num = remote_qpn;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 0x12;
  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = dlid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = config.ib_port;
  if (config.gid_idx >= 0) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = 1;
    memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = config.gid_idx;
    attr.ah_attr.grh.traffic_class = 0;
  }
  flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
          IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  LOG_TIME_CHECK(rc = ibv_modify_qp(qp, &attr, flags),
          "ibv_modify_qp(rtr)", rc == 0);
  return rc;
}
static int modify_qp_to_rts(struct ibv_qp *qp) {
  struct ibv_qp_attr attr;
  int flags;
  int rc;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 0x12;
  attr.retry_cnt = 6;
  attr.rnr_retry = 0;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
          IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  LOG_TIME_CHECK(rc = ibv_modify_qp(qp, &attr, flags),
          "ibv_modify_qp(rts)", rc == 0);
  return rc;
}

static int connect_qp(struct resources *res) {
  struct cm_con_data_t local_con_data;
  struct cm_con_data_t remote_con_data;
  struct cm_con_data_t tmp_con_data;
  int rc = 0;
  char temp_char;
  union ibv_gid my_gid;
  if (config.gid_idx >= 0) {
    LOG_TIME(rc = ibv_query_gid(res->ib_ctx, config.ib_port,
                config.gid_idx, &my_gid),
            "ibv_query_gid");
    RDMA_CHECK(0 == rc, "could not get gid for port %d, index %d",
            config.ib_port, config.gid_idx);
    return rc;
  } else {
    memset(&my_gid, 0, sizeof my_gid);
  }

  /* exchange using TCP sockets info required to connect QPs */
  local_con_data.addr = htonll((uintptr_t)res->buf);
  local_con_data.rkey = htonl(res->mr->rkey);
  local_con_data.qp_num = htonl(res->qp->qp_num);
  local_con_data.lid = htons(res->port_attr.lid);
  memcpy(local_con_data.gid, &my_gid, 16);
  PRINT("\nLocal LID = 0x%x\n", res->port_attr.lid);
  RDMA_CHECK_GOTO(0 == sock_sync_data(res->sock, sizeof(struct cm_con_data_t),
                     (char *)&local_con_data, (char *)&tmp_con_data),
    "failed to exchange connection data between sides", connect_qp_exit);

  remote_con_data.addr = ntohll(tmp_con_data.addr);
  remote_con_data.rkey = ntohl(tmp_con_data.rkey);
  remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
  remote_con_data.lid = ntohs(tmp_con_data.lid);
  memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
  /* save the remote side attributes, we will need it for the post SR */
  res->remote_props = remote_con_data;
  PRINT("Remote address = 0x%" PRIx64 "\n", remote_con_data.addr);
  PRINT("Remote rkey = 0x%x\n", remote_con_data.rkey);
  PRINT("Remote QP number = 0x%x\n", remote_con_data.qp_num);
  PRINT("Remote LID = 0x%x\n", remote_con_data.lid);
  if (config.gid_idx >= 0) {
    uint8_t *p = remote_con_data.gid;
    PRINT(
            "Remote GID "
            "=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:"
            "%02x:%02x:%02x\n ",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
            p[11], p[12], p[13], p[14], p[15]);
  }
  /* modify the QP to init */
  RDMA_CHECK_GOTO(0 == modify_qp_to_init(res->qp),
          "change QP state to INIT failed", connect_qp_exit);

  /* let the client post RR to be prepared for incoming messages */
  if (config.server_name) {
      RDMA_CHECK_GOTO(0 == post_receive(res), "failed to post RR", connect_qp_exit);
  }

  /* modify the QP to RTR */
  RDMA_CHECK_GOTO(0 == modify_qp_to_rtr(res->qp, remote_con_data.qp_num,
              remote_con_data.lid, remote_con_data.gid),
          "failed to modify QP state to RTR", connect_qp_exit);

  RDMA_CHECK_GOTO(0 == modify_qp_to_rts(res->qp),
          "failed to modify QP state to RTS", connect_qp_exit);

  /* sync to make sure that both sides are in states that they can connect to
   * prevent packet loose; just send a dummy char back and forth */
  RDMA_CHECK_GOTO(0 == sock_sync_data(res->sock, 1, "Q", &temp_char),
          "sync error after QPs are were moved to RTS", connect_qp_exit);

connect_qp_exit:
  return rc;
}
static int resources_destroy(struct resources *res) {
  int rc = 0;
  if (res->qp) {
      int ret;
    LOG_TIME_CHECK(ret = ibv_destroy_qp(res->qp), "ibv_destroy_qp", ret == 0);
  }
  if (res->mr) {
      int ret;
    LOG_TIME_CHECK(ret = ibv_dereg_mr(res->mr), "ibv_dereg_mr", ret == 0);
  }
  if (res->buf)
    free(res->buf);
  if (res->cq) {
      int ret;
    LOG_TIME_CHECK(ret = ibv_destroy_cq(res->cq), "ibv_destroy_cq", ret == 0);
  }
  if (res->pd) {
      int ret;
    LOG_TIME_CHECK(ret = ibv_dealloc_pd(res->pd), "ibv_dealloc_pd", ret == 0);
  }
  if (res->ib_ctx) {
      int ret;
      LOG_TIME_CHECK(ret = ibv_close_device(res->ib_ctx),
            "ibv_close_device", ret == 0);
  }
  return rc;
}
static int sock_destroy(struct resources *res) {
  int rc = 0;
  if (res->sock >= 0) {
    RDMA_CHECK(0 == close(res->sock), "failed to close socket");
  }
  return rc;
}

// Print out config information
static void print_config(void) {
  PRINT(" ------------------------------------------------\n");
  PRINT(" Device name : \"%s\"\n", config.dev_name);
  PRINT(" IB port : %u\n", config.ib_port);
  if (config.server_name)
    PRINT(" IP : %s\n", config.server_name);
  PRINT(" TCP port : %u\n", config.tcp_port);
  if (config.gid_idx >= 0)
    PRINT(" GID index : %u\n", config.gid_idx);
  PRINT(" ------------------------------------------------\n\n");
}

// print a description of command line syntax
static void usage(const char *argv0) {
  PRINT("Usage:\n");
  PRINT(" %s start a server and wait for connection\n", argv0);
  PRINT(" %s <host> connect to server at <host>\n", argv0);
  PRINT("\n");
  PRINT("Options:\n");
  PRINT(
      stdout,
      " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
  PRINT(
      stdout,
      " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
  PRINT(
          " -i, --ib-port <port> use port <port> of IB device (default 1)\n");
  PRINT(" -g, --gid_idx <git index> gid index to be used in GRH "
                  "(default not used)\n");
  PRINT(" -s, --size <size> use size <size> for transport data size\n");
  PRINT(" -l, --loop <loop number> use <loop number> for test loop number\n");
}

/******************************************************************************
 * Function: main
 *
 * Input
 * argc number of items in argv
 * argv command line parameters
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, 1 on failure
 *
 * Description
 * Main program code
 ******************************************************************************/
int main(int argc, char *argv[]) {
  struct resources res;
  int rc = 1;
  char temp_char;
  /* parse the command line parameters */
  while (1) {
    int c;
    static struct option long_options[] = {
        {.name = "port", .has_arg = 1, .val = 'p'},
        {.name = "ib-dev", .has_arg = 1, .val = 'd'},
        {.name = "ib-port", .has_arg = 1, .val = 'i'},
        {.name = "gid-idx", .has_arg = 1, .val = 'g'},
        {.name = "size", .has_arg = 1, .val = 's'},
        {.name = "loop", .has_arg = 1, .val = 'l'},
        {.name = NULL, .has_arg = 0, .val = '\0'}};
    c = getopt_long(argc, argv, "p:d:i:g:s:l:", long_options, NULL);
    if (c == -1)
      break;
    switch (c) {
    case 'p':
      config.tcp_port = strtoul(optarg, NULL, 0);
      break;
    case 'd':
      config.dev_name = strdup(optarg);
      break;
    case 'i':
      config.ib_port = strtoul(optarg, NULL, 0);
      if (config.ib_port < 0) {
        usage(argv[0]);
        return 1;
      }
      break;
    case 'g':
      config.gid_idx = strtoul(optarg, NULL, 0);
      if (config.gid_idx < 0) {
        usage(argv[0]);
        return 1;
      }
      break;
    case 's':
      MSG_SIZE = strtouq(optarg, NULL, 0);
      break;
    case 'l':
      LOOP = strtouq(optarg, NULL, 0);
      break;

    default:
      usage(argv[0]);
      return 1;
    }
  }
  /* parse the last parameter (if exists) as the server name */
  if (optind == argc - 1)
    config.server_name = argv[optind];
  if (config.server_name) {
    PRINT("servername=%s\n", config.server_name);
  } else if (optind < argc) {
    usage(argv[0]);
    return 1;
  }
  /* print the used parameters for info*/
  print_config();
  /* init all of the resources, so cleanup will be easy */
  resources_init(&res);
  /* create resources before using them */

  RDMA_CHECK_GOTO(0 == sock_create(&res), "failed to create sock", main_exit);

  for (int i = 0; i < LOOP; ++i) {
    RDMA_CHECK_GOTO(0 == resources_create(&res), "failed to create resources", main_exit);
    /* connect the QPs */
    RDMA_CHECK_GOTO(0 == connect_qp(&res), "failed to connect QPs", main_exit);
    /* let the server post the sr */
    if (!config.server_name) {
        RDMA_CHECK_GOTO(0 == post_send(&res, IBV_WR_SEND), "failed to post sr", main_exit);
    }
    /* in both sides we expect to get a completion */
    RDMA_CHECK_GOTO(0 == poll_completion(&res), "poll completion failed", main_exit);

    /* Sync so we are sure server side has data ready before client tries to
     * read it; just send a dummy char back and forth */
    RDMA_CHECK_GOTO(0 == sock_sync_data(res.sock, 1, "R", &temp_char),
            "sync error before RDMA ops", main_exit);

    RDMA_CHECK(0 == resources_destroy(&res), "failed to destroy resources");
    rc = 0;
  } // end for

main_exit:
  if (rc != 0) {
      RDMA_CHECK(0 == resources_destroy(&res), "failed to destroy resources");
  }

  RDMA_CHECK(0 == sock_destroy(&res), "failed to destroy socket resources");

  if (config.dev_name)
    free((char *)config.dev_name);
  if (rc == 0) {
      PRINT_OK("\ntest result is OK\n");
  } else {
      PRINT_ERR("\ntest result is ERROR\n");
  }
  return rc;
}
