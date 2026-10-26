#ifndef DPDK_CONTROLLER_H
#define DPDK_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_build_config.h>

#define MAX_CORES_PER_PORT (16)

typedef struct {
    uint16_t port_id;

    //current statistics
    uint64_t packets_rx;
    uint64_t packets_tx;
    uint64_t packets_rx_errors;
    uint64_t packets_tx_errors;
    uint64_t bytes_rx;
    uint64_t bytes_tx;

    //previous statistics
    uint64_t prev_packets_rx;
    uint64_t prev_packets_tx;
    uint64_t prev_packets_rx_errors;
    uint64_t prev_packets_tx_errors;
    uint64_t prev_bytes_rx;
    uint64_t prev_bytes_tx;
} port_stat_t;

/* Parameters used by each core */
struct core_params {
    int port;  /* Port identifier */
    int queue; /* Queue mapped for the core running */
    bool used; /* Whether the core is used or not */
};

/* Per-port core mapping */
struct port_cores {
    int port;
    int cores[MAX_CORES_PER_PORT];
    int core_count;
};

extern bool force_quit;

/* Function declarations */
void dpdk_init(int argc, char **argv,uint16_t **free_cores, int *free_cores_count);
void dpdk_system_init(int argc, char **argv);
int dpdk_queues_and_ports_init();
int dpdk_ports_init();
void dpdk_ports_fini();
int port_init(uint8_t port, uint16_t rx_rings, uint16_t tx_rings);
void cores_map_queue();
struct rte_mempool *allocate_mempool(uint32_t total_nb_mbufs, int socket_id);
void dpdk_launch_process();
int launch_one_lcore(); 
void fetch_all_stats();

#endif /* DPDK_CONTROLLER_H */
