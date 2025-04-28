#include "dpdk_controller.h"

#include <net/ethernet.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_version.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"
#include "packets_handler.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define RX_RING_SIZE (1024 * 4) /* RX ring size */
#define TX_RING_SIZE (1024 * 2) /* TX ring size */
#define RING_SIZE (1024 * 8)
#define MBUF_CACHE_SIZE (250) /* mempool cache size */
#define BURST_TX_DRAIN_US (100)

static uint16_t free_cores[RTE_MAX_LCORE];
struct rte_ring *rx_tx_rings[RTE_MAX_ETHPORTS][RTE_MAX_LCORE];
static int free_cores_count = 0;

static struct rte_mempool *mempools[RTE_MAX_ETHPORTS];
static int mempools_count = 0;

int port_max_cores[RTE_MAX_NUMA_NODES];

typedef struct {
    int cores;
    int ports;
} numa_node_t;

static numa_node_t numa_nodes[RTE_MAX_NUMA_NODES];
static port_stat_t port_stat[RTE_MAX_ETHPORTS];
struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS][RTE_MAX_LCORE];

CoreStat *current_cores_stat[RTE_MAX_LCORE];
CoreStat prev_cores_stat[RTE_MAX_LCORE];

static bool force_quit = false;

typedef struct {
    int port;
    int queue;
    int used;
    bool rx;
} core_params_t;

static core_params_t core_params_arr[RTE_MAX_LCORE];

typedef struct {
    int port;
    int cores[MAX_CORES_PER_PORT];
    int core_count;
    int rx_queue_count;
    int tx_queue_count;
} port_cores_t;

static port_cores_t port_cores[RTE_MAX_ETHPORTS];

void dpdk_init(int argc, char **argv, uint16_t **cores, int *cores_count) {
    LOG(INFO, "Initializing DPDK system");
    memset(port_stat, 0, sizeof(port_stat));
    memset(rx_tx_rings, 0, sizeof(rx_tx_rings));
    memset(current_cores_stat, 0, sizeof(current_cores_stat));
    memset(prev_cores_stat, 0, sizeof(prev_cores_stat));

    dpdk_system_init(argc, argv);
    dpdk_queues_and_ports_init();

    *cores_count = free_cores_count;
    *cores = free_cores;
}

void dpdk_system_init(int argc, char **argv) {
    LOG(INFO, "Initializing DPDK EAL");

    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        LOG(ERR, "Not available ports");
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    }
}

int dpdk_queues_and_ports_init() {
    LOG(INFO, "Mapping cores to queues and initializing ports");

    cores_map_queue();

    int result = dpdk_ports_init();
    if (result != 0) {
        LOG(ERR, "Failed to initialize DPDK ports");
        rte_exit(EXIT_FAILURE, "Failed to initialize DPDK ports\n");
    }

    LOG(INFO, "Finished initializing queues and ports");
    return 0;
}

