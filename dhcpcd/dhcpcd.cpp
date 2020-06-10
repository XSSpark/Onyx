/*
* Copyright (c) 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <vector>
#include <thread>
#include <memory>
#include <assert.h>
#include <stdexcept>

#include <sys/syscall.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <netdb.h>

#include <arpa/inet.h>

#include <dhcp.h>
#include "dhcpcd.hpp"

#define DHCP_MIN_OPT_OFFSET	4

char *program_name;
void error(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "%s: error: ", program_name);
	vfprintf(stderr, msg, ap);
	va_end(ap);
}

void errorx(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "%s: error: ", program_name);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	exit(1);
}

namespace dhcpcd
{

void init_entropy(void)
{
	unsigned int seed = 0;
	if(syscall(SYS_getrandom, &seed, sizeof(seed), 0) < 0)
		errorx("Couldn't gather entropy: %s\n", strerror(errno));
	struct timespec t = {0};
	clock_gettime(CLOCK_REALTIME, &t);
	srandom(seed ^ t.tv_nsec | t.tv_sec);
}

off_t dhcp_add_option(dhcp_packet_t *pkt, off_t off, unsigned char len,
		      const void *buf, size_t size_buf, unsigned char opt)
{
	pkt->options[off++] = opt;
	pkt->options[off++] = len;
	memcpy(&pkt->options[off], buf, size_buf);
	return off + size_buf;
}

off_t dhcp_close_options(dhcp_packet_t *pkt, off_t off)
{
	/* Add the needed padding */
	memset(&pkt->options[off], 0, 3);
	off += 3;
	pkt->options[off] = DHO_END;

	return off + 1;
}

bool packet::decode()
{
	unsigned char *limit = (unsigned char *) packet + length;
 
	unsigned char *opt = (unsigned char *) &packet->options;

	if(length <= DHCP_FIXED_NON_UDP)
		return false;

	if(memcmp(opt, DHCP_OPTIONS_COOKIE, 4) == 1)
	{
		printf("dhcpcd: Bad cookie\n");
		return false;
	}

	bool has_message_type = false;

	opt += 4;
	while(*opt != DHO_END)
	{
		/* Check of OOB */
		if(opt >= limit)
			return false;

		unsigned char type = *opt;
		opt++;
		unsigned char length = *opt;

		if(type == DHO_DHCP_MESSAGE_TYPE)
			has_message_type = true;

		dhcp_option option{opt + 1, type, length};

		options.push_back(std::move(option));

		opt = opt + length + 1;
	}

	if(!has_message_type)
		return false;

	return true;
}

void instance::send_discover()
{
	auto boot_packet = buf;
	memset(boot_packet, 0, sizeof(dhcp_packet_t));

	memcpy(&boot_packet->chaddr, &mac, 6);
	boot_packet->xid = xid;
	boot_packet->hlen = 6;
	boot_packet->htype = HTYPE_ETHER;
	boot_packet->op = BOOTREQUEST;
	boot_packet->flags = 0;

	off_t off = DHCP_MIN_OPT_OFFSET;
	memcpy(&boot_packet->options, DHCP_OPTIONS_COOKIE, 4);

	unsigned char message_type = DHCPDISCOVER;
	off = dhcp_add_option(boot_packet, off, 1, &message_type,
			      sizeof(message_type), DHO_DHCP_MESSAGE_TYPE);
	unsigned char opts[3] = {DHO_SUBNET_MASK, DHO_ROUTERS,
				 DHO_DOMAIN_NAME_SERVERS};
	off = dhcp_add_option(boot_packet, off, 3, &opts, sizeof(opts),
			      DHO_DHCP_PARAMETER_REQUEST_LIST);
	off = dhcp_close_options(boot_packet, off);

	if(send(sockfd, boot_packet, DHCP_FIXED_NON_UDP + off, 0) < 0)
	{
		throw std::runtime_error(std::string("send: Error sending the boot packet: ") + strerror(errno));
	}
}

