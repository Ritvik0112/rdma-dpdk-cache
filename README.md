# RDMA-Powered Distributed Cache

Built in C using real RDMA verbs. GET operations use one-sided RDMA READ — server CPU never involved. Benchmarked 2x faster than TCP across 1000 operations.

## Benchmark Results

| Operation | RDMA (us) | TCP (us) | Speedup |
|-----------|-----------|----------|---------|
| SET (avg) | 576       | 1016     | 1.8x    |
| GET (avg) | 482       | 1012     | 2.1x    |

## Tech Stack
- Language: C
- RDMA API: libibverbs
- Transport: soft-RoCE (RoCEv2)
- OS: Ubuntu 26.04 LTS ARM64

## Files
- rdma_cache.c — server/client using real RDMA verbs
- benchmark.c — 1000-operation benchmark vs TCP baseline

## How to Run
```
gcc -o rdma_cache rdma_cache.c -libverbs
gcc -o benchmark benchmark.c -libverbs
./rdma_cache          (terminal 1)
./rdma_cache client   (terminal 2)
```

## Key Concepts
- Memory Registration with rkey
- Queue Pairs with INIT/RTR/RTS state machine
- One-sided RDMA READ and WRITE
- Completion Queue polling

## Next Steps
- Add DPDK packet classification layer
- Extend to NVMe-oF storage transport
- Test on physical Marvell NIC
