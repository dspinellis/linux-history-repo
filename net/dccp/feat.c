/*
 *  net/dccp/feat.c
 *
 *  An implementation of the DCCP protocol
 *  Andrea Bittau <a.bittau@cs.ucl.ac.uk>
 *
 *  ASSUMPTIONS
 *  -----------
 *  o Feature negotiation is coordinated with connection setup (as in TCP), wild
 *    changes of parameters of an established connection are not supported.
 *  o All currently known SP features have 1-byte quantities. If in the future
 *    extensions of RFCs 4340..42 define features with item lengths larger than
 *    one byte, a feature-specific extension of the code will be required.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>

#include "ccid.h"
#include "feat.h"

#define DCCP_FEAT_SP_NOAGREE (-123)

static const struct {
	u8			feat_num;		/* DCCPF_xxx */
	enum dccp_feat_type	rxtx;			/* RX or TX  */
	enum dccp_feat_type	reconciliation;		/* SP or NN  */
	u8			default_value;		/* as in 6.4 */
/*
 *    Lookup table for location and type of features (from RFC 4340/4342)
 *  +--------------------------+----+-----+----+----+---------+-----------+
 *  | Feature                  | Location | Reconc. | Initial |  Section  |
 *  |                          | RX | TX  | SP | NN |  Value  | Reference |
 *  +--------------------------+----+-----+----+----+---------+-----------+
 *  | DCCPF_CCID               |    |  X  | X  |    |   2     | 10        |
 *  | DCCPF_SHORT_SEQNOS       |    |  X  | X  |    |   0     |  7.6.1    |
 *  | DCCPF_SEQUENCE_WINDOW    |    |  X  |    | X  | 100     |  7.5.2    |
 *  | DCCPF_ECN_INCAPABLE      | X  |     | X  |    |   0     | 12.1      |
 *  | DCCPF_ACK_RATIO          |    |  X  |    | X  |   2     | 11.3      |
 *  | DCCPF_SEND_ACK_VECTOR    | X  |     | X  |    |   0     | 11.5      |
 *  | DCCPF_SEND_NDP_COUNT     |    |  X  | X  |    |   0     |  7.7.2    |
 *  | DCCPF_MIN_CSUM_COVER     | X  |     | X  |    |   0     |  9.2.1    |
 *  | DCCPF_DATA_CHECKSUM      | X  |     | X  |    |   0     |  9.3.1    |
 *  | DCCPF_SEND_LEV_RATE      | X  |     | X  |    |   0     | 4342/8.4  |
 *  +--------------------------+----+-----+----+----+---------+-----------+
 */
} dccp_feat_table[] = {
	{ DCCPF_CCID,		 FEAT_AT_TX, FEAT_SP, 2 },
	{ DCCPF_SHORT_SEQNOS,	 FEAT_AT_TX, FEAT_SP, 0 },
	{ DCCPF_SEQUENCE_WINDOW, FEAT_AT_TX, FEAT_NN, 100 },
	{ DCCPF_ECN_INCAPABLE,	 FEAT_AT_RX, FEAT_SP, 0 },
	{ DCCPF_ACK_RATIO,	 FEAT_AT_TX, FEAT_NN, 2 },
	{ DCCPF_SEND_ACK_VECTOR, FEAT_AT_RX, FEAT_SP, 0 },
	{ DCCPF_SEND_NDP_COUNT,  FEAT_AT_TX, FEAT_SP, 0 },
	{ DCCPF_MIN_CSUM_COVER,  FEAT_AT_RX, FEAT_SP, 0 },
	{ DCCPF_DATA_CHECKSUM,	 FEAT_AT_RX, FEAT_SP, 0 },
	{ DCCPF_SEND_LEV_RATE,	 FEAT_AT_RX, FEAT_SP, 0 },
};
#define DCCP_FEAT_SUPPORTED_MAX		ARRAY_SIZE(dccp_feat_table)

/**
 * dccp_feat_index  -  Hash function to map feature number into array position
 * Returns consecutive array index or -1 if the feature is not understood.
 */
static int dccp_feat_index(u8 feat_num)
{
	/* The first 9 entries are occupied by the types from RFC 4340, 6.4 */
	if (feat_num > DCCPF_RESERVED && feat_num <= DCCPF_DATA_CHECKSUM)
		return feat_num - 1;

	/*
	 * Other features: add cases for new feature types here after adding
	 * them to the above table.
	 */
	switch (feat_num) {
	case DCCPF_SEND_LEV_RATE:
			return DCCP_FEAT_SUPPORTED_MAX - 1;
	}
	return -1;
}

static u8 dccp_feat_type(u8 feat_num)
{
	int idx = dccp_feat_index(feat_num);

	if (idx < 0)
		return FEAT_UNKNOWN;
	return dccp_feat_table[idx].reconciliation;
}

static int dccp_feat_default_value(u8 feat_num)
{
	int idx = dccp_feat_index(feat_num);
	/*
	 * There are no default values for unknown features, so encountering a
	 * negative index here indicates a serious problem somewhere else.
	 */
	DCCP_BUG_ON(idx < 0);

	return idx < 0 ? 0 : dccp_feat_table[idx].default_value;
}

