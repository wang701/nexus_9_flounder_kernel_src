/*
 * isobus.c - ISOBUS sockets for protocol family CAN
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/uio.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
/* #include "patched/can.h" */
#include <uapi/linux/can.h> 
#include <linux/can/core.h>
#include "isobus.h" /* #include <linux/can/isobus.h> */
#include <net/sock.h>
#include <net/net_namespace.h>

#define ISOBUS_VERSION CAN_VERSION
static __initconst const char banner[] =
	KERN_INFO "can: isobus protocol (rev " ISOBUS_VERSION ")\n";

MODULE_DESCRIPTION("PF_CAN isobus 11783 protocol");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Alex Layton <alex@layton.in>, "
		"Urs Thuermann <urs.thuermann@volkswagen.de>, "
		"Oliver Hartkopp <oliver.hartkopp@volkswagen.de>");
MODULE_ALIAS("can-proto-" __stringify(CAN_ISOBUS));
#ifdef BUILD_NUM
	MODULE_INFO(build, BUILD_NUM);
#endif

#define ISOBUS_MIN_SC_ADDR	128U
#define ISOBUS_MAX_SC_ADDR	247U

/* Macros for going between CAN IDs and PDU/PGN fields */
#define ISOBUS_PRI_POS	26
#define ISOBUS_PRI_MASK	0x07U
#define ISOBUS_PGN_POS	8
#define ISOBUS_PGN_MASK	0x03FFFFLU
#define ISOBUS_PGN1_MASK	0x03FF00LU
#define ISOBUS_PS_POS	8
#define ISOBUS_PS_MASK	0xFFU
#define ISOBUS_PF_POS	16
#define ISOBUS_PF_MASK	0xFFU
#define ISOBUS_SA_POS	0
#define ISOBUS_SA_MASK	0xFFU
#define ISOBUS_DP_POS	24
#define ISOBUS_DP_MASK	0x01U
#define ISOBUS_EDP_POS	25
#define ISOBUS_EDP_MASK	0x01U
#define CANID(pri, pgn, da, sa)	( \
		CAN_EFF_FLAG | \
		((pri & ISOBUS_PRI_MASK) << ISOBUS_PRI_POS) | \
		((pgn & ISOBUS_PGN_MASK) << ISOBUS_PGN_POS) | \
		((da & ISOBUS_PS_MASK) << ISOBUS_PS_POS) | \
		((sa & ISOBUS_SA_MASK) << ISOBUS_SA_POS) )