void instance::send_request(uint32_t ip, uint32_t selected_server)
{
	auto boot_packet = buf;
	memset(boot_packet, 0, sizeof(dhcp_packet_t));

	memcpy(&boot_packet->chaddr, &mac, 6);
	boot_packet->xid = xid;
	boot_packet->hlen = 6;
	boot_packet->htype = HTYPE_ETHER;
	boot_packet->op = BOOTREQUEST;
	boot_packet->flags = 0;

	off_t off = DHCP_MIN_OPT_OFFSET;
	memcpy(&boot_packet->options, DHCP_OPTIONS_COOKIE, 4);

	unsigned char message_type = DHCPREQUEST;
	off = dhcp_add_option(boot_packet, off, 1, &message_type,
			      sizeof(message_type), DHO_DHCP_MESSAGE_TYPE);
	off = dhcp_add_option(boot_packet, off, 4, &ip, 4, DHO_DHCP_REQUESTED_ADDRESS);
	off = dhcp_add_option(boot_packet, off, 4, &selected_server, 4, DHO_DHCP_SERVER_IDENTIFIER);

	unsigned char opts[3] = {DHO_SUBNET_MASK, DHO_ROUTERS,
				 DHO_DOMAIN_NAME_SERVERS};
	off = dhcp_add_option(boot_packet, off, 3, &opts, sizeof(opts),
			      DHO_DHCP_PARAMETER_REQUEST_LIST);
	off = dhcp_close_options(boot_packet, off);

	if(send(sockfd, boot_packet, DHCP_FIXED_NON_UDP + off, 0) < 0)
	{
		throw std::runtime_error(std::string("send: Error sending the boot packet: ") + strerror(errno));
	}
}

std::unique_ptr<packet> instance::get_packets(std::function<bool (packet *)> pred)
{
	std::unique_ptr<packet> p = std::make_unique<packet>();

	dhcp_packet_t *packet = new dhcp_packet_t();
	p->packet = packet;
	struct sockaddr addr;
	socklen_t addrlen;

	auto length = recvfrom(sockfd, packet, sizeof(dhcp_packet_t), 0, &addr, &addrlen);

	if(length < 0)
	{
		throw std::runtime_error(std::string("recv: Error recieving packet: ") + strerror(errno));
	}

	p->src = addr;
	p->len = addrlen;
	p->length = length;

	if(!p->decode())
	{
		/* Note that the packet struct took ownership of the packet buffer */
		return nullptr;
	}

	/* Probably not for us */
	if(p->packet->xid != xid)
		return nullptr;

	auto message_type = p->get_option(DHO_DHCP_MESSAGE_TYPE, 1);
		
	assert(message_type != nullptr);
		
	auto pdata = message_type->option.data();

	if(*pdata == DHCPOFFER)
	{
		if(got_dhcp_offer)
			return nullptr;
		else
			got_dhcp_offer = true;
	}

	/* If it's not the packet we want, delete it */
	if(!pred(p.get()))
	{
		return nullptr;
	}

	return p;
}

void tcp_test()
{
	int sockfd, connfd; 
    struct sockaddr_in servaddr, cli; 
  
    // TCP test
    sockfd = socket(AF_INET, SOCK_STREAM, 0); 
    if (sockfd == -1) { 
        printf("socket creation failed...\n"); 
        exit(0); 
    } 
    else
        printf("Socket successfully created..\n"); 
    bzero(&servaddr, sizeof(servaddr)); 
	
	struct hostent *ent = gethostbyname2("google.com", AF_INET);
	if(!ent)
	{
		herror("gethostbyname2");
		printf("Failed to resolve google.com\n");
		exit(0);
	}

	struct in_addr **address_list = (struct in_addr **) ent->h_addr_list;
    servaddr.sin_family = AF_INET; 
    servaddr.sin_addr.s_addr = address_list[0]->s_addr;
    servaddr.sin_port = htons(80);
	
	connect(sockfd, (const struct sockaddr *) &servaddr, sizeof(struct sockaddr_in));
	perror("connect");
	send(sockfd, "Hello World!\n", strlen("Hello World!\n"), 0);
	perror("send");
#if 0
	char buffer[4096];
	ssize_t read = 0;
	while((read = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr)) >= 0)
	{
		printf("buffer: %s", buffer);
		memset(buffer, 0, sizeof(buffer));
	}
#endif
}

