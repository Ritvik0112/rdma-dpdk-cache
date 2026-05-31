#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#define CACHE_SIZE 4096
#define PORT 12345

struct rdma_info {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qpn;
    uint16_t lid;
};

long get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_LOCAL_WRITE,
    };
    ibv_modify_qp(qp, &attr,
                  IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                  IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

void modify_qp_to_rtr(struct ibv_qp *qp, uint32_t dest_qpn, uint16_t dlid) {
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_256,
        .dest_qp_num        = dest_qpn,
        .rq_psn             = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr = {
            .dlid          = dlid,
            .sl            = 0,
            .src_path_bits = 0,
            .port_num      = 1,
            .is_global     = 0,
        },
    };
    ibv_modify_qp(qp, &attr,
                  IBV_QP_STATE | IBV_QP_AV |
                  IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                  IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                  IBV_QP_MIN_RNR_TIMER);
}

void modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state      = IBV_QPS_RTS,
        .timeout       = 14,
        .retry_cnt     = 7,
        .rnr_retry     = 7,
        .sq_psn        = 0,
        .max_rd_atomic = 1,
    };
    ibv_modify_qp(qp, &attr,
                  IBV_QP_STATE | IBV_QP_TIMEOUT |
                  IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                  IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

int poll_cq(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int ret;
    int tries = 0;
    while ((ret = ibv_poll_cq(cq, 1, &wc)) == 0) {
        tries++;
        if (tries > 10000000) {
            printf("[CLIENT] CQ poll timeout\n");
            return -1;
        }
    }
    if (wc.status != IBV_WC_SUCCESS) {
        printf("[CLIENT] WC error: %d\n", wc.status);
        return -1;
    }
    return 0;
}

int main() {
    printf("[CLIENT] Starting RDMA Cache Client\n");

    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);

    char *local_buf = calloc(1, CACHE_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, local_buf, CACHE_SIZE,
                                    IBV_ACCESS_LOCAL_WRITE |
                                    IBV_ACCESS_REMOTE_READ |
                                    IBV_ACCESS_REMOTE_WRITE);

    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr  = 10,
            .max_recv_wr  = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init);

    // TCP connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT),
    };
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);
    connect(sock, (struct sockaddr*)&saddr, sizeof(saddr));

    struct rdma_info srv;
    recv(sock, &srv, sizeof(srv), 0);
    printf("[CLIENT] Server rkey=%u addr=%lu\n", srv.rkey, srv.addr);

    struct ibv_port_attr port_attr;
    ibv_query_port(ctx, 1, &port_attr);

    struct rdma_info my_info = {
        .addr = (uint64_t)(uintptr_t)local_buf,
        .rkey = mr->rkey,
        .qpn  = qp->qp_num,
        .lid  = port_attr.lid,
    };
    send(sock, &my_info, sizeof(my_info), 0);

    // Transition QP
    modify_qp_to_init(qp);
    modify_qp_to_rtr(qp, srv.qpn, srv.lid);
    modify_qp_to_rts(qp);
    printf("[CLIENT] QP ready (RTS)\n\n");

    struct ibv_send_wr *bad_wr;

    // ── SET (RDMA WRITE) ─────────────────────────────────
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("SET user:1 = John\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    strcpy(local_buf, "John");
    long t1 = get_time_us();

    struct ibv_sge sge1 = {
        .addr   = (uint64_t)(uintptr_t)local_buf,
        .length = strlen("John") + 1,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr wr1 = {
        .wr_id      = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .sg_list    = &sge1,
        .num_sge    = 1,
        .wr.rdma    = { .remote_addr = srv.addr, .rkey = srv.rkey },
    };
    ibv_post_send(qp, &wr1, &bad_wr);

    if (poll_cq(cq) == 0) {
        long t2 = get_time_us();
        printf("[CLIENT] SET complete. Latency: %ld µs\n", t2 - t1);
        printf("[CLIENT] Server CPU was never involved\n\n");
    }

    // ── GET (RDMA READ) ──────────────────────────────────
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("GET user:1\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    memset(local_buf, 0, CACHE_SIZE);
    long t3 = get_time_us();

    struct ibv_sge sge2 = {
        .addr   = (uint64_t)(uintptr_t)local_buf,
        .length = 64,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr wr2 = {
        .wr_id      = 2,
        .opcode     = IBV_WR_RDMA_READ,
        .send_flags = IBV_SEND_SIGNALED,
        .sg_list    = &sge2,
        .num_sge    = 1,
        .wr.rdma    = { .remote_addr = srv.addr, .rkey = srv.rkey },
    };
    ibv_post_send(qp, &wr2, &bad_wr);

    if (poll_cq(cq) == 0) {
        long t4 = get_time_us();
        printf("[CLIENT] GET value  : %s\n", local_buf);
        printf("[CLIENT] GET latency: %ld µs\n", t4 - t3);
        printf("[CLIENT] Server CPU was never involved\n");
    }

    // Signal done
    char done = 1;
    send(sock, &done, 1, 0);
    sleep(1);

    close(sock);
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(local_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    return 0;
}
