/*
 * Copyright (c) 1998, Larry Lile
 * All rights reserved.
 *
 * For latest sources and information on this driver, please
 * go to http://anarchy.stdio.com.
 *
 * Questions, comments or suggestions should be directed to
 * Larry Lile <lile@stdio.com>.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 *
 */

/*
 *
 * General ISO 802.5 (Token Ring) support routines
 * 
 */

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ipx.h"
#include "opt_mac.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mac.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/sockio.h> 

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_llc.h>
#include <net/if_types.h>

#include <net/netisr.h>
#include <net/route.h>
#include <net/bpf.h>
#include <net/iso88025.h>

#if defined(INET) || defined(INET6)
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#endif
#ifdef INET6
#include <netinet6/nd6.h>
#endif

#ifdef IPX
#include <netipx/ipx.h>
#include <netipx/ipx_if.h>
#endif

static u_char iso88025_broadcastaddr[ISO88025_ADDR_LEN] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int iso88025_resolvemulti (struct ifnet *, struct sockaddr **,
				  struct sockaddr *));

#define	IFP2AC(IFP)	((struct arpcom *)IFP)
#define	senderr(e)	do { error = (e); goto bad; } while (0)

void
iso88025_ifattach(struct ifnet *ifp)
{
    struct ifaddr *ifa = NULL;
    struct sockaddr_dl *sdl;

    ifp->if_type = IFT_ISO88025;
    ifp->if_addrlen = ISO88025_ADDR_LEN;
    ifp->if_hdrlen = ISO88025_HDR_LEN;
    if (ifp->if_baudrate == 0)
        ifp->if_baudrate = TR_16MBPS; /* 16Mbit should be a safe default */
    if (ifp->if_mtu == 0)
        ifp->if_mtu = ISO88025_DEFAULT_MTU;
    ifp->if_broadcastaddr = iso88025_broadcastaddr;

    ifa = ifaddr_byindex(ifp->if_index);
    if (ifa == 0) {
            printf("iso88025_ifattach: no lladdr!\n");
            return;
    }
    sdl = (struct sockaddr_dl *)ifa->ifa_addr;
    sdl->sdl_type = IFT_ISO88025;
    sdl->sdl_alen = ifp->if_addrlen;
    bcopy(IFP2AC(ifp)->ac_enaddr, LLADDR(sdl), ifp->if_addrlen);
}

/*
 * Perform common duties while detaching a Token Ring interface
 */
void
iso88025_ifdetach(ifp, bpf)
        struct ifnet *ifp;
        int bpf;
{

	if (bpf)
                bpfdetach(ifp);

	if_detach(ifp);

	return;
}

int
iso88025_ioctl(struct ifnet *ifp, int command, caddr_t data)
{
        struct ifaddr *ifa;
        struct ifreq *ifr;
        int error;

	ifa = (struct ifaddr *) data;
	ifr = (struct ifreq *) data;
	error = 0;

        switch (command) {
        case SIOCSIFADDR:
                ifp->if_flags |= IFF_UP;

                switch (ifa->ifa_addr->sa_family) {
#ifdef INET
                case AF_INET:
                        ifp->if_init(ifp->if_softc);    /* before arpwhohas */
                        arp_ifinit(ifp, ifa);
                        break;
#endif	/* INET */
#ifdef IPX
                /*
                 * XXX - This code is probably wrong
                 */
                case AF_IPX:
                        {
                        struct ipx_addr *ina = &(IA_SIPX(ifa)->sipx_addr);
                        struct arpcom *ac = IFP2AC(ifp);

                        if (ipx_nullhost(*ina))
                                ina->x_host = *(union ipx_host *)ac->ac_enaddr;
                        else {
                                bcopy((caddr_t) ina->x_host.c_host,
                                      (caddr_t) ac->ac_enaddr,
                                      ISO88025_ADDR_LEN);
                        }

                        /*
                         * Set new address
                         */
                        ifp->if_init(ifp->if_softc);
                        break;
                        }
#endif	/* IPX */
                default:
                        ifp->if_init(ifp->if_softc);
                        break;
                }
                break;

        case SIOCGIFADDR:
                {
                        struct sockaddr *sa;

                        sa = (struct sockaddr *) & ifr->ifr_data;
                        bcopy(IFP2AC(ifp)->ac_enaddr,
                              (caddr_t) sa->sa_data, ISO88025_ADDR_LEN);
                }
                break;

        case SIOCSIFMTU:
                /*
                 * Set the interface MTU.
                 */
                if (ifr->ifr_mtu > ISO88025_MAX_MTU) {
                        error = EINVAL;
                } else {
                        ifp->if_mtu = ifr->ifr_mtu;
                }
                break;
	default:
		error = EINVAL;			/* XXX netbsd has ENOTTY??? */
		break;
        }

        return (error);
}