#define ID_FIELD(id, field)	\
	((id >> ISOBUS_ ## field ## _POS) & ISOBUS_ ## field ## _MASK)
#define PGN_FIELD(pgn, field)	ID_FIELD(pgn << ISOBUS_PGN_POS, field)
#define ISOBUS_MIN_PDU2	240
#define ID_PDU_FMT(id) (ID_FIELD(id, PF) < ISOBUS_MIN_PDU2 ? 1 : 2)
#define PGN_PDU_FMT(pgn)	ID_PDU_FMT(pgn << ISOBUS_PGN_POS)

/* Stuff for NAME fields */
#define ISOBUS_NAME_ID_MASK	0x00000000001FFFFFLU
#define ISOBUS_NAME_ID_POS	0
#define ISOBUS_NAME_MAN_MASK	0x00000000FFE00000LU
#define ISOBUS_NAME_MAN_POS	21
#define ISOBUS_NAME_ECU_MASK	0x0000000700000000LU
#define ISOBUS_NAME_ECU_POS	32
#define ISOBUS_NAME_FINST_MASK	0x000000F800000000LU
#define ISOBUS_NAME_FINST_POS	35
#define ISOBUS_NAME_FUNC_MASK	0x0000FF0000000000LU
#define ISOBUS_NAME_FUNC_POS	40
#define ISOBUS_NAME_CLASS_MASK	0x00FE000000000000LU
#define ISOBUS_NAME_CLASS_POS	49
#define ISOBUS_NAME_CINST_MASK	0x0F00000000000000LU
#define ISOBUS_NAME_CINST_POS	56
#define ISOBUS_NAME_IG_MASK	0x7000000000000000LU
#define ISOBUS_NAME_IG_POS	60

/* Timeouts etc. (100's on ns) */
#define ISOBUS_ADDR_CLAIM_TIMEOUT	2500L
#define ISOBUS_RTXD_MULTIPLIER	6L

/* Priority stuff */
#define MIN_PRI	0
#define MAX_PRI	7
#define ISOBUS_PRIO(p)	\
	(MAX_PRI - ((p < MIN_PRI ? MIN_PRI : p) > MAX_PRI ? MAX_PRI : p) + MIN_PRI)
#define SK_PRIO(p)	(MAX_PRI - p + MIN_PRI)

/*
 * A isobus socket has a list of can_filters attached to it, each receiving
 * the CAN frames matching that filter.  If the filter list is empty,
 * no CAN frames will be received by the socket.  The default after
 * opening the socket, is to have one filter which receives all frames.
 * The filter list is allocated dynamically with the exception of the
 * list containing only one item.  This common case is optimized by
 * storing the single filter in dfilter, to avoid using dynamic memory.
 */
struct isobus_sock {
	struct sock sk;
	bool bound;
	int ifindex;
	struct notifier_block notifier;
	int loopback;
	int recv_own_msgs;
	int daddr_opt;
	int count;                 /* number of active filters */
	struct can_filter dfilter; /* default/single filter */
	struct can_filter *filter; /* pointer to filter(s) */
	can_err_mask_t err_mask;

	__u8 pref_addr;
	__u8 s_addr;
	name_t name;
	enum {
		ISOBUS_IDLE = 0,
		ISOBUS_WAIT_ADDR,
		ISOBUS_WAIT_HAVE_ADDR,
		ISOBUS_HAVE_ADDR,
		ISOBUS_LOST_ADDR,
	} state;
	wait_queue_head_t wait;

	bool sc_addrs[ISOBUS_MAX_SC_ADDR - ISOBUS_MIN_SC_ADDR + 1];
	bool pref_avail;
};

/* Netowrk management messages */
static const struct isobus_mesg req_addr_claimed_mesg = {
	ISOBUS_PGN_REQUEST,
	3,
	{(ISOBUS_PGN_ADDR_CLAIMED >> 16) & 0xFF,
		(ISOBUS_PGN_ADDR_CLAIMED >> 8) & 0xFF,
		ISOBUS_PGN_ADDR_CLAIMED & 0xFF},
};
static const struct isobus_mesg addr_claimed_mesg = {
	ISOBUS_PGN_ADDR_CLAIMED,
	8,
	{0, 0, 0, 0, 0, 0, 0, 0},
};

/*
 * Return pointer to store the extra msg flags for isobus_recvmsg().
 * We use the space of one unsigned int beyond the 'struct sockaddr_can'
 * in skb->cb.
 */
static inline unsigned int *isobus_flags(struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(skb->cb) <= (sizeof(struct sockaddr_can) +
					 sizeof(unsigned int)));

	/* return pointer after struct sockaddr_can */
	return (unsigned int *)(&((struct sockaddr_can *)skb->cb)[1]);
}

static inline struct isobus_sock *isobus_sk(const struct sock *sk)
{
	return (struct isobus_sock *)sk;
}

/* Genereates a random transmit delay (in 100's of ns) */
static inline long isobus_rtxd(void)
{
	long l;

	l = 0;
	get_random_bytes(&l, 1);

	return l * ISOBUS_RTXD_MULTIPLIER;
}

/* Determine the PGN of a CAN frame */
static inline pgn_t get_pgn(canid_t id)
{
	pgn_t pgn;

	/* PDU1 format */
	if(ID_PDU_FMT(id) == 1) {
		pgn = (id >> ISOBUS_PGN_POS) & ISOBUS_PGN1_MASK;
	}
	/* PDU2 format */
	else {
		pgn = (id >> ISOBUS_PGN_POS) & ISOBUS_PGN_MASK;
	}

	return pgn;
}

/* Called when a CAN frame is received */
/* TODO: Add support for connections */
static void isobus_rcv(struct sk_buff *oskb, void *data)
{
	struct sock *sk = (struct sock *)data;
	struct isobus_sock *ro = isobus_sk(sk);
	struct sockaddr_can *addr;
	struct sk_buff *skb;
	unsigned int *pflags;

	struct can_frame *cf;
	struct isobus_mesg *mesg;

	/* check the received tx sock reference */
	if (!ro->recv_own_msgs && oskb->sk == sk) {
		return;
	}

	/* set pointer to received CAN frame */
	cf = (struct can_frame *) oskb->data;

	/* do not pass frames with DLC > 8 */
	if (unlikely(cf->can_dlc > CAN_MAX_DLEN)) {
		return;
	}

	/* Check for invalid PGNs */
	if (unlikely(ID_FIELD(cf->can_id, EDP))) {
		if (likely(ID_FIELD(cf->can_id, DP))) {
			/* 
			 * Check for ISO 15765-3 PGNs which can coexist with ISO 11783 PGNs
			 * but have a different format for the CAN identifier.
			 * TODO: Tell SocketCAN to filter these frames out for this module.
			 */
			printk(KERN_NOTICE "can_isobus: ISO 15765-3 PGN encountered\n");
		} else {
			/* 
			 * Check for ISO 11783 reserved PGNs which do not yet have a
			 * defined stucture, so nothing can be done with them yet.
			 * TODO: Tell SocketCAN to filter these frames out for this module.
			 */
			printk(KERN_NOTICE "can_isobus: ISO 11783 reserved PGN encountered\n");
		}
		return;
	}

	/* Create skb to put ISOBUS message in */
	skb = alloc_skb(sizeof(*mesg), gfp_any());
	if (!skb) {
		return;
	}
	skb->tstamp = oskb->tstamp;
	skb->dev = oskb->dev;

	/* Copy ISOBUS message into the skb */
	mesg = (struct isobus_mesg *)skb_put(skb, sizeof(struct isobus_mesg));
	if(!mesg) {
		return;
	}
	mesg->pgn = get_pgn(cf->can_id);
	mesg->dlen = cf->can_dlc;
	memcpy(mesg->data, cf->data, mesg->dlen);

	/*
	 *  Put the datagram to the queue so that isobus_recvmsg() can
	 *  get it from there.  We need to pass the interface index to
	 *  isobus_recvmsg().  We pass a whole struct sockaddr_can in skb->cb
	 *  containing the interface index.
	 */

	BUILD_BUG_ON(sizeof(skb->cb) < (2 * sizeof(struct sockaddr_can)));
	addr = (struct sockaddr_can *)skb->cb;
	memset(addr, 0, 2 * sizeof(*addr));
	addr[0].can_family  = AF_CAN;
	addr[0].can_ifindex = skb->dev->ifindex;
	addr[0].can_addr.isobus.addr = ID_FIELD(cf->can_id, SA);
	addr[1].can_family  = AF_CAN;
	addr[1].can_ifindex = skb->dev->ifindex;
	addr[1].can_addr.isobus.addr = ID_FIELD(cf->can_id, PS);

	/* add CAN specific message flags for isobus_recvmsg() */
	pflags = isobus_flags(skb);
	*pflags = 0;
	if (oskb->sk)
		*pflags |= MSG_DONTROUTE;
	if (oskb->sk == sk)
		*pflags |= MSG_CONFIRM;

	if (sock_queue_rcv_skb(sk, skb) < 0)
		kfree_skb(skb);
}

/* Called when userland sends */
/* TODO: Implement sending more than 8 bytes */
static int isobus_sendmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size)
{
	struct sock *sk = sock->sk;
	struct isobus_sock *ro = isobus_sk(sk);
	struct sk_buff *skb;
	struct net_device *dev;
	int ifindex;
	int err;
	struct isobus_mesg *mesg;
	struct sockaddr_can *addr;
	struct can_frame *cf;
	__u8 da;

	/* Check for being kicked off the bus */
	if(ro->state != ISOBUS_HAVE_ADDR)
		return -EADDRINUSE;

	/* Find pointer to ISOBUS message to be sent */
	mesg = (struct isobus_mesg *)msg->msg_iov->iov_base;

	/*
	 * Get interface to send on and address to send to.
	 * 
	 * If the socket was bound to a particular interface use that one,
	 * otherwise check for one passed in the message name.
	 *
	 * Get directed address if one was passed in.
	 */
	ifindex = ro->ifindex;
	da = 0;
	addr = (struct sockaddr_can *)msg->msg_name;
	if(addr) {
		/* Only PDU 1 format should have a DA */
		if(PGN_PDU_FMT(mesg->pgn) == 1) {
			/* TODO: Resolve address from NAME */
			da = addr->can_addr.isobus.addr;
		}

		if (!ro->ifindex) {
			if (msg->msg_namelen < sizeof(*addr)) {
				printk(KERN_ERR "can_isobus: address wrong size\n");
				return -EINVAL;
			}

			if (addr->can_family != AF_CAN) {
				printk(KERN_ERR "can_isobus: address not CAN address family\n");
				return -EINVAL;
			}

			ifindex = addr->can_ifindex;
		}
	} else if(PGN_PDU_FMT(mesg->pgn) == 1) {
		/* PDU 1 format needs a DA */
		printk(KERN_ERR "can_isobus: no address given for PDU 1 PGN\n");
		return -EINVAL;
	}

	if (unlikely(size != CAN_MTU))
		return -EINVAL;

	dev = dev_get_by_index(&init_net, ifindex);
	if (!dev)
		return -ENXIO;

	/* Allocate an skb which will hold a can frame */
	skb = sock_alloc_send_skb(sk, sizeof(*cf), msg->msg_flags & MSG_DONTWAIT,
				  &err);
	if (!skb)
		goto put_dev;

	/* Place CAN frame in skbuff */
	cf = (struct can_frame *)skb_put(skb, sizeof(struct can_frame));
	if(!cf) {
		goto free_skb;
	}
	/* Fill out CAN frame with ISOBUS message */
	cf->can_id = CANID(ISOBUS_PRIO(sk->sk_priority), mesg->pgn, da, ro->s_addr);
	memcpy(cf->data, mesg->data, cf->can_dlc = mesg->dlen);

	sock_tx_timestamp(sk, &skb_shinfo(skb)->tx_flags);

	skb->dev = dev;
	skb->sk  = sk;

	err = can_send(skb, ro->loopback);

	dev_put(dev);
	if (err)
		goto send_failed;

	return size;

free_skb:
	kfree_skb(skb);
put_dev:
	dev_put(dev);
send_failed:
	return err;
}

