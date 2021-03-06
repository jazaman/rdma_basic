/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2005 Hewlett Packard, Inc (Grant Grundler)
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <byteswap.h>
#include <time.h>

#include <infiniband/verbs.h>

#include "get_clock.h"

#define PINGPONG_SEND_WRID  1
#define PINGPONG_RECV_WRID  2
#define RC 0
#define UC 1
#define UD 3
#define VERSION 1.1
#define SIGNAL 1
#define MAX_INLINE 400
#define MCG_LID 0xc001
#define MCG_GID {255,1,0,0,0,2,201,133,0,0,0,0,0,0,0,0}
static int sl = 0;
static int page_size;
cycles_t                *tstamp;
struct user_parameters {
	const char              *servername;
	int connection_type;
	int mtu;
	int signal_comp;
	int all; /* run all msg size */
	int iters;
	int tx_depth;
	int use_event;
	int inline_size;
	int use_mcg;
	int qp_timeout;
	int gid_index; /* if value not negative, we use gid AND gid_index=value */
};

struct report_options {
	int unsorted;
	int histogram;
	int cycles;   /* report delta's in cycles, not microsec's */
};


struct pingpong_context {
	struct ibv_sge list;
	struct ibv_sge recv_list;
	struct ibv_send_wr wr;
	struct ibv_recv_wr rwr;
	struct ibv_context *context;
    struct ibv_comp_channel *channel;
	struct ibv_pd      *pd;
	struct ibv_mr      *mr;
	struct ibv_cq      *scq;
	struct ibv_cq      *rcq;
	struct ibv_qp      *qp;
	struct ibv_ah		*ah;
	void               *buf;
	volatile char      *post_buf;
	volatile char      *poll_buf;
	int                 size;
	int                 tx_depth;
	union ibv_gid       dgid;
};

struct pingpong_dest {
	unsigned long long vaddr;
	int lid;
	int qpn;
	int psn;
	unsigned int rkey;
	union ibv_gid       dgid;
};


static uint16_t pp_get_local_lid(struct pingpong_context *ctx, int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(ctx->context, port, &attr))
		return 0;

	return attr.lid;
}

static struct ibv_device *pp_find_dev(const char *ib_devname) {
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;

	dev_list = ibv_get_device_list(NULL);

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev)
			fprintf(stderr, "No IB devices found\n");
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev)
			fprintf(stderr, "IB device %s not found\n", ib_devname);
	}
	return ib_dev;
}

#define KEY_MSG_SIZE (sizeof "0000:000000:000000:00000000:0000000000000000")
#define KEY_PRINT_FMT "%04x:%06x:%06x:%08x:%016Lx"
#define KEY_MSG_SIZE_GID (sizeof "0000:000000:000000:00000000:0000000000000000:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00")
#define KEY_PRINT_FMT_GID "%04x:%06x:%06x:%08x:%016Lx:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x"

static int pp_write_keys(int sockfd, const struct pingpong_dest *my_dest, struct user_parameters *user_parm)
{
	if (user_parm->gid_index < 0) {
		char msg[KEY_MSG_SIZE];

		sprintf(msg, KEY_PRINT_FMT, my_dest->lid, my_dest->qpn,
			my_dest->psn, my_dest->rkey, my_dest->vaddr);

		if (write(sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
		}

		return 0;
	} else {
	char msg[KEY_MSG_SIZE_GID];

		sprintf(msg, KEY_PRINT_FMT_GID, my_dest->lid, my_dest->qpn,
			my_dest->psn, my_dest->rkey, my_dest->vaddr,
			my_dest->dgid.raw[0], my_dest->dgid.raw[1], my_dest->dgid.raw[2], my_dest->dgid.raw[3],
			my_dest->dgid.raw[4], my_dest->dgid.raw[5], my_dest->dgid.raw[6], my_dest->dgid.raw[7],
			my_dest->dgid.raw[8], my_dest->dgid.raw[9], my_dest->dgid.raw[10], my_dest->dgid.raw[11],
			my_dest->dgid.raw[12], my_dest->dgid.raw[13], my_dest->dgid.raw[14], my_dest->dgid.raw[15]);

	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client write");
		fprintf(stderr, "Couldn't send local address\n");
		return -1;
	}

	return 0;
	}
}

static int pp_read_keys(int sockfd, const struct pingpong_dest *my_dest,
			struct pingpong_dest *rem_dest, struct user_parameters *user_parm)
{
	if (user_parm->gid_index < 0) {
		int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
		}