/* copy constructor, fval must not already contain allocated memory */
static int dccp_feat_clone_sp_val(dccp_feat_val *fval, u8 const *val, u8 len)
{
	fval->sp.len = len;
	if (fval->sp.len > 0) {
		fval->sp.vec = kmemdup(val, len, gfp_any());
		if (fval->sp.vec == NULL) {
			fval->sp.len = 0;
			return -ENOBUFS;
		}
	}
	return 0;
}

static void dccp_feat_val_destructor(u8 feat_num, dccp_feat_val *val)
{
	if (unlikely(val == NULL))
		return;
	if (dccp_feat_type(feat_num) == FEAT_SP)
		kfree(val->sp.vec);
	memset(val, 0, sizeof(*val));
}

static struct dccp_feat_entry *
	      dccp_feat_clone_entry(struct dccp_feat_entry const *original)
{
	struct dccp_feat_entry *new;
	u8 type = dccp_feat_type(original->feat_num);

	if (type == FEAT_UNKNOWN)
		return NULL;

	new = kmemdup(original, sizeof(struct dccp_feat_entry), gfp_any());
	if (new == NULL)
		return NULL;

	if (type == FEAT_SP && dccp_feat_clone_sp_val(&new->val,
						      original->val.sp.vec,
						      original->val.sp.len)) {
		kfree(new);
		return NULL;
	}
	return new;
}

static void dccp_feat_entry_destructor(struct dccp_feat_entry *entry)
{
	if (entry != NULL) {
		dccp_feat_val_destructor(entry->feat_num, &entry->val);
		kfree(entry);
	}
}

/*
 * List management functions
 *
 * Feature negotiation lists rely on and maintain the following invariants:
 * - each feat_num in the list is known, i.e. we know its type and default value
 * - each feat_num/is_local combination is unique (old entries are overwritten)
 * - SP values are always freshly allocated
 * - list is sorted in increasing order of feature number (faster lookup)
 */

/**
 * dccp_feat_entry_new  -  Central list update routine (called by all others)
 * @head:  list to add to
 * @feat:  feature number
 * @local: whether the local (1) or remote feature with number @feat is meant
 * This is the only constructor and serves to ensure the above invariants.
 */
static struct dccp_feat_entry *
	      dccp_feat_entry_new(struct list_head *head, u8 feat, bool local)
{
	struct dccp_feat_entry *entry;

	list_for_each_entry(entry, head, node)
		if (entry->feat_num == feat && entry->is_local == local) {
			dccp_feat_val_destructor(entry->feat_num, &entry->val);
			return entry;
		} else if (entry->feat_num > feat) {
			head = &entry->node;
			break;
		}

	entry = kmalloc(sizeof(*entry), gfp_any());
	if (entry != NULL) {
		entry->feat_num = feat;
		entry->is_local = local;
		list_add_tail(&entry->node, head);
	}
	return entry;
}

/**
 * dccp_feat_push_change  -  Add/overwrite a Change option in the list
 * @fn_list: feature-negotiation list to update
 * @feat: one of %dccp_feature_numbers
 * @local: whether local (1) or remote (0) @feat_num is meant
 * @needs_mandatory: whether to use Mandatory feature negotiation options
 * @fval: pointer to NN/SP value to be inserted (will be copied)
 */
static int dccp_feat_push_change(struct list_head *fn_list, u8 feat, u8 local,
				 u8 mandatory, dccp_feat_val *fval)
{
	struct dccp_feat_entry *new = dccp_feat_entry_new(fn_list, feat, local);

	if (new == NULL)
		return -ENOMEM;

	new->feat_num	     = feat;
	new->is_local	     = local;
	new->state	     = FEAT_INITIALISING;
	new->needs_confirm   = 0;
	new->empty_confirm   = 0;
	new->val	     = *fval;
	new->needs_mandatory = mandatory;

	return 0;
}

static inline void dccp_feat_list_pop(struct dccp_feat_entry *entry)
{
	list_del(&entry->node);
	dccp_feat_entry_destructor(entry);
}

void dccp_feat_list_purge(struct list_head *fn_list)
{
	struct dccp_feat_entry *entry, *next;

	list_for_each_entry_safe(entry, next, fn_list, node)
		dccp_feat_entry_destructor(entry);
	INIT_LIST_HEAD(fn_list);
}
EXPORT_SYMBOL_GPL(dccp_feat_list_purge);

/* generate @to as full clone of @from - @to must not contain any nodes */
int dccp_feat_clone_list(struct list_head const *from, struct list_head *to)
{
	struct dccp_feat_entry *entry, *new;

	INIT_LIST_HEAD(to);
	list_for_each_entry(entry, from, node) {
		new = dccp_feat_clone_entry(entry);
		if (new == NULL)
			goto cloning_failed;
		list_add_tail(&new->node, to);
	}
	return 0;

cloning_failed:
	dccp_feat_list_purge(to);
	return -ENOMEM;
}

