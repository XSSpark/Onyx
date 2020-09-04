/*
* Copyright (c) 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <assert.h>
#include <errno.h>

#include <onyx/net/netif.h>
#include <onyx/spinlock.h>
#include <onyx/dev.h>
#include <onyx/net/udp.h>
#include <onyx/net/tcp.h>
#include <onyx/byteswap.h>
#include <onyx/softirq.h>
#include <onyx/init.h>
#include <onyx/vector.h>

#include <sys/ioctl.h>

static struct spinlock netif_list_lock = {0};
cul::vector<netif*> netif_list;

unsigned int netif_ioctl(int request, void *argp, struct file* f)
{
	auto netif = static_cast<struct netif *>(f->f_ino->i_helper);
	assert(netif != nullptr);
	switch(request)
	{
		case SIOSETINET4:
		{
			struct if_config_inet *c = static_cast<if_config_inet *>(argp);

			struct if_config_inet i;
			if(copy_from_user(&i, c, sizeof(struct if_config_inet)) < 0)
				return -EFAULT;
			auto local = &netif->local_ip;
			memcpy(&local->sin_addr, &i.address, sizeof(struct in_addr));
			return 0;
		}
		case SIOGETINET4:
		{
			struct if_config_inet *c = static_cast<if_config_inet *>(argp);
			auto local = &netif->local_ip;
			if(copy_to_user(&c->address, &local->sin_addr, sizeof(struct in_addr)) < 0)
				return -EFAULT;
			return 0;
		}
		case SIOSETINET6:
		{
			return -ENOSYS;
#if 0
			struct if_config_inet6 *c = static_cast<if_config_inet6 *>(argp);
			auto local = &netif->local_ip;
			auto router = &netif->router_ip;
			if(copy_to_user(&local->sin_addr, &c->address, sizeof(struct in6_addr)) <0)
				return -EFAULT;
			if(copy_to_user(&router->sin_addr, &c->router, sizeof(struct in6_addr)) < 0)
				return -EFAULT;
			return 0;
#endif
		}
		case SIOGETINET6:
		{
			return -ENOSYS;
#if 0
			struct if_config_inet6 *c = static_cast<if_config_inet6 *>(argp);
			auto local = &netif->local_ip;
			auto router = &netif->router_ip;
			if(copy_to_user(&c->address, &local->sin_addr, sizeof(struct in6_addr)) < 0)
				return -EFAULT;
			if(copy_to_user(&c->router, &router->sin_addr, sizeof(struct in6_addr)) < 0)
				return -EFAULT;
			return 0;
#endif
		}
		case SIOGETMAC:
		{
			if(copy_to_user(argp, &netif->mac_address, 6) < 0)
				return -EFAULT;
			return 0;
		}
	}
	return -ENOTTY;
}

void netif_register_if(struct netif *netif)
{
	assert(udp_init_netif(netif) == 0);
	
	assert(tcp_init_netif(netif) == 0);
		
	struct dev *d = dev_register(0, 0, (char*) netif->name);
	if(!d)
		return;

	d->priv = netif;

	d->fops.ioctl = netif_ioctl;

	device_show(d, DEVICE_NO_PATH, 0666);
	
	spin_lock(&netif_list_lock);

	assert(netif_list.push_back(netif) != false);

	spin_unlock(&netif_list_lock);

	bool is_loopback = netif->flags & NETIF_LOOPBACK;

	struct inet4_route route;
	
	route.mask = is_loopback ? htonl(0xff000000) : 0;
	route.dest = is_loopback ? htonl(INADDR_LOOPBACK) : 0;
	route.dest &= route.mask;
	route.gateway = 0;
	route.nif = netif;
	route.metric = is_loopback ? 1000 : 10;
	route.flags = INET4_ROUTE_FLAG_SCOPE_LOCAL;

	assert(ip::v4::add_route(route) == true);
}

int netif_unregister_if(struct netif *netif)
{
	spin_lock(&netif_list_lock);
	
	list_remove(&netif->list_node);

	spin_unlock(&netif_list_lock);

	return 0;
}

struct netif *netif_choose(void)
{
	/* TODO: Netif refcounting would be bery noice */
	spin_lock(&netif_list_lock);

	for(auto n : netif_list)
	{
		if(n->flags & NETIF_LINKUP && !(n->flags & NETIF_LOOPBACK))
		{
			spin_unlock(&netif_list_lock);
			return n;
		}
	}

	spin_unlock(&netif_list_lock);

	return NULL;
}

