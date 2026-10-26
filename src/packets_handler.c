#include "packets_handler.h"

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

#define FILTER_HMAP_SIZE (4 * 1024 * 1024)
#define CHANGE_HMAP_SIZE (4 * 1024 * 1024)

#ifdef RTE_ARCH_X86
#include <rte_hash_crc.h>
#define DEFAULT_HASH_FUNC rte_hash_crc
#else
#include <rte_jhash.h>
#define DEFAULT_HASH_FUNC rte_jhash
#endif

#define KEY_SIZE (sizeof(uint32_t))

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    struct rte_ether_addr *p_dst_mac;
} Header;

typedef struct {
    uint32_t dst_ip;
    struct rte_ether_addr dst_mac;
} ChangeMapEntry;

struct rte_ether_addr default_dst_mac = {
    .addr_bytes = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11}};

static inline uint32_t hash_function(const void *key, uint32_t key_len,
                                     uint32_t init_val) {
    // Calculate hash only with dst_ip from ChangeMapEntry
    return DEFAULT_HASH_FUNC(key, KEY_SIZE, init_val);
}

static struct rte_hash *filter_hash_maps[RTE_MAX_ETHPORTS];
static struct rte_hash *change_hash_maps[RTE_MAX_ETHPORTS];

struct rte_hash *create_change_hash_map(uint32_t size, const char *name,
                                        int socketid) {
    LOG(INFO, "Creating hash map: size=%u, name=%s", size, name);

    struct rte_hash_parameters hash_params = {
        .name = name,
        .entries = size,
        .key_len = sizeof(ChangeMapEntry),
        .hash_func = hash_function,
        .hash_func_init_val = 1000231,
        .socket_id = socketid,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                      RTE_HASH_EXTRA_FLAGS_TRANS_MEM_SUPPORT |
                      RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD};

    struct rte_hash *rte_hash_map = rte_hash_create(&hash_params);
    if (rte_hash_map == NULL) {
        LOG(ERR, "Unable to create hash map: %s on socket %d", name, socketid);
        rte_exit(EXIT_FAILURE, "Unable to create hash map: %s on socket %d\n",
                 name, socketid);
    }

    LOG(INFO, "Hash map created successfully: %s", name);
    return rte_hash_map;
}

struct rte_hash *create_filter_hash_map(uint32_t size, const char *name,
                                        int socketid) {
    LOG(INFO, "Creating hash map: size=%u, name=%s", size, name);

    struct rte_hash_parameters hash_params = {
        .name = name,
        .entries = size,
        .key_len = sizeof(uint32_t),
        .socket_id = socketid,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                      RTE_HASH_EXTRA_FLAGS_TRANS_MEM_SUPPORT |
                      RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD};

    struct rte_hash *rte_hash_map = rte_hash_create(&hash_params);
    if (rte_hash_map == NULL) {
        LOG(ERR, "Unable to create hash map: %s on socket %d", name, socketid);
        rte_exit(EXIT_FAILURE, "Unable to create hash map: %s on socket %d\n",
                 name, socketid);
    }

    LOG(INFO, "Hash map created successfully: %s", name);
    return rte_hash_map;
}

uint32_t ip_to_uint(const char *ip) {
    uint32_t a, b, c, d;
    sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d);
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// FOR FDEBUG
void add_ip_range_to_filter_hash_map(int port, uint32_t start_ip,
                                     uint32_t end_ip) {
    if (port < 0 || port >= RTE_MAX_ETHPORTS) {
        LOG(ERR, "Invalid port: %d", port);
        return;
    }

    if (filter_hash_maps[port] == NULL) {
        LOG(ERR, "Hash map for port %d is not initialized", port);
        return;
    }

    if (start_ip > end_ip) {
        LOG(ERR, "Invalid IP range: start_ip (%u) > end_ip (%u)", start_ip,
            end_ip);
        return;
    }

    for (uint32_t ip = start_ip; ip <= end_ip; ip++) {
        uint32_t src_ip = rte_cpu_to_be_32(ip);  // Mark as filtered (drop)
        int ret = rte_hash_add_key(filter_hash_maps[port], &src_ip);
        if (ret < 0) {
            LOG(ERR, "Failed to add IP %u to hash map for port %d", ip, port);
        }
    }

    LOG(INFO, "Finished adding IP range to hash map for port %d", port);
}