static u8 dccp_feat_is_valid_nn_val(u8 feat_num, u64 val)
{
	switch (feat_num) {
	case DCCPF_ACK_RATIO:
		return val <= DCCPF_ACK_RATIO_MAX;
	case DCCPF_SEQUENCE_WINDOW:
		return val >= DCCPF_SEQ_WMIN && val <= DCCPF_SEQ_WMAX;
	}
	return 0;	/* feature unknown - so we can't tell */
}

/* check that SP values are within the ranges defined in RFC 4340 */
static u8 dccp_feat_is_valid_sp_val(u8 feat_num, u8 val)
{
	switch (feat_num) {
	case DCCPF_CCID:
		return val == DCCPC_CCID2 || val == DCCPC_CCID3;
	/* Type-check Boolean feature values: */
	case DCCPF_SHORT_SEQNOS:
	case DCCPF_ECN_INCAPABLE:
	case DCCPF_SEND_ACK_VECTOR:
	case DCCPF_SEND_NDP_COUNT:
	case DCCPF_DATA_CHECKSUM:
	case DCCPF_SEND_LEV_RATE:
		return val < 2;
	case DCCPF_MIN_CSUM_COVER:
		return val < 16;
	}
	return 0;			/* feature unknown */
}

static u8 dccp_feat_sp_list_ok(u8 feat_num, u8 const *sp_list, u8 sp_len)
{
	if (sp_list == NULL || sp_len < 1)
		return 0;
	while (sp_len--)
		if (!dccp_feat_is_valid_sp_val(feat_num, *sp_list++))
			return 0;
	return 1;
}

/**
 * __feat_register_nn  -  Register new NN value on socket
 * @fn: feature-negotiation list to register with
 * @feat: an NN feature from %dccp_feature_numbers
 * @mandatory: use Mandatory option if 1
 * @nn_val: value to register (restricted to 4 bytes)
 * Note that NN features are local by definition (RFC 4340, 6.3.2).
 */
static int __feat_register_nn(struct list_head *fn, u8 feat,
			      u8 mandatory, u64 nn_val)
{
	dccp_feat_val fval = { .nn = nn_val };

	if (dccp_feat_type(feat) != FEAT_NN ||
	    !dccp_feat_is_valid_nn_val(feat, nn_val))
		return -EINVAL;

	/* Don't bother with default values, they will be activated anyway. */
	if (nn_val - (u64)dccp_feat_default_value(feat) == 0)
		return 0;

	return dccp_feat_push_change(fn, feat, 1, mandatory, &fval);
}

/**
 * __feat_register_sp  -  Register new SP value/list on socket
 * @fn: feature-negotiation list to register with
 * @feat: an SP feature from %dccp_feature_numbers
 * @is_local: whether the local (1) or the remote (0) @feat is meant
 * @mandatory: use Mandatory option if 1
 * @sp_val: SP value followed by optional preference list
 * @sp_len: length of @sp_val in bytes
 */
static int __feat_register_sp(struct list_head *fn, u8 feat, u8 is_local,
			      u8 mandatory, u8 const *sp_val, u8 sp_len)
{
	dccp_feat_val fval;

	if (dccp_feat_type(feat) != FEAT_SP ||
	    !dccp_feat_sp_list_ok(feat, sp_val, sp_len))
		return -EINVAL;

	/* Avoid negotiating alien CCIDs by only advertising supported ones */
	if (feat == DCCPF_CCID && !ccid_support_check(sp_val, sp_len))
		return -EOPNOTSUPP;

	if (dccp_feat_clone_sp_val(&fval, sp_val, sp_len))
		return -ENOMEM;

	return dccp_feat_push_change(fn, feat, is_local, mandatory, &fval);
}

int dccp_feat_change(struct dccp_minisock *dmsk, u8 type, u8 feature,
		     u8 *val, u8 len, gfp_t gfp)
{
	struct dccp_opt_pend *opt;

	dccp_feat_debug(type, feature, *val);

	if (len > 3) {
		DCCP_WARN("invalid length %d\n", len);
		return -EINVAL;
	}
	/* XXX add further sanity checks */

	/* check if that feature is already being negotiated */
	list_for_each_entry(opt, &dmsk->dccpms_pending, dccpop_node) {
		/* ok we found a negotiation for this option already */
		if (opt->dccpop_feat == feature && opt->dccpop_type == type) {
			dccp_pr_debug("Replacing old\n");
			/* replace */
			BUG_ON(opt->dccpop_val == NULL);
			kfree(opt->dccpop_val);
			opt->dccpop_val	 = val;
			opt->dccpop_len	 = len;
			opt->dccpop_conf = 0;
			return 0;
		}
	}

	/* negotiation for a new feature */
	opt = kmalloc(sizeof(*opt), gfp);
	if (opt == NULL)
		return -ENOMEM;

	opt->dccpop_type = type;
	opt->dccpop_feat = feature;
	opt->dccpop_len	 = len;
	opt->dccpop_val	 = val;
	opt->dccpop_conf = 0;
	opt->dccpop_sc	 = NULL;

	BUG_ON(opt->dccpop_val == NULL);

	list_add_tail(&opt->dccpop_node, &dmsk->dccpms_pending);
	return 0;
}

