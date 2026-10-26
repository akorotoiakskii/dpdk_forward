#ifndef PACKETS_HANDLER_H
#define PACKETS_HANDLER_H

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

#define BURST_SIZE (64)

// Enum for packet actions
typedef enum {
    DROP,
    FILTER,
    FORWARD,
    UNKNOWN
} PacketAction;

typedef struct {
    uint64_t filetered_packets;
    uint64_t error_packets;//not IP packets
    uint64_t ring_dropped_packets;
    uint8_t padding[40];
} CoreStat;

void handler_init();
void process_packet(PacketAction *actions, struct rte_mbuf **mbuf, int count, int port,
         int core, CoreStat * core_stat);
void handler_clean();

#endif // PACKETS_HANDLER_H