		parsed = sscanf(msg, KEY_PRINT_FMT, &rem_dest->lid, &rem_dest->qpn,
				&rem_dest->psn, &rem_dest->rkey, &rem_dest->vaddr);

		if (parsed != 5) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",
				(int)sizeof msg, msg);
			return -1;
		}

		return 0;
	} else {
		char msg[KEY_MSG_SIZE_GID];
		if (read(sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
		}
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->lid = (int)strtol(tmp, NULL, 16); // LID

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->qpn = (int)strtol(tmp, NULL, 16); // QPN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->psn = (int)strtol(tmp, NULL, 16); // PSN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->rkey = (unsigned)strtol(tmp, NULL, 16); // RKEY

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->vaddr = strtoull(tmp, NULL, 16); // VA

		for (i = 0; i < 15; ++i) {
			pstr += term - pstr + 1;
			term = strpbrk(pstr, ":");
			memcpy(tmp, pstr, term - pstr);
			tmp[term - pstr] = 0;
			rem_dest->dgid.raw[i] = (unsigned char)strtoll(tmp, NULL, 16);
			}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		rem_dest->dgid.raw[15] = (unsigned char)strtoll(tmp, NULL, 16);
		return 0;
	}
}

static int pp_client_connect(const char *servername, int port)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = 
	{
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int n;
	int sockfd = -1;

	if (asprintf(&service, "%d", port) < 0)
		return -1;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		return n;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return sockfd;
	}
	return sockfd;
}

static int pp_client_exch_dest(int sockfd, const struct pingpong_dest *my_dest,
			       struct pingpong_dest *rem_dest, struct user_parameters *user_parm)
{
	if (pp_write_keys(sockfd, my_dest, user_parm))
		return -1;

	return pp_read_keys(sockfd, my_dest, rem_dest, user_parm);
}

static int pp_server_connect(int port)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int sockfd = -1, connfd;
	int n;

	if (asprintf(&service, "%d", port) < 0)
		return -1;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		return n;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return sockfd;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	if (connfd < 0) {
		perror("server accept");
		fprintf(stderr, "accept() failed\n");
		close(sockfd);
		return connfd;
	}

	close(sockfd);
	return connfd;
}

static int pp_server_exch_dest(int sockfd, const struct pingpong_dest *my_dest,
			       struct pingpong_dest* rem_dest, struct user_parameters *user_parm)
{

	if (pp_read_keys(sockfd, my_dest, rem_dest, user_parm))
		return -1;