EXPORT_SYMBOL_GPL(dccp_feat_change);

/*
 *	Tracking features whose value depend on the choice of CCID
 *
 * This is designed with an extension in mind so that a list walk could be done
 * before activating any features. However, the existing framework was found to
 * work satisfactorily up until now, the automatic verification is left open.
 * When adding new CCIDs, add a corresponding dependency table here.
 */
static const struct ccid_dependency *dccp_feat_ccid_deps(u8 ccid, bool is_local)
{
	static const struct ccid_dependency ccid2_dependencies[2][2] = {
		/*
		 * CCID2 mandates Ack Vectors (RFC 4341, 4.): as CCID is a TX
		 * feature and Send Ack Vector is an RX feature, `is_local'
		 * needs to be reversed.
		 */
		{	/* Dependencies of the receiver-side (remote) CCID2 */
			{
				.dependent_feat	= DCCPF_SEND_ACK_VECTOR,
				.is_local	= true,
				.is_mandatory	= true,
				.val		= 1
			},
			{ 0, 0, 0, 0 }
		},
		{	/* Dependencies of the sender-side (local) CCID2 */
			{
				.dependent_feat	= DCCPF_SEND_ACK_VECTOR,
				.is_local	= false,
				.is_mandatory	= true,
				.val		= 1
			},
			{ 0, 0, 0, 0 }
		}
	};
	static const struct ccid_dependency ccid3_dependencies[2][5] = {
		{	/*
			 * Dependencies of the receiver-side CCID3
			 */
			{	/* locally disable Ack Vectors */
				.dependent_feat	= DCCPF_SEND_ACK_VECTOR,
				.is_local	= true,
				.is_mandatory	= false,
				.val		= 0
			},
			{	/* see below why Send Loss Event Rate is on */
				.dependent_feat	= DCCPF_SEND_LEV_RATE,
				.is_local	= true,
				.is_mandatory	= true,
				.val		= 1
			},
			{	/* NDP Count is needed as per RFC 4342, 6.1.1 */
				.dependent_feat	= DCCPF_SEND_NDP_COUNT,
				.is_local	= false,
				.is_mandatory	= true,
				.val		= 1
			},
			{ 0, 0, 0, 0 },
		},
		{	/*
			 * CCID3 at the TX side: we request that the HC-receiver
			 * will not send Ack Vectors (they will be ignored, so
			 * Mandatory is not set); we enable Send Loss Event Rate
			 * (Mandatory since the implementation does not support
			 * the Loss Intervals option of RFC 4342, 8.6).
			 * The last two options are for peer's information only.
			*/
			{
				.dependent_feat	= DCCPF_SEND_ACK_VECTOR,
				.is_local	= false,
				.is_mandatory	= false,
				.val		= 0
			},
			{
				.dependent_feat	= DCCPF_SEND_LEV_RATE,
				.is_local	= false,
				.is_mandatory	= true,
				.val		= 1
			},
			{	/* this CCID does not support Ack Ratio */
				.dependent_feat	= DCCPF_ACK_RATIO,
				.is_local	= true,
				.is_mandatory	= false,
				.val		= 0
			},
			{	/* tell receiver we are sending NDP counts */
				.dependent_feat	= DCCPF_SEND_NDP_COUNT,
				.is_local	= true,
				.is_mandatory	= false,
				.val		= 1
			},
			{ 0, 0, 0, 0 }
		}
	};
	switch (ccid) {
	case DCCPC_CCID2:
		return ccid2_dependencies[is_local];
	case DCCPC_CCID3:
		return ccid3_dependencies[is_local];
	default:
		return NULL;
	}
}

/**
 * dccp_feat_propagate_ccid - Resolve dependencies of features on choice of CCID
 * @fn: feature-negotiation list to update
 * @id: CCID number to track
 * @is_local: whether TX CCID (1) or RX CCID (0) is meant
 * This function needs to be called after registering all other features.
 */
static int dccp_feat_propagate_ccid(struct list_head *fn, u8 id, bool is_local)
{
	const struct ccid_dependency *table = dccp_feat_ccid_deps(id, is_local);
	int i, rc = (table == NULL);

	for (i = 0; rc == 0 && table[i].dependent_feat != DCCPF_RESERVED; i++)
		if (dccp_feat_type(table[i].dependent_feat) == FEAT_SP)
			rc = __feat_register_sp(fn, table[i].dependent_feat,
						    table[i].is_local,
						    table[i].is_mandatory,
						    &table[i].val, 1);
		else
			rc = __feat_register_nn(fn, table[i].dependent_feat,
						    table[i].is_mandatory,
						    table[i].val);
	return rc;
}

/**
 * dccp_feat_finalise_settings  -  Finalise settings before starting negotiation
 * @dp: client or listening socket (settings will be inherited)
 * This is called after all registrations (socket initialisation, sysctls, and
 * sockopt calls), and before sending the first packet containing Change options
 * (ie. client-Request or server-Response), to ensure internal consistency.
 */