int port_init(uint8_t port, uint16_t rx_rings, uint16_t tx_rings) {
    LOG(INFO, "Initializing port %d with %d RX rings and %d TX rings", port,
        rx_rings, tx_rings);

    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf port_conf = {
        .rxmode =
            {
                .mq_mode = RTE_ETH_MQ_RX_RSS,
            },
        .rx_adv_conf =
            {
                .rss_conf =
                    {
                        .rss_hf = RTE_ETH_RSS_IP,
                    },
            },
    };

    int ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret < 0) {
        LOG(ERR, "Failed to get device info for port %d, error: %s", port,
            strerror(-ret));
        return -1;
    }

    port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;

    ret = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (ret < 0) {
        LOG(ERR, "Failed to configure port %d, error: %d", port, ret);
        return -1;
    }

    ret = rte_eth_promiscuous_enable(port);
    if (ret < 0) {
        LOG(ERR, "Failed to enable promiscuous mode for port %d, error: %d",
            port, ret);
        return -1;
    }

    // if rx_rings == tx_rings
    uint32_t mempool_size =
        rx_rings * (RING_SIZE + RX_RING_SIZE + TX_RING_SIZE + MBUF_CACHE_SIZE +
                    BURST_SIZE);
    struct rte_mempool *mbuf_pool =
        allocate_mempool(mempool_size, rte_eth_dev_socket_id(port));
    if (mbuf_pool == NULL) {
        LOG(ERR, "Failed to allocate mempool for port %d", port);
        return -1;
    }

    for (int q = 0; q < rx_rings; q++) {
        ret = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
                                     rte_eth_dev_socket_id(port), NULL,
                                     mbuf_pool);
        if (ret < 0) {
            LOG(ERR, "Failed to set up RX queue %d for port %d, error: %d", q,
                port, ret);
            return -1;
        }
    }

    for (int q = 0; q < tx_rings; q++) {
        ret = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
                                     rte_eth_dev_socket_id(port), NULL);
        if (ret < 0) {
            LOG(ERR, "Failed to set up TX queue %d for port %d, error: %d", q,
                port, ret);
            return -1;
        }
    }

    for (int q = 0; q < rx_rings; q++) {
        struct rte_ring *rx_to_tx_ring;
        char name[64];
        snprintf(name, sizeof(name), "rx_to_tx_%d_%d", port, q);
        rx_to_tx_ring =
            rte_ring_create(name, RING_SIZE, rte_eth_dev_socket_id(port),
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (!rx_to_tx_ring) {
            LOG(ERR, "Cannot create ring");
            rte_exit(EXIT_FAILURE, "Cannot create ring\n");
        }

        rx_tx_rings[port][q] = rx_to_tx_ring;
    }

    uint32_t packets_count = BURST_SIZE;
    size_t alloc_size = RTE_ETH_TX_BUFFER_SIZE(packets_count);

    for (int q = 0; q < tx_rings; q++) {
        tx_buffer[port][q] = (struct rte_eth_dev_tx_buffer *)rte_zmalloc_socket(
            "tx_buffer", alloc_size, 0, rte_eth_dev_socket_id(port));

        if (tx_buffer[port][q] == NULL) {
            LOG(ERR, "Cannot allocate buffer for TX on port %u", port);
            rte_exit(EXIT_FAILURE, "Cannot allocate buffer for TX on port %u\n",
                     port);
        }

        rte_eth_tx_buffer_init(tx_buffer[port][q], packets_count);

        ret = rte_eth_tx_buffer_set_err_callback(
            tx_buffer[port][q], rte_eth_tx_buffer_count_callback, NULL);
        if (ret < 0) {
            LOG(ERR, "Cannot set error callback for TX buffer on port %u",
                port);
            rte_exit(EXIT_FAILURE,
                     "Cannot set error callback for TX buffer on port %u\n",
                     port);
        }
    }

    ret = rte_eth_stats_reset(port);
    if (ret < 0) {
        LOG(ERR, "Failed to reset stats for port %d, error: %d", port, ret);
        return -1;
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        LOG(ERR, "Failed to start port %d, error: %d", port, ret);
        return -1;
    }

    LOG(INFO, "Port %d initialized successfully", port);
    return 0;
}

int dpdk_ports_init() {
    LOG(INFO, "Initializing DPDK ports");

    for (uint8_t port = 0; port < RTE_MAX_ETHPORTS; port++) {
        if (!rte_eth_dev_is_valid_port(port)) {
            continue;
        }

        uint16_t rx_rings = port_cores[port].core_count/2;
        uint16_t tx_rings = rx_rings;

        int result = port_init(port, rx_rings, tx_rings);
        if (result != 0) {
            LOG(ERR, "Failed to initialize port %d", port);
            dpdk_ports_fini();
            return result;
        }
    }

    LOG(INFO, "Finished initializing DPDK ports");
    return 0;
}

struct rte_mempool *allocate_mempool(uint32_t size, int socket_id) {
    LOG(INFO, "Allocating mempool of size: %u", size);

    static int idx = 0;
    char name[32];
    snprintf(name, sizeof(name), "MBUF_%d", idx++);

