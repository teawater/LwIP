/* Copyright (c) 2015, IBM
 * Author(s): Dan Williams <djwillia@us.ibm.com>
 *            Ricardo Koller <kollerr@us.ibm.com>
 * Copyright (c) 2017, RWTH Aachen University
 * Author(s): Tim van de Kamp <tim.van.de.kamp@rwth-aachen.de>
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* We used several existing projects as guides
 * kvmtest.c: http://lwn.net/Articles/658512/
 * lkvm: http://github.com/clearlinux/kvmtool
 */

/*
 * 15.1.2017: extend original version (https://github.com/Solo5/solo5)
 *            for HermitCore
 */


#include <hermit/stddef.h>
#include <hermit/stdio.h>
#include <hermit/errno.h>
#include <lwip/sys.h>
#include <sys/poll.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/snmp.h>
#include <lwip/sockets.h>
#include <lwip/err.h>
#include <lwip/stats.h>
#include <lwip/ethip6.h>
#include <lwip/netifapi.h>
#include <netif/etharp.h>

#include "uhyve-net.h"
#include <arch_io.h>

#define UHYVE_IRQ	11

static int8_t uhyve_net_init_ok = 0;
static struct netif* mynetif = NULL;

static int uhyve_net_write_sync(uint8_t *data, int n)
{
	volatile uhyve_netwrite_t uhyve_netwrite;
	uhyve_netwrite.data = (uint8_t*)virt_to_phys((size_t)data);
	uhyve_netwrite.len = n;
	uhyve_netwrite.ret = 0;

	outportl(UHYVE_PORT_NETWRITE, (unsigned)virt_to_phys((size_t)&uhyve_netwrite));

	return uhyve_netwrite.ret;
}

static int uhyve_net_stat(void)
{
        volatile uhyve_netstat_t uhyve_netstat;

        outportl(UHYVE_PORT_NETSTAT, (unsigned)virt_to_phys((size_t)&uhyve_netstat));

        return uhyve_netstat.status;
}

static int uhyve_net_read_sync(uint8_t *data, int *n)
{
	volatile uhyve_netread_t uhyve_netread;

	uhyve_netread.data = (uint8_t*)virt_to_phys((size_t)data);
	uhyve_netread.len = *n;
	uhyve_netread.ret = 0;

	outportl(UHYVE_PORT_NETREAD, (unsigned)virt_to_phys((size_t)&uhyve_netread));
	*n = uhyve_netread.len;

	return uhyve_netread.ret;
}

static char mac_str[18];
static char *hermit_net_mac_str(void)
{
	volatile uhyve_netinfo_t uhyve_netinfo;

	outportl(UHYVE_PORT_NETINFO, (unsigned)virt_to_phys((size_t)&uhyve_netinfo));
	memcpy(mac_str, (void *)&uhyve_netinfo.mac_str, 18);

	return mac_str;
}

static inline uint8_t dehex(char c)
{
        if (c >= '0' && c <= '9')
                return (c - '0');
        else if (c >= 'a' && c <= 'f')
                return 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F')
                return 10 + (c - 'A');
        else
                return 0;
}

//---------------------------- OUTPUT --------------------------------------------

static err_t uhyve_netif_output(struct netif* netif, struct pbuf* p)
{
	uhyve_netif_t* uhyve_netif = netif->state;
	uint32_t i;
	struct pbuf *q;

	if(BUILTIN_EXPECT(p->tot_len > 1792, 0)) {
		kprintf("uhyve_netif_output: packet (%i bytes) is longer than 1792 bytes\n", p->tot_len);
		return ERR_IF;
	}

#if ETH_PAD_SIZE
	pbuf_header(p, -ETH_PAD_SIZE); /*drop padding word */
#endif

	/*
	 * q traverses through linked list of pbuf's
	 * This list MUST consist of a single packet ONLY
	 */
	for (q = p, i = 0; q != 0; q = q->next) {
		// send the packet
 		uhyve_net_write_sync(q->payload, q->len);
		i += q->len;
	}

#if ETH_PAD_SIZE
	pbuf_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

	LINK_STATS_INC(link.xmit);

	return ERR_OK;
}