int dccp_feat_finalise_settings(struct dccp_sock *dp)
{
	struct list_head *fn = &dp->dccps_featneg;
	struct dccp_feat_entry *entry;
	int i = 2, ccids[2] = { -1, -1 };

	/*
	 * Propagating CCIDs:
	 * 1) not useful to propagate CCID settings if this host advertises more
	 *    than one CCID: the choice of CCID  may still change - if this is
	 *    the client, or if this is the server and the client sends
	 *    singleton CCID values.
	 * 2) since is that propagate_ccid changes the list, we defer changing
	 *    the sorted list until after the traversal.
	 */
	list_for_each_entry(entry, fn, node)
		if (entry->feat_num == DCCPF_CCID && entry->val.sp.len == 1)
			ccids[entry->is_local] = entry->val.sp.vec[0];
	while (i--)
		if (ccids[i] > 0 && dccp_feat_propagate_ccid(fn, ccids[i], i))
			return -1;
	return 0;
}

static int dccp_feat_update_ccid(struct sock *sk, u8 type, u8 new_ccid_nr)
{
	struct dccp_sock *dp = dccp_sk(sk);
	struct dccp_minisock *dmsk = dccp_msk(sk);
	/* figure out if we are changing our CCID or the peer's */
	const int rx = type == DCCPO_CHANGE_R;
	const u8 ccid_nr = rx ? dmsk->dccpms_rx_ccid : dmsk->dccpms_tx_ccid;
	struct ccid *new_ccid;

	/* Check if nothing is being changed. */
	if (ccid_nr == new_ccid_nr)
		return 0;

	new_ccid = ccid_new(new_ccid_nr, sk, rx, GFP_ATOMIC);
	if (new_ccid == NULL)
		return -ENOMEM;

	if (rx) {
		ccid_hc_rx_delete(dp->dccps_hc_rx_ccid, sk);
		dp->dccps_hc_rx_ccid = new_ccid;
		dmsk->dccpms_rx_ccid = new_ccid_nr;
	} else {
		ccid_hc_tx_delete(dp->dccps_hc_tx_ccid, sk);
		dp->dccps_hc_tx_ccid = new_ccid;
		dmsk->dccpms_tx_ccid = new_ccid_nr;
	}

	return 0;
}

static int dccp_feat_update(struct sock *sk, u8 type, u8 feat, u8 val)
{
	dccp_feat_debug(type, feat, val);

	switch (feat) {
	case DCCPF_CCID:
		return dccp_feat_update_ccid(sk, type, val);
	default:
		dccp_pr_debug("UNIMPLEMENTED: %s(%d, ...)\n",
			      dccp_feat_typename(type), feat);
		break;
	}
	return 0;
}

static int dccp_feat_reconcile(struct sock *sk, struct dccp_opt_pend *opt,
			       u8 *rpref, u8 rlen)
{
	struct dccp_sock *dp = dccp_sk(sk);
	u8 *spref, slen, *res = NULL;
	int i, j, rc, agree = 1;

	BUG_ON(rpref == NULL);

	/* check if we are the black sheep */
	if (dp->dccps_role == DCCP_ROLE_CLIENT) {
		spref = rpref;
		slen  = rlen;
		rpref = opt->dccpop_val;
		rlen  = opt->dccpop_len;
	} else {
		spref = opt->dccpop_val;
		slen  = opt->dccpop_len;
	}
	/*
	 * Now we have server preference list in spref and client preference in
	 * rpref
	 */
	BUG_ON(spref == NULL);
	BUG_ON(rpref == NULL);

	/* FIXME sanity check vals */

	/* Are values in any order?  XXX Lame "algorithm" here */
	for (i = 0; i < slen; i++) {
		for (j = 0; j < rlen; j++) {
			if (spref[i] == rpref[j]) {
				res = &spref[i];
				break;
			}
		}
		if (res)
			break;
	}

	/* we didn't agree on anything */
	if (res == NULL) {
		/* confirm previous value */
		switch (opt->dccpop_feat) {
		case DCCPF_CCID:
			/* XXX did i get this right? =P */
			if (opt->dccpop_type == DCCPO_CHANGE_L)
				res = &dccp_msk(sk)->dccpms_tx_ccid;
			else
				res = &dccp_msk(sk)->dccpms_rx_ccid;
			break;

		default:
			DCCP_BUG("Fell through, feat=%d", opt->dccpop_feat);
			/* XXX implement res */
			return -EFAULT;
		}

		dccp_pr_debug("Don't agree... reconfirming %d\n", *res);
		agree = 0; /* this is used for mandatory options... */
	}

	/* need to put result and our preference list */
	rlen = 1 + opt->dccpop_len;
	rpref = kmalloc(rlen, GFP_ATOMIC);
	if (rpref == NULL)
		return -ENOMEM;

	*rpref = *res;
	memcpy(&rpref[1], opt->dccpop_val, opt->dccpop_len);

	/* put it in the "confirm queue" */
	if (opt->dccpop_sc == NULL) {
		opt->dccpop_sc = kmalloc(sizeof(*opt->dccpop_sc), GFP_ATOMIC);
		if (opt->dccpop_sc == NULL) {
			kfree(rpref);
			return -ENOMEM;
		}
	} else {
		/* recycle the confirm slot */
		BUG_ON(opt->dccpop_sc->dccpoc_val == NULL);
		kfree(opt->dccpop_sc->dccpoc_val);
		dccp_pr_debug("recycling confirm slot\n");
	}
	memset(opt->dccpop_sc, 0, sizeof(*opt->dccpop_sc));

	opt->dccpop_sc->dccpoc_val = rpref;
	opt->dccpop_sc->dccpoc_len = rlen;

	/* update the option on our side [we are about to send the confirm] */
	rc = dccp_feat_update(sk, opt->dccpop_type, opt->dccpop_feat, *res);
	if (rc) {
		kfree(opt->dccpop_sc->dccpoc_val);
		kfree(opt->dccpop_sc);
		opt->dccpop_sc = NULL;
		return rc;
	}

	dccp_pr_debug("Will confirm %d\n", *rpref);

	/* say we want to change to X but we just got a confirm X, suppress our
	 * change
	 */
	if (!opt->dccpop_conf) {
		if (*opt->dccpop_val == *res)
			opt->dccpop_conf = 1;
		dccp_pr_debug("won't ask for change of same feature\n");
	}

	return agree ? 0 : DCCP_FEAT_SP_NOAGREE; /* used for mandatory opts */
}

