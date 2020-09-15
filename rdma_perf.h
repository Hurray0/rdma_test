#include <byteswap.h>
#include <endian.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 2000
#define MSG "Hello HURRAY!"
#define RDMAMSGR "RDMA read operation "
#define RDMAMSGW "RDMA write operation"

//#define LOG_TO_FILE

#define PRINT_ERR(msg...)                             \
    do {                                              \
        fprintf(stderr, "\033[0;31m");                \
            fprintf(stderr, msg);                     \
            fprintf(stderr, "\033[0;0m");             \
            fprintf(stderr, "\n");                    \
    } while(0)
#define PRINT_OK(msg...)                              \
    do {                                              \
        fprintf(stderr, "\033[0;32m");                \
            fprintf(stderr, msg);                     \
            fprintf(stderr, "\033[0;0m");             \
            fprintf(stderr, "\n");                    \
    } while(0)

#ifndef LOG_TO_FILE
#define PRINT_TIME(name, time)                        \
    do {                                              \
        fprintf(stdout, "\033[0;34m[time]");          \
            fprintf(stdout, "%s: %zu", name, time);   \
            fprintf(stdout, "\033[0;0m");             \
            fprintf(stdout, "\n");                    \
    } while(0)
#define PRINT_SUCC(msg...)                            \
    do {                                              \
        fprintf(stdout, "\033[0;0m");                 \
            fprintf(stdout, msg);                     \
            fprintf(stdout, "\033[0;0m");             \
            fprintf(stdout, "\n");                    \
    } while(0)
#define PRINT(msg...) fprintf(stdout, msg);
#else
#define PRINT_TIME(name, time)                        \
    do {                                              \
            fprintf(stdout, "%s %zu", name, time);   \
            fprintf(stdout, "\n");                    \
    } while(0)
#define PRINT_SUCC(msg...)
#define PRINT(msg...) 
#endif


#define RDMA_CHECK(expr, msg...)                      \
    do {                                              \
        if (!(expr)) {                                \
            PRINT_ERR(msg);                           \
        }                                             \
    } while(0)

#define RDMA_CHECK_GOTO(expr, msg, place)             \
    do {                                              \
        if (!(expr)) {                                \
            PRINT_ERR(msg);                           \
            goto place;                               \
        }                                             \
    } while(0)

size_t get_timestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000000 + tv.tv_usec;
}

#define LOG_TIME(expr, name)                          \
    do {                                              \
        size_t t0 = get_timestamp();                  \
        (expr);                                       \
        size_t t1 = get_timestamp();                  \
        PRINT_TIME(name, t1 - t0);         \
    } while(0)

#define LOG_TIME_CHECK(expr, name, checkop)           \
    do {                                              \
        LOG_TIME(expr, name);                         \
        if (!checkop) {                               \
            PRINT_ERR("faild to run %s", name);       \
        } else {                                      \
            PRINT_SUCC("success to run %s", name);    \
        }                                             \
    } while(0)



//#define RDMA_CHECK(expr, msg, place) do { int res = (expr); printf(msg); goto place;} while(0)

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif
/* structure of test parameters */
struct config_t {
  const char *dev_name; /* IB device name */
  char *server_name;    /* server host name */
  u_int32_t tcp_port;   /* server TCP port */
  int ib_port;          /* local IB port to work with */
  int gid_idx;          /* gid index to use */
};
/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t {
  uint64_t addr;   /* Buffer address */
  uint32_t rkey;   /* Remote key */
  uint32_t qp_num; /* QP number */
  uint16_t lid;    /* LID of the IB port */
  uint8_t gid[16]; /* gid */
} __attribute__((packed));

/* structure of system resources */
struct resources {
  struct ibv_device_attr device_attr;
  /* Device attributes */
  struct ibv_port_attr port_attr;    /* IB port attributes */
  struct cm_con_data_t remote_props; /* values to connect to remote side */
  struct ibv_context *ib_ctx;        /* device handle */
  struct ibv_pd *pd;                 /* PD handle */
  struct ibv_cq *cq;                 /* CQ handle */
  struct ibv_qp *qp;                 /* QP handle */
  struct ibv_mr *mr;                 /* MR handle for buf */
  char *buf; /* memory buffer pointer, used for RDMA and send
ops */
  int sock;  /* TCP socket file descriptor */
};