// FOR FDEBUG
void add_ip_range_to_change_hash_map(int port, uint32_t start_ip,
                                     uint32_t end_ip) {
    if (port < 0 || port >= RTE_MAX_ETHPORTS) {
        LOG(ERR, "Invalid port: %d", port);
        return;
    }

    if (change_hash_maps[port] == NULL) {
        LOG(ERR, "Hash map for port %d is not initialized", port);
        return;
    }

    if (start_ip > end_ip) {
        LOG(ERR, "Invalid IP range: start_ip (%u) > end_ip (%u)", start_ip,
            end_ip);
        return;
    }

    LOG(INFO, "Adding IP range [%u - %u] to hash map for port %d", start_ip,
        end_ip, port);

    for (uint32_t ip = start_ip; ip <= end_ip; ip++) {
        ChangeMapEntry entry;
        entry.dst_ip = rte_cpu_to_be_32(ip);  // Mark as filtered (drop)
        entry.dst_mac.addr_bytes[0] = rand() % 256;
        entry.dst_mac.addr_bytes[1] = rand() % 256;
        entry.dst_mac.addr_bytes[2] = rand() % 256;
        entry.dst_mac.addr_bytes[3] = rand() % 256;
        entry.dst_mac.addr_bytes[4] = rand() % 256;
        entry.dst_mac.addr_bytes[5] = rand() % 256;
        int ret = rte_hash_add_key(change_hash_maps[port], &entry);
        if (ret < 0) {
            LOG(ERR, "Failed to add IP %u to hash map for port %d", ip, port);
        }
    }

    LOG(INFO, "Finished adding IP range to hash map for port %d", port);
}

void handler_init() {
    uint16_t port_count = rte_eth_dev_count_avail();
    LOG(INFO, "Initializing handler with port count: %d", port_count);

    if (port_count <= 0) {
        LOG(ERR, "Invalid port count: %d", port_count);
        fprintf(stderr, "Error: port_count must be greater than 0\n");
        exit(EXIT_FAILURE);
    }
    memset(filter_hash_maps, 0, sizeof(filter_hash_maps));

    for (int port = 0; port < RTE_MAX_ETHPORTS; port++) {
        if (!rte_eth_dev_is_valid_port(port)) {
            continue;
        }
        int socket_id = rte_eth_dev_socket_id(port);
        char map_name[64];
        snprintf(map_name, sizeof(map_name), "filter_hash_map_%d", port);
        filter_hash_maps[port] =
            create_filter_hash_map(FILTER_HMAP_SIZE, map_name, socket_id);

        snprintf(map_name, sizeof(map_name), "change_hash_map_%d", port);
        change_hash_maps[port] =
            create_change_hash_map(CHANGE_HMAP_SIZE, map_name, socket_id);

        // DEBUG TODO: remove
        // IPS 38.10.0.0 -  38.19.255.255
        add_ip_range_to_filter_hash_map(port, ip_to_uint("38.10.0.0"),
                                        ip_to_uint("38.11.255.255"));

        // DEBUG TODO: remove
        // IPS 100.10.10.106 - 100.10.10.150
        add_ip_range_to_change_hash_map(port, ip_to_uint("100.10.10.106 "),
                                        ip_to_uint("100.10.10.150"));
    }

    LOG(INFO, "Handler initialization completed");
}

void parsing(PacketAction *actions, struct rte_mbuf **mbuf, int count,
             Header *hdr, CoreStat *core_stat) {
    for (uint16_t j = 0; j < count; j++) {
        // prefetch 64B of the next packet
        int next = j + 1;
        if (next < count) rte_prefetch0(rte_pktmbuf_mtod(mbuf[next], void *));

        struct rte_ipv4_hdr *ip_hdr;
        struct rte_ether_hdr *eth_hdr;

        eth_hdr = rte_pktmbuf_mtod(mbuf[j], struct rte_ether_hdr *);

        // default action
        actions[j] = UNKNOWN;

        if (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) == eth_hdr->ether_type) {
            ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

            actions[j] = FORWARD;
            hdr[j].p_dst_mac = &eth_hdr->dst_addr;
            hdr[j].src_ip = ip_hdr->src_addr;
            hdr[j].dst_ip = ip_hdr->dst_addr;

        } else {
            actions[j] = DROP;
            core_stat->error_packets++;
        }
    }
}