static void consume_packet(void* ctx)
{
	struct pbuf *p = (struct pbuf*) ctx;

	mynetif->input(p, mynetif);
}

//------------------------------- POLLING ----------------------------------------

void uhyve_netif_poll(void)
{
	if (!uhyve_net_init_ok)
		return;

	uhyve_netif_t* uhyve_netif = mynetif->state;
	int len = RX_BUF_LEN;
	struct pbuf *p = NULL;
	struct pbuf *q;

	while (uhyve_net_read_sync(uhyve_netif->rx_buf, &len) == 0)
	{
#if ETH_PAD_SIZE
		len += ETH_PAD_SIZE; /*allow room for Ethernet padding */
#endif
		p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
		if(p) {
#if ETH_PAD_SIZE
			pbuf_header(p, -ETH_PAD_SIZE); /*drop the padding word */
#endif
			uint8_t pos = 0;
			for (q=p; q!=NULL; q=q->next) {
				memcpy((uint8_t*) q->payload, uhyve_netif->rx_buf + pos, q->len);
				pos += q->len;
			}
#if ETH_PAD_SIZE
			pbuf_header(p, ETH_PAD_SIZE); /*reclaim the padding word */
#endif


			//forward packet to the IP thread
			if (tcpip_callback_with_block(consume_packet, p, 0) == ERR_OK) {
				LINK_STATS_INC(link.recv);
			} else {
				LINK_STATS_INC(link.drop);
				pbuf_free(p);
			}
		} else {
			kprintf("uhyve_netif_poll: not enough memory!\n");
			LINK_STATS_INC(link.memerr);
			LINK_STATS_INC(link.drop);
		}
	}

	eoi();
}

#if defined(__x86_64__)
void uhyve_irqhandler(void);

__asm__(".global uhyve_irqhandler\n"
        "uhyve_irqhandler:\n\t"
        "cld\n\t"	/* Set direction flag forward for C functions */
	"push %rax\n\t"
        "push %rcx\n\t"
	"push %rdx\n\t"
	"push %rsi\n\t"
	"push %rdi\n\t"
	"push %r8\n\t"
	"push %r9\n\t"
	"push %r10\n\t"
	"push %r11\n\t"
        "call uhyve_netif_poll\n\t"
        "pop %r11\n\t"
	"pop %r10\n\t"
	"pop %r9\n\t"
	"pop %r8\n\t"
	"pop %rdi\n\t"
	"pop %rsi\n\t"
	"pop %rdx\n\t"
	"pop %rcx\n\t"
	"pop %rax\n\t"
        "iretq");
#elif defined(__aarch64__)
void uhyve_irqhandler(void)
{
	kprintf("TODO: Implement uhyve_irqhandler for AArch64\n");
}
#else
#error Invalid architecture
#endif

//--------------------------------- INIT -----------------------------------------

static uhyve_netif_t static_uhyve_netif;
static uhyve_netif_t* uhyve_netif = NULL;