netif *netif_get_from_addr(const inet_sock_address& s, int domain)
{
	spin_lock(&netif_list_lock);

	//printk("trying to find %x\n", in->sin_addr.s_addr);

	for(auto n : netif_list)
	{
		//printk("local %x\n", n->local_ip.sin_addr.s_addr);
		if(domain == AF_INET && n->local_ip.sin_addr.s_addr == s.in4.s_addr)
		{
			spin_unlock(&netif_list_lock);
			return n;
		}
	}

	spin_unlock(&netif_list_lock);

	return nullptr;
}

cul::vector<netif*>& netif_lock_and_get_list(void)
{
	spin_lock(&netif_list_lock);

	return netif_list;
}

void netif_unlock_list(void)
{
	spin_unlock(&netif_list_lock);
}

int netif_send_packet(netif *netif, packetbuf *buf)
{
	assert(netif != nullptr);
	if(netif->sendpacket)
		return netif->sendpacket(buf, netif);
	return -ENODEV;
}

void netif_get_ipv4_addr(struct sockaddr_in *s, struct netif *netif)
{
	memcpy(&s, &netif->local_ip, sizeof(struct sockaddr));
}

netif *netif_from_name(const char *name)
{
	spin_lock(&netif_list_lock);

	//printk("trying to find %x\n", in->sin_addr.s_addr);

	for(auto n : netif_list)
	{
		//printk("local %x\n", n->local_ip.sin_addr.s_addr);
		if(!strcmp(n->name, name))
		{
			spin_unlock(&netif_list_lock);
			return n;
		}
	}

	spin_unlock(&netif_list_lock);

	return nullptr;
}

struct rx_queue_percpu
{
	struct list_head to_rx_list;
	struct spinlock lock;
};

PER_CPU_VAR(rx_queue_percpu rx_queue);

static void init_rx_queues()
{
	auto q = get_per_cpu_ptr(rx_queue);
	spinlock_init(&q->lock);
	INIT_LIST_HEAD(&q->to_rx_list);
}

INIT_LEVEL_CORE_PERCPU_CTOR(init_rx_queues);

extern "C"
void netif_signal_rx(netif *nif)
{
	unsigned int flags, og_flags;

	do
	{
		flags = nif->flags;
		og_flags = flags;

		flags |= NETIF_HAS_RX_AVAILABLE;

		if(og_flags & NETIF_DOING_RX_POLL)
			flags |= NETIF_MISSED_RX;
		

	} while(!__atomic_compare_exchange_n(&nif->flags, &og_flags, flags,
		                               false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
	
	if(og_flags & NETIF_HAS_RX_AVAILABLE)
		return;

	auto queue = get_per_cpu_ptr(rx_queue);

	unsigned long cpu_flags = spin_lock_irqsave(&queue->lock);

	list_add_tail(&nif->rx_queue_node, &queue->to_rx_list);

	spin_unlock_irqrestore(&queue->lock, cpu_flags);

	softirq_raise(softirq_vector::SOFTIRQ_VECTOR_NETRX);
}

void netif_do_rxpoll(netif *nif)
{
	__atomic_or_fetch(&nif->flags, NETIF_DOING_RX_POLL, __ATOMIC_RELAXED);

	while(true)
	{
		nif->poll_rx(nif);

		unsigned int flags, og_flags;

		do
		{
			og_flags = flags = nif->flags;

			if(!(og_flags & NETIF_MISSED_RX))
			{
				nif->rx_end(nif);
				flags &= ~(NETIF_HAS_RX_AVAILABLE | NETIF_DOING_RX_POLL);
			}
			
			flags &= ~NETIF_MISSED_RX;

		} while(!__atomic_compare_exchange_n(&nif->flags, &og_flags, flags,
		                               false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
		
		if(!(flags & NETIF_DOING_RX_POLL))
			break;
	}
}

extern "C"
int netif_do_rx(void)
{
	auto queue = get_per_cpu_ptr(rx_queue);

	scoped_lock g{&queue->lock};

	list_for_every(&queue->to_rx_list)
	{
		netif *n = container_of(l, netif, rx_queue_node);

		netif_do_rxpoll(n);
	}

	list_reset(&queue->to_rx_list);

	return 0;
}

int netif_process_pbuf(netif *nif, packetbuf *buf)
{
	return nif->dll_ops->rx_packet(nif, buf);
}
