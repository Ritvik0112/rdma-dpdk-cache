#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

int main() {
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "Failed to get device list\n");
        return 1;
    }
    printf("[RDMA] Found %d device(s)\n", num_devices);

    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "Failed to open device\n");
        return 1;
    }
    printf("[RDMA] Opened device: %s\n", ibv_get_device_name(dev_list[0]));

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        return 1;
    }
    printf("[RDMA] Protection Domain created\n");

    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    strcpy(buffer, "Hello from RDMA!");

    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, sizeof(buffer),
                                    IBV_ACCESS_LOCAL_WRITE |
                                    IBV_ACCESS_REMOTE_READ |
                                    IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        fprintf(stderr, "Failed to register MR\n");
        return 1;
    }
    printf("[RDMA] Memory Region registered\n");
    printf("[RDMA] Buffer address : %p\n", buffer);
    printf("[RDMA] rkey           : %u\n", mr->rkey);
    printf("[RDMA] Buffer content : %s\n", buffer);

    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Failed to create CQ\n");
        return 1;
    }
    printf("[RDMA] Completion Queue created\n");

    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);

    printf("[RDMA] All resources cleaned up\n");
    printf("[RDMA] Hello World complete!\n");
    return 0;
}