	return pp_write_keys(sockfd, my_dest, user_parm);
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int tx_depth, int port,struct user_parameters *user_parm) {
	struct pingpong_context *ctx;
	struct ibv_device_attr device_attr;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->tx_depth = tx_depth;
	/* in case of UD need space for the GRH */
	if (user_parm->connection_type==UD) {
		ctx->buf = memalign(page_size, ( size + 40 ) * 2);
		if (!ctx->buf) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return NULL;
		}
		memset(ctx->buf, 0, ( size + 40 ) * 2);
	} else {
		ctx->buf = memalign(page_size, size * 2);
		if (!ctx->buf) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			return NULL;
		}
		memset(ctx->buf, 0, size * 2);
	}

	ctx->post_buf = (char*)ctx->buf + (size - 1);
	ctx->poll_buf = (char*)ctx->buf + (2 * size - 1);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		return NULL;
	}
	if (user_parm->mtu == 0) {/*user did not ask for specific mtu */
		if (ibv_query_device(ctx->context, &device_attr)) {
			fprintf(stderr, "Failed to query device props");
			return NULL;
		}
		if (device_attr.vendor_part_id == 23108 || user_parm->gid_index > -1) {
			user_parm->mtu = 1024;
		} else {
			user_parm->mtu = 2048;
		}
	}
    if (user_parm->use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return NULL;
		}
	} else
		ctx->channel = NULL;
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}
	if (user_parm->connection_type==UD) {
		ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, (size + 40 ) * 2,
				     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
		if (!ctx->mr) {
			fprintf(stderr, "Couldn't allocate MR\n");
			return NULL;
		}
	} else {
		ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size * 2,
				     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
		if (!ctx->mr) {
			fprintf(stderr, "Couldn't allocate MR\n");
			return NULL;
		}
	}

	ctx->scq = ibv_create_cq(ctx->context, tx_depth, NULL, ctx->channel, 0);
	if (!ctx->scq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}
	ctx->rcq = ibv_create_cq(ctx->context, tx_depth, NULL, ctx->channel, 0);
	if (!ctx->rcq) {
		fprintf(stderr, "Couldn't create Recieve CQ\n");
		return NULL;
	}
	{
		struct ibv_qp_init_attr attr;
		memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
		attr.send_cq = ctx->scq;
		attr.recv_cq = ctx->rcq;
		attr.cap.max_send_wr  = tx_depth;
		/* Work around:  driver doesnt support
		 * recv_wr = 0 */
		attr.cap.max_recv_wr  = tx_depth;
		attr.cap.max_send_sge = 1;
		attr.cap.max_recv_sge = 1;
		attr.cap.max_inline_data = user_parm->inline_size;
		switch (user_parm->connection_type) {
		case RC :
			attr.qp_type = IBV_QPT_RC;
			break;
		case UC :
			attr.qp_type = IBV_QPT_UC;
			break;
		case UD :
			attr.qp_type = IBV_QPT_UD;
			break;
		default:
			fprintf(stderr, "Unknown connection type %d \n",user_parm->connection_type);
			return NULL;
		}
		attr.sq_sig_all = 0;
		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp) {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr;
		memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
		attr.qp_state        = IBV_QPS_INIT;
		attr.pkey_index      = 0;
		attr.port_num        = port;
		if (user_parm->connection_type==UD) {
			attr.qkey            = 0x11111111;
		} else {
			attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
		}

		if (user_parm->connection_type==UD) {
			if (ibv_modify_qp(ctx->qp, &attr,
					  IBV_QP_STATE              |
					  IBV_QP_PKEY_INDEX         |
					  IBV_QP_PORT               |
					  IBV_QP_QKEY)) {
				fprintf(stderr, "Failed to modify UD QP to INIT\n");
				return NULL;
			}

			if (user_parm->use_mcg) {
				union ibv_gid gid;
				uint8_t mcg_gid[16] = MCG_GID;

				/* use the local QP number as part of the mcg */
				mcg_gid[11] = (user_parm->servername) ? 0 : 1;
				*(uint32_t *)(&mcg_gid[12]) = ctx->qp->qp_num;
				memcpy(gid.raw, mcg_gid, 16);

				if (ibv_attach_mcast(ctx->qp, &gid, MCG_LID)) {
					fprintf(stderr, "Couldn't attach QP to mcg\n");
					return NULL;
				}
			}
		} else if (ibv_modify_qp(ctx->qp, &attr,
					 IBV_QP_STATE              |
					 IBV_QP_PKEY_INDEX         |
					 IBV_QP_PORT               |
					 IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}
	//send                        
	ctx->wr.wr_id      = PINGPONG_SEND_WRID;
	ctx->wr.sg_list    = &ctx->list;
	ctx->wr.num_sge    = 1;
	ctx->wr.opcode     = IBV_WR_SEND;
	ctx->wr.next       = NULL;
	//recieve
	ctx->rwr.wr_id      = PINGPONG_RECV_WRID;
	ctx->rwr.sg_list    = &ctx->recv_list;
	ctx->rwr.num_sge    = 1;
	ctx->rwr.next       = NULL;
	return ctx;
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  struct pingpong_dest *dest,struct user_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(struct ibv_qp_attr));
	attr.qp_state                 = IBV_QPS_RTR;
	if (user_parm->connection_type != UD) {
		switch (user_parm->mtu) {
		case 256 : 
			attr.path_mtu               = IBV_MTU_256;
			break;
		case 512 :
			attr.path_mtu               = IBV_MTU_512;
			break;
		case 1024 :
			attr.path_mtu               = IBV_MTU_1024;
			break;
		case 2048 :
			attr.path_mtu               = IBV_MTU_2048;
			break;
		case 4096 :
			attr.path_mtu               = IBV_MTU_4096;
			break;
		}
		printf("Mtu : %d\n", user_parm->mtu);
		attr.dest_qp_num              = dest->qpn;
		attr.rq_psn           = dest->psn;
	}
	if (user_parm->connection_type==RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}

	if (user_parm->gid_index < 0) {
		attr.ah_attr.is_global      = 0;
		attr.ah_attr.dlid           = dest->lid;
		attr.ah_attr.sl             = sl;
	} else {
		attr.ah_attr.is_global      = 1;
		attr.ah_attr.grh.dgid       = dest->dgid;
		attr.ah_attr.grh.hop_limit  = 1;
		attr.ah_attr.sl             = 0;
	}
	attr.ah_attr.src_path_bits  = 0;
	attr.ah_attr.port_num       = port;
	if ((user_parm->connection_type==UD) && (user_parm->use_mcg)) {
		uint8_t mcg_gid[16] = MCG_GID;

		/* send the message to the mcg of the other side */
		mcg_gid[11] = (user_parm->servername) ? 1 : 0;
		*(uint32_t *)(&mcg_gid[12]) = dest->qpn;

		attr.ah_attr.dlid       = MCG_LID;
		attr.ah_attr.is_global  = 1;
		attr.ah_attr.grh.sgid_index = 0;
		memcpy(attr.ah_attr.grh.dgid.raw, mcg_gid, 16);
	}

	if (user_parm->connection_type==RC) {
		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN             |
				  IBV_QP_MIN_RNR_TIMER      |
				  IBV_QP_MAX_DEST_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify RC QP to RTR\n");
			return 1;
		}
		attr.timeout            = user_parm->qp_timeout;
		attr.retry_cnt          = 7;
		attr.rnr_retry          = 7;
	} else if (user_parm->connection_type==UC) {
		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTR\n");
			return 1;
		}

	} else {
		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE )) {
			fprintf(stderr, "Failed to modify UC QP to RTR\n");
			return 1;
		}

	}
	attr.qp_state             = IBV_QPS_RTS;
	attr.sq_psn       = my_psn;
	if (user_parm->connection_type==RC) {
		attr.max_rd_atomic  = 1;
		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN             |
				  IBV_QP_TIMEOUT            |
				  IBV_QP_RETRY_CNT          |
				  IBV_QP_RNR_RETRY          |
				  IBV_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify RC QP to RTS\n");
			return 1;
		}
	} else { /*both UC and UD */
		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_SQ_PSN)) {
			fprintf(stderr, "Failed to modify UC/UD  QP to RTS\n");
			return 1;
		}

	}
	if (user_parm->connection_type==UD) {
		ctx->ah = ibv_create_ah(ctx->pd, &attr.ah_attr);
		if (!ctx->ah) {
			fprintf(stderr, "Failed to create AH for UD\n");
			return 1;
		}
	}
	/* post recieve max msg size*/
	{
		int i;
		struct ibv_recv_wr      *bad_wr_recv;

		ctx->recv_list.addr = (uintptr_t) ctx->buf;
		if (user_parm->connection_type==UD) {
			ctx->recv_list.length = ctx->size + 40;
		} else {
			ctx->recv_list.length = ctx->size;
		}
		ctx->recv_list.lkey = ctx->mr->lkey;
		for (i = 0; i < user_parm->tx_depth / 2; ++i) {
			if (ibv_post_recv(ctx->qp, &ctx->rwr, &bad_wr_recv)) {
				fprintf(stderr, "Couldn't post recv: counter=%d\n",
					i);
				return 14;
			}               
		}
	}
	return 0;
}