    struct rte_mempool *mbuf = rte_pktmbuf_pool_create(
        name, size, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
    if (mbuf == NULL) {
        LOG(ERR, "Cannot allocate mbuf pool");
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf pool");
    }

    mempools[mempools_count++] = mbuf;
    return mbuf;
}

void dpdk_ports_fini() {
    LOG(INFO, "Finalizing DPDK ports");

    for (int port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
        if (!rte_eth_dev_is_valid_port(port_id)) {
            continue;
        }

        int result = rte_eth_dev_stop(port_id);
        if (result != 0) {
            LOG(ERR, "Failed to stop port %d, error: %d", port_id, result);
        }

        result = rte_eth_dev_close(port_id);
        if (result != 0) {
            LOG(ERR, "Failed to close port %d, error: %d", port_id, result);
        }
    }

    for (int i = 0; i < mempools_count; i++) {
        if (mempools[i] != NULL) {
            rte_mempool_free(mempools[i]);
        }
    }

    LOG(INFO, "Finished finalizing DPDK ports");
}

void dpdk_stop_process() {
    LOG(INFO, "Stopping process");
    force_quit = true;
}

int rx_thread() {
    const uint32_t core_id = rte_lcore_id();

    struct rte_mbuf *mbufs[BURST_SIZE];
    PacketAction actions[BURST_SIZE];

    const uint16_t rx_port_id = core_params_arr[core_id].port;
    const uint32_t queue_id = core_params_arr[core_id].queue;
    struct rte_ring *rx_to_tx_ring = rx_tx_rings[rx_port_id][queue_id];
    if (rx_to_tx_ring == NULL) {
        LOG(ERR, "RX to TX ring is NULL for core %u", core_id);
        return -1;
    }

    LOG(INFO, "RX Core %u processes: port %u; queue %u", core_id, rx_port_id,
        queue_id);

    CoreStat core_stat;
    memset(&core_stat, 0, sizeof(core_stat));
    current_cores_stat[core_id] = &core_stat;

    while (!force_quit) {
        uint16_t nb_rx =
            rte_eth_rx_burst(rx_port_id, queue_id, mbufs, BURST_SIZE);

        process_packet(actions, mbufs, nb_rx, rx_port_id, core_id, &core_stat);

        if (nb_rx > 0) {
            for (uint16_t i = 0; i < nb_rx; i++) {
                if (actions[i] == DROP) {
                    rte_pktmbuf_free(mbufs[i]);
                } else if (actions[i] == FILTER) {
                    rte_pktmbuf_free(mbufs[i]);
                } else if (actions[i] == FORWARD) {
                    // Enqueue the packet to the ring for transmission
                    int ret = rte_ring_enqueue(rx_to_tx_ring, mbufs[i]);
                    if (unlikely(ret != 0)) {
                        rte_pktmbuf_free(mbufs[i]);
                    }
                } else {
                    rte_pktmbuf_free(mbufs[i]);
                }
            }
        }
    }

    LOG(INFO, "Core %u finished processing", core_id);
    return 0;
}

int tx_thread() {
    const uint32_t core_id = rte_lcore_id();

    if (!core_params_arr[core_id].used) {
        return 0;
    }

    struct rte_mbuf *mbufs[BURST_SIZE];
    const uint16_t rx_port_id = core_params_arr[core_id].port;
    const uint16_t tx_port_id = rx_port_id ^ 1;
    const uint32_t queue_id = core_params_arr[core_id].queue;

    struct rte_ring *rx_to_tx_ring = rx_tx_rings[rx_port_id][queue_id];
    if (rx_to_tx_ring == NULL) {
        LOG(ERR, "RX to TX ring is NULL for core %u", core_id);
        return -1;
    }

    const uint64_t drain_tsc =
        (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;
    uint64_t prev_tsc = 0;
    struct rte_eth_dev_tx_buffer *p_tx_buffer = tx_buffer[tx_port_id][queue_id];

    LOG(INFO, "TX Core %u processes: port %u; queue %u", core_id, tx_port_id,
        queue_id);

    while (!force_quit) {
        uint64_t cur_tsc = rte_rdtsc();
        uint64_t diff_tsc = cur_tsc - prev_tsc;

        if (unlikely(diff_tsc > drain_tsc)) {
            rte_eth_tx_buffer_flush(tx_port_id, queue_id, p_tx_buffer);
            prev_tsc = cur_tsc;
        }

        unsigned nb_rx = rte_ring_dequeue_burst(rx_to_tx_ring, (void **)mbufs,
                                                BURST_SIZE, NULL);
        if (nb_rx > 0) {
            for (uint16_t j = 0; j < nb_rx; j++) {
                rte_eth_tx_buffer(tx_port_id, queue_id, p_tx_buffer, mbufs[j]);
            }
        }
    }

    LOG(INFO, "Core %u finished processing", core_id);
    return 0;
}

int launch_one_lcore() {
    const uint32_t core_id = rte_lcore_id();

    if (!core_params_arr[core_id].used) {
        return 0;
    }
    if (core_params_arr[core_id].rx)
        rx_thread();
    else
        tx_thread();
    return 0;
}

void dpdk_launch_process() {
    LOG(INFO, "Launching process");
    rte_eal_mp_remote_launch(launch_one_lcore, NULL, CALL_MAIN);
    rte_eal_mp_wait_lcore();
}

bool is_rx_core(unsigned lcore_id) {
    return lcore_id % 2 == 0;  // even cores are RX
}

void cores_map_queue() {
    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        LOG(ERR, "No available ports");
        rte_exit(EXIT_FAILURE, "No available ports\n");
    }

    uint32_t nb_lcores = rte_lcore_count();
    if (nb_lcores < nb_ports) {
        LOG(ERR, "Not enough lcores available");
        rte_exit(EXIT_FAILURE, "Not enough lcores available\n");
    }

    memset(core_params_arr, 0, sizeof(core_params_arr));
    memset(port_cores, 0, sizeof(port_cores));
    memset(port_max_cores, 0, sizeof(port_max_cores));
    memset(numa_nodes, 0, sizeof(numa_nodes));

    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);