/* 
 * Send an ISOBUS message (for use within this module)
 */
static int isobus_send(struct isobus_sock *ro, struct isobus_mesg *mesg,
		__u8 addr)
{
	int err;
	struct net_device *dev;
	struct sk_buff *skb;
	struct can_frame *cf;

	dev = dev_get_by_index(&init_net, ro->ifindex);
	if (!dev)
		return -ENXIO;

	skb = alloc_skb(sizeof(struct can_frame), gfp_any());
	if(!skb) {
		goto put_dev;
	}

	skb->dev = dev_get_by_index(&init_net, ro->ifindex);
	if(!skb->dev) {
		goto free_skb;
	}

	skb->sk = &ro->sk;

	cf = (struct can_frame *)skb_put(skb, sizeof(*cf));
	if(!cf) {
		goto free_skb;
	}

	/* Fill out CAN frame with ISOBUS message */
	cf->can_id = CANID(ISOBUS_PRIO(ro->sk.sk_priority), mesg->pgn,
			addr, ro->s_addr);
	memcpy(cf->data, mesg->data, cf->can_dlc = mesg->dlen);

	err = can_send(skb, 1);

	dev_put(dev);

	return err;

free_skb:
	kfree_skb(skb);
put_dev:
	dev_put(dev);
	return -1;
}

/* TODO: Check if this is really portable */
#define DATA2NAME(data)	le64_to_cpup((uint64_t *) data)
#define NAME2DATA(name)	cpu_to_le64((uint64_t) name)

static inline int isobus_send_addr_claimed(struct isobus_sock *ro)
{
	struct isobus_mesg mesg;
	int ret;

	mesg = addr_claimed_mesg;
	*(uint64_t *)mesg.data = NAME2DATA(ro->name);
	ret = isobus_send(ro, &mesg, ISOBUS_GLOBAL_ADDR);

	if(ro->s_addr == ISOBUS_NULL_ADDR)
		printk(KERN_DEBUG "can_isobus:%p cannot claim address sent\n", ro);
	else
		printk(KERN_DEBUG "can_isobus:%p address claimed sent\n", ro);

	return ret;
}