/*
 * ISO88025 encapsulation
 */
int
iso88025_output(ifp, m, dst, rt0)
	struct ifnet *ifp;
	struct mbuf *m;
	struct sockaddr *dst;
	struct rtentry *rt0;
{
	u_int16_t snap_type = 0;
	int loop_copy = 0, error = 0, rif_len = 0;
	u_char edst[ISO88025_ADDR_LEN];
	struct iso88025_header *th;
	struct iso88025_header gen_th;
	struct sockaddr_dl *sdl = NULL;
	struct rtentry *rt;
	struct arpcom *ac = (struct arpcom *)ifp;

#ifdef MAC
	error = mac_check_ifnet_transmit(ifp, m);
	if (error)
		senderr(error);
#endif

	if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) != (IFF_UP|IFF_RUNNING))
		senderr(ENETDOWN);
	getmicrotime(&ifp->if_lastchange);

	error = rt_check(&rt, &rt0, dst);
	if (error)
		goto bad;

	/* Calculate routing info length based on arp table entry */
	if (rt && (sdl = (struct sockaddr_dl *)rt->rt_gateway))
		if (SDL_ISO88025(sdl)->trld_rcf != 0)
			rif_len = TR_RCF_RIFLEN(SDL_ISO88025(sdl)->trld_rcf);

	/* Generate a generic 802.5 header for the packet */
	gen_th.ac = TR_AC;
	gen_th.fc = TR_LLC_FRAME;
	(void)memcpy((caddr_t)gen_th.iso88025_shost, (caddr_t)ac->ac_enaddr,
		     ISO88025_ADDR_LEN);
	if (rif_len) {
		gen_th.iso88025_shost[0] |= TR_RII;
		if (rif_len > 2) {
			gen_th.rcf = SDL_ISO88025(sdl)->trld_rcf;
			(void)memcpy((caddr_t)gen_th.rd,
				(caddr_t)SDL_ISO88025(sdl)->trld_route,
				rif_len - 2);
		}
	}
	
	switch (dst->sa_family) {
#ifdef INET
	case AF_INET:
		if (!arpresolve(ifp, rt, m, dst, edst, rt0))
			return (0);	/* if not yet resolved */
		snap_type = ETHERTYPE_IP;
		break;
#endif	/* INET */
#ifdef NOT_YET
#ifdef INET6
	case AF_INET6:
		if (!nd6_storelladdr(&ac->ac_if, rt, m, dst, (u_char *)edst)) {
			/* Something bad happened */
			return(0);
		}
		snap_type = ETHERTYPE_IPV6;
		break;
#endif	/* INET6 */
#endif	/* NOT_YET */
#ifdef IPX
	case AF_IPX:
	{
		u_int8_t	*cp;

		snap_type = 0;
		bcopy((caddr_t)&(satoipx_addr(dst).x_host), (caddr_t)edst,
		      ISO88025_ADDR_LEN);

		M_PREPEND(m, 3, M_TRYWAIT);
		if (m == 0)
			senderr(ENOBUFS);
		m = m_pullup(m, 3);
		if (m == 0)
			senderr(ENOBUFS);
		cp = mtod(m, u_int8_t *);
		*cp++ = ETHERTYPE_IPX_8022;
		*cp++ = ETHERTYPE_IPX_8022;
		*cp++ = LLC_UI;
	}
	break;
#endif	/* IPX */
	case AF_UNSPEC:
	{
		struct iso88025_sockaddr_data *sd;
		/*
		 * For AF_UNSPEC sockaddr.sa_data must contain all of the
		 * mac information needed to send the packet.  This allows
		 * full mac, llc, and source routing function to be controlled.
		 * llc and source routing information must already be in the
		 * mbuf provided, ac/fc are set in sa_data.  sockaddr.sa_data
		 * should be an iso88025_sockaddr_data structure see iso88025.h
		 */
                loop_copy = -1;
		sd = (struct iso88025_sockaddr_data *)dst->sa_data;
		gen_th.ac = sd->ac;
		gen_th.fc = sd->fc;
		(void)memcpy((caddr_t)edst, (caddr_t)sd->ether_dhost,
			     ISO88025_ADDR_LEN);
		(void)memcpy((caddr_t)gen_th.iso88025_shost,
			     (caddr_t)sd->ether_shost, ISO88025_ADDR_LEN);
		rif_len = 0;
		break;
	}
	default:
		if_printf(ifp, "can't handle af%d\n", dst->sa_family);
		senderr(EAFNOSUPPORT);
		break;
	}

	/*
	 * Add LLC header.
	 */
	if (snap_type != 0) {
        	struct llc *l;
		M_PREPEND(m, LLC_SNAPFRAMELEN, M_DONTWAIT);
		if (m == 0)
			senderr(ENOBUFS);
		l = mtod(m, struct llc *);
		l->llc_control = LLC_UI;
		l->llc_dsap = l->llc_ssap = LLC_SNAP_LSAP;
		l->llc_snap.org_code[0] =
			l->llc_snap.org_code[1] =
			l->llc_snap.org_code[2] = 0;
		l->llc_snap.ether_type = htons(snap_type);
	}

	/*
	 * Add local net header.  If no space in first mbuf,
	 * allocate another.
	 */
	M_PREPEND(m, ISO88025_HDR_LEN + rif_len, M_DONTWAIT);
	if (m == 0)
		senderr(ENOBUFS);
	th = mtod(m, struct iso88025_header *);
	bcopy((caddr_t)edst, (caddr_t)&gen_th.iso88025_dhost, ISO88025_ADDR_LEN);

	/* Copy as much of the generic header as is needed into the mbuf */
	memcpy(th, &gen_th, ISO88025_HDR_LEN + rif_len);

        /*
         * If a simplex interface, and the packet is being sent to our
         * Ethernet address or a broadcast address, loopback a copy.
         * XXX To make a simplex device behave exactly like a duplex
         * device, we should copy in the case of sending to our own
         * ethernet address (thus letting the original actually appear
         * on the wire). However, we don't do that here for security
         * reasons and compatibility with the original behavior.
         */     
        if ((ifp->if_flags & IFF_SIMPLEX) && (loop_copy != -1)) {
                if ((m->m_flags & M_BCAST) || (loop_copy > 0)) { 
                        struct mbuf *n;
			n = m_copy(m, 0, (int)M_COPYALL);
                        (void) if_simloop(ifp, n, dst->sa_family,
					  ISO88025_HDR_LEN);
                } else if (bcmp(th->iso88025_dhost, th->iso88025_shost,
				 ETHER_ADDR_LEN) == 0) {
			(void) if_simloop(ifp, m, dst->sa_family,
					  ISO88025_HDR_LEN);
                       	return(0);      /* XXX */
		}       
        }      

	if (! IF_HANDOFF_ADJ(&ifp->if_snd, m, ifp, ISO88025_HDR_LEN + LLC_SNAPFRAMELEN) ) {
		printf("iso88025_output: packet dropped QFULL.\n");
		senderr(ENOBUFS);
	}
	return (error);

