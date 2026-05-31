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
#define PORT_TCP 12346
#define GID_INDEX 1
#define NUM_OPS 1000

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
        .qp_state=IBV_QPS_INIT,.pkey_index=0,.port_num=1,
        .qp_access_flags=IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE
    };
    ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS);
}

void qp_to_rtr(struct ibv_qp *qp, uint32_t dqpn, union ibv_gid *dgid) {
    struct ibv_qp_attr a = {
        .qp_state=IBV_QPS_RTR,.path_mtu=IBV_MTU_1024,
        .dest_qp_num=dqpn,.rq_psn=0,
        .max_dest_rd_atomic=1,.min_rnr_timer=12,
        .ah_attr={.port_num=1,.sl=0,.src_path_bits=0,.is_global=1,
            .grh={.dgid=*dgid,.sgid_index=GID_INDEX,.hop_limit=0xFF}}
    };
    ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|
        IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);
}

void qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr a = {
        .qp_state=IBV_QPS_RTS,.timeout=14,.retry_cnt=7,
        .rnr_retry=7,.sq_psn=0,.max_rd_atomic=1
    };
    ibv_modify_qp(qp,&a,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|
        IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC);
}

int poll_cq(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int tries=0;
    while(ibv_poll_cq(cq,1,&wc)==0)
        if(++tries>10000000) return -1;
    return (wc.status==IBV_WC_SUCCESS)?0:-1;
}