static inline void isobus_lose_addr(struct isobus_sock *ro)
{
	ro->bound = false;
	ro->s_addr = ISOBUS_NULL_ADDR;
	ro->state = ISOBUS_LOST_ADDR;

	isobus_send_addr_claimed(ro);

	wake_up_interruptible(&ro->wait);
}

/* Function for network management to process address claimed messages */
static void isobus_addr_claimed_handler(struct sk_buff *skb, void *data)
{
	struct sock *sk = (struct sock *)data;
	struct isobus_sock *ro = isobus_sk(sk);
	struct can_frame *cf;
	__u8 sa;

	/* check the received tx sock reference */
	if (skb->sk == sk) {
		return;
	}

	printk(KERN_DEBUG "can_isobus:%p address claimed seen\n", ro);

	/* set pointer to received CAN frame */
	cf = (struct can_frame *) skb->data;

	/* Get source address of message */
	sa = ID_FIELD(cf->can_id, SA);

	/* No action for cannot claim address messages */
	if(sa == ISOBUS_NULL_ADDR)
		return;

	if(ro->state == ISOBUS_WAIT_ADDR) {
		/* Record occupied addresses in the self-configurable range */
		if(sa <= ISOBUS_MAX_SC_ADDR && sa >= ISOBUS_MIN_SC_ADDR) {
			ro->sc_addrs[sa - ISOBUS_MIN_SC_ADDR] = false;
		}

		/* Determine whether or not preferred address is available */
		if(sa == ro->pref_addr) {
			if(ro->name < DATA2NAME(cf->data)) {
				ro->state = ISOBUS_WAIT_HAVE_ADDR;
				wake_up_interruptible(&ro->wait);
			} else {
				ro->pref_avail = false;
				if(!(ro->name & ISOBUS_NAME_SC_BIT))
					isobus_lose_addr(ro);
			}
		}
	} else {
		/* Determine if address must be given up */
		if(sa == ro->s_addr) {
			if(ro->name <= DATA2NAME(cf->data))
				isobus_send_addr_claimed(ro);
			else
				isobus_lose_addr(ro);
		}
	}
}

/* 
 * Function for network management to process request for address claimed
 * messages
 */
static void isobus_req_addr_claimed_handler(struct sk_buff *skb, void *data)
{
	struct sock *sk = (struct sock *)data;
	struct isobus_sock *ro = isobus_sk(sk);
	struct can_frame *cf;

	/* check the received tx sock reference */
	if (ro->state == ISOBUS_WAIT_ADDR && skb->sk == sk) {
		return;
	}

	/* set pointer to received CAN frame */
	cf = (struct can_frame *) skb->data;

	/* Discard request for things besides address claimed */
	if(cf->can_dlc != 3 || cf->data[0] != req_addr_claimed_mesg.data[0] ||
			cf->data[1] != req_addr_claimed_mesg.data[1] ||
			cf->data[2] != req_addr_claimed_mesg.data[2]) {
		return;
	}

	/* Check if claimed address is mine */
	/* TODO: Should this check be done with filters? */
	if(ID_FIELD(cf->can_id, PS) == ro->s_addr ||
			ID_FIELD(cf->can_id, PS) == ISOBUS_GLOBAL_ADDR) {
		printk(KERN_DEBUG "can_isobus:%p request for address claimed seen\n",
				ro);
		isobus_send_addr_claimed(ro);
	}
}

static int isobus_enable_filters(struct net_device *dev, struct sock *sk,
			    struct can_filter *filter, int count)
{
	int err = 0;
	int i;

	for (i = 0; i < count; i++) {
		err = can_rx_register(dev, filter[i].can_id,
				      filter[i].can_mask,
				      isobus_rcv, sk, "isobus");
		if (err) {
			/* clean up successfully registered filters */
			while (--i >= 0)
				can_rx_unregister(dev, filter[i].can_id,
						  filter[i].can_mask,
						  isobus_rcv, sk);
			break;
		}
	}

	return err;
}

static int isobus_enable_errfilter(struct net_device *dev, struct sock *sk,
				can_err_mask_t err_mask)
{
	int err = 0;

	if (err_mask)
		err = can_rx_register(dev, 0, err_mask | CAN_ERR_FLAG,
				      isobus_rcv, sk, "isobus");

	return err;
}

/* Register filters for network management PGNs */
static int isobus_enable_nmfilters(struct net_device *dev, struct sock *sk)
{
	int err;

	err = can_rx_register(dev,
			CANID(0, ISOBUS_PGN_ADDR_CLAIMED, ISOBUS_GLOBAL_ADDR, 0),
			CANID(0, ISOBUS_PGN1_MASK, ISOBUS_PS_MASK, 0),
			isobus_addr_claimed_handler, sk, "isobus-nm");
	if(err) {
		return err;
	}

	err = can_rx_register(dev,
			CANID(0, ISOBUS_PGN_REQUEST, 0, 0),
			CANID(0, ISOBUS_PGN1_MASK, 0, 0),
			isobus_req_addr_claimed_handler, sk, "isobus-nm");
	if(err) {
		can_rx_unregister(dev,
				CANID(0, ISOBUS_PGN_ADDR_CLAIMED, ISOBUS_GLOBAL_ADDR, 0),
				CANID(0, ISOBUS_PGN1_MASK, ISOBUS_PS_MASK, 0),
				isobus_addr_claimed_handler, sk);
	}

	return err;
}