int instance::setup_netif()
{
	/* DHCP essentially works like this:
	 * 1) The client sends a DHCP discover request through broadcast
	 * 2) The various DHCP servers on the local network reply
	 * 3) The client picks one and sends a DHCPREQUEST through broadcast, requesting the picked IP address. 
	 * It's sent through broadcast as to signal the other DHCP servers that we didn't pick their address.
	 * We get send a DHCP ACK packet, containing a NACK or ACK.
	 */

	send_discover();

	std::unique_ptr<packet> packet;
	
	/* If for some reason we can't retrieve a packet, the get_packets will throw an exception */
	while(!(packet = get_packets([](dhcpcd::packet *data) -> bool
	{
		if(!data->get_option(DHO_ROUTERS, 4))
			return false;
	
		auto message_type = data->get_option(DHO_DHCP_MESSAGE_TYPE, 1);
		
		assert(message_type != nullptr);

		/* Sanitise the option parameters */
		
		auto pdata = message_type->option.data();
		return *pdata == DHCPOFFER;
	})))
	{
	}

	uint32_t router_ip = 0;
	uint32_t assigned_ip = 0;
	uint32_t subnet_mask = 0;
	uint32_t dns_server = 0;
	uint32_t lease_time = 0;

	uint32_t our_ip = packet->packet->yiaddr;

	dhcp_option *opt = packet->get_option(DHO_DOMAIN_NAME_SERVERS, 4);

	if(opt != nullptr)
	{
		dns_server = *(uint32_t *) opt->option.data();
	}

	opt = packet->get_option(DHO_ROUTERS, 4);

	if(opt != nullptr)
	{
		router_ip = *(uint32_t *) opt->option.data();
	}

	opt = packet->get_option(DHO_SUBNET_MASK, 4);

	if(opt != nullptr)
	{
		subnet_mask = *(uint32_t *) opt->option.data();
	}

	opt = packet->get_option(DHO_DHCP_LEASE_TIME, 4);

	if(opt != nullptr)
	{
		lease_time = ntohl(*(uint32_t *) opt->option.data());
	}

	memset(buf, 0, sizeof(dhcp_packet_t));

	struct sockaddr_in *inaddr = (struct sockaddr_in *) &packet->src;
	send_request(our_ip, inaddr->sin_addr.s_addr);

	while(!(packet = get_packets([](dhcpcd::packet *data) -> bool
	{	
		auto message_type = data->get_option(DHO_DHCP_MESSAGE_TYPE, 1);
		
		assert(message_type != nullptr);
		
		auto pdata = message_type->option.data();
		return *pdata == DHCPACK || *pdata == DHCPNAK;
	})))
	{
	}

	auto message_type = packet->get_option(DHO_DHCP_MESSAGE_TYPE, 1);

	auto pdata = message_type->option.data();

	/* TODO: What should we do on DHCP nack? */
	if(*pdata != DHCPACK)
		return -1;
	
	struct if_config_inet cfg;
	cfg.address.s_addr = our_ip;
	cfg.subnet.s_addr = subnet_mask;
	cfg.router.s_addr = router_ip;
	if(ioctl(fd, SIOSETINET4, &cfg) < 0)
	{
		perror("SIOSETINET4");
		return -1;
	}

	tcp_test();

	return 0;
}

void instance::run()
{
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if(sockfd < 0)
	{
		throw std::runtime_error(std::string("socket: ") + strerror(errno));
	}

	struct sockaddr_in sockaddr = {0};
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(68);

	if(bind(sockfd, (const struct sockaddr *) &sockaddr, sizeof(struct sockaddr)) < 0)
	{
		throw std::runtime_error(std::string("bind: ") + strerror(errno));
	}

	sockaddr.sin_port = htons(67);
	sockaddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	if(connect(sockfd, (const struct sockaddr *) &sockaddr, sizeof(struct sockaddr)) < 0)
	{
		throw std::runtime_error(std::string("connect: ") + strerror(errno));
	}

	setup_netif();
}

std::vector<std::unique_ptr<instance> > instances;

int create_instance(std::string& name)
{
	int fd = open(name.c_str(), O_RDWR);
	if(fd < 0)
	{
		auto error = strerror(errno);

		throw std::runtime_error("Failed to open " + name + ": " + error);
	}

	auto inst = std::make_unique<instance>(fd, name);

	instances.push_back(std::move(inst));

	return 0;
}

}

int main(int argc, char **argv, char **envp)
{
	program_name = argv[0];
	int logfd = open("/dev/null", O_RDWR);
	if(logfd < 0)
	{
		perror("could not create logfd");
		return 1;
	}

#if 0
	dup2(logfd, 0);
	dup2(logfd, 1);
	dup2(logfd, 2);
#endif

	close(logfd);

	printf("%s: Daemon initialized\n", argv[0]);

	/* TODO: Discover NICs in /dev (maybe using netlink? or sysfs) */
	
	std::string name{"/dev/eth0"};
	dhcpcd::init_entropy();

	dhcpcd::create_instance(name);

	while(1)
		sleep(100000);
	return 0;
}