    for (uint32_t core = 0; core < num_cores; core++) {
        if (!rte_lcore_is_enabled(core)) {
            free_cores[free_cores_count++] = core;
            continue;
        }
        int socket_id = rte_lcore_to_socket_id(core);
        numa_nodes[socket_id].cores++;
    }

    for (int port = 0; port < RTE_MAX_ETHPORTS; port++) {
        if (!rte_eth_dev_is_valid_port(port)) {
            continue;
        }
        int socket_id = rte_eth_dev_socket_id(port);
        numa_nodes[socket_id].ports++;
    }

    for (int numa = 0; numa < RTE_MAX_NUMA_NODES; numa++) {
        if (numa_nodes[numa].ports == 0) {
            continue;
        }
        port_max_cores[numa] = numa_nodes[numa].cores / numa_nodes[numa].ports;
    }

    for (uint32_t core = 0; core < num_cores; core++) {
        if (!rte_lcore_is_enabled(core)) {
            free_cores[free_cores_count++] = core;
            continue;
        }

        int socket_id = rte_lcore_to_socket_id(core);

        int assigned = 0;
        for (int port = 0; port < RTE_MAX_ETHPORTS; port++) {
            if (!rte_eth_dev_is_valid_port(port)) {
                // check next port
                continue;
            }

            int numa = rte_eth_dev_socket_id(port);

            if (socket_id != numa) {
                // check next port
                continue;
            }

            int max_cores = MIN(port_max_cores[numa], MAX_CORES_PER_PORT);

            if (port_cores[port].core_count < max_cores) {
                bool is_rx = is_rx_core(core);

                core_params_arr[core].used = 1;
                core_params_arr[core].port = port;

                core_params_arr[core].rx = is_rx;
                port_cores[port].cores[port_cores[port].core_count++] = core;

                if(is_rx){
                    core_params_arr[core].queue = port_cores[port].rx_queue_count;
                    port_cores[port].rx_queue_count++;
                }
                else{
                    core_params_arr[core].queue = port_cores[port].tx_queue_count;
                    port_cores[port].tx_queue_count++;
                }


                assigned = 1;
                break;
            }
        }

        if (!assigned) {
            free_cores[free_cores_count++] = core;
        }
    }
}

uint64_t phy_discard_packets(uint16_t port_id) {
    LOG(INFO, "Fetching physical discard packets for port %u", port_id);
    uint64_t rx_phy_discard_packets = 0;

    if (!rte_eth_dev_is_valid_port(port_id)) {
        return rx_phy_discard_packets;
    }

    const char *name = "rx_phy_discard_packets";
    uint64_t id = 0;
    const int size = 1;  // Count of elements in the array

    if (!rte_eth_xstats_get_id_by_name(port_id, name, &id)) {
        if (size != rte_eth_xstats_get_by_id(port_id, &id,
                                             &rx_phy_discard_packets, size)) {
            LOG(ERR, "There isn't xstats with the id: %lu", id);
        }
    } else {
        LOG(ERR, "There isn't a name with the id: %s", name);
    }

    return rx_phy_discard_packets;
}

