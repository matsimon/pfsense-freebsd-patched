/*-
 * Copyright (c) 2001-2002 Packet Design, LLC.
 * All rights reserved.
 * 
 * Subject to the following obligations and disclaimer of warranty,
 * use and redistribution of this software, in source or object code
 * forms, with or without modifications are expressly permitted by
 * Packet Design; provided, however, that:
 * 
 *    (i)  Any and all reproductions of the source or object code
 *         must include the copyright notice above and the following
 *         disclaimer of warranties; and
 *    (ii) No rights are granted, in any manner or form, to use
 *         Packet Design trademarks, including the mark "PACKET DESIGN"
 *         on advertising, endorsements, or otherwise except as such
 *         appears in the above copyright notice or in the software.
 * 
 * THIS SOFTWARE IS BEING PROVIDED BY PACKET DESIGN "AS IS", AND
 * TO THE MAXIMUM EXTENT PERMITTED BY LAW, PACKET DESIGN MAKES NO
 * REPRESENTATIONS OR WARRANTIES, EXPRESS OR IMPLIED, REGARDING
 * THIS SOFTWARE, INCLUDING WITHOUT LIMITATION, ANY AND ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR NON-INFRINGEMENT.  PACKET DESIGN DOES NOT WARRANT, GUARANTEE,
 * OR MAKE ANY REPRESENTATIONS REGARDING THE USE OF, OR THE RESULTS
 * OF THE USE OF THIS SOFTWARE IN TERMS OF ITS CORRECTNESS, ACCURACY,
 * RELIABILITY OR OTHERWISE.  IN NO EVENT SHALL PACKET DESIGN BE
 * LIABLE FOR ANY DAMAGES RESULTING FROM OR ARISING OUT OF ANY USE
 * OF THIS SOFTWARE, INCLUDING WITHOUT LIMITATION, ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, PUNITIVE, OR CONSEQUENTIAL
 * DAMAGES, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES, LOSS OF
 * USE, DATA OR PROFITS, HOWEVER CAUSED AND UNDER ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF PACKET DESIGN IS ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * Author: Archie Cobbs <archie@freebsd.org>
 *
 * $FreeBSD$
 */

/*
 * L2TP netgraph node type.
 *
 * This node type implements the lower layer of the
 * L2TP protocol as specified in RFC 2661.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/time.h>
#include <sys/conf.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/errno.h>
#include <sys/libkern.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>
#include <netgraph/ng_l2tp.h>

#ifdef NG_SEPARATE_MALLOC
MALLOC_DEFINE(M_NETGRAPH_L2TP, "netgraph_l2tp", "netgraph l2tp node");
#else
#define M_NETGRAPH_L2TP M_NETGRAPH
#endif

/* L2TP header format (first 2 bytes only) */
#define L2TP_HDR_CTRL		0x8000			/* control packet */
#define L2TP_HDR_LEN		0x4000			/* has length field */
#define L2TP_HDR_SEQ		0x0800			/* has ns, nr fields */
#define L2TP_HDR_OFF		0x0200			/* has offset field */
#define L2TP_HDR_PRIO		0x0100			/* give priority */
#define L2TP_HDR_VERS_MASK	0x000f			/* version field mask */
#define L2TP_HDR_VERSION	0x0002			/* version field */

/* Bits that must be zero or one in first two bytes of header */
#define L2TP_CTRL_0BITS		0x030d			/* ctrl: must be 0 */
#define L2TP_CTRL_1BITS		0xc802			/* ctrl: must be 1 */
#define L2TP_DATA_0BITS		0x800d			/* data: must be 0 */
#define L2TP_DATA_1BITS		0x0002			/* data: must be 1 */

/* Standard xmit ctrl and data header bits */
#define L2TP_CTRL_HDR		(L2TP_HDR_CTRL | L2TP_HDR_LEN \
				    | L2TP_HDR_SEQ | L2TP_HDR_VERSION)
#define L2TP_DATA_HDR		(L2TP_HDR_VERSION)	/* optional: len, seq */

/* Some hard coded values */
#define L2TP_MAX_XWIN		16			/* my max xmit window */
#define L2TP_MAX_REXMIT		5			/* default max rexmit */
#define L2TP_MAX_REXMIT_TO	30			/* default rexmit to */
#define L2TP_DELAYED_ACK	((hz + 19) / 20)	/* delayed ack: 50 ms */

/* Default data sequence number configuration for new sessions */
#define L2TP_CONTROL_DSEQ	1			/* we are the lns */
#define L2TP_ENABLE_DSEQ	1			/* enable data seq # */

/* Compare sequence numbers using circular math */
#define L2TP_SEQ_DIFF(x, y)	((int)((int16_t)(x) - (int16_t)(y)))

/*
 * Sequence number state
 *
 * Invariants:
 *    - If cwnd < ssth, we're doing slow start, otherwise congestion avoidance
 *    - The number of unacknowledged xmit packets is (ns - rack) <= seq->wmax
 *    - The first (ns - rack) mbuf's in xwin[] array are copies of these
 *	unacknowledged packets; the remainder of xwin[] consists first of
 *	zero or more further untransmitted packets in the transmit queue
 *    - We try to keep the peer's receive window as full as possible.
 *	Therefore, (i < cwnd && xwin[i] != NULL) implies (ns - rack) > i.
 *    - rack_timer is running iff (ns - rack) > 0 (unack'd xmit'd pkts)
 *    - If xack != nr, there are unacknowledged recv packet(s) (delayed ack)
 *    - xack_timer is running iff xack != nr (unack'd rec'd pkts)
 */
struct l2tp_seq {
	u_int16_t		ns;		/* next xmit seq we send */
	u_int16_t		nr;		/* next recv seq we expect */
	u_int16_t		rack;		/* last 'nr' we rec'd */
	u_int16_t		xack;		/* last 'nr' we sent */
	u_int16_t		wmax;		/* peer's max recv window */
	u_int16_t		cwnd;		/* current congestion window */
	u_int16_t		ssth;		/* slow start threshold */
	u_int16_t		acks;		/* # consecutive acks rec'd */
	u_int16_t		rexmits;	/* # retransmits sent */
	struct callout		rack_timer;	/* retransmit timer */
	struct callout		xack_timer;	/* delayed ack timer */
	struct mbuf		*xwin[L2TP_MAX_XWIN];	/* transmit window */
};

/* Node private data */
struct ng_l2tp_private {
	node_p			node;		/* back pointer to node */
	hook_p			ctrl;		/* hook to upper layers */
	hook_p			lower;		/* hook to lower layers */
	struct ng_l2tp_config	conf;		/* node configuration */
	struct ng_l2tp_stats	stats;		/* node statistics */
	struct l2tp_seq		seq;		/* ctrl sequence number state */
	ng_ID_t			ftarget;	/* failure message target */
};
typedef struct ng_l2tp_private *priv_p;

