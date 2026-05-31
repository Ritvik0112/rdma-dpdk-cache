```
# RDMA-Powered Distributed Cache

A high-performance distributed in-memory cache built in C using real RDMA verbs (libibverbs) on Linux. Designed to demonstrate CPU offload architecture inspired by Marvell's Octeon DPU — where network and memory operations are handled without involving the main CPU.

## What This Project Does

Traditional caches like Redis use TCP sockets — every request goes through the Linux kernel, copies data multiple times, and interrupts the CPU. This project replaces that with RDMA:

- SET operations use one-sided RDMA WRITE — data is pushed directly into server memory. Server CPU never wakes up.
- GET operations use one-sided RDMA READ — data is pulled directly from server memory. Server CPU never wakes up.
- Connection setup uses TCP (one time only), then all data movement is kernel-bypass RDMA.

## Benchmark Results

1000 operations each, measured on soft-RoCE (RoCEv2) using libibverbs:

| Operation | RDMA avg (µs) | TCP avg (µs) | Speedup |
|-----------|---------------|--------------|---------|
| SET       | 576           | 1016         | 1.8x    |
| GET       | 482           | 1012         | 2.1x    |

Cache node CPU utilization during RDMA data transfers: ~0%

## Architecture

```
Client Node                        Cache Node
───────────                        ──────────
SET "user:1" = "John"
      │
      │──── RDMA WRITE ──────────► server RAM updated
      │                            server CPU = sleeping
GET "user:1"
      │
      │──── RDMA READ ───────────► reads server RAM
      ◄─────────────────────────── "John" returned
                                   server CPU = sleeping
```

## Tech Stack

- Language: C
- RDMA API: libibverbs (rdma-core)
- Transport: soft-RoCE (RoCEv2) via rdma_rxe kernel module
- OS: Ubuntu 26.04 LTS ARM64
- Platform: QEMU VM on Apple M3 (same ARM architecture as Marvell Octeon DPU cores)

## Files

- rdma_cache.c — unified server/client with full QP state machine (INIT → RTR → RTS)
- cache_node.c — cache server implementation
- client.c — cache client with GET/SET via real RDMA verbs
- benchmark.c — 1000-operation benchmark comparing RDMA vs TCP latency

## How to Run

Requirements:
```
sudo apt install libibverbs-dev rdma-core build-essential
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev enp0s1
```

Build:
```
gcc -o rdma_cache rdma_cache.c -libverbs
gcc -o benchmark benchmark.c -libverbs
```

Run cache demo:
```
./rdma_cache          # terminal 1 — starts cache server
./rdma_cache client   # terminal 2 — runs GET and SET
```

Run benchmark:
```
./benchmark           # terminal 1
./benchmark client    # terminal 2
```

## Key RDMA Concepts Implemented

- Memory Registration — pinning RAM with rkey for remote access
- Protection Domain — security boundary for RDMA operations
- Queue Pairs — RC (Reliable Connected) with full INIT/RTR/RTS state machine
- Completion Queue — polling for operation completion
- One-sided RDMA READ — client pulls data, zero server CPU involvement
- One-sided RDMA WRITE — client pushes data, zero server CPU involvement
- GID-based addressing — RoCEv2 global routing for soft-RoCE

## Connection to Marvell Octeon DPU

Marvell's Octeon DPU has dedicated ARM cores that run RDMA and packet processing workloads — completely freeing the main server CPU. This project demonstrates that architecture in software:

- Same libibverbs API used on real Marvell ConnectX and Octeon NICs
- RoCEv2 transport — identical protocol to hardware RoCE
- CPU offload story — cache server CPU stays near 0% during all data transfers
- Developed on ARM (Apple M3) — same ISA as Marvell Octeon ARM cores

## Next Steps

- Add DPDK packet classification layer as network frontend
- Extend RDMA transport to NVMe-oF storage protocol
- Test on physical RDMA-capable NIC (ConnectX or similar)
- Add consistent hashing across multiple cache nodes
- Add replication for fault tolerance
```