static void isobus_disable_filters(struct net_device *dev, struct sock *sk,
			      struct can_filter *filter, int count)
{
	int i;

	for (i = 0; i < count; i++)
		can_rx_unregister(dev, filter[i].can_id, filter[i].can_mask,
				  isobus_rcv, sk);
}

static inline void isobus_disable_errfilter(struct net_device *dev,
					 struct sock *sk, can_err_mask_t err_mask)

{
	if (err_mask)
		can_rx_unregister(dev, 0, err_mask | CAN_ERR_FLAG,
				  isobus_rcv, sk);
}

/* Unregister a filter for network management PGNs */
static inline void isobus_disable_nmfilters(struct net_device *dev,
		struct sock *sk)
{
	can_rx_unregister(dev,
			CANID(0, ISOBUS_PGN_ADDR_CLAIMED, ISOBUS_GLOBAL_ADDR, 0),
			CANID(0, ISOBUS_PGN1_MASK, ISOBUS_PS_MASK, 0),
			isobus_addr_claimed_handler, sk);
	can_rx_unregister(dev, 
			CANID(0, ISOBUS_PGN_REQUEST, 0, 0),
			CANID(0, ISOBUS_PGN1_MASK, 0, 0),
			isobus_req_addr_claimed_handler, sk);
}

static inline void isobus_disable_allfilters(struct net_device *dev,
					  struct sock *sk)
{
	struct isobus_sock *ro = isobus_sk(sk);

	isobus_disable_filters(dev, sk, ro->filter, ro->count);
	isobus_disable_nmfilters(dev, sk);
	isobus_disable_errfilter(dev, sk, ro->err_mask);
}

static int isobus_enable_allfilters(struct net_device *dev, struct sock *sk)
{
	struct isobus_sock *ro = isobus_sk(sk);
	int err;

	err = isobus_enable_filters(dev, sk, ro->filter, ro->count);
	if(!err) {
		err = isobus_enable_nmfilters(dev, sk);
		if (!err) {
			err = isobus_enable_errfilter(dev, sk, ro->err_mask);
			if (err)
				isobus_disable_filters(dev, sk, ro->filter, ro->count);
		}
	}

	return err;
}

static int isobus_notifier(struct notifier_block *nb,
			unsigned long msg, void *data)
{
	struct net_device *dev = (struct net_device *)data;
	struct isobus_sock *ro = container_of(nb, struct isobus_sock, notifier);
	struct sock *sk = &ro->sk;

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;

	if (dev->type != ARPHRD_CAN)
		return NOTIFY_DONE;

	if (ro->ifindex != dev->ifindex)
		return NOTIFY_DONE;

	switch (msg) {

	case NETDEV_UNREGISTER:
		lock_sock(sk);
		/* remove current filters & unregister */
		if (ro->bound)
			isobus_disable_allfilters(dev, sk);

		if (ro->count > 1)
			kfree(ro->filter);

		ro->ifindex = 0;
		ro->bound   = false;
		ro->count   = 0;
		release_sock(sk);

		sk->sk_err = ENODEV;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
		break;

	case NETDEV_DOWN:
		sk->sk_err = ENETDOWN;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
		break;
	}

	return NOTIFY_DONE;
}

static int isobus_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct isobus_sock *ro;

	if (!sk)
		return 0;

	ro = isobus_sk(sk);

	unregister_netdevice_notifier(&ro->notifier);

	lock_sock(sk);

	/* remove current filters & unregister */
	if (ro->bound) {
		if (ro->ifindex) {
			struct net_device *dev;

			dev = dev_get_by_index(&init_net, ro->ifindex);
			if (dev) {
				isobus_disable_allfilters(dev, sk);
				dev_put(dev);
			}
		} else
			isobus_disable_allfilters(NULL, sk);
	}

	if (ro->count > 1)
		kfree(ro->filter);

	ro->ifindex = 0;
	ro->bound   = false;
	ro->count   = 0;

	sock_orphan(sk);
	sock->sk = NULL;

	release_sock(sk);
	sock_put(sk);

	return 0;
}

static int isobus_getname(struct socket *sock, struct sockaddr *uaddr,
		       int *len, int peer)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct isobus_sock *ro = isobus_sk(sk);

	if (peer)
		return -EOPNOTSUPP;

	memset(addr, 0, sizeof(*addr));
	addr->can_family  = AF_CAN;
	addr->can_ifindex = ro->ifindex;

	*len = sizeof(*addr);

	return 0;
}

/* TODO: Things matching multiple filters will be "received" multiple times */
static inline int isobus_filter_conv(struct isobus_filter *fi,
		struct can_filter *f, int count) {
	int i;
	pgn_t pgn_mask;

	for(i = 0; i < count; i++) {
		pgn_mask = fi[i].pgn_mask;

		if(PGN_PDU_FMT(fi[i].pgn) == 2) {
			if(fi[i].daddr_mask) {
				/* PDU2 format PGNs with a DA are invalid */
				return -EINVAL;
			}
		} else {
			pgn_mask &= ISOBUS_PGN1_MASK;
		}

		f[i].can_id = CANID(0, fi[i].pgn, fi[i].daddr, fi[i].saddr);
		f[i].can_mask = CANID(0, pgn_mask, fi[i].daddr_mask, fi[i].saddr_mask);

		if(fi[i].inverted) {
			f[i].can_id |= CAN_INV_FILTER;
		}

		printk(KERN_DEBUG "can_isobus: %x&%x %x&%x %x&%x | %x&%x\n",
				fi[i].pgn, fi[i].pgn_mask,
				fi[i].daddr, fi[i].daddr_mask,
				fi[i].saddr, fi[i].saddr_mask,
				f[i].can_id, f[i].can_mask);
	}

	return 0;
}