/* Hook private data (data session hooks only) */
struct ng_l2tp_hook_private {
	struct ng_l2tp_sess_config	conf;	/* hook/session config */
	struct ng_l2tp_session_stats	stats;	/* per sessions statistics */
	u_int16_t			ns;	/* data ns sequence number */
	u_int16_t			nr;	/* data nr sequence number */
};
typedef struct ng_l2tp_hook_private *hookpriv_p;

/* Netgraph node methods */
static ng_constructor_t	ng_l2tp_constructor;
static ng_rcvmsg_t	ng_l2tp_rcvmsg;
static ng_shutdown_t	ng_l2tp_shutdown;
static ng_newhook_t	ng_l2tp_newhook;
static ng_rcvdata_t	ng_l2tp_rcvdata;
static ng_disconnect_t	ng_l2tp_disconnect;

/* Internal functions */
static int	ng_l2tp_recv_lower(node_p node, item_p item);
static int	ng_l2tp_recv_ctrl(node_p node, item_p item);
static int	ng_l2tp_recv_data(node_p node, item_p item, hookpriv_p hpriv);

static int	ng_l2tp_xmit_ctrl(priv_p priv, struct mbuf *m, u_int16_t ns);

static void	ng_l2tp_seq_init(priv_p priv);
static int	ng_l2tp_seq_set(priv_p priv,
			const struct ng_l2tp_seq_config *conf);
static int	ng_l2tp_seq_adjust(priv_p priv,
			const struct ng_l2tp_config *conf);
static void	ng_l2tp_seq_reset(priv_p priv);
static void	ng_l2tp_seq_failure(priv_p priv);
static void	ng_l2tp_seq_recv_nr(priv_p priv, u_int16_t nr);
static int	ng_l2tp_seq_recv_ns(priv_p priv, u_int16_t ns);
static void	ng_l2tp_seq_xack_timeout(node_p node, hook_p hook,
		    void *arg1, int arg2);
static void	ng_l2tp_seq_rack_timeout(node_p node, hook_p hook,
		    void *arg1, int arg2);

static ng_fn_eachhook	ng_l2tp_find_session;
static ng_fn_eachhook	ng_l2tp_reset_session;

#ifdef INVARIANTS
static void	ng_l2tp_seq_check(struct l2tp_seq *seq);
#endif

/* Parse type for struct ng_l2tp_seq_config. */
static const struct ng_parse_struct_field
	ng_l2tp_seq_config_fields[] = NG_L2TP_SEQ_CONFIG_TYPE_INFO;
static const struct ng_parse_type ng_l2tp_seq_config_type = {
	&ng_parse_struct_type,
	&ng_l2tp_seq_config_fields
};

/* Parse type for struct ng_l2tp_config */
static const struct ng_parse_struct_field
	ng_l2tp_config_type_fields[] = NG_L2TP_CONFIG_TYPE_INFO;
static const struct ng_parse_type ng_l2tp_config_type = {
	&ng_parse_struct_type,
	&ng_l2tp_config_type_fields,
};

/* Parse type for struct ng_l2tp_sess_config */
static const struct ng_parse_struct_field
	ng_l2tp_sess_config_type_fields[] = NG_L2TP_SESS_CONFIG_TYPE_INFO;
static const struct ng_parse_type ng_l2tp_sess_config_type = {
	&ng_parse_struct_type,
	&ng_l2tp_sess_config_type_fields,
};

/* Parse type for struct ng_l2tp_stats */
static const struct ng_parse_struct_field
	ng_l2tp_stats_type_fields[] = NG_L2TP_STATS_TYPE_INFO;
static const struct ng_parse_type ng_l2tp_stats_type = {
	&ng_parse_struct_type,
	&ng_l2tp_stats_type_fields
};

/* Parse type for struct ng_l2tp_session_stats. */
static const struct ng_parse_struct_field
	ng_l2tp_session_stats_type_fields[] = NG_L2TP_SESSION_STATS_TYPE_INFO;
static const struct ng_parse_type ng_l2tp_session_stats_type = {
	&ng_parse_struct_type,
	&ng_l2tp_session_stats_type_fields
};

/* List of commands and how to convert arguments to/from ASCII */
static const struct ng_cmdlist ng_l2tp_cmdlist[] = {
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_SET_CONFIG,
	  "setconfig",
	  &ng_l2tp_config_type,
	  NULL
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_GET_CONFIG,
	  "getconfig",
	  NULL,
	  &ng_l2tp_config_type
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_SET_SESS_CONFIG,
	  "setsessconfig",
	  &ng_l2tp_sess_config_type,
	  NULL
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_GET_SESS_CONFIG,
	  "getsessconfig",
	  &ng_parse_hint16_type,
	  &ng_l2tp_sess_config_type
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_GET_STATS,
	  "getstats",
	  NULL,
	  &ng_l2tp_stats_type
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_CLR_STATS,
	  "clrstats",
	  NULL,
	  NULL
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_GETCLR_STATS,
	  "getclrstats",
	  NULL,
	  &ng_l2tp_stats_type
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_GET_SESSION_STATS,
	  "getsessstats",
	  &ng_parse_int16_type,
	  &ng_l2tp_session_stats_type
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_CLR_SESSION_STATS,
	  "clrsessstats",
	  &ng_parse_int16_type,
	  NULL
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_GETCLR_SESSION_STATS,
	  "getclrsessstats",
	  &ng_parse_int16_type,
	  &ng_l2tp_session_stats_type
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_ACK_FAILURE,
	  "ackfailure",
	  NULL,
	  NULL
	},
	{
	  NGM_L2TP_COOKIE,
	  NGM_L2TP_SET_SEQ,
	  "setsequence",
	  &ng_l2tp_seq_config_type,
	  NULL
	},
	{ 0 }
};

/* Node type descriptor */
static struct ng_type ng_l2tp_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		NG_L2TP_NODE_TYPE,
	.constructor =	ng_l2tp_constructor,
	.rcvmsg =	ng_l2tp_rcvmsg,
	.shutdown =	ng_l2tp_shutdown,
	.newhook =	ng_l2tp_newhook,
	.rcvdata =	ng_l2tp_rcvdata,
	.disconnect =	ng_l2tp_disconnect,
	.cmdlist =	ng_l2tp_cmdlist,
};
NETGRAPH_INIT(l2tp, &ng_l2tp_typestruct);

/* Sequence number state sanity checking */
#ifdef INVARIANTS
#define L2TP_SEQ_CHECK(seq)	ng_l2tp_seq_check(seq)
#else
#define L2TP_SEQ_CHECK(x)	do { } while (0)
#endif