static int dccp_feat_sp(struct sock *sk, u8 type, u8 feature, u8 *val, u8 len)
{
	struct dccp_minisock *dmsk = dccp_msk(sk);
	struct dccp_opt_pend *opt;
	int rc = 1;
	u8 t;

	/*
	 * We received a CHANGE.  We gotta match it against our own preference
	 * list.  If we got a CHANGE_R it means it's a change for us, so we need
	 * to compare our CHANGE_L list.
	 */
	if (type == DCCPO_CHANGE_L)
		t = DCCPO_CHANGE_R;
	else
		t = DCCPO_CHANGE_L;

	/* find our preference list for this feature */
	list_for_each_entry(opt, &dmsk->dccpms_pending, dccpop_node) {
		if (opt->dccpop_type != t || opt->dccpop_feat != feature)
			continue;

		/* find the winner from the two preference lists */
		rc = dccp_feat_reconcile(sk, opt, val, len);
		break;
	}

	/* We didn't deal with the change.  This can happen if we have no
	 * preference list for the feature.  In fact, it just shouldn't
	 * happen---if we understand a feature, we should have a preference list
	 * with at least the default value.
	 */
	BUG_ON(rc == 1);

	return rc;
}

static int dccp_feat_nn(struct sock *sk, u8 type, u8 feature, u8 *val, u8 len)
{
	struct dccp_opt_pend *opt;
	struct dccp_minisock *dmsk = dccp_msk(sk);
	u8 *copy;
	int rc;

	/* NN features must be Change L (sec. 6.3.2) */
	if (type != DCCPO_CHANGE_L) {
		dccp_pr_debug("received %s for NN feature %d\n",
				dccp_feat_typename(type), feature);
		return -EFAULT;
	}

	/* XXX sanity check opt val */

	/* copy option so we can confirm it */
	opt = kzalloc(sizeof(*opt), GFP_ATOMIC);
	if (opt == NULL)
		return -ENOMEM;

	copy = kmemdup(val, len, GFP_ATOMIC);
	if (copy == NULL) {
		kfree(opt);
		return -ENOMEM;
	}

	opt->dccpop_type = DCCPO_CONFIRM_R; /* NN can only confirm R */
	opt->dccpop_feat = feature;
	opt->dccpop_val	 = copy;
	opt->dccpop_len	 = len;

	/* change feature */
	rc = dccp_feat_update(sk, type, feature, *val);
	if (rc) {
		kfree(opt->dccpop_val);
		kfree(opt);
		return rc;
	}

	dccp_feat_debug(type, feature, *copy);

	list_add_tail(&opt->dccpop_node, &dmsk->dccpms_conf);

	return 0;
}

static void dccp_feat_empty_confirm(struct dccp_minisock *dmsk,
				    u8 type, u8 feature)
{
	/* XXX check if other confirms for that are queued and recycle slot */
	struct dccp_opt_pend *opt = kzalloc(sizeof(*opt), GFP_ATOMIC);

	if (opt == NULL) {
		/* XXX what do we do?  Ignoring should be fine.  It's a change
		 * after all =P
		 */
		return;
	}

	switch (type) {
	case DCCPO_CHANGE_L:
		opt->dccpop_type = DCCPO_CONFIRM_R;
		break;
	case DCCPO_CHANGE_R:
		opt->dccpop_type = DCCPO_CONFIRM_L;
		break;
	default:
		DCCP_WARN("invalid type %d\n", type);
		kfree(opt);
		return;
	}
	opt->dccpop_feat = feature;
	opt->dccpop_val	 = NULL;
	opt->dccpop_len	 = 0;

	/* change feature */
	dccp_pr_debug("Empty %s(%d)\n", dccp_feat_typename(type), feature);

	list_add_tail(&opt->dccpop_node, &dmsk->dccpms_conf);
}