static inline int isobus_filter_unconv(struct can_filter *f,
		struct isobus_filter *fi, int count)
{
	int i;

	for(i = 0; i < count; i++) {
		fi[i].pgn = get_pgn(f[i].can_id);
		fi[i].pgn_mask = get_pgn(f[i].can_mask);

		fi[i].daddr = ID_FIELD(f[i].can_id, PS);
		fi[i].daddr_mask = ID_FIELD(f[i].can_mask, PS);

		fi[i].saddr = ID_FIELD(f[i].can_id, SA);
		fi[i].saddr_mask = ID_FIELD(f[i].can_mask, SA);

		fi[i].inverted = f[i].can_id & CAN_INV_FILTER;
	}

	return 0;
}

static int isobus_setsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct isobus_sock *ro = isobus_sk(sk);
	struct can_filter *filter = NULL;  /* dyn. alloc'ed filters */
	struct can_filter sfilter;         /* single filter */
	struct isobus_filter *ifilter;
	struct isobus_filter sifilter;
	struct net_device *dev = NULL;
	int count = 0;
	int err = 0;
	int tmp;

	if (level != SOL_CAN_ISOBUS)
		return -EINVAL;

	switch (optname) {

	case CAN_ISOBUS_FILTER:
		if (optlen % sizeof(struct isobus_filter) != 0)
			return -EINVAL;

		count = optlen / sizeof(struct isobus_filter);

		if (count > 1) {
			/* filter does not fit into dfilter => alloc space */
			ifilter = memdup_user(optval, optlen);
			if (IS_ERR(ifilter))
				return PTR_ERR(ifilter);

			/* Interpret ISOBUS filters */
			filter = kmalloc(count * sizeof(*filter), GFP_KERNEL);
			err = isobus_filter_conv(ifilter, filter, count);
			kfree(ifilter);
		} else if (count == 1) {
			if (copy_from_user(&sifilter, optval, sizeof(sifilter)))
				return -EFAULT;

			/* Interpret ISOBUS filter */
			err = isobus_filter_conv(&sifilter, &sfilter, 1);
		}

		if(err) {
			return err;
		}

		lock_sock(sk);

		if (ro->bound && ro->ifindex)
			dev = dev_get_by_index(&init_net, ro->ifindex);

		if (ro->bound) {
			/* (try to) register the new filters */
			if (count == 1)
				err = isobus_enable_filters(dev, sk, &sfilter, 1);
			else
				err = isobus_enable_filters(dev, sk, filter,
							 count);
			if (err) {
				if (count > 1)
					kfree(filter);
				goto out_fil;
			}

			/* remove old filter registrations */
			isobus_disable_filters(dev, sk, ro->filter, ro->count);
		}

		/* remove old filter space */
		if (ro->count > 1)
			kfree(ro->filter);

		/* link new filters to the socket */
		if (count == 1) {
			/* copy filter data for single filter */
			ro->dfilter = sfilter;
			filter = &ro->dfilter;
		}
		ro->filter = filter;
		ro->count  = count;

 out_fil:
		if (dev)
			dev_put(dev);
		release_sock(sk);
		break;

	case CAN_ISOBUS_LOOPBACK:
		if (optlen != sizeof(ro->loopback))
			return -EINVAL;
		if (copy_from_user(&ro->loopback, optval, optlen))
			return -EFAULT;
		break;

	case CAN_ISOBUS_RECV_OWN_MSGS:
		if (optlen != sizeof(ro->recv_own_msgs))
			return -EINVAL;
		if (copy_from_user(&ro->recv_own_msgs, optval, optlen))
			return -EFAULT;
		break;

	case CAN_ISOBUS_SEND_PRIO:
		if (optlen != sizeof(tmp))
			return -EINVAL;

		if (copy_from_user(&tmp, optval, optlen))
			return -EFAULT;

		if ((tmp < MIN_PRI) || (tmp > MAX_PRI))
			return -EDOM;

		lock_sock(sk);
		sk->sk_priority = SK_PRIO(tmp);
		release_sock(sk);
		break;

	case CAN_ISOBUS_DADDR:
		if (optlen != sizeof(ro->daddr_opt))
			return -EINVAL;
		if (copy_from_user(&ro->daddr_opt, optval, optlen))
			return -EFAULT;
		break;

	case CAN_ISOBUS_NAME:
		if (optlen != sizeof(ro->name))
			return -EINVAL;
		if (copy_from_user(&ro->name, optval, optlen))
			return -EFAULT;
		break;

	default:
		return -ENOPROTOOPT;
	}
	return err;
}