/* memmove macro */
#define memmove(d, s, l)	bcopy(s, d, l)

/* Whether to use m_copypacket() or m_dup() */
#define L2TP_COPY_MBUF		m_copypacket

/************************************************************************
			NETGRAPH NODE STUFF
************************************************************************/

/*
 * Node type constructor
 */
static int
ng_l2tp_constructor(node_p node)
{
	priv_p priv;

	/* Allocate private structure */
	MALLOC(priv, priv_p, sizeof(*priv), M_NETGRAPH_L2TP, M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return (ENOMEM);
	NG_NODE_SET_PRIVATE(node, priv);
	priv->node = node;

	/* Apply a semi-reasonable default configuration */
	priv->conf.peer_win = 1;
	priv->conf.rexmit_max = L2TP_MAX_REXMIT;
	priv->conf.rexmit_max_to = L2TP_MAX_REXMIT_TO;

	/* Initialize sequence number state */
	ng_l2tp_seq_init(priv);

	/* Done */
	return (0);
}

/*
 * Give our OK for a hook to be added.
 */
static int
ng_l2tp_newhook(node_p node, hook_p hook, const char *name)
{
	const priv_p priv = NG_NODE_PRIVATE(node);

	/* Check hook name */
	if (strcmp(name, NG_L2TP_HOOK_CTRL) == 0) {
		if (priv->ctrl != NULL)
			return (EISCONN);
		priv->ctrl = hook;
	} else if (strcmp(name, NG_L2TP_HOOK_LOWER) == 0) {
		if (priv->lower != NULL)
			return (EISCONN);
		priv->lower = hook;
	} else {
		static const char hexdig[16] = "0123456789abcdef";
		u_int16_t session_id;
		hookpriv_p hpriv;
		const char *hex;
		int i;
		int j;

		/* Parse hook name to get session ID */
		if (strncmp(name, NG_L2TP_HOOK_SESSION_P,
		    sizeof(NG_L2TP_HOOK_SESSION_P) - 1) != 0)
			return (EINVAL);
		hex = name + sizeof(NG_L2TP_HOOK_SESSION_P) - 1;
		for (session_id = i = 0; i < 4; i++) {
			for (j = 0; j < 16 && hex[i] != hexdig[j]; j++);
			if (j == 16)
				return (EINVAL);
			session_id = (session_id << 4) | j;
		}
		if (hex[i] != '\0')
			return (EINVAL);

		/* Create hook private structure */
		MALLOC(hpriv, hookpriv_p,
		    sizeof(*hpriv), M_NETGRAPH_L2TP, M_NOWAIT | M_ZERO);
		if (hpriv == NULL)
			return (ENOMEM);
		hpriv->conf.session_id = htons(session_id);
		hpriv->conf.control_dseq = L2TP_CONTROL_DSEQ;
		hpriv->conf.enable_dseq = L2TP_ENABLE_DSEQ;
		NG_HOOK_SET_PRIVATE(hook, hpriv);
	}

	/* Done */
	return (0);
}

/*
 * Receive a control message.
 */
static int
ng_l2tp_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct ng_mesg *resp = NULL;
	struct ng_mesg *msg;
	int error = 0;

	NGI_GET_MSG(item, msg);
	switch (msg->header.typecookie) {
	case NGM_L2TP_COOKIE:
		switch (msg->header.cmd) {
		case NGM_L2TP_SET_CONFIG:
		    {
			struct ng_l2tp_config *const conf =
				(struct ng_l2tp_config *)msg->data;

			/* Check for invalid or illegal config */
			if (msg->header.arglen != sizeof(*conf)) {
				error = EINVAL;
				break;
			}
			conf->enabled = !!conf->enabled;
			conf->match_id = !!conf->match_id;
			conf->tunnel_id = htons(conf->tunnel_id);
			conf->peer_id = htons(conf->peer_id);
			if (priv->conf.enabled
			    && ((priv->conf.tunnel_id != 0
			       && conf->tunnel_id != priv->conf.tunnel_id)
			      || ((priv->conf.peer_id != 0
			       && conf->peer_id != priv->conf.peer_id)))) {
				error = EBUSY;
				break;
			}

			/* Save calling node as failure target */
			priv->ftarget = NGI_RETADDR(item);

			/* Adjust sequence number state */
			if ((error = ng_l2tp_seq_adjust(priv, conf)) != 0)
				break;

			/* Update node's config */
			priv->conf = *conf;
			break;
		    }
		case NGM_L2TP_GET_CONFIG:
		    {
			struct ng_l2tp_config *conf;

			NG_MKRESPONSE(resp, msg, sizeof(*conf), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			conf = (struct ng_l2tp_config *)resp->data;
			*conf = priv->conf;

			/* Put ID's in host order */
			conf->tunnel_id = ntohs(conf->tunnel_id);
			conf->peer_id = ntohs(conf->peer_id);
			break;
		    }
		case NGM_L2TP_SET_SESS_CONFIG:
		    {
			struct ng_l2tp_sess_config *const conf =
			    (struct ng_l2tp_sess_config *)msg->data;
			hookpriv_p hpriv;
			hook_p hook;

			/* Check for invalid or illegal config. */
			if (msg->header.arglen != sizeof(*conf)) {
				error = EINVAL;
				break;
			}

			/* Put ID's in network order */
			conf->session_id = htons(conf->session_id);
			conf->peer_id = htons(conf->peer_id);

			/* Find matching hook */
			NG_NODE_FOREACH_HOOK(node, ng_l2tp_find_session,
			    (void *)(uintptr_t)conf->session_id, hook);
			if (hook == NULL) {
				error = ENOENT;
				break;
			}
			hpriv = NG_HOOK_PRIVATE(hook);

			/* Update hook's config */
			hpriv->conf = *conf;
			break;
		    }
		case NGM_L2TP_GET_SESS_CONFIG:
		    {
			struct ng_l2tp_sess_config *conf;
			u_int16_t session_id;
			hookpriv_p hpriv;
			hook_p hook;

			/* Get session ID */
			if (msg->header.arglen != sizeof(session_id)) {
				error = EINVAL;
				break;
			}
			memcpy(&session_id, msg->data, 2);
			session_id = htons(session_id);

			/* Find matching hook */
			NG_NODE_FOREACH_HOOK(node, ng_l2tp_find_session,
			    (void *)(uintptr_t)session_id, hook);
			if (hook == NULL) {
				error = ENOENT;
				break;
			}
			hpriv = NG_HOOK_PRIVATE(hook);

			/* Send response */
			NG_MKRESPONSE(resp, msg, sizeof(hpriv->conf), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			conf = (struct ng_l2tp_sess_config *)resp->data;
			*conf = hpriv->conf;

			/* Put ID's in host order */
			conf->session_id = ntohs(conf->session_id);
			conf->peer_id = ntohs(conf->peer_id);
			break;
		    }
		case NGM_L2TP_GET_STATS:
		case NGM_L2TP_CLR_STATS:
		case NGM_L2TP_GETCLR_STATS:
		    {
			if (msg->header.cmd != NGM_L2TP_CLR_STATS) {
				NG_MKRESPONSE(resp, msg,
				    sizeof(priv->stats), M_NOWAIT);
				if (resp == NULL) {
					error = ENOMEM;
					break;
				}
				memcpy(resp->data,
				    &priv->stats, sizeof(priv->stats));
			}
			if (msg->header.cmd != NGM_L2TP_GET_STATS)
				memset(&priv->stats, 0, sizeof(priv->stats));
			break;
		    }
		case NGM_L2TP_GET_SESSION_STATS:
		case NGM_L2TP_CLR_SESSION_STATS:
		case NGM_L2TP_GETCLR_SESSION_STATS:
		    {
			uint16_t session_id;
			hookpriv_p hpriv;
			hook_p hook;

			/* Get session ID. */
			if (msg->header.arglen != sizeof(session_id)) {
				error = EINVAL;
				break;
			}
			bcopy(msg->data, &session_id, sizeof(uint16_t));
			session_id = htons(session_id);

			/* Find matching hook. */
			NG_NODE_FOREACH_HOOK(node, ng_l2tp_find_session,
			    (void *)(uintptr_t)session_id, hook);
			if (hook == NULL) {
				error = ENOENT;
				break;
			}
			hpriv = NG_HOOK_PRIVATE(hook);

			if (msg->header.cmd != NGM_L2TP_CLR_SESSION_STATS) {
				NG_MKRESPONSE(resp, msg,
				    sizeof(hpriv->stats), M_NOWAIT);
				if (resp == NULL) {
					error = ENOMEM;
					break;
				}
				bcopy(&hpriv->stats, resp->data,
					sizeof(hpriv->stats));
			}
			if (msg->header.cmd != NGM_L2TP_GET_SESSION_STATS)
				bzero(&hpriv->stats, sizeof(hpriv->stats));
			break;
		    }
		case NGM_L2TP_SET_SEQ:
		    {
			struct ng_l2tp_seq_config *const conf =
				(struct ng_l2tp_seq_config *)msg->data;

			/* Check for invalid or illegal seq config. */
			if (msg->header.arglen != sizeof(*conf)) {
				error = EINVAL;
				break;
			}
			conf->ns = htons(conf->ns);
			conf->nr = htons(conf->nr);
			conf->rack = htons(conf->rack);
			conf->xack = htons(conf->xack);

			/* Set sequence numbers. */
			error = ng_l2tp_seq_set(priv, conf);
			break;
		    }
		default:
			error = EINVAL;
			break;
		}
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Done */
	NG_RESPOND_MSG(error, node, item, resp);
	NG_FREE_MSG(msg);
	return (error);
}

/*
 * Receive incoming data on a hook.
 */
static int
ng_l2tp_rcvdata(hook_p hook, item_p item)
{
	const node_p node = NG_HOOK_NODE(hook);
	const priv_p priv = NG_NODE_PRIVATE(node);
	int error;

	/* Sanity check */
	L2TP_SEQ_CHECK(&priv->seq);

	/* If not configured, reject */
	if (!priv->conf.enabled) {
		NG_FREE_ITEM(item);
		return (ENXIO);
	}

	/* Handle incoming frame from below */
	if (hook == priv->lower) {
		error = ng_l2tp_recv_lower(node, item);
		goto done;
	}

	/* Handle outgoing control frame */
	if (hook == priv->ctrl) {
		error = ng_l2tp_recv_ctrl(node, item);
		goto done;
	}

	/* Handle outgoing data frame */
	error = ng_l2tp_recv_data(node, item, NG_HOOK_PRIVATE(hook));

done:
	/* Done */
	L2TP_SEQ_CHECK(&priv->seq);
	return (error);
}

/*
 * Destroy node
 */
static int
ng_l2tp_shutdown(node_p node)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct l2tp_seq *const seq = &priv->seq;

	/* Sanity check */
	L2TP_SEQ_CHECK(seq);

	/* Reset sequence number state */
	ng_l2tp_seq_reset(priv);

	/* Free private data if neither timer is running */
	ng_uncallout(&seq->rack_timer, node);
	ng_uncallout(&seq->xack_timer, node);

	FREE(priv, M_NETGRAPH_L2TP);

	/* Unref node */
	NG_NODE_UNREF(node);
	return (0);
}

/*
 * Hook disconnection
 */
static int
ng_l2tp_disconnect(hook_p hook)
{
	const node_p node = NG_HOOK_NODE(hook);
	const priv_p priv = NG_NODE_PRIVATE(node);

	/* Zero out hook pointer */
	if (hook == priv->ctrl)
		priv->ctrl = NULL;
	else if (hook == priv->lower)
		priv->lower = NULL;
	else {
		FREE(NG_HOOK_PRIVATE(hook), M_NETGRAPH_L2TP);
		NG_HOOK_SET_PRIVATE(hook, NULL);
	}

	/* Go away if no longer connected to anything */
	if (NG_NODE_NUMHOOKS(node) == 0 && NG_NODE_IS_VALID(node))
		ng_rmnode_self(node);
	return (0);
}

/*************************************************************************
			INTERNAL FUNCTIONS
*************************************************************************/

/*
 * Find the hook with a given session ID.
 */
static int
ng_l2tp_find_session(hook_p hook, void *arg)
{
	const hookpriv_p hpriv = NG_HOOK_PRIVATE(hook);
	const u_int16_t sid = (u_int16_t)(uintptr_t)arg;

	if (hpriv == NULL || hpriv->conf.session_id != sid)
		return (-1);
	return (0);
}

/*
 * Reset a hook's session state.
 */
static int
ng_l2tp_reset_session(hook_p hook, void *arg)
{
	const hookpriv_p hpriv = NG_HOOK_PRIVATE(hook);

	if (hpriv != NULL) {
		hpriv->conf.control_dseq = 0;
		hpriv->conf.enable_dseq = 0;
		bzero(&hpriv->conf, sizeof(struct ng_l2tp_session_stats));
		hpriv->nr = 0;
		hpriv->ns = 0;
	}
	return (-1);
}

/*
 * Handle an incoming frame from below.
 */
static int
ng_l2tp_recv_lower(node_p node, item_p item)
{
	static const u_int16_t req_bits[2][2] = {
		{ L2TP_DATA_0BITS, L2TP_DATA_1BITS },
		{ L2TP_CTRL_0BITS, L2TP_CTRL_1BITS },
	};
	const priv_p priv = NG_NODE_PRIVATE(node);
	hookpriv_p hpriv = NULL;
	hook_p hook = NULL;
	u_int16_t ids[2];
	struct mbuf *m;
	u_int16_t hdr;
	u_int16_t ns;
	u_int16_t nr;
	int is_ctrl;
	int error;
	int len, plen;

	/* Grab mbuf */
	NGI_GET_M(item, m);

	/* Remember full packet length; needed for per session accounting. */
	plen = m->m_pkthdr.len;

	/* Update stats */
	priv->stats.recvPackets++;
	priv->stats.recvOctets += plen;

	/* Get initial header */
	if (m->m_pkthdr.len < 6) {
		priv->stats.recvRunts++;
		NG_FREE_ITEM(item);
		NG_FREE_M(m);
		return (EINVAL);
	}
	if (m->m_len < 2 && (m = m_pullup(m, 2)) == NULL) {
		priv->stats.memoryFailures++;
		NG_FREE_ITEM(item);
		return (EINVAL);
	}
	hdr = ntohs(*mtod(m, u_int16_t *));
	m_adj(m, 2);

	/* Check required header bits and minimum length */
	is_ctrl = (hdr & L2TP_HDR_CTRL) != 0;
	if ((hdr & req_bits[is_ctrl][0]) != 0
	    || (~hdr & req_bits[is_ctrl][1]) != 0) {
		priv->stats.recvInvalid++;
		NG_FREE_ITEM(item);
		NG_FREE_M(m);
		return (EINVAL);
	}
	if (m->m_pkthdr.len < 4				/* tunnel, session id */
	    + (2 * ((hdr & L2TP_HDR_LEN) != 0))		/* length field */
	    + (4 * ((hdr & L2TP_HDR_SEQ) != 0))		/* seq # fields */
	    + (2 * ((hdr & L2TP_HDR_OFF) != 0))) {	/* offset field */
		priv->stats.recvRunts++;
		NG_FREE_ITEM(item);
		NG_FREE_M(m);
		return (EINVAL);
	}

	/* Get and validate length field if present */
	if ((hdr & L2TP_HDR_LEN) != 0) {
		if (m->m_len < 2 && (m = m_pullup(m, 2)) == NULL) {
			priv->stats.memoryFailures++;
			NG_FREE_ITEM(item);
			return (EINVAL);
		}
		len = (u_int16_t)ntohs(*mtod(m, u_int16_t *)) - 4;
		m_adj(m, 2);
		if (len < 0 || len > m->m_pkthdr.len) {
			priv->stats.recvInvalid++;
			NG_FREE_ITEM(item);
			NG_FREE_M(m);
			return (EINVAL);
		}
		if (len < m->m_pkthdr.len)		/* trim extra bytes */
			m_adj(m, -(m->m_pkthdr.len - len));
	}

	/* Get tunnel ID and session ID */
	if (m->m_len < 4 && (m = m_pullup(m, 4)) == NULL) {
		priv->stats.memoryFailures++;
		NG_FREE_ITEM(item);
		return (EINVAL);
	}
	memcpy(ids, mtod(m, u_int16_t *), 4);
	m_adj(m, 4);

	/* Check tunnel ID */
	if (ids[0] != priv->conf.tunnel_id
	    && (priv->conf.match_id || ids[0] != 0)) {
		priv->stats.recvWrongTunnel++;
		NG_FREE_ITEM(item);
		NG_FREE_M(m);
		return (EADDRNOTAVAIL);
	}

	/* Check session ID (for data packets only) */
	if ((hdr & L2TP_HDR_CTRL) == 0) {
		NG_NODE_FOREACH_HOOK(node, ng_l2tp_find_session,
		    (void *)(uintptr_t)ids[1], hook);
		if (hook == NULL) {
			priv->stats.recvUnknownSID++;
			NG_FREE_ITEM(item);
			NG_FREE_M(m);
			return (ENOTCONN);
		}
		hpriv = NG_HOOK_PRIVATE(hook);
	}

	/* Get Ns, Nr fields if present */
	if ((hdr & L2TP_HDR_SEQ) != 0) {
		if (m->m_len < 4 && (m = m_pullup(m, 4)) == NULL) {
			priv->stats.memoryFailures++;
			NG_FREE_ITEM(item);
			return (EINVAL);
		}
		memcpy(&ns, &mtod(m, u_int16_t *)[0], 2);
		ns = ntohs(ns);
		memcpy(&nr, &mtod(m, u_int16_t *)[1], 2);
		nr = ntohs(nr);
		m_adj(m, 4);
	}

	/* Strip offset padding if present */
	if ((hdr & L2TP_HDR_OFF) != 0) {
		u_int16_t offset;

		/* Get length of offset padding */
		if (m->m_len < 2 && (m = m_pullup(m, 2)) == NULL) {
			priv->stats.memoryFailures++;
			NG_FREE_ITEM(item);
			return (EINVAL);
		}
		memcpy(&offset, mtod(m, u_int16_t *), 2);
		offset = ntohs(offset);

		/* Trim offset padding */
		if ((2+offset) > m->m_pkthdr.len) {
			priv->stats.recvInvalid++;
			NG_FREE_ITEM(item);
			NG_FREE_M(m);
			return (EINVAL);
		}
		m_adj(m, 2+offset);
	}

	/* Handle control packets */
	if ((hdr & L2TP_HDR_CTRL) != 0) {

		/* Handle receive ack sequence number Nr */
		ng_l2tp_seq_recv_nr(priv, nr);

		/* Discard ZLB packets */
		if (m->m_pkthdr.len == 0) {
			priv->stats.recvZLBs++;
			NG_FREE_ITEM(item);
			NG_FREE_M(m);
			return (0);
		}

		/*
		 * Prepend session ID to packet here: we don't want to accept
		 * the send sequence number Ns if we have to drop the packet
		 * later because of a memory error, because then the upper
		 * layer would never get the packet.
		 */
		M_PREPEND(m, 2, M_DONTWAIT);
		if (m == NULL) {
			priv->stats.memoryFailures++;
			NG_FREE_ITEM(item);
			return (ENOBUFS);
		}
		memcpy(mtod(m, u_int16_t *), &ids[1], 2);

		/* Now handle send sequence number */
		if (ng_l2tp_seq_recv_ns(priv, ns) == -1) {
			NG_FREE_ITEM(item);
			NG_FREE_M(m);
			return (0);
		}

		/* Deliver packet to upper layers */
		NG_FWD_NEW_DATA(error, item, priv->ctrl, m);
		return (error);
	}

	/* Per session packet, account it. */
	hpriv->stats.recvPackets++;
	hpriv->stats.recvOctets += plen;

	/* Follow peer's lead in data sequencing, if configured to do so */
	if (!hpriv->conf.control_dseq)
		hpriv->conf.enable_dseq = ((hdr & L2TP_HDR_SEQ) != 0);

	/* Handle data sequence numbers if present and enabled */
	if ((hdr & L2TP_HDR_SEQ) != 0) {
		if (hpriv->conf.enable_dseq
		    && L2TP_SEQ_DIFF(ns, hpriv->nr) < 0) {
			NG_FREE_ITEM(item);	/* duplicate or out of order */
			NG_FREE_M(m);
			priv->stats.recvDataDrops++;
			return (0);
		}
		hpriv->nr = ns + 1;
	}

	/* Drop empty data packets */
	if (m->m_pkthdr.len == 0) {
		NG_FREE_ITEM(item);
		NG_FREE_M(m);
		return (0);
	}

	/* Deliver data */
	NG_FWD_NEW_DATA(error, item, hook, m);
	return (error);
}

/*
 * Handle an outgoing control frame.
 */
static int
ng_l2tp_recv_ctrl(node_p node, item_p item)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct l2tp_seq *const seq = &priv->seq;
	struct mbuf *m;
	int i;

	/* Grab mbuf and discard other stuff XXX */
	NGI_GET_M(item, m);
	NG_FREE_ITEM(item);

	/* Packet should have session ID prepended */
	if (m->m_pkthdr.len < 2) {
		priv->stats.xmitInvalid++;
		m_freem(m);
		return (EINVAL);
	}

	/* Check max length */
	if (m->m_pkthdr.len >= 0x10000 - 14) {
		priv->stats.xmitTooBig++;
		m_freem(m);
		return (EOVERFLOW);
	}

	/* Find next empty slot in transmit queue */
	for (i = 0; i < L2TP_MAX_XWIN && seq->xwin[i] != NULL; i++);
	if (i == L2TP_MAX_XWIN) {
		priv->stats.xmitDrops++;
		m_freem(m);
		return (ENOBUFS);
	}
	seq->xwin[i] = m;

	/* Sanity check receive ack timer state */
	KASSERT((i == 0) ^ callout_active(&seq->rack_timer),
	    ("%s: xwin %d full but rack timer %s running",
	    __func__, i, callout_active(&seq->rack_timer) ? "" : "not "));

	/* If peer's receive window is already full, nothing else to do */
	if (i >= seq->cwnd)
		return (0);

	/* Start retransmit timer if not already running */
	if (!callout_active(&seq->rack_timer))
		ng_callout(&seq->rack_timer, node, NULL,
		    hz, ng_l2tp_seq_rack_timeout, NULL, 0);

	/* Copy packet */
	if ((m = L2TP_COPY_MBUF(seq->xwin[i], M_DONTWAIT)) == NULL) {
		priv->stats.memoryFailures++;
		return (ENOBUFS);
	}

	/* Send packet and increment xmit sequence number */
	return (ng_l2tp_xmit_ctrl(priv, m, seq->ns++));
}

/*
 * Handle an outgoing data frame.
 */
static int
ng_l2tp_recv_data(node_p node, item_p item, hookpriv_p hpriv)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct mbuf *m;
	u_int16_t hdr;
	int error;
	int i = 1;

	/* Get mbuf */
	NGI_GET_M(item, m);

	/* Check max length */
	if (m->m_pkthdr.len >= 0x10000 - 12) {
		priv->stats.xmitDataTooBig++;
		NG_FREE_ITEM(item);
		NG_FREE_M(m);
		return (EOVERFLOW);
	}

	/* Prepend L2TP header */
	M_PREPEND(m, 6
	    + (2 * (hpriv->conf.include_length != 0))
	    + (4 * (hpriv->conf.enable_dseq != 0)),
	    M_DONTWAIT);
	if (m == NULL) {
		priv->stats.memoryFailures++;
		NG_FREE_ITEM(item);
		return (ENOBUFS);
	}
	hdr = L2TP_DATA_HDR;
	if (hpriv->conf.include_length) {
		hdr |= L2TP_HDR_LEN;
		mtod(m, u_int16_t *)[i++] = htons(m->m_pkthdr.len);
	}
	mtod(m, u_int16_t *)[i++] = priv->conf.peer_id;
	mtod(m, u_int16_t *)[i++] = hpriv->conf.peer_id;
	if (hpriv->conf.enable_dseq) {
		hdr |= L2TP_HDR_SEQ;
		mtod(m, u_int16_t *)[i++] = htons(hpriv->ns);
		mtod(m, u_int16_t *)[i++] = htons(hpriv->nr);
		hpriv->ns++;
	}
	mtod(m, u_int16_t *)[0] = htons(hdr);

	/* Update per session stats. */
	hpriv->stats.xmitPackets++;
	hpriv->stats.xmitOctets += m->m_pkthdr.len;

	/* And the global one. */
	priv->stats.xmitPackets++;
	priv->stats.xmitOctets += m->m_pkthdr.len;

	/* Send packet */
	NG_FWD_NEW_DATA(error, item, priv->lower, m);
	return (error);
}

