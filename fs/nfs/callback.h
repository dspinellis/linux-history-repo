/*
 * linux/fs/nfs/callback.h
 *
 * Copyright (C) 2004 Trond Myklebust
 *
 * NFSv4 callback definitions
 */
#ifndef __LINUX_FS_NFS_CALLBACK_H
#define __LINUX_FS_NFS_CALLBACK_H

#define NFS4_CALLBACK 0x40000000
#define NFS4_CALLBACK_XDRSIZE 2048
#define NFS4_CALLBACK_BUFSIZE (1024 + NFS4_CALLBACK_XDRSIZE)

enum nfs4_callback_procnum {
	CB_NULL = 0,
	CB_COMPOUND = 1,
};

enum nfs4_callback_opnum {
	OP_CB_GETATTR = 3,
	OP_CB_RECALL  = 4,
/* Callback operations new to NFSv4.1 */
	OP_CB_LAYOUTRECALL  = 5,
	OP_CB_NOTIFY        = 6,
	OP_CB_PUSH_DELEG    = 7,
	OP_CB_RECALL_ANY    = 8,
	OP_CB_RECALLABLE_OBJ_AVAIL = 9,
	OP_CB_RECALL_SLOT   = 10,
	OP_CB_SEQUENCE      = 11,
	OP_CB_WANTS_CANCELLED = 12,
	OP_CB_NOTIFY_LOCK   = 13,
	OP_CB_NOTIFY_DEVICEID = 14,
	OP_CB_ILLEGAL = 10044,
};

struct cb_compound_hdr_arg {
	unsigned int taglen;
	const char *tag;
	unsigned int minorversion;
	unsigned nops;
};

struct cb_compound_hdr_res {
	__be32 *status;
	unsigned int taglen;
	const char *tag;
	__be32 *nops;
};

struct cb_getattrargs {
	struct sockaddr *addr;
	struct nfs_fh fh;
	uint32_t bitmap[2];
};

struct cb_getattrres {
	__be32 status;
	uint32_t bitmap[2];
	uint64_t size;
	uint64_t change_attr;
	struct timespec ctime;
	struct timespec mtime;
};

struct cb_recallargs {
	struct sockaddr *addr;
	struct nfs_fh fh;
	nfs4_stateid stateid;
	uint32_t truncate;
};

extern __be32 nfs4_callback_getattr(struct cb_getattrargs *args, struct cb_getattrres *res);
extern __be32 nfs4_callback_recall(struct cb_recallargs *args, void *dummy);

#ifdef CONFIG_NFS_V4
extern int nfs_callback_up(u32 minorversion, struct rpc_xprt *xprt);
extern void nfs_callback_down(int minorversion);
#endif /* CONFIG_NFS_V4 */

/*
 * nfs41: Callbacks are expected to not cause substantial latency,
 * so we limit their concurrency to 1 by setting up the maximum number
 * of slots for the backchannel.
 */
#define NFS41_BC_MIN_CALLBACKS 1

extern unsigned int nfs_callback_set_tcpport;
extern unsigned short nfs_callback_tcpport;
extern unsigned short nfs_callback_tcpport6;

#endif /* __LINUX_FS_NFS_CALLBACK_H */