static int pp_open_port(struct pingpong_context *ctx, const char * servername,
			int ib_port, int port, struct pingpong_dest *rem_dest,struct user_parameters *user_parm)
{
	char addr_fmt[] = "%8s address: LID %#04x QPN %#06x PSN %#06x\n";
	struct pingpong_dest  my_dest;
	int                   sockfd;
	int                   rc;
	union ibv_gid         gid;


	/* Create connection between client and server.
	 * We do it by exchanging data over a TCP socket connection. */

	my_dest.lid = pp_get_local_lid(ctx, ib_port);
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;
	if (user_parm->gid_index < 0) {/*We do not fail test upon lid in RDMA0E/Eth conf*/
		if (!my_dest.lid) {
			fprintf(stderr, "Local lid 0x0 detected. Is an SM running?\n");
			return -1;
		}
	}
	if (user_parm->gid_index != -1) {
		int err=0;
		err = ibv_query_gid (ctx->context, ib_port, user_parm->gid_index, &gid);
		if (err) {
			return -1;
		}
		ctx->dgid=gid;
	}
	my_dest.dgid = gid;
	my_dest.rkey = ctx->mr->rkey;
	my_dest.vaddr = (uintptr_t)ctx->buf + ctx->size;

	printf(addr_fmt, "local", my_dest.lid, my_dest.qpn, my_dest.psn);
	if (user_parm->gid_index > -1) {
		printf("                  GID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		my_dest.dgid.raw[0],my_dest.dgid.raw[1],
		my_dest.dgid.raw[2], my_dest.dgid.raw[3], my_dest.dgid.raw[4],
		my_dest.dgid.raw[5], my_dest.dgid.raw[6], my_dest.dgid.raw[7],
		my_dest.dgid.raw[8], my_dest.dgid.raw[9], my_dest.dgid.raw[10],
		my_dest.dgid.raw[11], my_dest.dgid.raw[12], my_dest.dgid.raw[13],
		my_dest.dgid.raw[14], my_dest.dgid.raw[15]);
	}

	sockfd = servername ? pp_client_connect(servername, port) :
		pp_server_connect(port);

	if (sockfd < 0) {
		printf("pp_connect_sock(%s,%d) failed (%d)!\n",
		       servername, port, sockfd);
		return sockfd;
	}

	rc = servername ? pp_client_exch_dest(sockfd, &my_dest, rem_dest, user_parm) :
		pp_server_exch_dest(sockfd, &my_dest, rem_dest, user_parm);
	if (rc)
		return rc;

	printf(addr_fmt, "remote", rem_dest->lid, rem_dest->qpn, rem_dest->psn,
	       rem_dest->rkey, rem_dest->vaddr);
	if (user_parm->gid_index > -1) {
		printf("                  GID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		rem_dest->dgid.raw[0],rem_dest->dgid.raw[1],
		rem_dest->dgid.raw[2], rem_dest->dgid.raw[3], rem_dest->dgid.raw[4],
		rem_dest->dgid.raw[5], rem_dest->dgid.raw[6], rem_dest->dgid.raw[7],
		rem_dest->dgid.raw[8], rem_dest->dgid.raw[9], rem_dest->dgid.raw[10],
		rem_dest->dgid.raw[11], rem_dest->dgid.raw[12], rem_dest->dgid.raw[13],
		rem_dest->dgid.raw[14], rem_dest->dgid.raw[15]);
	}

	if ((rc = pp_connect_ctx(ctx, ib_port, my_dest.psn, rem_dest,user_parm)))
		return rc;

	/* An additional handshake is required *after* moving qp to RTR.
	 * Arbitrarily reuse exch_dest for this purpose.
	 */

	rc = servername ? pp_client_exch_dest(sockfd, &my_dest, rem_dest, user_parm) :
		pp_server_exch_dest(sockfd, &my_dest, rem_dest, user_parm);

	if (rc)
		return rc;

	if (write(sockfd, "done", sizeof "done") != sizeof "done"){
		perror("write");
		fprintf(stderr, "Couldn't write to socket\n");
		return 1;
	}

	close(sockfd);
	return 0;
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>            listen on/connect to port <port> (default 18515)\n");
	printf("  -c, --connection=<RC/UC/UD>  connection type RC/UC/UD (default RC)\n");
	printf("  -m, --mtu=<mtu>             mtu size (256 - 4096. default for hermon is 2048)\n");
	printf("  -d, --ib-dev=<dev>           use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>         use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>            size of message to exchange (default 1)\n");
	printf("  -t, --tx-depth=<dep>         size of tx queue (default 50)\n");
	printf("  -l, --signal                 signal completion on each msg\n");
	printf("  -a, --all                    Run sizes from 2 till 2^23\n");
	printf("  -n, --iters=<iters>          number of exchanges (at least 2, default 1000)\n");
	printf("  -I, --inline_size=<size>     max size of message to be sent in inline mode (default 400)\n");
	printf("  -u, --qp-timeout=<timeout> QP timeout, timeout value is 4 usec * 2 ^(timeout), default 14\n");
	printf("  -S, --sl=<sl>                SL (default 0)\n");
	printf("  -x, --gid-index=<index>   test uses GID with GID index taken from command line (for RDMAoE index should be 0)\n");
	printf("  -C, --report-cycles          report times in cpu cycle units (default microseconds)\n");
	printf("  -H, --report-histogram       print out all results (default print summary only)\n");
	printf("  -U, --report-unsorted        (implies -H) print out unsorted results (default sorted)\n");
	printf("  -V, --version                display version number\n");
	printf("  -e, --events                 sleep on CQ events (default poll)\n");
	printf("  -g, --mcg                    send messages to multicast group(only available in UD connection\n");
	printf("  -F, --CPU-freq               do not fail even if cpufreq_ondemand module is loaded\n");
}