/*
 * Send a message to our controlling node that we've failed.
 */
static void
ng_l2tp_seq_failure(priv_p priv)
{
	struct ng_mesg *msg;
	int error;

	NG_MKMESSAGE(msg, NGM_L2TP_COOKIE, NGM_L2TP_ACK_FAILURE, 0, M_NOWAIT);
	if (msg == NULL)
		return;
	NG_SEND_MSG_ID(error, priv->node, msg, priv->ftarget, 0);
}

/************************************************************************
			SEQUENCE NUMBER HANDLING
************************************************************************/

/*
 * Initialize sequence number state.
 */
static void
ng_l2tp_seq_init(priv_p priv)
{
	struct l2tp_seq *const seq = &priv->seq;

	KASSERT(priv->conf.peer_win >= 1,
	    ("%s: peer_win is zero", __func__));
	memset(seq, 0, sizeof(*seq));
	seq->cwnd = 1;
	seq->wmax = priv->conf.peer_win;
	if (seq->wmax > L2TP_MAX_XWIN)
		seq->wmax = L2TP_MAX_XWIN;
	seq->ssth = seq->wmax;
	ng_callout_init(&seq->rack_timer);
	ng_callout_init(&seq->xack_timer);
	L2TP_SEQ_CHECK(seq);
}