static int isobus_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct isobus_sock *ro = isobus_sk(sk);
	int len;
	void *val;
	int err = 0;
	int tmp;

	val = &tmp;

	if (level != SOL_CAN_ISOBUS)
		return -EINVAL;
	if (get_user(len, optlen))
		return -EFAULT;
	if (len < 0)
		return -EINVAL;

	switch (optname) {

	case CAN_ISOBUS_FILTER:
		lock_sock(sk);
		if (ro->count > 0) {
			int fsize = ro->count * sizeof(struct isobus_filter);
			struct isobus_filter *fi = kmalloc(fsize, GFP_KERNEL);
			isobus_filter_unconv(ro->filter, fi, ro->count);

			if (len > fsize)
				len = fsize;
			if (copy_to_user(optval, fi, len))
				err = -EFAULT;
			kfree(fi);
		} else
			len = 0;
		release_sock(sk);

		if (!err)
			err = put_user(len, optlen);
		return err;

	case CAN_ISOBUS_LOOPBACK:
		if (len > sizeof(ro->loopback))
			len = sizeof(ro->loopback);
		val = &ro->loopback;
		break;

	case CAN_ISOBUS_RECV_OWN_MSGS:
		if (len > sizeof(ro->recv_own_msgs))
			len = sizeof(ro->recv_own_msgs);
		val = &ro->recv_own_msgs;
		break;

	case CAN_ISOBUS_SEND_PRIO:
		if (len > sizeof(sk->sk_priority))
			len = sizeof(sk->sk_priority);
		tmp = ISOBUS_PRIO(sk->sk_priority);
		break;

	case CAN_ISOBUS_DADDR:
		if (len > sizeof(ro->daddr_opt))
			len = sizeof(ro->daddr_opt);
		val = &ro->daddr_opt;
		break;

	case CAN_ISOBUS_NAME:
		if (len > sizeof(ro->name))
			len = sizeof(ro->name);
		val = &ro->name;
		break;

	default:
		return -ENOPROTOOPT;
	}

	if (put_user(len, optlen))
		return -EFAULT;
	if (copy_to_user(optval, val, len))
		return -EFAULT;
	return 0;
}

/* Called when userland reads from socket */
static int isobus_recvmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct isobus_sock *ro = isobus_sk(sk);
	struct sockaddr_can *addr;
	struct sk_buff *skb;
	int err = 0;
	int noblock;

	noblock =  flags & MSG_DONTWAIT;
	flags   &= ~MSG_DONTWAIT;

	/* Check for being kicked off the bus */
	if(ro->state != ISOBUS_HAVE_ADDR)
		return -EADDRINUSE;

	skb = skb_recv_datagram(sk, flags, noblock, &err);
	if (!skb) {
		return err;
	}
	addr = (struct sockaddr_can *)skb->cb;

	if (size < CAN_MTU)
		msg->msg_flags |= MSG_TRUNC;
	else
		size = CAN_MTU;

	err = memcpy_toiovec(msg->msg_iov, skb->data, size);
	if (err < 0) {
		skb_free_datagram(sk, skb);
		return err;
	}

	sock_recv_ts_and_drops(msg, sk, skb);

	/* Create ancillary header with the source CAN address */
	put_cmsg(msg, SOL_CAN_ISOBUS, CAN_ISOBUS_DADDR,
			sizeof(struct sockaddr_can), &addr[1]);
 
	if (msg->msg_name) {
		msg->msg_namelen = sizeof(struct sockaddr_can);
		memcpy(msg->msg_name, &addr[0], msg->msg_namelen);
	}

	/* assign the flags that have been recorded in isobus_rcv() */
	msg->msg_flags |= *(isobus_flags(skb));

	skb_free_datagram(sk, skb);

	return size;
}

static inline __u8 avail_sc_addr(struct isobus_sock *ro)
{
	int i;

	for(i = 0; i < ISOBUS_MAX_SC_ADDR - ISOBUS_MIN_SC_ADDR + 1; i++) {
		if(ro->sc_addrs[i]) {
			return i + ISOBUS_MIN_SC_ADDR;
		}
	}

	return ISOBUS_NULL_ADDR;
}

static inline int isobus_claim_addr(struct isobus_sock *ro)
{
	long wait;
	
	ro->s_addr = ISOBUS_NULL_ADDR;
	ro->state = ISOBUS_WAIT_ADDR;
	memset(ro->sc_addrs, -1, sizeof(ro->sc_addrs));
	ro->pref_avail = true;
	/* Send request for address claimed message */
	isobus_send(ro, (struct isobus_mesg *)&req_addr_claimed_mesg,
			ISOBUS_GLOBAL_ADDR);
	printk(KERN_DEBUG "can_isobus:%p request for address claimed sent\n", ro);

	/* Wait until we have tried to claim an address */
	wait = (ISOBUS_ADDR_CLAIM_TIMEOUT + isobus_rtxd()) * HZ / 10000;
	printk(KERN_DEBUG "can_isobus:%p waiting %ld jiffies (%d / sec)\n", ro,
			wait, HZ);
	wait_event_interruptible_timeout(ro->wait, ro->state != ISOBUS_WAIT_ADDR,
			wait);

	if(ro->state == ISOBUS_LOST_ADDR)
		return -EADDRINUSE;

	/* See if there was an address available */
	if(ro->pref_addr != ISOBUS_ANY_ADDR && ro->pref_avail)
		ro->s_addr = ro->pref_addr;
	else if(ro->name & ISOBUS_NAME_SC_BIT)
		ro->s_addr = avail_sc_addr(ro);

	if(ro->s_addr == ISOBUS_NULL_ADDR) {
		isobus_lose_addr(ro);
		return -EADDRINUSE;
	}

	/* Send address claimed message */
	ro->state = ISOBUS_WAIT_HAVE_ADDR;
	isobus_send_addr_claimed(ro);

	/* Set timer to give ECUs time to respond with address contentions */
	wait = ISOBUS_ADDR_CLAIM_TIMEOUT * HZ / 10000;
	printk(KERN_DEBUG "can_isobus:%p waiting %ld jiffies (%d / sec)\n", ro,
			wait, HZ);
	wait_event_interruptible_timeout(ro->wait,
			ro->state != ISOBUS_WAIT_HAVE_ADDR, wait);

	/* Check if we still have an address */
	if(ro->state == ISOBUS_LOST_ADDR) {
		return -EADDRINUSE;
	}

	ro->state = ISOBUS_HAVE_ADDR;
	printk(KERN_DEBUG "can_isobus:%p ready to use address\n", ro);

	return 0;
}