/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 *	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if ((n - 1) % 2)
		return(delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}

static int cycles_compare(const void * aptr, const void * bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;
	return 0;

}

static void print_report(struct report_options * options,
			 unsigned int iters, cycles_t *tstamp,int size, int no_cpu_freq_fail)
{
	double cycles_to_units;
	cycles_t median;
	unsigned int i;
	const char* units;
	cycles_t *delta = malloc((iters - 1) * sizeof *delta);

	if (!delta) {
		perror("malloc");
		return;
	}

	for (i = 0; i < iters - 1; ++i)
		delta[i] = tstamp[i + 1] - tstamp[i];


	if (options->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
		cycles_to_units = get_cpu_mhz(no_cpu_freq_fail);
		units = "usec";
	}

	if (options->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	qsort(delta, iters - 1, sizeof *delta, cycles_compare);

	if (options->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / 2);
	}

	median = get_median(iters - 1, delta);
	printf("%7d        %d        %7.2f        %7.2f          %7.2f\n",
	       size,iters,delta[0] / cycles_to_units / 2,
	       delta[iters - 2] / cycles_to_units / 2,median / cycles_to_units / 2);
	free(delta);
}

int run_iter(struct pingpong_context *ctx, struct user_parameters *user_param,
	     struct pingpong_dest *rem_dest, int size)
{
	struct ibv_qp           *qp;
	struct ibv_send_wr      *wr;
	struct ibv_recv_wr       rwr;
	struct ibv_recv_wr      *bad_wr_recv;
	volatile char           *poll_buf; 
	volatile char           *post_buf;