/*
 * Set sequence number state as given from user.
 */
static int
ng_l2tp_seq_set(priv_p priv, const struct ng_l2tp_seq_config *conf)
{
	struct l2tp_seq *const seq = &priv->seq;

	/* If node is enabled, deny update to sequence numbers. */
	if (priv->conf.enabled)
		return (EBUSY);

	/* We only can handle the simple cases. */
	if (conf->xack != conf->nr || conf->ns != conf->rack)
		return (EINVAL);

	/* Set ns,nr,rack,xack parameters. */
	seq->ns = conf->ns;
	seq->nr = conf->nr;
	seq->rack = conf->rack;
	seq->xack = conf->xack;

	return (0);
}

/*
 * Adjust sequence number state accordingly after reconfiguration.
 */
static int
ng_l2tp_seq_adjust(priv_p priv, const struct ng_l2tp_config *conf)
{
	struct l2tp_seq *const seq = &priv->seq;
	u_int16_t new_wmax;

	/* If disabling node, reset state sequence number */
	if (!conf->enabled) {
		ng_l2tp_seq_reset(priv);
		return (0);
	}

	/* Adjust peer's max recv window; it can only increase */
	new_wmax = conf->peer_win;
	if (new_wmax > L2TP_MAX_XWIN)
		new_wmax = L2TP_MAX_XWIN;
	if (new_wmax == 0)
		return (EINVAL);
	if (new_wmax < seq->wmax)
		return (EBUSY);
	seq->wmax = new_wmax;

	/* Done */
	return (0);
}