bad:
	if (m)
		m_freem(m);
	return (error);
}

/*
 * ISO 88025 de-encapsulation
 */
void
iso88025_input(ifp, th, m)
	struct ifnet *ifp;
	struct iso88025_header *th;
	struct mbuf *m;
{
	int isr;
	struct llc *l;

	/*
	 * Discard packet if interface is not up.
	 */
	if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) != (IFF_UP|IFF_RUNNING))
		goto dropanyway;

#ifdef MAC
	mac_create_mbuf_from_ifnet(ifp, m);
#endif

	/*
	 * Update interface statistics.
	 */
	ifp->if_ibytes += m->m_pkthdr.len + sizeof(*th);
	getmicrotime(&ifp->if_lastchange);

	/*
	 * Discard non local unicast packets when interface
	 * is in promiscuous mode.
	 */
	if ((ifp->if_flags & IFF_PROMISC) &&
	    ((th->iso88025_dhost[0] & 1) == 0) &&
	     (bcmp(IFP2AC(ifp)->ac_enaddr, (caddr_t) th->iso88025_dhost,
	     ISO88025_ADDR_LEN) != 0))
		goto dropanyway;
	}

	/*
	 * Set mbuf flags for bcast/mcast.
	 */
	if (th->iso88025_dhost[0] & 1) {
		if (bcmp((caddr_t)iso88025_broadcastaddr,
			 (caddr_t)th->iso88025_dhost, ISO88025_ADDR_LEN) == 0)
			m->m_flags |= M_BCAST;
		else
			m->m_flags |= M_MCAST;
		ifp->if_imcasts++;
	}

	l = mtod(m, struct llc *);

	switch (l->llc_dsap) {
#ifdef IPX
	case ETHERTYPE_IPX_8022:	/* Thanks a bunch Novell */
		if ((l->llc_control != LLC_UI) ||
		    (l->llc_ssap != ETHERTYPE_IPX_8022)) {
			ifp->if_noproto++;
			goto dropanyway;
		}

		th->iso88025_shost[0] &= ~(TR_RII); 
		m_adj(m, 3);
		isr = NETISR_IPX;
		break;
#endif	/* IPX */
	case LLC_SNAP_LSAP: {
		u_int16_t type;
		if ((l->llc_control != LLC_UI) ||
		    (l->llc_ssap != LLC_SNAP_LSAP)) {
			ifp->if_noproto++;
			goto dropanyway;
		}

		if (l->llc_snap.org_code[0] != 0 ||
		    l->llc_snap.org_code[1] != 0 ||
		    l->llc_snap.org_code[2] != 0)
			goto dropanyway;

		type = ntohs(l->llc_snap.ether_type);
		m_adj(m, LLC_SNAPFRAMELEN);
		switch (type) {
#ifdef INET
		case ETHERTYPE_IP:
			th->iso88025_shost[0] &= ~(TR_RII); 
			if (ipflow_fastforward(m))
				return;
			isr = NETISR_IP;
			break;

		case ETHERTYPE_ARP:
			if (ifp->if_flags & IFF_NOARP)
				goto dropanyway;
			isr = NETISR_ARP;
			break;
#endif	/* INET */
#ifdef IPX_SNAP	/* XXX: Not supported! */
		case ETHERTYPE_IPX:
			th->iso88025_shost[0] &= ~(TR_RII); 
			isr = NETISR_IPX;
			break;
#endif	/* IPX_SNAP */
#ifdef NOT_YET
#ifdef INET6
		case ETHERTYPE_IPV6:
			th->iso88025_shost[0] &= ~(TR_RII); 
			isr = NETISR_IPV6;
			break;
#endif	/* INET6 */
#endif	/* NOT_YET */
		default:
			printf("iso88025_input: unexpected llc_snap ether_type  0x%02x\n", type);
			ifp->if_noproto++;
			goto dropanyway;
		}
		break;
	}
	case LLC_ISO_LSAP:
		switch (l->llc_control) {
		case LLC_UI:
			ifp->if_noproto++;
			goto dropanyway;
			break;
                case LLC_XID:
                case LLC_XID_P:
			if(m->m_len < ISO88025_ADDR_LEN)
				goto dropanyway;
			l->llc_window = 0;
			l->llc_fid = 9;  
			l->llc_class = 1;
			l->llc_dsap = l->llc_ssap = 0;
			/* Fall through to */  
		case LLC_TEST:
		case LLC_TEST_P:
		{
			struct sockaddr sa;
			struct arpcom *ac = (struct arpcom *)ifp;
			struct iso88025_sockaddr_data *th2;
			int i;
			u_char c = l->llc_dsap;

			if (th->iso88025_shost[0] & TR_RII) { /* XXX */
				printf("iso88025_input: dropping source routed LLC_TEST\n");
				goto dropanyway;
			}
			l->llc_dsap = l->llc_ssap;
			l->llc_ssap = c;
			if (m->m_flags & (M_BCAST | M_MCAST))
				bcopy((caddr_t)ac->ac_enaddr, 
				      (caddr_t)th->iso88025_dhost,
					ISO88025_ADDR_LEN);
			sa.sa_family = AF_UNSPEC;
			sa.sa_len = sizeof(sa);
			th2 = (struct iso88025_sockaddr_data *)sa.sa_data;
			for (i = 0; i < ISO88025_ADDR_LEN; i++) {
				th2->ether_shost[i] = c = th->iso88025_dhost[i];
				th2->ether_dhost[i] = th->iso88025_dhost[i] =
					th->iso88025_shost[i];
				th->iso88025_shost[i] = c;
			}
			th2->ac = TR_AC;
			th2->fc = TR_LLC_FRAME;
			ifp->if_output(ifp, m, &sa, NULL);
			return;
		}
		default:
			printf("iso88025_input: unexpected llc control 0x%02x\n", l->llc_control);
			ifp->if_noproto++;
			goto dropanyway;
		}
		break;
	default:
		printf("iso88025_input: unknown dsap 0x%x\n", l->llc_dsap);
		ifp->if_noproto++;
		goto dropanyway;
	}

	netisr_dispatch(isr, m);
	return;

