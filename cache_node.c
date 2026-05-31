#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define CACHE_SIZE 4096
#define PORT 12345

struct rdma_info {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qpn;
    uint16_t lid;
};

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

int main() {
    printf("[CACHE] Starting RDMA Cache Node\n");

    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    printf("[CACHE] Device: %s\n", ibv_get_device_name(dev_list[0]));

    struct ibv_pd *pd = ibv_alloc_pd(ctx);

    char *cache = calloc(1, CACHE_SIZE);
    strcpy(cache, "default_value");

    struct ibv_mr *mr = ibv_reg_mr(pd, cache, CACHE_SIZE,
                                    IBV_ACCESS_LOCAL_WRITE |
                                    IBV_ACCESS_REMOTE_READ |
                                    IBV_ACCESS_REMOTE_WRITE);
    printf("[CACHE] Memory registered. rkey=%u\n", mr->rkey);
    printf("[CACHE] Initial value: %s\n", cache);

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
    printf("[CACHE] QP created. QPN=%u\n", qp->qp_num);

    struct ibv_port_attr port_attr;
    ibv_query_port(ctx, 1, &port_attr);

    // TCP exchange
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in saddr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(server_fd, (struct sockaddr*)&saddr, sizeof(saddr));
    listen(server_fd, 1);
    printf("[CACHE] Waiting for client on port %d\n", PORT);

    int client_fd = accept(server_fd, NULL, NULL);

    struct rdma_info my_info = {
        .addr = (uint64_t)(uintptr_t)cache,
        .rkey = mr->rkey,
        .qpn  = qp->qp_num,
        .lid  = port_attr.lid,
    };
    send(client_fd, &my_info, sizeof(my_info), 0);

    struct rdma_info cli_info;
    recv(client_fd, &cli_info, sizeof(cli_info), 0);
    printf("[CACHE] Client QPN=%u LID=%u\n", cli_info.qpn, cli_info.lid);

    // Transition QP
    modify_qp_to_init(qp);
    modify_qp_to_rtr(qp, cli_info.qpn, cli_info.lid);
    modify_qp_to_rts(qp);
    printf("[CACHE] QP ready (RTS). Sleeping — client accesses memory directly\n");

    // Wait for client to finish
    char done;
    recv(client_fd, &done, 1, 0);

    printf("[CACHE] Final cache value: %s\n", cache);

    close(client_fd);
    close(server_fd);
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(cache);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    return 0;
}