static void dccp_feat_flush_confirm(struct sock *sk)
{
	struct dccp_minisock *dmsk = dccp_msk(sk);
	/* Check if there is anything to confirm in the first place */
	int yes = !list_empty(&dmsk->dccpms_conf);

	if (!yes) {
		struct dccp_opt_pend *opt;

		list_for_each_entry(opt, &dmsk->dccpms_pending, dccpop_node) {
			if (opt->dccpop_conf) {
				yes = 1;
				break;
			}
		}
	}

	if (!yes)
		return;

	/* OK there is something to confirm... */
	/* XXX check if packet is in flight?  Send delayed ack?? */
	if (sk->sk_state == DCCP_OPEN)
		dccp_send_ack(sk);
}

int dccp_feat_change_recv(struct sock *sk, u8 type, u8 feature, u8 *val, u8 len)
{
	int rc;

	/* Ignore Change requests other than during connection setup */
	if (sk->sk_state != DCCP_LISTEN && sk->sk_state != DCCP_REQUESTING)
		return 0;
	dccp_feat_debug(type, feature, *val);

	/* figure out if it's SP or NN feature */
	switch (feature) {
	/* deal with SP features */
	case DCCPF_CCID:
		rc = dccp_feat_sp(sk, type, feature, val, len);
		break;

	/* deal with NN features */
	case DCCPF_ACK_RATIO:
		rc = dccp_feat_nn(sk, type, feature, val, len);
		break;

	/* XXX implement other features */
	default:
		dccp_pr_debug("UNIMPLEMENTED: not handling %s(%d, ...)\n",
			      dccp_feat_typename(type), feature);
		rc = -EFAULT;
		break;
	}

	/* check if there were problems changing features */
	if (rc) {
		/* If we don't agree on SP, we sent a confirm for old value.
		 * However we propagate rc to caller in case option was
		 * mandatory
		 */
		if (rc != DCCP_FEAT_SP_NOAGREE)
			dccp_feat_empty_confirm(dccp_msk(sk), type, feature);
	}

	/* generate the confirm [if required] */
	dccp_feat_flush_confirm(sk);

	return rc;
}

EXPORT_SYMBOL_GPL(dccp_feat_change_recv);

int dccp_feat_confirm_recv(struct sock *sk, u8 type, u8 feature,
			   u8 *val, u8 len)
{
	u8 t;
	struct dccp_opt_pend *opt;
	struct dccp_minisock *dmsk = dccp_msk(sk);
	int found = 0;
	int all_confirmed = 1;

	/* Ignore Confirm options other than during connection setup */
	if (sk->sk_state != DCCP_LISTEN && sk->sk_state != DCCP_REQUESTING)
		return 0;
	dccp_feat_debug(type, feature, *val);

	/* locate our change request */
	switch (type) {
	case DCCPO_CONFIRM_L: t = DCCPO_CHANGE_R; break;
	case DCCPO_CONFIRM_R: t = DCCPO_CHANGE_L; break;
	default:	      DCCP_WARN("invalid type %d\n", type);
			      return 1;

	}
	/* XXX sanity check feature value */

	list_for_each_entry(opt, &dmsk->dccpms_pending, dccpop_node) {
		if (!opt->dccpop_conf && opt->dccpop_type == t &&
		    opt->dccpop_feat == feature) {
			found = 1;
			dccp_pr_debug("feature %d found\n", opt->dccpop_feat);

			/* XXX do sanity check */

			opt->dccpop_conf = 1;

			/* We got a confirmation---change the option */
			dccp_feat_update(sk, opt->dccpop_type,
					 opt->dccpop_feat, *val);

			/* XXX check the return value of dccp_feat_update */
			break;
		}

		if (!opt->dccpop_conf)
			all_confirmed = 0;
	}

	if (!found)
		dccp_pr_debug("%s(%d, ...) never requested\n",
			      dccp_feat_typename(type), feature);
	return 0;
}

EXPORT_SYMBOL_GPL(dccp_feat_confirm_recv);

void dccp_feat_clean(struct dccp_minisock *dmsk)
{
	struct dccp_opt_pend *opt, *next;

	list_for_each_entry_safe(opt, next, &dmsk->dccpms_pending,
				 dccpop_node) {
		BUG_ON(opt->dccpop_val == NULL);
		kfree(opt->dccpop_val);

		if (opt->dccpop_sc != NULL) {
			BUG_ON(opt->dccpop_sc->dccpoc_val == NULL);
			kfree(opt->dccpop_sc->dccpoc_val);
			kfree(opt->dccpop_sc);
		}

		kfree(opt);
	}
	INIT_LIST_HEAD(&dmsk->dccpms_pending);

	list_for_each_entry_safe(opt, next, &dmsk->dccpms_conf, dccpop_node) {
		BUG_ON(opt == NULL);
		if (opt->dccpop_val != NULL)
			kfree(opt->dccpop_val);
		kfree(opt);
	}
	INIT_LIST_HEAD(&dmsk->dccpms_conf);
}