int get_stat(uint16_t port, port_stat_t *stat) {
    LOG(INFO, "Fetching statistics for port %u", port);
    struct rte_eth_stats rte_port_stat;
    memset(&rte_port_stat, 0, sizeof(rte_port_stat));

    if (rte_eth_stats_get(port, &rte_port_stat) != 0) {
        LOG(ERR, "Failed to fetch statistics for port %u", port);
        return -1;
    }

    uint64_t rx_phy_discard_packets = phy_discard_packets(port);

    // Populate the port_stat_t structure
    stat->port_id = port;
    stat->packets_rx = rte_port_stat.ipackets;
    stat->packets_tx = rte_port_stat.opackets;
    stat->packets_rx_errors =
        rte_port_stat.ierrors + rte_port_stat.imissed + rx_phy_discard_packets;
    stat->packets_tx_errors = rte_port_stat.oerrors;

    stat->bytes_rx = rte_port_stat.ibytes;
    stat->bytes_tx = rte_port_stat.obytes;

    return 0;
}

void fetch_all_stats() {
    // Clear the terminal screen
    printf("\033[2J\033[H");  // ANSI escape codes to clear screen and move
                              // cursor to top-left

    // Print table header
    printf(
        "+-------+------------+------------+----------------+----------------+-"
        "---------------+----------------+------------------+------------------"
        "+\n");
    printf(
        "| Port  | Packets RX | Packets TX | RX Errors      | TX Errors      | "
        "Bytes RX       | Bytes TX       | Filtered Packets |  non-IP packets  "
        "|\n");
    printf(
        "+-------+------------+------------+----------------+----------------+-"
        "---------------+----------------+------------------+------------------"
        "+\n");

    for (uint16_t port = 0; port < RTE_MAX_ETHPORTS; port++) {
        if (!rte_eth_dev_is_valid_port(port)) {
            continue;
        }

        port_stat_t *stat = &port_stat[port];
        uint64_t total_filtered_packets = 0;
        uint64_t total_error_packets = 0;

        // Sum up core statistics for this port
        for (uint32_t core_id = 0; core_id < RTE_MAX_LCORE; core_id++) {
            if (current_cores_stat[core_id] != NULL &&
                core_params_arr[core_id].port == port) {
                total_filtered_packets +=
                    current_cores_stat[core_id]->filetered_packets -
                    prev_cores_stat[core_id].filetered_packets;
                total_error_packets +=
                    current_cores_stat[core_id]->error_packets -
                    prev_cores_stat[core_id].error_packets;

                prev_cores_stat[core_id].error_packets =
                    current_cores_stat[core_id]->error_packets;
                prev_cores_stat[core_id].filetered_packets =
                    current_cores_stat[core_id]->filetered_packets;
            }
        }

        if (get_stat(port, stat) == 0) {
            // Print statistics for the current port
            printf("| %-5u | %-10" PRIi64 " | %-10" PRIi64 " | %-14" PRIi64
                   " | %-14" PRIi64 " | %-14" PRIi64 " | %-14" PRIi64
                   " | %-16" PRIi64 " | %-16" PRIi64 " |\n",
                   stat->port_id, stat->packets_rx - stat->prev_packets_rx,
                   stat->packets_tx - stat->prev_packets_tx,
                   stat->packets_rx_errors - stat->prev_packets_rx_errors,
                   stat->packets_tx_errors - stat->prev_packets_tx_errors,
                   stat->bytes_rx - stat->prev_bytes_rx,
                   stat->bytes_tx - stat->prev_bytes_tx, total_filtered_packets,
                   total_error_packets);
        }

        // Update previous stats
        stat->prev_packets_rx = stat->packets_rx;
        stat->prev_packets_tx = stat->packets_tx;
        stat->prev_packets_rx_errors = stat->packets_rx_errors;
        stat->prev_packets_tx_errors = stat->packets_tx_errors;
        stat->prev_bytes_rx = stat->bytes_rx;
        stat->prev_bytes_tx = stat->bytes_tx;
    }

    // Print table footer
    printf(
        "+-------+------------+------------+----------------+----------------+-"
        "---------------+----------------+------------------+------------------"
        "+\n");
}