/*
 * Reset sequence number state.
 */
static void
ng_l2tp_seq_reset(priv_p priv)
{
	struct l2tp_seq *const seq = &priv->seq;
	hook_p hook;
	int i;

	/* Sanity check */
	L2TP_SEQ_CHECK(seq);

	/* Stop timers */
	ng_uncallout(&seq->rack_timer, priv->node);
	ng_uncallout(&seq->xack_timer, priv->node);

	/* Free retransmit queue */
	for (i = 0; i < L2TP_MAX_XWIN; i++) {
		if (seq->xwin[i] == NULL)
			break;
		m_freem(seq->xwin[i]);
	}

	/* Reset session hooks' sequence number states */
	NG_NODE_FOREACH_HOOK(priv->node, ng_l2tp_reset_session, NULL, hook);

	/* Reset node's sequence number state */
	memset(seq, 0, sizeof(*seq));
	seq->cwnd = 1;
	seq->wmax = L2TP_MAX_XWIN;
	seq->ssth = seq->wmax;

	/* Done */
	L2TP_SEQ_CHECK(seq);
}

/*
 * Handle receipt of an acknowledgement value (Nr) from peer.
 */
static void
ng_l2tp_seq_recv_nr(priv_p priv, u_int16_t nr)
{
	struct l2tp_seq *const seq = &priv->seq;
	struct mbuf *m;
	int nack;
	int i;

	/* Verify peer's ACK is in range */
	if ((nack = L2TP_SEQ_DIFF(nr, seq->rack)) <= 0)
		return;				/* duplicate ack */
	if (L2TP_SEQ_DIFF(nr, seq->ns) > 0) {
		priv->stats.recvBadAcks++;	/* ack for packet not sent */
		return;
	}
	KASSERT(nack <= L2TP_MAX_XWIN,
	    ("%s: nack=%d > %d", __func__, nack, L2TP_MAX_XWIN));

	/* Update receive ack stats */
	seq->rack = nr;
	seq->rexmits = 0;

	/* Free acknowledged packets and shift up packets in the xmit queue */
	for (i = 0; i < nack; i++)
		m_freem(seq->xwin[i]);
	memmove(seq->xwin, seq->xwin + nack,
	    (L2TP_MAX_XWIN - nack) * sizeof(*seq->xwin));
	memset(seq->xwin + (L2TP_MAX_XWIN - nack), 0,
	    nack * sizeof(*seq->xwin));

	/*
	 * Do slow-start/congestion avoidance windowing algorithm described
	 * in RFC 2661, Appendix A. Here we handle a multiple ACK as if each
	 * ACK had arrived separately.
	 */
	if (seq->cwnd < seq->wmax) {

		/* Handle slow start phase */
		if (seq->cwnd < seq->ssth) {
			seq->cwnd += nack;
			nack = 0;
			if (seq->cwnd > seq->ssth) {	/* into cg.av. phase */
				nack = seq->cwnd - seq->ssth;
				seq->cwnd = seq->ssth;
			}
		}

		/* Handle congestion avoidance phase */
		if (seq->cwnd >= seq->ssth) {
			seq->acks += nack;
			while (seq->acks >= seq->cwnd) {
				seq->acks -= seq->cwnd;
				if (seq->cwnd < seq->wmax)
					seq->cwnd++;
			}
		}
	}

	/* Stop xmit timer */
	if (callout_active(&seq->rack_timer))
		ng_uncallout(&seq->rack_timer, priv->node);

	/* If transmit queue is empty, we're done for now */
	if (seq->xwin[0] == NULL)
		return;

	/* Start restransmit timer again */
	ng_callout(&seq->rack_timer, priv->node, NULL,
	    hz, ng_l2tp_seq_rack_timeout, NULL, 0);

	/*
	 * Send more packets, trying to keep peer's receive window full.
	 * If there is a memory error, pretend packet was sent, as it
	 * will get retransmitted later anyway.
	 */
	while ((i = L2TP_SEQ_DIFF(seq->ns, seq->rack)) < seq->cwnd
	    && seq->xwin[i] != NULL) {
		if ((m = L2TP_COPY_MBUF(seq->xwin[i], M_DONTWAIT)) == NULL)
			priv->stats.memoryFailures++;
		else
			ng_l2tp_xmit_ctrl(priv, m, seq->ns);
		seq->ns++;
	}
}