static err_t uhyve_netif_init (struct netif* netif)
{
	uint8_t tmp8 = 0;

	LWIP_ASSERT("netif != NULL", (netif != NULL));

	kprintf("uhyve_netif_init: Found uhyve_net interface\n");

#if 0
	uhyve_netif_t* uhyve_netif = kmalloc(sizeof(uhyve_netif_t));
	if (!uhyve_netif) {
		kprintf("uhyve_netif_init: out of memory\n");
		return ERR_MEM;
	}

	memset(uhyve_netif, 0x00, sizeof(uhyve_netif_t));
#else
	LWIP_ASSERT("uhyve_netif == NULL", (uhyve_netif == NULL));

	// currently we support only one device => use a static variable uhyve_netif
	uhyve_netif = &static_uhyve_netif;
#endif

#if 0
	uhyve_netif->rx_buf = page_alloc(RX_BUF_LEN + 16 /* header size */, VMA_READ|VMA_WRITE);
	if (!(uhyve_netif->rx_buf)) {
		kprintf("uhyve_netif_init: out of memory\n");
		kfree(uhyve_netif);
		return ERR_MEM;
	}
	memset(uhyve_netif->rx_buf, 0x00, RX_BUF_LEN + 16);
#endif

	netif->state = uhyve_netif;
	mynetif = netif;

	netif->hwaddr_len = ETHARP_HWADDR_LEN;

	LWIP_DEBUGF(NETIF_DEBUG, ("uhyve_netif_init: MAC address "));
	char *hermit_mac = hermit_net_mac_str();
	for (tmp8=0; tmp8 < ETHARP_HWADDR_LEN; tmp8++) {
		netif->hwaddr[tmp8] = dehex(*hermit_mac++) << 4;
		netif->hwaddr[tmp8] |= dehex(*hermit_mac++);
		hermit_mac++;
		LWIP_DEBUGF(NETIF_DEBUG, ("%02x ", (uint32_t) netif->hwaddr[tmp8]));
	}
	LWIP_DEBUGF(NETIF_DEBUG, ("\n"));
	uhyve_netif->ethaddr = (struct eth_addr *)netif->hwaddr;

	kprintf("uhye_netif uses irq %d\n", UHYVE_IRQ);
	irq_install_handler(UHYVE_IRQ, uhyve_irqhandler);

	/*
	 * Initialize the snmp variables and counters inside the struct netif.
	 * The last argument should be replaced with your link speed, in units
	 * of bits per second.
	 */
	NETIF_INIT_SNMP(netif, snmp_ifType_ethernet_csmacd, 1000);

	netif->name[0] = 'e';
	netif->name[1] = 'n';
	netif->num = 0;
	/* downward functions */
	netif->output = etharp_output;
	netif->linkoutput = uhyve_netif_output;
	/* maximum transfer unit */
	netif->mtu = 32768;
	/* broadcast capability */
	netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP | NETIF_FLAG_LINK_UP | NETIF_FLAG_MLD6;

#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
	netif_create_ip6_linklocal_address(netif, 1);
	netif->ip6_autoconfig_enabled = 1;
#endif

	kprintf("uhyve_netif_init: OK\n");
	uhyve_net_init_ok = 1;

	/* check if we already receive an interrupt */
	uhyve_netif_poll();

	return ERR_OK;
}

typedef struct kernel_header {
	uint32_t magic_number;
	uint32_t version;
	uint64_t base;
	uint64_t limit;
	uint64_t image_size;
	uint64_t current_stack_address;
	uint64_t current_percore_address;
	uint64_t host_logical_addr;
	uint64_t boot_gtod;
	uint64_t mb_info;
	uint64_t cmdline;
	uint64_t cmdsize;
	uint32_t cpu_freq;
	uint32_t boot_processor;
	uint32_t cpu_online;
	uint32_t possible_cpus;
	uint32_t current_boot_id;
	uint16_t uartport;
	uint8_t single_kernel;
	uint8_t uhyve;
	uint8_t hcip[4];
	uint8_t hcgateway[4];
	uint8_t hcmask[4];
} kernel_header_t;

static struct netif default_netif;
extern kernel_start;

int init_uhyve_netif(void)
{
	ip_addr_t	ipaddr;
	ip_addr_t	netmask;
	ip_addr_t	gw;
	kernel_header_t* kheader = (kernel_header_t*) &kernel_start;

	if (uhyve_net_stat()) {
		/* Set network address variables */
		IP_ADDR4(&gw, kheader->hcgateway[0], kheader->hcgateway[1], kheader->hcgateway[2], kheader->hcgateway[3]);
		IP_ADDR4(&ipaddr, kheader->hcip[0], kheader->hcip[1], kheader->hcip[2], kheader->hcip[3]);
		IP_ADDR4(&netmask, kheader->hcmask[0], kheader->hcmask[1], kheader->hcmask[2], kheader->hcmask[3]);

		if ((netifapi_netif_add(&default_netif, ip_2_ip4(&ipaddr), ip_2_ip4(&netmask), ip_2_ip4(&gw), NULL, uhyve_netif_init, ethernet_input)) != ERR_OK) {
			kprintf("Unable to add the uhyve_net network interface\n");
			return -ENODEV;
		}

		/* tell lwip all initialization is done and we want to set it up */
		netifapi_netif_set_default(&default_netif);
		netifapi_netif_set_up(&default_netif);
	} else {
		return -ENODEV;
	}

	return ERR_OK;
}
