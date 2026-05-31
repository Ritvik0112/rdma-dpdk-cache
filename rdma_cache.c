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
#define GID_INDEX 1

struct rdma_info {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qpn;
    union ibv_gid gid;
};

long get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr a = {
        .qp_state=IBV_QPS_INIT, .pkey_index=0, .port_num=1,
        .qp_access_flags=IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE
    };
    int r = ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS);
    printf("[QP] INIT %s\n", r?"FAILED":"OK");
}

void qp_to_rtr(struct ibv_qp *qp, uint32_t dqpn, union ibv_gid *dgid) {
    struct ibv_qp_attr a = {
        .qp_state=IBV_QPS_RTR, .path_mtu=IBV_MTU_1024,
        .dest_qp_num=dqpn, .rq_psn=0,
        .max_dest_rd_atomic=1, .min_rnr_timer=12,
        .ah_attr={.port_num=1,.sl=0,.src_path_bits=0,.is_global=1,
            .grh={.dgid=*dgid,.sgid_index=GID_INDEX,.hop_limit=0xFF}}
    };
    int r = ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|
        IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);
    printf("[QP] RTR %s\n", r?"FAILED":"OK");
}

void qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr a = {
        .qp_state=IBV_QPS_RTS,.timeout=14,.retry_cnt=7,
        .rnr_retry=7,.sq_psn=0,.max_rd_atomic=1
    };
    int r = ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|
        IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC);
    printf("[QP] RTS %s\n", r?"FAILED":"OK");
}

int poll_cq(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int tries=0;
    while(ibv_poll_cq(cq,1,&wc)==0)
        if(++tries>10000000){printf("CQ timeout\n");return -1;}
    if(wc.status!=IBV_WC_SUCCESS){
        printf("WC error: %s\n",ibv_wc_status_str(wc.status));return -1;}
    return 0;
}

int main(int argc, char *argv[]) {
    int is_server=(argc<2);
    printf("[%s] Starting\n",is_server?"CACHE":"CLIENT");

    int nd;
    struct ibv_device **dl=ibv_get_device_list(&nd);
    struct ibv_context *ctx=ibv_open_device(dl[0]);
    struct ibv_pd *pd=ibv_alloc_pd(ctx);

    char *buf=calloc(1,CACHE_SIZE);
    if(is_server) strcpy(buf,"default_value");

    struct ibv_mr *mr=ibv_reg_mr(pd,buf,CACHE_SIZE,
        IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE);

    struct ibv_cq *cq=ibv_create_cq(ctx,10,NULL,NULL,0);
    struct ibv_qp_init_attr qi={
        .send_cq=cq,.recv_cq=cq,
        .cap={.max_send_wr=10,.max_recv_wr=10,.max_send_sge=1,.max_recv_sge=1},
        .qp_type=IBV_QPT_RC
    };
    struct ibv_qp *qp=ibv_create_qp(pd,&qi);

    union ibv_gid my_gid;
    ibv_query_gid(ctx,1,GID_INDEX,&my_gid);

    struct rdma_info me={
        .addr=(uint64_t)(uintptr_t)buf,
        .rkey=mr->rkey,.qpn=qp->qp_num,.gid=my_gid
    };
    struct rdma_info peer;

    if(is_server){
        printf("[CACHE] rkey=%u value='%s'\n",mr->rkey,buf);
        int sfd=socket(AF_INET,SOCK_STREAM,0);
        int opt=1; setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        struct sockaddr_in sa={.sin_family=AF_INET,.sin_port=htons(PORT),.sin_addr.s_addr=INADDR_ANY};
        bind(sfd,(struct sockaddr*)&sa,sizeof(sa));
        listen(sfd,1);
        printf("[CACHE] Waiting for client...\n");
        int cfd=accept(sfd,NULL,NULL);
        send(cfd,&me,sizeof(me),0);
        recv(cfd,&peer,sizeof(peer),0);
        qp_to_init(qp); qp_to_rtr(qp,peer.qpn,&peer.gid); qp_to_rts(qp);
        printf("[CACHE] Ready. Waiting for client to finish...\n");
        char done; recv(cfd,&done,1,0);
        printf("[CACHE] Final value: '%s'\n",buf);
        close(cfd); close(sfd);
    } else {
        int sock=socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in sa={.sin_family=AF_INET,.sin_port=htons(PORT)};
        inet_pton(AF_INET,"127.0.0.1",&sa.sin_addr);
        connect(sock,(struct sockaddr*)&sa,sizeof(sa));
        recv(sock,&peer,sizeof(peer),0);
        send(sock,&me,sizeof(me),0);
        printf("[CLIENT] Server rkey=%u\n",peer.rkey);
        qp_to_init(qp); qp_to_rtr(qp,peer.qpn,&peer.gid); qp_to_rts(qp);
        printf("[CLIENT] QP ready\n\n");

        struct ibv_send_wr *bw;

        // SET
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("SET user:1 = John\n");
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        strcpy(buf,"John");
        long t1=get_time_us();
        struct ibv_sge s1={.addr=(uint64_t)(uintptr_t)buf,.length=strlen("John")+1,.lkey=mr->lkey};
        struct ibv_send_wr w1={.wr_id=1,.opcode=IBV_WR_RDMA_WRITE,
            .send_flags=IBV_SEND_SIGNALED,.sg_list=&s1,.num_sge=1,
            .wr.rdma={.remote_addr=peer.addr,.rkey=peer.rkey}};
        ibv_post_send(qp,&w1,&bw);
        if(poll_cq(cq)==0)
            printf("[CLIENT] SET done. Latency: %ld us\n\n",get_time_us()-t1);

        // GET
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("GET user:1\n");
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        memset(buf,0,CACHE_SIZE);
        long t3=get_time_us();
        struct ibv_sge s2={.addr=(uint64_t)(uintptr_t)buf,.length=64,.lkey=mr->lkey};
        struct ibv_send_wr w2={.wr_id=2,.opcode=IBV_WR_RDMA_READ,
            .send_flags=IBV_SEND_SIGNALED,.sg_list=&s2,.num_sge=1,
            .wr.rdma={.remote_addr=peer.addr,.rkey=peer.rkey}};
        ibv_post_send(qp,&w2,&bw);
        if(poll_cq(cq)==0){
            printf("[CLIENT] GET value  : %s\n",buf);
            printf("[CLIENT] GET latency: %ld us\n",get_time_us()-t3);
        }

        char done=1;
        send(sock,&done,1,0);
        sleep(1);
        close(sock);
    }

    ibv_destroy_qp(qp); ibv_destroy_cq(cq);
    ibv_dereg_mr(mr); free(buf);
    ibv_dealloc_pd(pd); ibv_close_device(ctx);
    ibv_free_device_list(dl);
    return 0;
}