dropanyway:
	ifp->if_iqdrops++;
	if (m)
		m_freem(m);
	return;
}

static int
iso88025_resolvemulti (ifp, llsa, sa)
	struct ifnet *ifp;
	struct sockaddr **llsa;
	struct sockaddr *sa;
{
	struct sockaddr_dl *sdl;
	struct sockaddr_in *sin;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	u_char *e_addr;

	switch(sa->sa_family) {
	case AF_LINK:
		/*
		 * No mapping needed. Just check that it's a valid MC address.
		 */
		sdl = (struct sockaddr_dl *)sa;
		e_addr = LLADDR(sdl);
		if ((e_addr[0] & 1) != 1) {
			return (EADDRNOTAVAIL);
		}
		*llsa = 0;
		return (0);

#ifdef INET
	case AF_INET:
		sin = (struct sockaddr_in *)sa;
		if (!IN_MULTICAST(ntohl(sin->sin_addr.s_addr))) {
			return (EADDRNOTAVAIL);
		}
		MALLOC(sdl, struct sockaddr_dl *, sizeof *sdl, M_IFMADDR,
		       M_WAITOK|M_ZERO);
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_ISO88025;
		sdl->sdl_alen = ISO88025_ADDR_LEN;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IP_MULTICAST(&sin->sin_addr, e_addr);
		*llsa = (struct sockaddr *)sdl;
		return (0);
#endif
#ifdef INET6
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)sa;
		if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
			/*
			 * An IP6 address of 0 means listen to all
			 * of the Ethernet multicast address used for IP6.
			 * (This is used for multicast routers.)
			 */
			ifp->if_flags |= IFF_ALLMULTI;
			*llsa = 0;
			return (0);
		}
		if (!IN6_IS_ADDR_MULTICAST(&sin6->sin6_addr)) {
			return (EADDRNOTAVAIL);
		}
		MALLOC(sdl, struct sockaddr_dl *, sizeof *sdl, M_IFMADDR,
		       M_WAITOK|M_ZERO);
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_ISO88025;
		sdl->sdl_alen = ISO88025_ADDR_LEN;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IPV6_MULTICAST(&sin6->sin6_addr, e_addr);
		*llsa = (struct sockaddr *)sdl;
		return (0);
#endif

	default:
		/*
		 * Well, the text isn't quite right, but it's the name
		 * that counts...
		 */
		return (EAFNOSUPPORT);
	}

	return (0);
}

static moduledata_t iso88025_mod = {
	"iso88025",
	NULL,
	0
};

DECLARE_MODULE(iso88025, iso88025_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(iso88025, 1);
