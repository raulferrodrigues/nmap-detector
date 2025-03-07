#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>			 // close()
#include <string.h>			 // strcpy, memset(), and memcpy()
#include <netdb.h>			 // struct addrinfo
#include <sys/types.h>		 // needed for socket(), uint8_t, uint16_t, uint32_t
#include <sys/socket.h>		 // needed for socket()
#include <netinet/in.h>		 // IPPROTO_TCP, INET6_ADDRSTRLEN
#include <netinet/ip.h>		 // IP_MAXPACKET(which is 65535)
#include <netinet/ip6.h>	 // struct ip6_hdr
#include <netinet/tcp.h>	 // struct tcphdr
#include <arpa/inet.h>		 // inet_pton() and inet_ntop()
#include <sys/ioctl.h>		 // macro ioctl is defined
#include <bits/ioctls.h>	 // defines values for argument "request" of ioctl.
#include <net/if.h>			 // struct ifreq
#include <linux/if_ether.h>	 // ETH_P_IP = 0x0800, ETH_P_IPV6 = 0x86DD
#include <linux/if_packet.h> // struct sockaddr_ll(see man 7 packet)
#include <net/ethernet.h>
#include <errno.h> // errno, perror()
#include <fcntl.h>
#include "message.h"
#include <time.h>
#include <ifaddrs.h>