/*
 * Handle receipt of a sequence number value (Ns) from peer.
 * We make no attempt to re-order out of order packets.
 *
 * This function should only be called for non-ZLB packets.
 *
 * Returns:
 *	 0	Accept packet
 *	-1	Drop packet
 */
static int
ng_l2tp_seq_recv_ns(priv_p priv, u_int16_t ns)
{
	struct l2tp_seq *const seq = &priv->seq;

	/* If not what we expect, drop packet and send an immediate ZLB ack */
	if (ns != seq->nr) {
		if (L2TP_SEQ_DIFF(ns, seq->nr) < 0)
			priv->stats.recvDuplicates++;
		else
			priv->stats.recvOutOfOrder++;
		ng_l2tp_xmit_ctrl(priv, NULL, seq->ns);
		return (-1);
	}

	/* Update recv sequence number */
	seq->nr++;

	/* Start receive ack timer, if not already running */
	if (!callout_active(&seq->xack_timer))
		ng_callout(&seq->xack_timer, priv->node, NULL,
		    L2TP_DELAYED_ACK, ng_l2tp_seq_xack_timeout, NULL, 0);

	/* Accept packet */
	return (0);
}

/*
 * Handle an ack timeout. We have an outstanding ack that we
 * were hoping to piggy-back, but haven't, so send a ZLB.
 */