void filter(PacketAction *actions, Header *hdr, int32_t count, int port,
            CoreStat *core_stat) {
    int32_t positions[BURST_SIZE];
    uint32_t *p_src_ip_array[BURST_SIZE];

    uint32_t zero = 0;

    for (int32_t i = 0; i < count; i++) {
        if (actions[i] != DROP) {
            p_src_ip_array[i] = &(hdr[i].src_ip);
        } else {
            // already marked to drop
            p_src_ip_array[i] = &zero;
        }
    }

    int32_t ret =
        rte_hash_lookup_bulk(filter_hash_maps[port],
                             (const void **)p_src_ip_array, count, positions);
    if (ret != 0) {
        LOG(ERR, "Fail in the \"rte_hash_lookup_bulk\"");
    }

    for (int32_t i = 0; i < count; i++) {
        // if ip_src is in the filter hash map - drop it
        if (actions[i] != DROP && positions[i] >= 0) {
            actions[i] = DROP;
            core_stat->filetered_packets++;
        }
    }
}

void change_mac(PacketAction *actions, Header *hdr, int32_t count, int port) {
    int32_t positions[BURST_SIZE];
    ChangeMapEntry keys[BURST_SIZE];

    ChangeMapEntry *p_keys[BURST_SIZE];

    uint32_t zero = 0;

    for (int32_t i = 0; i < count; i++) {
        p_keys[i] = &keys[i];

        if (actions[i] != DROP) {
            keys[i].dst_ip = hdr[i].dst_ip;
        } else {
            // already marked to drop
            keys[i].dst_ip = zero;
        }
    }

    int32_t ret = rte_hash_lookup_bulk(change_hash_maps[port],
                                       (const void **)p_keys, count, positions);
    if (ret != 0) {
        LOG(ERR, "Fail in the \"rte_hash_lookup_bulk\"");
        return;
    }

    for (int32_t i = 0; i < count; i++) {
        if (actions[i] != DROP) {
            if (positions[i] >= 0) {
                ChangeMapEntry *key = NULL;
                ret = rte_hash_get_key_with_position(
                    change_hash_maps[port], positions[i], (void **)key);
                if (ret != 0 || key == NULL) {
                    continue;
                }
                rte_ether_addr_copy(&key->dst_mac, hdr[i].p_dst_mac);

            } else {
                rte_ether_addr_copy(&default_dst_mac, hdr[i].p_dst_mac);
            }
        }
    }
}

inline void process_packet(PacketAction *actions, struct rte_mbuf **mbuf,
                           int count, int port, int core, CoreStat *core_stat) {
    Header hdr[BURST_SIZE];

    if (count) {
        parsing(actions, mbuf, count, hdr, core_stat);
        filter(actions, hdr, count, port, core_stat);
        // change mac only if the packet is not dropped
        change_mac(actions, hdr, count, port);
    }
}

void clear_hash_maps() {
    LOG(INFO, "Clearing hash maps");

    for (int port = 0; port < RTE_MAX_ETHPORTS; port++) {
        if (filter_hash_maps[port] != NULL) {
            LOG(INFO, "Clearing filter hash map for port %d", port);
            rte_hash_free(filter_hash_maps[port]);  // Free the hash map
            filter_hash_maps[port] = NULL;          // Set the pointer to NULL
        }
        if (change_hash_maps[port] != NULL) {
            LOG(INFO, "Clearing change hash map for port %d", port);
            rte_hash_free(change_hash_maps[port]);  // Free the hash map
            change_hash_maps[port] = NULL;          // Set the pointer to NULL
        }
    }
    LOG(INFO, "All hash maps cleared");
}

void handler_clean() {
    LOG(INFO, "Starting cleanup process");
    clear_hash_maps();
    LOG(INFO, "Cleanup process completed");
}