/******************************************************************************
Socket operations
For simplicity, the example program uses TCP sockets to exchange control
information. If a TCP/IP stack/connection is not available, connection manager
(CM) may be used to pass this information. Use of CM is beyond the scope of
this example
******************************************************************************/
/******************************************************************************
 * Function: sock_connect
 *
 * Input
 * servername URL of server to connect to (NULL for server mode)
 * port port of service
 *
 * Output
 * none
 *
 * Returns
 * socket (fd) on success, negative error code on failure
 *
 * Description
 * Connect a socket. If servername is specified a client connection will be
 * initiated to the indicated server and port. Otherwise listen on the
 * indicated port for an incoming connection.
 *
 ******************************************************************************/
static int sock_connect(const char *servername, int port);

/******************************************************************************
 * Function: sock_sync_data
 *
 * Input
 * sock socket to transfer data on
 * xfer_size size of data to transfer
 * local_data pointer to data to be sent to remote
 *
 * Output
 * remote_data pointer to buffer to receive remote data
 *
 * Returns
 * 0 on success, negative error code on failure
 *
 * Description
 * Sync data across a socket. The indicated local data will be sent to the
 * remote. It will then wait for the remote to send its data back. It is
 * assumed that the two sides are in sync and call this function in the proper
 * order. Chaos will ensue if they are not. :)
 *
 * Also note this is a blocking function and will wait for the full data to be
 * received from the remote.
 *
 ******************************************************************************/
int sock_sync_data(int sock, int xfer_size, char *local_data,
                   char *remote_data);

/******************************************************************************
End of socket operations
******************************************************************************/
/* poll_completion */
/******************************************************************************
 * Function: poll_completion
 *
 * Input
 * res pointer to resources structure
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, 1 on failure
 *
 * Description
 * Poll the completion queue for a single event. This function will continue to
 * poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
 *
 ******************************************************************************/
static int poll_completion(struct resources *res);

/******************************************************************************
 * Function: post_send
 *
 * Input
 * res pointer to resources structure
 * opcode IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, error code on failure
 *
 * Description
 * This function will create and post a send work request
 ******************************************************************************/
static int post_send(struct resources *res, int opcode);

/******************************************************************************
 * Function: post_receive
 *
 * Input
 * res pointer to resources structure
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, error code on failure
 *
 * Description
 *
 ******************************************************************************/
static int post_receive(struct resources *res);

/******************************************************************************
 * Function: resources_init
 *
 * Input
 * res pointer to resources structure
 *
 * Output
 * res is initialized
 *
 * Returns
 * none
 *
 * Description
 * res is initialized to default values
 ******************************************************************************/
static void resources_init(struct resources *res);

/******************************************************************************
 * Function: resources_create
 *
 * Input
 * res pointer to resources structure to be filled in
 *
 * Output
 * res filled in with resources
 *
 * Returns
 * 0 on success, 1 on failure
 *
 * Description
 *
 * This function creates and allocates all necessary system resources. These
 * are stored in res.
 *****************************************************************************/
static int sock_create(struct resources *res);
static int resources_create(struct resources *res);

/******************************************************************************
 * Function: modify_qp_to_init
 *
 * Input
 * qp QP to transition
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, ibv_modify_qp failure code on failure
 *
 * Description
 * Transition a QP from the RESET to INIT state
 ******************************************************************************/
static int modify_qp_to_init(struct ibv_qp *qp);

/******************************************************************************
 * Function: modify_qp_to_rtr
 *
 * Input
 * qp QP to transition
 * remote_qpn remote QP number
 * dlid destination LID
 * dgid destination GID (mandatory for RoCEE)
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, ibv_modify_qp failure code on failure
 *
 * Description
 * Transition a QP from the INIT to RTR state, using the specified QP number
 ******************************************************************************/
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn,
                            uint16_t dlid, uint8_t *dgid);

/******************************************************************************
 * Function: modify_qp_to_rts
 *
 * Input
 * qp QP to transition
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, ibv_modify_qp failure code on failure
 *
 * Description
 * Transition a QP from the RTR to RTS state
 ******************************************************************************/
static int modify_qp_to_rts(struct ibv_qp *qp);

/******************************************************************************
 * Function: connect_qp
 *
 * Input
 * res pointer to resources structure
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, error code on failure
 *
 * Description
 * Connect the QP. Transition the server side to RTR, sender side to RTS
 ******************************************************************************/
static int connect_qp(struct resources *res);
 
/******************************************************************************
 * Function: resources_destroy
 *
 * Input
 * res pointer to resources structure
 *
 * Output
 * none
 *
 * Returns
 * 0 on success, 1 on failure
 *
 * Description
 * Cleanup and deallocate all resources used
 ******************************************************************************/
static int resources_destroy(struct resources *res);
