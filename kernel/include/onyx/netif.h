/*
* Copyright (c) 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _KERNEL_NETIF_H
#define _KERNEL_NETIF_H
#include <stdint.h>

#include <onyx/vfs.h>
#include <onyx/spinlock.h>
struct netif;
#include <onyx/arp.h>

#include <netinet/in.h>
#include <sys/socket.h>

struct udp_socket;

#define NETIF_LINKUP		(1 << 0)
struct netif
{
	const char *name;
	struct inode *device_file;
	void *priv;
	unsigned int flags;
	unsigned int mtu;
	unsigned char mac_address[6];
	unsigned char router_mac[6];
	struct sockaddr_in local_ip;
	struct sockaddr_in router_ip;
	int (*sendpacket)(const void *buffer, uint16_t size, struct netif *nif);
	struct netif *next;
	struct arp_hashtable arp_hashtable;
	struct spinlock hashtable_spinlock;
	struct spinlock udp_socket_lock_v4;
	struct list_head udp_sockets_v4;
	struct spinlock udp_socket_lock_v6;
	struct list_head udp_sockets_v6;
	struct spinlock tcp_socket_lock_v4;
	struct list_head tcp_sockets_v4;
	struct spinlock tcp_socket_lock_v6;
	struct list_head tcp_sockets_v6;
	struct packetbuf_proto * (*get_packetbuf_proto)(struct netif *n);
	/* To be filled for stuff like virtio */
	struct packetbuf_proto *if_proto;
};

#ifdef __cplusplus
extern "C" {
#endif

void netif_register_if(struct netif *netif);
int netif_unregister_if(struct netif *netif);
struct netif *netif_choose(void);
int netif_send_packet(struct netif *netif, const void *buffer, uint16_t size);
void netif_get_ipv4_addr(struct sockaddr_in *s, struct netif *netif);

#ifdef __cplusplus
}
#endif

#endif
