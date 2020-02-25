/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <endian.h>

#include <onyx/dev.h>
#include <onyx/ip.h>
#include <onyx/udp.h>
#include <onyx/netif.h>
#include <onyx/compiler.h>
#include <onyx/utils.h>
#include <onyx/byteswap.h>

#include <netinet/in.h>

uint16_t udpv4_calculate_checksum(udp_header_t *header, uint32_t srcip, uint32_t dstip)
{
	srcip = htonl(srcip);
	dstip = htonl(dstip);
	uint16_t proto = IPV4_UDP << 8;
	uint16_t packet_length = ntohs(header->len);
	
	uint16_t r = __ipsum_unfolded(&srcip, sizeof(srcip), 0);
	r = __ipsum_unfolded(&dstip, sizeof(dstip), r);
	r = __ipsum_unfolded(&proto, sizeof(proto), r);
	r = __ipsum_unfolded(&header->len, sizeof(header->len), r);
	
	r = __ipsum_unfolded(header, packet_length, r);

	return ipsum_fold(r);
}

int udp_send_packet(char *payload, size_t payload_size, int source_port,
	            int dest_port, uint32_t srcip, uint32_t destip,
		    	struct netif *netif)
{
	udp_header_t *udp_header = malloc(sizeof(udp_header_t) + payload_size);
	if(!udp_header)
		return errno = ENOMEM, 1;
	
	memset(udp_header, 0, sizeof(udp_header_t));

	udp_header->source_port = htons(source_port);
	udp_header->dest_port = htons(dest_port);
	udp_header->len = htons((uint16_t)(sizeof(udp_header_t) +
					  payload_size));
	memcpy(&udp_header->payload, payload, payload_size);

	// TODO: Doesn't work yet, investigate.
	udp_header->checksum = udpv4_calculate_checksum(udp_header, srcip, destip);
	int ret = ipv4_send_packet(srcip, destip, IPV4_UDP, (char*) udp_header,
				   sizeof(udp_header_t) + payload_size, netif);
	free(udp_header);
	return ret;
}

struct udp_socket *udp_get_port(struct netif *nif, in_port_t port)
{
	spin_lock(&nif->udp_socket_lock);

	list_for_every(&nif->udp_sockets)
	{
		struct udp_socket *s = container_of(l, struct udp_socket, socket_list_head);

		if(s->src_addr.sin_port == port)
		{
			socket_ref(&s->socket);
			spin_unlock(&nif->udp_socket_lock);
			return s;
		}
	}

	spin_unlock(&nif->udp_socket_lock);

	return NULL;
}

void udp_append_socket(struct netif *nif, struct udp_socket *s)
{
	spin_lock(&nif->udp_socket_lock);
	list_add_tail(&s->socket_list_head, &nif->udp_sockets);
	spin_unlock(&nif->udp_socket_lock);
}

int udp_bind(const struct sockaddr *addr, socklen_t addrlen, struct inode *vnode)
{
	struct udp_socket *socket = (struct udp_socket*) vnode->i_helper;
	if(socket->socket.bound)
		return -EINVAL;
	if(!socket->socket.netif)
		socket->socket.netif = netif_choose();
	struct sockaddr_in *in = (struct sockaddr_in *) addr;

	/* Check if there's any socket bound to this address yet */
	struct udp_socket *s = udp_get_port(socket->socket.netif, in->sin_port);
	if(s)
	{
		socket_unref(&s->socket);
		return -EADDRINUSE;
	}
	

	if(in->sin_port == 0){}
	/* TODO: Add port allocation */

	/* TODO: This may not be correct behavior. */
	if(in->sin_addr.s_addr == INADDR_ANY)
		in->sin_addr.s_addr = INADDR_LOOPBACK;

	memcpy(&socket->src_addr, addr, sizeof(struct sockaddr));
	
	udp_append_socket(socket->socket.netif, socket);
	socket->socket.bound = true;

	return 0;
}

int udp_connect(const struct sockaddr *addr, socklen_t addrlen, struct inode *vnode)
{
	struct udp_socket *socket = (struct udp_socket*) vnode->i_helper;
	memcpy(&socket->dest_addr, addr, sizeof(struct sockaddr));
	if(!socket->socket.netif)
		socket->socket.netif = netif_choose();
	socket->socket.connected = true;
	return 0;
}