	int                      scnt, rcnt, ccnt, poll;
	int                      iters;
	int                      tx_depth;
	iters = user_param->iters;
	tx_depth = user_param->tx_depth;


	if (user_param->connection_type==UD) {
		if (size > 2048) {
			if (user_param->gid_index < 0) {
				size = 2048;
			} else {
				size = 1024;
			}
		}
	}

	///send //
	wr = &ctx->wr;
	if (user_param->connection_type==UD) {
		ctx->list.addr = (uintptr_t) ctx->buf + 40;
	} else {
		ctx->list.addr = (uintptr_t) ctx->buf;
	}
	ctx->list.length = size;
	ctx->list.lkey = ctx->mr->lkey;
	if (user_param->connection_type==UD) {
		ctx->wr.wr.ud.ah          = ctx->ah;
		ctx->wr.wr.ud.remote_qpn  = rem_dest->qpn;
		ctx->wr.wr.ud.remote_qkey = 0x11111111;
		if (user_param->use_mcg) {
			ctx->wr.wr.ud.remote_qpn = 0xffffff;
		} else {
			ctx->wr.wr.ud.remote_qpn = rem_dest->qpn;
		}
	}
	/// receive //
	rwr = ctx->rwr;
	ctx->recv_list.addr = (uintptr_t) ctx->buf;
	if (user_param->connection_type==UD) {
		ctx->recv_list.length = ctx->size + 40;
	} else {
		ctx->recv_list.length = ctx->size;
	}

	ctx->recv_list.lkey = ctx->mr->lkey;

	scnt = 0;
	rcnt = 0;
	ccnt = 0;
	poll = 0;
	poll_buf = ctx->poll_buf;
	post_buf = ctx->post_buf;
	qp = ctx->qp;
	if (size > user_param->inline_size || size == 0) {/* complaince to perf_main  don't signal*/
		ctx->wr.send_flags = 0;
	} else {
		ctx->wr.send_flags = IBV_SEND_INLINE;
	}