EXPORT_SYMBOL_GPL(dccp_feat_clean);

/* this is to be called only when a listening sock creates its child.  It is
 * assumed by the function---the confirm is not duplicated, but rather it is
 * "passed on".
 */
int dccp_feat_clone(struct sock *oldsk, struct sock *newsk)
{
	struct dccp_minisock *olddmsk = dccp_msk(oldsk);
	struct dccp_minisock *newdmsk = dccp_msk(newsk);
	struct dccp_opt_pend *opt;
	int rc = 0;

	INIT_LIST_HEAD(&newdmsk->dccpms_pending);
	INIT_LIST_HEAD(&newdmsk->dccpms_conf);

	list_for_each_entry(opt, &olddmsk->dccpms_pending, dccpop_node) {
		struct dccp_opt_pend *newopt;
		/* copy the value of the option */
		u8 *val = kmemdup(opt->dccpop_val, opt->dccpop_len, GFP_ATOMIC);

		if (val == NULL)
			goto out_clean;

		newopt = kmemdup(opt, sizeof(*newopt), GFP_ATOMIC);
		if (newopt == NULL) {
			kfree(val);
			goto out_clean;
		}

		/* insert the option */
		newopt->dccpop_val = val;
		list_add_tail(&newopt->dccpop_node, &newdmsk->dccpms_pending);

		/* XXX what happens with backlogs and multiple connections at
		 * once...
		 */
		/* the master socket no longer needs to worry about confirms */
		opt->dccpop_sc = NULL; /* it's not a memleak---new socket has it */

		/* reset state for a new socket */
		opt->dccpop_conf = 0;
	}

	/* XXX not doing anything about the conf queue */

out:
	return rc;

out_clean:
	dccp_feat_clean(newdmsk);
	rc = -ENOMEM;
	goto out;
}

EXPORT_SYMBOL_GPL(dccp_feat_clone);

int dccp_feat_init(struct sock *sk)
{
	struct dccp_sock *dp = dccp_sk(sk);
	struct dccp_minisock *dmsk = dccp_msk(sk);
	int rc;

	INIT_LIST_HEAD(&dmsk->dccpms_pending);	/* XXX no longer used */
	INIT_LIST_HEAD(&dmsk->dccpms_conf);	/* XXX no longer used */

	/* CCID L */
	rc = __feat_register_sp(&dp->dccps_featneg, DCCPF_CCID, 1, 0,
				&dmsk->dccpms_tx_ccid, 1);
	if (rc)
		goto out;

	/* CCID R */
	rc = __feat_register_sp(&dp->dccps_featneg, DCCPF_CCID, 0, 0,
				&dmsk->dccpms_rx_ccid, 1);
	if (rc)
		goto out;

	/* Ack ratio */
	rc = __feat_register_nn(&dp->dccps_featneg, DCCPF_ACK_RATIO, 0,
				dmsk->dccpms_ack_ratio);
out:
	return rc;
}

EXPORT_SYMBOL_GPL(dccp_feat_init);

#ifdef CONFIG_IP_DCCP_DEBUG
const char *dccp_feat_typename(const u8 type)
{
	switch(type) {
	case DCCPO_CHANGE_L:  return("ChangeL");
	case DCCPO_CONFIRM_L: return("ConfirmL");
	case DCCPO_CHANGE_R:  return("ChangeR");
	case DCCPO_CONFIRM_R: return("ConfirmR");
	/* the following case must not appear in feature negotation  */
	default:	      dccp_pr_debug("unknown type %d [BUG!]\n", type);
	}
	return NULL;
}

EXPORT_SYMBOL_GPL(dccp_feat_typename);

const char *dccp_feat_name(const u8 feat)
{
	static const char *feature_names[] = {
		[DCCPF_RESERVED]	= "Reserved",
		[DCCPF_CCID]		= "CCID",
		[DCCPF_SHORT_SEQNOS]	= "Allow Short Seqnos",
		[DCCPF_SEQUENCE_WINDOW]	= "Sequence Window",
		[DCCPF_ECN_INCAPABLE]	= "ECN Incapable",
		[DCCPF_ACK_RATIO]	= "Ack Ratio",
		[DCCPF_SEND_ACK_VECTOR]	= "Send ACK Vector",
		[DCCPF_SEND_NDP_COUNT]	= "Send NDP Count",
		[DCCPF_MIN_CSUM_COVER]	= "Min. Csum Coverage",
		[DCCPF_DATA_CHECKSUM]	= "Send Data Checksum",
	};
	if (feat > DCCPF_DATA_CHECKSUM && feat < DCCPF_MIN_CCID_SPECIFIC)
		return feature_names[DCCPF_RESERVED];

	if (feat ==  DCCPF_SEND_LEV_RATE)
		return "Send Loss Event Rate";
	if (feat >= DCCPF_MIN_CCID_SPECIFIC)
		return "CCID-specific";

	return feature_names[feat];
}

EXPORT_SYMBOL_GPL(dccp_feat_name);
#endif /* CONFIG_IP_DCCP_DEBUG */