int main(int argc, char *argv[]) {
    int is_server=(argc<2);

    if(is_server) {
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  RDMA Cache Benchmark Server\n");
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

        // RDMA setup
        int nd;
        struct ibv_device **dl=ibv_get_device_list(&nd);
        struct ibv_context *ctx=ibv_open_device(dl[0]);
        struct ibv_pd *pd=ibv_alloc_pd(ctx);
        char *buf=calloc(1,CACHE_SIZE);
        strcpy(buf,"default_value");
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

        // RDMA TCP handshake
        int sfd=socket(AF_INET,SOCK_STREAM,0);
        int opt=1; setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        struct sockaddr_in sa={.sin_family=AF_INET,.sin_port=htons(PORT),.sin_addr.s_addr=INADDR_ANY};
        bind(sfd,(struct sockaddr*)&sa,sizeof(sa)); listen(sfd,1);
        printf("[SERVER] Waiting for RDMA client...\n");
        int cfd=accept(sfd,NULL,NULL);
        send(cfd,&me,sizeof(me),0);
        recv(cfd,&peer,sizeof(peer),0);
        qp_to_init(qp); qp_to_rtr(qp,peer.qpn,&peer.gid); qp_to_rts(qp);
        printf("[SERVER] RDMA ready. Client accesses memory directly.\n");
        char done; recv(cfd,&done,1,0);
        close(cfd); close(sfd);

        // TCP server for comparison
        int tfd=socket(AF_INET,SOCK_STREAM,0);
        setsockopt(tfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        struct sockaddr_in ta={.sin_family=AF_INET,.sin_port=htons(PORT_TCP),.sin_addr.s_addr=INADDR_ANY};
        bind(tfd,(struct sockaddr*)&ta,sizeof(ta)); listen(tfd,1);
        printf("[SERVER] Waiting for TCP client...\n");
        int tcfd=accept(tfd,NULL,NULL);
        char tbuf[CACHE_SIZE];
        for(int i=0;i<NUM_OPS*2;i++) {
            recv(tcfd,tbuf,CACHE_SIZE,0);
            send(tcfd,tbuf,strlen(tbuf)+1,0);
        }
        char tdone; recv(tcfd,&tdone,1,0);
        close(tcfd); close(tfd);

        ibv_destroy_qp(qp); ibv_destroy_cq(cq);
        ibv_dereg_mr(mr); free(buf);
        ibv_dealloc_pd(pd); ibv_close_device(ctx);
        ibv_free_device_list(dl);
        printf("[SERVER] Done.\n");

    } else {
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  RDMA Cache Benchmark Client (%d operations)\n", NUM_OPS);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

        // RDMA setup
        int nd;
        struct ibv_device **dl=ibv_get_device_list(&nd);
        struct ibv_context *ctx=ibv_open_device(dl[0]);
        struct ibv_pd *pd=ibv_alloc_pd(ctx);
        char *buf=calloc(1,CACHE_SIZE);
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

        int sock=socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in sa={.sin_family=AF_INET,.sin_port=htons(PORT)};
        inet_pton(AF_INET,"127.0.0.1",&sa.sin_addr);
        connect(sock,(struct sockaddr*)&sa,sizeof(sa));
        recv(sock,&peer,sizeof(peer),0);
        send(sock,&me,sizeof(me),0);
        qp_to_init(qp); qp_to_rtr(qp,peer.qpn,&peer.gid); qp_to_rts(qp);

        // RDMA benchmark
        printf("\n[RDMA] Running %d SET operations...\n", NUM_OPS);
        long rdma_set_total=0;
        for(int i=0;i<NUM_OPS;i++) {
            sprintf(buf,"value_%d",i);
            long t1=get_time_us();
            struct ibv_sge s={.addr=(uint64_t)(uintptr_t)buf,.length=strlen(buf)+1,.lkey=mr->lkey};
            struct ibv_send_wr w={.wr_id=1,.opcode=IBV_WR_RDMA_WRITE,
                .send_flags=IBV_SEND_SIGNALED,.sg_list=&s,.num_sge=1,
                .wr.rdma={.remote_addr=peer.addr,.rkey=peer.rkey}};
            struct ibv_send_wr *bw;
            ibv_post_send(qp,&w,&bw);
            poll_cq(cq);
            rdma_set_total+=get_time_us()-t1;
        }

        printf("[RDMA] Running %d GET operations...\n", NUM_OPS);
        long rdma_get_total=0;
        for(int i=0;i<NUM_OPS;i++) {
            memset(buf,0,CACHE_SIZE);
            long t1=get_time_us();
            struct ibv_sge s={.addr=(uint64_t)(uintptr_t)buf,.length=64,.lkey=mr->lkey};
            struct ibv_send_wr w={.wr_id=2,.opcode=IBV_WR_RDMA_READ,
                .send_flags=IBV_SEND_SIGNALED,.sg_list=&s,.num_sge=1,
                .wr.rdma={.remote_addr=peer.addr,.rkey=peer.rkey}};
            struct ibv_send_wr *bw;
            ibv_post_send(qp,&w,&bw);
            poll_cq(cq);
            rdma_get_total+=get_time_us()-t1;
        }

        char done=1; send(sock,&done,1,0);
        sleep(1); close(sock);

        ibv_destroy_qp(qp); ibv_destroy_cq(cq);
        ibv_dereg_mr(mr); free(buf);
        ibv_dealloc_pd(pd); ibv_close_device(ctx);
        ibv_free_device_list(dl);

        // TCP benchmark
        printf("\n[TCP] Running %d SET operations...\n", NUM_OPS);
        int tsock=socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in tsa={.sin_family=AF_INET,.sin_port=htons(PORT_TCP)};
        inet_pton(AF_INET,"127.0.0.1",&tsa.sin_addr);
        connect(tsock,(struct sockaddr*)&tsa,sizeof(tsa));

        char tbuf[CACHE_SIZE];
        long tcp_set_total=0;
        for(int i=0;i<NUM_OPS;i++) {
            sprintf(tbuf,"value_%d",i);
            long t1=get_time_us();
            send(tsock,tbuf,strlen(tbuf)+1,0);
            recv(tsock,tbuf,CACHE_SIZE,0);
            tcp_set_total+=get_time_us()-t1;
        }

        printf("[TCP] Running %d GET operations...\n", NUM_OPS);
        long tcp_get_total=0;
        for(int i=0;i<NUM_OPS;i++) {
            sprintf(tbuf,"get_%d",i);
            long t1=get_time_us();
            send(tsock,tbuf,strlen(tbuf)+1,0);
            recv(tsock,tbuf,CACHE_SIZE,0);
            tcp_get_total+=get_time_us()-t1;
        }

        char tdone=1; send(tsock,&tdone,1,0);
        close(tsock);

        // Results
        printf("\n");
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  BENCHMARK RESULTS (%d operations each)\n", NUM_OPS);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  %-20s %10s %10s %10s\n","Operation","RDMA(µs)","TCP(µs)","Speedup");
        printf("  %-20s %10s %10s %10s\n","─────────","────────","───────","───────");
        long rdma_set_avg=rdma_set_total/NUM_OPS;
        long rdma_get_avg=rdma_get_total/NUM_OPS;
        long tcp_set_avg=tcp_set_total/NUM_OPS;
        long tcp_get_avg=tcp_get_total/NUM_OPS;
        printf("  %-20s %10ld %10ld %9.1fx\n","SET (avg)",rdma_set_avg,tcp_set_avg,(float)tcp_set_avg/rdma_set_avg);
        printf("  %-20s %10ld %10ld %9.1fx\n","GET (avg)",rdma_get_avg,tcp_get_avg,(float)tcp_get_avg/rdma_get_avg);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    }
    return 0;
}