static void
ng_l2tp_seq_xack_timeout(node_p node, hook_p hook, void *arg1, int arg2)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct l2tp_seq *const seq = &priv->seq;

	/* Make sure callout is still active before doing anything */
	if (callout_pending(&seq->xack_timer) ||
	    (!callout_active(&seq->xack_timer)))
		return;

	/* Sanity check */
	L2TP_SEQ_CHECK(seq);

	/* Send a ZLB */
	ng_l2tp_xmit_ctrl(priv, NULL, seq->ns);

	/* callout_deactivate() is not needed here 
	    as ng_uncallout() was called by ng_l2tp_xmit_ctrl() */

	/* Sanity check */
	L2TP_SEQ_CHECK(seq);
}

/* 
 * Handle a transmit timeout. The peer has failed to respond
 * with an ack for our packet, so retransmit it.
 */
static void
ng_l2tp_seq_rack_timeout(node_p node, hook_p hook, void *arg1, int arg2)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct l2tp_seq *const seq = &priv->seq;
	struct mbuf *m;
	u_int delay;

	/* Make sure callout is still active before doing anything */
	if (callout_pending(&seq->rack_timer) ||
	    (!callout_active(&seq->rack_timer)))
		return;

	/* Sanity check */
	L2TP_SEQ_CHECK(seq);

	priv->stats.xmitRetransmits++;

	/* Have we reached the retransmit limit? If so, notify owner. */
	if (seq->rexmits++ >= priv->conf.rexmit_max)
		ng_l2tp_seq_failure(priv);

	/* Restart timer, this time with an increased delay */
	delay = (seq->rexmits > 12) ? (1 << 12) : (1 << seq->rexmits);
	if (delay > priv->conf.rexmit_max_to)
		delay = priv->conf.rexmit_max_to;
	ng_callout(&seq->rack_timer, node, NULL,
	    hz * delay, ng_l2tp_seq_rack_timeout, NULL, 0);

	/* Do slow-start/congestion algorithm windowing algorithm */
	seq->ssth = (seq->cwnd + 1) / 2;
	seq->cwnd = 1;
	seq->acks = 0;

	/* Retransmit oldest unack'd packet */
	if ((m = L2TP_COPY_MBUF(seq->xwin[0], M_DONTWAIT)) == NULL)
		priv->stats.memoryFailures++;
	else
		ng_l2tp_xmit_ctrl(priv, m, seq->rack);

	/* callout_deactivate() is not needed here 
	    as ng_callout() is getting called each time */

	/* Sanity check */
	L2TP_SEQ_CHECK(seq);
}

/*
 * Transmit a control stream packet, payload optional.
 * The transmit sequence number is not incremented.
 */
static int
ng_l2tp_xmit_ctrl(priv_p priv, struct mbuf *m, u_int16_t ns)
{
	struct l2tp_seq *const seq = &priv->seq;
	u_int16_t session_id = 0;
	int error;

	/* Stop ack timer: we're sending an ack with this packet.
	   Doing this before to keep state predictable after error. */
	if (callout_active(&seq->xack_timer))
		ng_uncallout(&seq->xack_timer, priv->node);

	seq->xack = seq->nr;

	/* If no mbuf passed, send an empty packet (ZLB) */
	if (m == NULL) {

		/* Create a new mbuf for ZLB packet */
		MGETHDR(m, M_DONTWAIT, MT_DATA);
		if (m == NULL) {
			priv->stats.memoryFailures++;
			return (ENOBUFS);
		}
		m->m_len = m->m_pkthdr.len = 12;
		m->m_pkthdr.rcvif = NULL;
		priv->stats.xmitZLBs++;
	} else {

		/* Strip off session ID */
		if (m->m_len < 2 && (m = m_pullup(m, 2)) == NULL) {
			priv->stats.memoryFailures++;
			return (ENOBUFS);
		}
		memcpy(&session_id, mtod(m, u_int16_t *), 2);
		m_adj(m, 2);

		/* Make room for L2TP header */
		M_PREPEND(m, 12, M_DONTWAIT);
		if (m == NULL) {
			priv->stats.memoryFailures++;
			return (ENOBUFS);
		}
	}

	/* Fill in L2TP header */
	mtod(m, u_int16_t *)[0] = htons(L2TP_CTRL_HDR);
	mtod(m, u_int16_t *)[1] = htons(m->m_pkthdr.len);
	mtod(m, u_int16_t *)[2] = priv->conf.peer_id;
	mtod(m, u_int16_t *)[3] = session_id;
	mtod(m, u_int16_t *)[4] = htons(ns);
	mtod(m, u_int16_t *)[5] = htons(seq->nr);

	/* Update sequence number info and stats */
	priv->stats.xmitPackets++;
	priv->stats.xmitOctets += m->m_pkthdr.len;

	/* Send packet */
	NG_SEND_DATA_ONLY(error, priv->lower, m);
	return (error);
}

#ifdef INVARIANTS
/*
 * Sanity check sequence number state.
 */
static void
ng_l2tp_seq_check(struct l2tp_seq *seq)
{
	const int self_unack = L2TP_SEQ_DIFF(seq->nr, seq->xack);
	const int peer_unack = L2TP_SEQ_DIFF(seq->ns, seq->rack);
	int i;

#define CHECK(p)	KASSERT((p), ("%s: not: %s", __func__, #p))

	CHECK(seq->wmax <= L2TP_MAX_XWIN);
	CHECK(seq->cwnd >= 1);
	CHECK(seq->cwnd <= seq->wmax);
	CHECK(seq->ssth >= 1);
	CHECK(seq->ssth <= seq->wmax);
	if (seq->cwnd < seq->ssth)
		CHECK(seq->acks == 0);
	else
		CHECK(seq->acks <= seq->cwnd);
	CHECK(self_unack >= 0);
	CHECK(peer_unack >= 0);
	CHECK(peer_unack <= seq->wmax);
	CHECK((self_unack == 0) ^ callout_active(&seq->xack_timer));
	CHECK((peer_unack == 0) ^ callout_active(&seq->rack_timer));
	for (i = 0; i < peer_unack; i++)
		CHECK(seq->xwin[i] != NULL);
	for ( ; i < seq->cwnd; i++)	    /* verify peer's recv window full */
		CHECK(seq->xwin[i] == NULL);

#undef CHECK
}
#endif	/* INVARIANTS */
