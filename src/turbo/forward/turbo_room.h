#ifndef TURBO_ROOM_H
#define TURBO_ROOM_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include "../netif/turbo_netif.h"
#include "../netif/turbo_port.h"


/* Forward declarations */
struct turbo_room_mgr;
struct turbo_room;
struct turbo_member;

/**
 * Room member structure
 */
struct turbo_member {
    uint32_t id;                    /* Member ID */
    struct sockaddr_in addr;        /* Member address */
    uint16_t port;                  /* Member port */
    uint8_t marked_for_delete;      /* RCU deletion flag */
    struct turbo_member *next;      /* Next in hash chain */
};

/**
 * Room structure
 */
struct turbo_room {
    uint32_t room_id;               /* Room ID */
    uint32_t member_count;          /* Current member count */
    struct turbo_member *members;   /* Member list (hash table) */
    uint8_t marked_for_delete;      /* RCU deletion flag */
    struct turbo_room *next;        /* Next in global list */
};

/**
 * Room manager structure
 */
struct turbo_room_mgr {
    struct turbo_netif *netif;      /* Network interface */

    /* Custom lock-free hash table implementation */
    struct {
        struct turbo_room **buckets;
        uint32_t num_buckets;
        uint32_t hash_seed;
    } room_hash;

    struct turbo_room *rooms;       /* Global room list */
    uint32_t max_rooms;             /* Maximum number of rooms */
    uint32_t max_members_per_room;  /* Maximum members per room */
};

/**
 * Initialize room manager
 * @param mgr Room manager to initialize
 * @param netif Network interface
 * @param max_rooms Maximum number of rooms
 * @param max_members_per_room Maximum members per room
 * @return 0 on success, negative error code on failure
 */
int turbo_room_mgr_init(struct turbo_room_mgr *mgr, struct turbo_netif *netif,
                       uint32_t max_rooms, uint32_t max_members_per_room);

/**
 * Create and initialize a new room manager
 * @param netif Network interface
 * @param max_rooms Maximum number of rooms
 * @param max_members_per_room Maximum members per room
 * @return Pointer to initialized room manager, NULL on failure
 */
struct turbo_room_mgr* turbo_room_mgr_create(struct turbo_netif *netif,
    uint32_t max_rooms, uint32_t max_members_per_room);

/**
 * Cleanup room manager
 * @param mgr Room manager to cleanup
 */
void turbo_room_mgr_cleanup(struct turbo_room_mgr *mgr);

/**
 * Destroy and free a room manager
 * @param mgr Room manager to destroy
 */
void turbo_room_mgr_destroy(struct turbo_room_mgr *mgr);

/**
 * Create a new room
 * @param mgr Room manager
 * @param room_id Room ID
 * @return 0 on success, negative error code on failure
 */
int turbo_room_create(struct turbo_room_mgr *mgr, uint32_t room_id);

/**
 * Destroy a room
 * @param mgr Room manager
 * @param room_id Room ID
 */
void turbo_room_destroy(struct turbo_room_mgr *mgr, uint32_t room_id);

/**
 * Add member to room
 * @param mgr Room manager
 * @param room_id Room ID
 * @param member_id Member ID
 * @param addr Member address
 * @param port Member port
 * @return 0 on success, negative error code on failure
 */
int turbo_room_add_member(struct turbo_room_mgr *mgr, uint32_t room_id,
                         uint32_t member_id, struct sockaddr_in *addr, uint16_t port);

/**
 * Remove member from room
 * @param mgr Room manager
 * @param room_id Room ID
 * @param member_id Member ID
 */
void turbo_room_remove_member(struct turbo_room_mgr *mgr, uint32_t room_id,
                             uint32_t member_id);

/**
 * Broadcast packet to all members in room except sender
 * @param mgr Room manager
 * @param room_id Room ID
 * @param sender_id Sender member ID (to exclude from broadcast)
 * @param pkt Packet to broadcast
 * @return Number of members packet was sent to
 */
int turbo_room_broadcast(struct turbo_room_mgr *mgr, uint32_t room_id,
                        uint32_t sender_id, struct turbo_packet *pkt);

/**
 * Get room by ID
 * @param mgr Room manager
 * @param room_id Room ID
 * @return Room pointer or NULL if not found
 */
struct turbo_room* turbo_room_get(struct turbo_room_mgr *mgr, uint32_t room_id);

/**
 * Reclaim memory from RCU deferred deletions
 * @param mgr Room manager
 */
void turbo_room_mgr_reclaim(struct turbo_room_mgr *mgr);

#endif /* TURBO_ROOM_H */