	while (scnt < iters || rcnt < iters) {
		if (rcnt < iters && !(scnt < 1 && user_param->servername)) {
			int ne;
			struct ibv_wc wc;
			/*Server is polling on recieve first */
			++rcnt;
			if (ibv_post_recv(qp, &rwr, &bad_wr_recv)) {
				fprintf(stderr, "Couldn't post recv: rcnt=%d\n",
					rcnt);
				return 15;
			}
            if (user_param->use_event) {
                struct ibv_cq *ev_cq;
                void          *ev_ctx;

                if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
                    fprintf(stderr, "Failed to get receive cq_event\n");
                    return 1;
                }

                if (ev_cq != ctx->rcq) {
                    fprintf(stderr, "CQ event for unknown RCQ %p\n", ev_cq);
                    return 1;
                }

                if (ibv_req_notify_cq(ctx->rcq, 0)) {
                    fprintf(stderr, "Couldn't request RCQ notification\n");
                    return 1;
                }
            }
			do {
				ne = ibv_poll_cq(ctx->rcq, 1, &wc);
			} while (!user_param->use_event && ne < 1);

			if (ne < 0) {
				fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
				return 12;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Recieve Completion wth error at %s:\n",
					user_param->servername ? "client" : "server");
				fprintf(stderr, "Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				fprintf(stderr, "scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				return 13;
			}
		}
		if (scnt < iters ) {
			if (ccnt == (tx_depth - 2) || (user_param->signal_comp == SIGNAL)
			    || (scnt == (iters - 1)) ) {
				ccnt = 0;
				poll=1;
				if (size > user_param->inline_size || size == 0) {/* complaince to perf_main */
					ctx->wr.send_flags = IBV_SEND_SIGNALED;
				} else {
					ctx->wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
				}

			}
			struct ibv_send_wr *bad_wr;
			/* client post first */
			tstamp[scnt] = get_cycles();
			*post_buf = (char)++scnt;
			if (ibv_post_send(qp, wr, &bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",
					scnt);
				return 11;
			}
		}
		if (poll == 1) {
			struct ibv_wc wc;
			int ne;
            if (user_param->use_event) {
                struct ibv_cq *ev_cq;
                void          *ev_ctx;

                if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
                    fprintf(stderr, "Failed to get send cq_event\n");
                    return 1;
                }

                if (ev_cq != ctx->scq) {
                    fprintf(stderr, "CQ event for unknown SCQ %p\n", ev_cq);
                    return 1;
                }

                if (ibv_req_notify_cq(ctx->scq, 0)) {
                    fprintf(stderr, "Couldn't request SCQ notification\n");
                    return 1;
                }
            }
			/* poll on scq */
			do {
				ne = ibv_poll_cq(ctx->scq, 1, &wc);
			} while (!user_param->use_event && ne < 1);