ssize_t udp_sendto(const void *buf, size_t len, int flags, struct sockaddr *addr,
	socklen_t addrlen, struct inode *vnode)
{
	struct udp_socket *socket = (struct udp_socket*) vnode->i_helper;
	bool not_conn = !socket->socket.connected;

	struct sockaddr_in *to = (struct sockaddr_in*) &socket->dest_addr;

	if(not_conn && addr == NULL)
		return -ENOTCONN;
	else if(addr != NULL)
		to = (struct sockaddr_in *) addr;

	struct sockaddr_in from = {0};
	if(!socket->socket.bound)
		netif_get_ipv4_addr(&from, socket->socket.netif);
	else
		memcpy(&from, &socket->src_addr, sizeof(struct sockaddr));
	
	if(udp_send_packet((char*) buf, len, from.sin_port, to->sin_port,
			   from.sin_addr.s_addr, to->sin_addr.s_addr, 
			   socket->socket.netif) < 0)
	{
		return -errno;
	}
	return len;
}

/* udp_get_queued_packet - Gets either a packet that was queued on receive or waits for one */
struct udp_packet *udp_get_queued_packet(struct udp_socket *socket)
{
	sem_wait(&socket->packet_semaphore);
	spin_lock(&socket->packet_lock);

	struct udp_packet *packet = socket->packet_list;
	socket->packet_list = packet->next;

	spin_unlock(&socket->packet_lock);

	return packet;
}

ssize_t udp_recvfrom(void *buf, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *slen, struct inode *vnode)
{
	struct udp_socket *socket = (struct udp_socket*) vnode->i_helper;
	struct sockaddr_in *addr = (struct sockaddr_in *) &socket->src_addr;
	if(!addr->sin_addr.s_addr)
		return -ENOTCONN;

	bool storing_src = src_addr ? true : false;

	struct udp_packet *packet = udp_get_queued_packet(socket);

	assert(packet != NULL);
	
	ssize_t to_copy = min(len, packet->size);

	memcpy(buf, packet->payload, to_copy);

	if(storing_src)
	{
		memset(&packet->addr.sin_zero, 0, sizeof(packet->addr.sin_zero));

		if(copy_to_user(src_addr, &packet->addr, sizeof(packet->addr)) < 0)
		{
			free(packet);
			return -EFAULT;
		}

		socklen_t length = sizeof(packet->addr);
		if(copy_to_user(slen, &length, sizeof(socklen_t)) < 0)
		{
			free(packet);
			return -EFAULT;
		}
	}

	free(packet);

	return to_copy;
}

size_t udp_write(size_t offset, size_t sizeofwrite, void* buffer, struct inode* this)
{
	return (size_t) udp_sendto(buffer, sizeofwrite, 0, NULL, 0, this);
}

static struct file_ops udp_ops = 
{
	.bind = udp_bind,
	.connect = udp_connect,
	.sendto = udp_sendto,
	.write = udp_write,
	.recvfrom = udp_recvfrom
};

struct socket *udp_create_socket(int type)
{
	struct udp_socket *socket = zalloc(sizeof(struct udp_socket));
	if(!socket)
		return NULL;

	
	socket->socket.ops = &udp_ops;
	
	socket->type = type;
	
	return (struct socket*) socket;
}

int udp_init_netif(struct netif *netif)
{
	/* TODO: Add IPv6 support */
	INIT_LIST_HEAD(&netif->udp_sockets);
	return 0;
}

void udp_append_packet(struct udp_packet *packet, struct udp_socket *socket)
{
	spin_lock(&socket->packet_lock);

	struct udp_packet **pp = &socket->packet_list;
	
	while(*pp)
		pp = &(*pp)->next;

	*pp = packet;

	spin_unlock(&socket->packet_lock);

	sem_signal(&socket->packet_semaphore);
}

void udp_handle_packet(ip_header_t *header, size_t length, struct netif *netif)
{
	udp_header_t *udp_header = (udp_header_t *) (header + 1);

	struct sockaddr_in addr;
	addr.sin_addr.s_addr = ntoh32(header->source_ip);
	uint32_t src_port = ntoh16(udp_header->dest_port);

	addr.sin_port = ntoh16(udp_header->source_port);

	addr.sin_family = AF_INET;
	
	struct udp_packet *packet = zalloc(sizeof(*packet));
	if(!packet)
	{
		printf("udp: Could not allocate packet memory\n");
		return;
	}

	size_t payload_len = ntoh16(udp_header->len);

	packet->size = payload_len;
	packet->payload = memdup(udp_header + 1, payload_len);
	memcpy(&packet->addr, &addr, sizeof(addr));

	if(!packet->payload)
	{
		printf("udp: Could not allocate payload memory\n");
		free(packet);
		return;
	}

	struct udp_socket *socket = udp_get_port(netif, src_port);
	if(!socket)
	{
		free(packet->payload);
		free(packet);
		return;
	}

	udp_append_packet(packet, socket);

	socket_unref(&socket->socket);
}