void *recvTCP(void *input)
{
	char *target_ipv6 = (char *)input;

	struct ifreq ifopts;
	int sockfd;
	char ifName[IFNAMSIZ];
	uint8_t raw_buffer[FRAME_LENGTH];
	if ((sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) == -1)
		perror("socket");

	// Define o socket como nao bloqueante na chamada recvfrom, assim, podemos definir um timeout
	// Para os ataques que a porta esta aberta quando nao ha resposta.
	int flags = fcntl(sockfd, F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(sockfd, F_SETFL, flags);

	struct ether_header *eth_hdr = (struct ether_header *)(&raw_buffer);

	clock_t start = clock();
	while (1)
	{
		clock_t end = clock();
		float seconds = (float)(end - start) / CLOCKS_PER_SEC;

		if (seconds >= TIME_OUT_SECONDS)
		{
			pthread_exit(NO_RESPONSE);
		}

		recvfrom(sockfd, raw_buffer, FRAME_LENGTH, 0, NULL, NULL);
		// Ethernet do tipo IPV6
		if (eth_hdr->ether_type == ntohs(0x86dd))
		{
			// Pega o header IPV6 e verifica o IP src (resposta da requisicao)
			struct ip6_hdr *iphdr = (struct ip6_hdr *)(&raw_buffer[ETH_HDRLEN]);
			char ipv6_src[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &(iphdr->ip6_src), ipv6_src, INET6_ADDRSTRLEN);

			// Se recebemos uma resposta do target e se o next header eh um tcp (para nao pegar ICMPv6)
			if (strcmp(ipv6_src, target_ipv6) == 0 && iphdr->ip6_ctlun.ip6_un1.ip6_un1_nxt == 6)
			{
				/** TCP header **/
				struct tcphdr tcphdr;
				memcpy(&tcphdr, raw_buffer + ETH_HDRLEN + IP6_HDRLEN, TCP_HDRLEN * sizeof(tcphdr));

				uint8_t tcp_flag = tcphdr.th_flags;

				// Retorna o valor da flag
				pthread_exit(tcp_flag);
			}
		}
	}
}

int sendTcp(char *dst_ip, uint8_t *dst_mac, int port, uint8_t tcp_flag, char *interface)
{
	int i, status, bytes, tcp_flags[8];
	struct addrinfo hints, *res;
	struct sockaddr_in6 *ipv6;
	void *tp;

	int socketDescriptor = openRawSocket(interface);
	struct sockaddr_ll device = getInterfaceDevice(interface);
	uint8_t *src_mac = getMacFromInterface(interface, socketDescriptor);

	struct ifaddrs *ifa, *ifa_tmp;
	char addr[INET6_ADDRSTRLEN];
	char *helpAddr = getIPV6FromInterface(interface);

	struct ip6_hdr iphdr = getIPV6Header(helpAddr, dst_ip);
	struct tcphdr tcphdr = getTCPHeader(iphdr, port, tcp_flag); // getTCPHeader(iphdr);
	uint8_t *ether_frame = getEthernetFrame(src_mac, dst_mac, iphdr, tcphdr);
	sendEthernetFrame(ether_frame, socketDescriptor, device);

	return (EXIT_SUCCESS);
}

char *getIPV6FromInterface(char *interface)
{
	struct ifaddrs *ifa, *ifa_tmp;
	char addr[INET6_ADDRSTRLEN];
	char *addr2 = malloc(INET6_ADDRSTRLEN * sizeof(char));

	if (getifaddrs(&ifa) == -1)
	{
		perror("getifaddrs failed");
		exit(1);
	}

	ifa_tmp = ifa;
	while (ifa_tmp)
	{
		if ((ifa_tmp->ifa_addr) && ((ifa_tmp->ifa_addr->sa_family == AF_INET) ||
									(ifa_tmp->ifa_addr->sa_family == AF_INET6)))
		{
			if (ifa_tmp->ifa_addr->sa_family == AF_INET)
			{
				// create IPv4 string
				struct sockaddr_in *in = (struct sockaddr_in *)ifa_tmp->ifa_addr;
				inet_ntop(AF_INET, &in->sin_addr, addr, sizeof(addr));
			}
			else
			{ // AF_INET6
				// create IPv6 string
				struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ifa_tmp->ifa_addr;
				inet_ntop(AF_INET6, &in6->sin6_addr, addr, sizeof(addr));

				if (strcmp(interface, ifa_tmp->ifa_name) == 0)
				{
					if (strlen(addr) > 16)
					{
						memcpy(addr2, addr, INET6_ADDRSTRLEN * sizeof(char));
						return addr2;
					}
				}
			}
		}
		ifa_tmp = ifa_tmp->ifa_next;
	}
	freeifaddrs(ifa);
}

int openRawSocket(char *interface)
{
	int socketDescriptor;
	// Submit request for a socket descriptor to look up interface.
	if ((socketDescriptor = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0)
	{
		perror("socket() failed to get socket descriptor for using ioctl() ");
		exit(EXIT_FAILURE);
	}

	return socketDescriptor;
}

uint8_t *getMacFromInterface(char *interface, int socketDescriptor)
{
	uint8_t *src_mac = malloc(sizeof(uint8_t) * 6);
	struct ifreq ifr;
	// Use ioctl() to look up interface name and get its MAC address.
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", interface);
	if (ioctl(socketDescriptor, SIOCGIFHWADDR, &ifr) < 0)
	{
		perror("ioctl() failed to get source MAC address ");
		return (EXIT_FAILURE);
	}

	// Copy source MAC address.
	memcpy(src_mac, ifr.ifr_hwaddr.sa_data, 6 * sizeof(uint8_t));

	return src_mac;
}

struct sockaddr_ll getInterfaceDevice(char *interface)
{
	struct sockaddr_ll device;
	// Find interface index from interface name and store index in
	// struct sockaddr_ll device, which will be used as an argument of sendto().
	memset(&device, 0, sizeof(device));
	if ((device.sll_ifindex = if_nametoindex(interface)) == 0)
	{
		perror("if_nametoindex() failed to obtain interface index ");
		exit(EXIT_FAILURE);
	}

	return device;
}

struct ip6_hdr getIPV6Header(char *src_ip, char *dst_ip)
{
	int status;
	struct ip6_hdr iphdr;

	iphdr.ip6_flow = htonl((6 << 28) | (0 << 20) | 0); // IPv6 version (4 bits), Traffic class (8 bits), Flow label (20 bits)
	iphdr.ip6_plen = htons(TCP_HDRLEN);				   // Payload length (16 bits): TCP header
	iphdr.ip6_nxt = IPPROTO_TCP;					   // Next header (8 bits): 6 for TCP
	iphdr.ip6_hops = 255;							   // Hop limit (8 bits): default to maximum value

	// Source IPv6 address (128 bits)
	if ((status = inet_pton(AF_INET6, src_ip, &(iphdr.ip6_src))) != 1)
	{
		fprintf(stderr, "inet_pton() failed.\nError message: %s", strerror(status));
		exit(EXIT_FAILURE);
	}

	// Destination IPv6 address (128 bits)
	if ((status = inet_pton(AF_INET6, dst_ip, &(iphdr.ip6_dst))) != 1)
	{
		fprintf(stderr, "inet_pton() failed.\nError message: %s", strerror(status));
		exit(EXIT_FAILURE);
	}

	return iphdr;
}

struct tcphdr getTCPHeader(struct ip6_hdr iphdr, uint16_t dst_port, uint8_t tcp_flag)
{
	struct tcphdr tcphdr;

	tcphdr.th_sport = htons(60);				  // Source port number (16 bits)
	tcphdr.th_dport = htons(dst_port);			  // Destination port number (16 bits)
	tcphdr.th_seq = htonl(0);					  // Sequence number (32 bits)
	tcphdr.th_ack = htonl(0);					  // Acknowledgement number (32 bits): 0 in first packet of SYN/ACK process
	tcphdr.th_x2 = 0;							  // Reserved(4 bits): should be 0
	tcphdr.th_off = TCP_HDRLEN / 4;				  // Data offset (4 bits): size of TCP header in 32-bit words
	tcphdr.th_flags = tcp_flag;					  // tcp flags: CWR, ECE, URG, ACK, PSH, RST, SYN, FIN
	tcphdr.th_win = htons(65535);				  // Window size (16 bits)
	tcphdr.th_urp = htons(0);					  // Urgent pointer (16 bits): 0 (only valid if URG flag is set)
	tcphdr.th_sum = tcp6_checksum(iphdr, tcphdr); // TCP checksum(16 bits)

	return tcphdr;
}

uint8_t *getEthernetFrame(char *src_mac, char *dst_mac, struct ip6_hdr iphdr, struct tcphdr tcphdr)
{
	uint8_t *ether_frame = malloc(sizeof(uint8_t) * 1518);

	// Destination and Source MAC addresse
	memcpy(ether_frame, dst_mac, 6 * sizeof(uint8_t));
	memcpy(ether_frame + 6, src_mac, 6 * sizeof(uint8_t));

	// Next is ethernet type code (ETH_P_IPV6 for IPv6).
	// http://www.iana.org/assignments/ethernet-numbers
	ether_frame[12] = ETH_P_IPV6 / 256;
	ether_frame[13] = ETH_P_IPV6 % 256;

	// Next is ethernet frame data (IPv6 header + TCP header).

	// IPv6 header
	memcpy(ether_frame + ETH_HDRLEN, &iphdr, IP6_HDRLEN * sizeof(uint8_t));

	// TCP header
	memcpy(ether_frame + ETH_HDRLEN + IP6_HDRLEN, &tcphdr, TCP_HDRLEN * sizeof(uint8_t));

	return ether_frame;
}

void sendEthernetFrame(uint8_t *ether_frame, int socketDescriptor, struct sockaddr_ll device)
{
	int bytes;
	// Send ethernet frame to socket.
	if ((bytes = sendto(socketDescriptor, ether_frame, FRAME_LENGTH, 0, (struct sockaddr *)&device, sizeof(device))) <= 0)
	{
		perror("sendto() failed");
		exit(EXIT_FAILURE);
	}

	// Close socket descriptor.
	close(socketDescriptor);
}

// Computing the internet checksum (RFC 1071).
// Note that the internet checksum does not preclude collisions.
uint16_t checksum(uint16_t *addr, int len)
{
	int count = len;
	register uint32_t sum = 0;
	uint16_t answer = 0;

	// Sum up 2-byte values until none or only one byte left.
	while (count > 1)
	{
		sum += *(addr++);
		count -= 2;
	}

	// Add left-over byte, if any.
	if (count > 0)
	{
		sum += *(uint8_t *)addr;
	}

	// Fold 32-bit sum into 16 bits; we lose information by doing this,
	// increasing the chances of a collision.
	// sum = (lower 16 bits) + (upper 16 bits shifted right 16 bits)
	while (sum >> 16)
	{
		sum = (sum & 0xffff) + (sum >> 16);
	}

	// Checksum is one's compliment of sum.
	answer = ~sum;

	return (answer);
}

// Build IPv6 TCP pseudo-header and call checksum function (Section 8.1 of RFC 2460).
uint16_t tcp6_checksum(struct ip6_hdr iphdr, struct tcphdr tcphdr)
{
	uint32_t lvalue;
	char buf[IP_MAXPACKET], cvalue;
	char *ptr;
	int chksumlen = 0;

	ptr = &buf[0]; // ptr points to beginning of buffer buf

	// Copy source IP address into buf (128 bits)
	memcpy(ptr, &iphdr.ip6_src, sizeof(iphdr.ip6_src));
	ptr += sizeof(iphdr.ip6_src);
	chksumlen += sizeof(iphdr.ip6_src);

	// Copy destination IP address into buf (128 bits)
	memcpy(ptr, &iphdr.ip6_dst, sizeof(iphdr.ip6_dst));
	ptr += sizeof(iphdr.ip6_dst);
	chksumlen += sizeof(iphdr.ip6_dst);

	// Copy TCP length to buf (32 bits)
	lvalue = htonl(sizeof(tcphdr));
	memcpy(ptr, &lvalue, sizeof(lvalue));
	ptr += sizeof(lvalue);
	chksumlen += sizeof(lvalue);

	// Copy zero field to buf (24 bits)
	*ptr = 0;
	ptr++;
	*ptr = 0;
	ptr++;
	*ptr = 0;
	ptr++;
	chksumlen += 3;

	// Copy next header field to buf (8 bits)
	memcpy(ptr, &iphdr.ip6_nxt, sizeof(iphdr.ip6_nxt));
	ptr += sizeof(iphdr.ip6_nxt);
	chksumlen += sizeof(iphdr.ip6_nxt);

	// Copy TCP source port to buf (16 bits)
	memcpy(ptr, &tcphdr.th_sport, sizeof(tcphdr.th_sport));
	ptr += sizeof(tcphdr.th_sport);
	chksumlen += sizeof(tcphdr.th_sport);

	// Copy TCP destination port to buf (16 bits)
	memcpy(ptr, &tcphdr.th_dport, sizeof(tcphdr.th_dport));
	ptr += sizeof(tcphdr.th_dport);
	chksumlen += sizeof(tcphdr.th_dport);

	// Copy sequence number to buf (32 bits)
	memcpy(ptr, &tcphdr.th_seq, sizeof(tcphdr.th_seq));
	ptr += sizeof(tcphdr.th_seq);
	chksumlen += sizeof(tcphdr.th_seq);

	// Copy acknowledgement number to buf (32 bits)
	memcpy(ptr, &tcphdr.th_ack, sizeof(tcphdr.th_ack));
	ptr += sizeof(tcphdr.th_ack);
	chksumlen += sizeof(tcphdr.th_ack);

	// Copy data offset to buf (4 bits) and
	// copy reserved bits to buf (4 bits)
	cvalue = (tcphdr.th_off << 4) + tcphdr.th_x2;
	memcpy(ptr, &cvalue, sizeof(cvalue));
	ptr += sizeof(cvalue);
	chksumlen += sizeof(cvalue);

	// Copy TCP flags to buf (8 bits)
	memcpy(ptr, &tcphdr.th_flags, sizeof(tcphdr.th_flags));
	ptr += sizeof(tcphdr.th_flags);
	chksumlen += sizeof(tcphdr.th_flags);

	// Copy TCP window size to buf (16 bits)
	memcpy(ptr, &tcphdr.th_win, sizeof(tcphdr.th_win));
	ptr += sizeof(tcphdr.th_win);
	chksumlen += sizeof(tcphdr.th_win);

	// Copy TCP checksum to buf (16 bits)
	// Zero, since we don't know it yet
	*ptr = 0;
	ptr++;
	*ptr = 0;
	ptr++;
	chksumlen += 2;

	// Copy urgent pointer to buf (16 bits)
	memcpy(ptr, &tcphdr.th_urp, sizeof(tcphdr.th_urp));
	ptr += sizeof(tcphdr.th_urp);
	chksumlen += sizeof(tcphdr.th_urp);

	return checksum((uint16_t *)buf, chksumlen);
}