			if (ne < 0) {
				fprintf(stderr, "poll SCQ failed %d\n", ne);
				return 12;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Completion wth error at %s:\n",
					user_param->servername ? "client" : "server");
				fprintf(stderr, "Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				fprintf(stderr, "scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				return 13;
			}
			poll = 0;
			if (size > user_param->inline_size || size == 0) {/* complaince to perf_main don't signal*/
				ctx->wr.send_flags = 0;
			} else {
				ctx->wr.send_flags = IBV_SEND_INLINE;
			}

		}
		++ccnt;
	}

	return(0);
}
int main(int argc, char *argv[])
{
	const char              *ib_devname = NULL;
	int                      port = 18515;
	int                      ib_port = 1;
	int                      size = 2;
	int                      i = 0;
	int                      size_max_pow = 24;
	struct report_options    report = {};

	struct pingpong_context *ctx;
	struct pingpong_dest     rem_dest;
	struct ibv_device       *ib_dev;
	struct user_parameters   user_param;
	int                      no_cpu_freq_fail = 0;

	/* init default values to user's parameters */
	memset(&user_param, 0, sizeof(struct user_parameters));
	user_param.mtu = 0;
	user_param.iters = 1000;
	user_param.tx_depth = 50;
	user_param.servername = NULL;
	user_param.use_event = 0;
	user_param.use_mcg = 0;
	user_param.inline_size = MAX_INLINE;
	user_param.signal_comp = 0;
	user_param.qp_timeout = 14;
	user_param.gid_index = -1; /*gid will not be used*/
	/* Parameter parsing. */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "connection",     .has_arg = 1, .val = 'c' },
			{ .name = "mtu",            .has_arg = 1, .val = 'm' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "inline_size",    .has_arg = 1, .val = 'I' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "signal",         .has_arg = 0, .val = 'l' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "report-cycles",  .has_arg = 0, .val = 'C' },
			{ .name = "report-histogram",.has_arg = 0, .val = 'H' },
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "mcg",            .has_arg = 0, .val = 'g' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ 0 }
		};
		c = getopt_long(argc, argv, "p:c:m:d:i:s:n:t:I:u:S:x:laeCHUVgF", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = strtol(optarg, NULL, 0);
			if (port < 0 || port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'c':
			if (strcmp("UC",optarg)==0)
				user_param.connection_type=UC;
			if (strcmp("UD",optarg)==0)
				user_param.connection_type=UD;
			/* default is 0 for any other option RC*/
			break;
        case 'e':
			++user_param.use_event;
			break;
		case 'g':
			++user_param.use_mcg;
			break;
		case 'm':
			user_param.mtu = strtol(optarg, NULL, 0);
			break;
		case 'l':
			user_param.signal_comp = SIGNAL;
			break;
		case 'a':
			user_param.all = SIGNAL;
			break;
		case 'V':
			printf("perftest version : %.2f\n",VERSION);
			return 0;
			break;
		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 0) {
				usage(argv[0]);
				return 2;
			}
			break;

		case 's':
			size = strtol(optarg, NULL, 0);
			if (size < 1) {
				usage(argv[0]); return 3;
			}
			break;

		case 'x':
			user_param.gid_index = strtol(optarg, NULL, 0);
			if (user_param.gid_index > 63) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 't':
			user_param.tx_depth = strtol(optarg, NULL, 0);
			if (user_param.tx_depth < 1) {
				usage(argv[0]); return 4;
			}
			break;

		case 'I':
			user_param.inline_size = strtol(optarg, NULL, 0);
			if (user_param.inline_size > MAX_INLINE) {
				usage(argv[0]);
				return 19;
			}
			break;

		case 'n':
			user_param.iters = strtol(optarg, NULL, 0);
			if (user_param.iters < 2) {
				usage(argv[0]);
				return 5;
			}

			break;

		case 'C':
			report.cycles = 1;
			break;

		case 'H':
			report.histogram = 1;
			break;

		case 'U':
			report.unsorted = 1;
			break;

		case 'F':
			no_cpu_freq_fail = 1;
			break;

		case 'u':
			user_param.qp_timeout = strtol(optarg, NULL, 0);
			break;

		case 'S':
			sl = strtol(optarg, NULL, 0);
			if (sl > 15) { usage(argv[0]); return 6; }
			break;

		default:
			usage(argv[0]);
			return 7;
		}
	}

	if (optind == argc - 1)
		user_param.servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 6;
	}

	/*
	 *  Done with parameter parsing. Perform setup.
	 */
	tstamp = malloc(user_param.iters * sizeof *tstamp);
	if (!tstamp) {
		perror("malloc");
		return 10;
	}
	/* Print header data */
	printf("------------------------------------------------------------------\n");
	if (user_param.use_mcg && (user_param.connection_type == UD))
		printf("                    Send Latency Multicast Test\n");
	else
		printf("                    Send Latency Test\n");
	printf("Inline data is used up to %d bytes message\n", user_param.inline_size);
	if (user_param.connection_type==RC) {
		printf("Connection type : RC\n");
	} else if (user_param.connection_type==UC) { 
		printf("Connection type : UC\n");
	} else {
		printf("Connection type : UD\n");
	}
	if (user_param.gid_index > -1) {
		printf("Using GID to support RDMAoE configuration. Refer to port type as Ethernet, default MTU 1024B\n");
	}
	if (user_param.all == 1) {
		/*since we run all sizes lets allocate big enough buffer */
		size = 8388608; /*2^23 */
	}
	if (user_param.connection_type == UD && size > 2048) {
		printf("Max msg size in UD is 2048 changing to 2048\n");
		size = 2048;
	}
	if (user_param.connection_type == UD && user_param.gid_index > -1 && size > 1024) {
		printf("Max msg size in UD RDMAoE is 1024. changing to 1024\n");
		size = 1024;
	}

	srand48(getpid() * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);

	ib_dev = pp_find_dev(ib_devname);
	if (!ib_dev)
		return 7;

	ctx = pp_init_ctx(ib_dev, size, user_param.tx_depth, ib_port,&user_param);
	if (!ctx)
		return 8;

	if (pp_open_port(ctx, user_param.servername, ib_port, port, &rem_dest,&user_param))
		return 9;
    if (user_param.use_event) {
        printf("Test with events.\n");
        if (ibv_req_notify_cq(ctx->rcq, 0)) {
			fprintf(stderr, "Couldn't request RCQ notification\n");
			return 1;
		} 
        if (ibv_req_notify_cq(ctx->scq, 0)) {
			fprintf(stderr, "Couldn't request SCQ notification\n");
			return 1;
		}

    }
	printf("------------------------------------------------------------------\n");
	printf(" #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]\n");
    
	if (user_param.all == 1) {
		if (user_param.connection_type==UD) {
			if (user_param.gid_index < 0) {
				size_max_pow = 12;
			} else {
				size_max_pow = 11;
			}
		}
		for (i = 1; i < size_max_pow ; ++i) {
			size = 1 << i;
			if(run_iter(ctx, &user_param, &rem_dest, size))
				return 17;

			print_report(&report, user_param.iters, tstamp, size, no_cpu_freq_fail);
		}
	} else {
		if(run_iter(ctx, &user_param, &rem_dest, size))
			return 18;	
		print_report(&report, user_param.iters, tstamp, size, no_cpu_freq_fail);
	}
	printf("------------------------------------------------------------------\n");
	free(tstamp);
	return 0;
}