static int isobus_bind(struct socket *sock, struct sockaddr *uaddr, int len)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct isobus_sock *ro = isobus_sk(sk);
	int ifindex;
	int err = 0;
	int notify_enetdown = 0;

	if (len < sizeof(*addr))
		return -EINVAL;

	lock_sock(sk);

	if (ro->bound && addr->can_ifindex == ro->ifindex)
		goto out;

	if (addr->can_ifindex) {
		struct net_device *dev;

		dev = dev_get_by_index(&init_net, addr->can_ifindex);
		if (!dev) {
			err = -ENODEV;
			goto out;
		}
		if (dev->type != ARPHRD_CAN) {
			dev_put(dev);
			err = -ENODEV;
			goto out;
		}
		if (!(dev->flags & IFF_UP))
			notify_enetdown = 1;

		ifindex = dev->ifindex;

		/* filters set by default/setsockopt */
		err = isobus_enable_allfilters(dev, sk);
		dev_put(dev);
	} else {
		/* ISOBUS needs an interface */
		err = -ENODEV;
		goto out;
	}

	if (!err) {
		if (ro->bound) {
			/* unregister old filters */
			if (ro->ifindex) {
				struct net_device *dev;

				dev = dev_get_by_index(&init_net, ro->ifindex);
				if (dev) {
					isobus_disable_allfilters(dev, sk);
					dev_put(dev);
				}
			} else {
				isobus_disable_allfilters(NULL, sk);
			}
		}
		ro->ifindex = ifindex;
		ro->bound = true;
	}

 out:
	release_sock(sk);

	if(!err) {
		ro->pref_addr = addr->can_addr.isobus.addr;
		err = isobus_claim_addr(ro);
	}

	if (notify_enetdown) {
		sk->sk_err = ENETDOWN;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
	}

	return err;
}

static int isobus_init(struct sock *sk)
{
	struct isobus_sock *ro = isobus_sk(sk);

	ro->bound            = false;
	ro->ifindex          = 0;

	/*
	 * Set default filter to single entry dfilter
	 * ISOBUS only uses extended frame format
	 */
	ro->dfilter.can_id   = CAN_EFF_FLAG;
	ro->dfilter.can_mask = CAN_EFF_FLAG;
	ro->filter           = &ro->dfilter;
	ro->count            = 1;

	/* Set default loopback behaviour */
	ro->loopback         = true;
	ro->recv_own_msgs    = false;

	/* Set default address */
	ro->pref_addr = ISOBUS_ANY_ADDR;
	ro->s_addr = ISOBUS_NULL_ADDR;

	/* Generate NAME with random identity/instance numbers */
	get_random_bytes(&ro->name, sizeof(ro->name));
	ro->name &= ISOBUS_NAME_CINST_MASK | ISOBUS_NAME_FINST_MASK |
			ISOBUS_NAME_ID_MASK;
	/* 
	 * Default manufacturer to all 1's
	 * TODO: Find a better way to handle this?
	 */
	ro->name |= ISOBUS_NAME_MAN_MASK;
	/* Default function to data logger */
	ro->name |= (130ULL << ISOBUS_NAME_FUNC_POS) & ISOBUS_NAME_FUNC_MASK;
	/* Default to self-configurable address */
	ro->name |= ISOBUS_NAME_SC_BIT;

	/* Set default priority */
	sk->sk_priority = SK_PRIO(6);

	/* Set default ancillary options */
	ro->daddr_opt = false;

	/* Set default state */
	ro->state = ISOBUS_IDLE;
	init_waitqueue_head(&ro->wait);

	/* set notifier */
	ro->notifier.notifier_call = isobus_notifier;

	register_netdevice_notifier(&ro->notifier);

	return 0;
}

static const struct proto_ops isobus_ops = {
	.family        = PF_CAN,
	.release       = isobus_release,
	.bind          = isobus_bind,
	.connect       = sock_no_connect,
	.socketpair    = sock_no_socketpair,
	.accept        = sock_no_accept,
	.getname       = isobus_getname,
	.poll          = datagram_poll,
	.ioctl         = can_ioctl,	/* use can_ioctl() from af_can.c */
	.listen        = sock_no_listen,
	.shutdown      = sock_no_shutdown,
	.setsockopt    = isobus_setsockopt,
	.getsockopt    = isobus_getsockopt,
	.sendmsg       = isobus_sendmsg,
	.recvmsg       = isobus_recvmsg,
	.mmap          = sock_no_mmap,
	.sendpage      = sock_no_sendpage,
};

static struct proto isobus_proto __read_mostly = {
	.name       = "ISOBUS",
	.owner      = THIS_MODULE,
	.obj_size   = sizeof(struct isobus_sock),
	.init       = isobus_init,
};

static const struct can_proto isobus_can_proto = {
	.type       = SOCK_DGRAM,
	.protocol   = CAN_ISOBUS,
	.ops        = &isobus_ops,
	.prot       = &isobus_proto,
};

static __init int isobus_module_init(void)
{
	int err;

	printk(banner);

	err = can_proto_register(&isobus_can_proto);
	if (err < 0)
		printk(KERN_ERR "can: registration of isobus protocol failed\n");

	return err;
}

static __exit void isobus_module_exit(void)
{
	can_proto_unregister(&isobus_can_proto);
}

module_init(isobus_module_init);
module_exit(isobus_module_exit);

