/*	$NetBSD: if_de.c,v 1.11 1995/12/24 02:30:52 mycroft Exp $	*/

/*
 * Copyright (c) 1982, 1986, 1989 Regents of the University of California.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)if_de.c	7.12 (Berkeley) 12/16/90
 */

#include "de.h"

/*
 * DEC DEUNA interface
 *
 *	Lou Salkind
 *	New York University
 *
 * TODO:
 *	timeout routine (get statistics)
 */

#include "sys/param.h"
#include "sys/systm.h"
#include "sys/mbuf.h"
#include "sys/buf.h"
#include "sys/protosw.h"
#include "sys/socket.h"
#include "sys/ioctl.h"
#include "sys/errno.h"
#include "sys/syslog.h"
#include "sys/device.h"

#include "machine/pte.h"
#include "machine/sid.h"

#include "net/if.h"
#include "net/netisr.h"
#include "net/route.h"

#ifdef INET
#include "netinet/in.h"
#include "netinet/in_systm.h"
#include "netinet/in_var.h"
#include "netinet/ip.h"
#include "netinet/if_ether.h"
#endif

#ifdef NS
#include "netns/ns.h"
#include "netns/ns_if.h"
#endif

#ifdef ISO
#include "netiso/iso.h"
#include "netiso/iso_var.h"
extern char all_es_snpa[], all_is_snpa[];
#endif

#include "../include/cpu.h"
#include "../include/mtpr.h"
#include "if_dereg.h"
#include "if_uba.h"
#include "../uba/ubareg.h"
#include "../uba/ubavar.h"

#define	NXMT	3	/* number of transmit buffers */
#define	NRCV	7	/* number of receive buffers (must be > 1) */

int	dedebug = 0;

/*
 * Ethernet software status per interface.
 *
 * Each interface is referenced by a network interface structure,
 * ds_if, which the routing code uses to locate the interface.
 * This structure contains the output queue for the interface, its address, ...
 * We also have, for each interface, a UBA interface structure, which
 * contains information about the UNIBUS resources held by the interface:
 * map registers, buffered data paths, etc.  Information is cached in this
 * structure for use by the if_uba.c routines in running the interface
 * efficiently.
 */
struct	de_softc {
	struct	device ds_device;	/* Configuration common part */
	struct	arpcom ds_ac;		/* Ethernet common part */
	struct	dedevice *ds_vaddr;	/* Virtual address of this interface */
#define	ds_if	ds_ac.ac_if		/* network-visible interface */
#define	ds_addr	ds_ac.ac_enaddr		/* hardware Ethernet address */
	int	ds_flags;
#define	DSF_RUNNING	2		/* board is enabled */
#define	DSF_SETADDR	4		/* physical address is changed */
	int	ds_ubaddr;		/* map info for incore structs */
	struct	ifubinfo ds_deuba;	/* unibus resource structure */
	struct	ifrw ds_ifr[NRCV];	/* unibus receive maps */
	struct	ifxmt ds_ifw[NXMT];	/* unibus xmt maps */
	/* the following structures are always mapped in */
	struct	de_pcbb ds_pcbb;	/* port control block */
	struct	de_ring ds_xrent[NXMT];	/* transmit ring entrys */
	struct	de_ring ds_rrent[NRCV];	/* receive ring entrys */
	struct	de_udbbuf ds_udbbuf;	/* UNIBUS data buffer */
	/* end mapped area */
#define	INCORE_BASE(p)	((char *)&(p)->ds_pcbb)
#define	RVAL_OFF(s,n)	((char *)&(s)->n - INCORE_BASE(s))
#define	LVAL_OFF(s,n)	((char *)(s)->n - INCORE_BASE(s))
#define	PCBB_OFFSET(s)	RVAL_OFF(s,ds_pcbb)
#define	XRENT_OFFSET(s)	LVAL_OFF(s,ds_xrent)
#define	RRENT_OFFSET(s)	LVAL_OFF(s,ds_rrent)
#define	UDBBUF_OFFSET(s)	RVAL_OFF(s,ds_udbbuf)
#define	INCORE_SIZE(s)	RVAL_OFF(s, ds_xindex)
	int	ds_xindex;		/* UNA index into transmit chain */
	int	ds_rindex;		/* UNA index into receive chain */
	int	ds_xfree;		/* index for next transmit buffer */
	int	ds_nxmit;		/* # of transmits in progress */
};

int	dematch __P((struct device *, void *, void *));
void	deattach __P((struct device *, struct device *, void *));
int	dewait __P((struct de_softc *, char *));
void	deinit __P((int));
int     deioctl __P((struct ifnet *, u_long, caddr_t));
int	dereset __P((int));
void    destart __P((struct ifnet *));
void	deread __P((struct de_softc *, struct ifrw *, int));
void    derecv __P((int));
void	de_setaddr __P((u_char *, int));


struct  cfdriver decd =
	{ 0,"de",dematch, deattach, DV_IFNET, sizeof(struct de_softc) };

/*
 * Interface exists: make available by filling in network interface
 * record.  System will initialize the interface when it is ready
 * to accept packets.  We get the ethernet address here.
 */
void
deattach(parent, self, aux)
	struct	device *parent, *self;
	void	*aux;
{
	struct	uba_attach_args *ua = aux;
	struct de_softc *ds = (struct de_softc *)self;
	struct ifnet *ifp = &ds->ds_if;
	struct dedevice *addr;
	int csr1;

	printf("\n");
	addr = (struct dedevice *)ua->ua_addr;
	ds->ds_vaddr = addr;
	ifp->if_unit = ds->ds_device.dv_unit;
	ifp->if_name = "de";
	ifp->if_flags = IFF_BROADCAST | IFF_NOTRAILERS;

	/*
	 * What kind of a board is this?
	 * The error bits 4-6 in pcsr1 are a device id as long as
	 * the high byte is zero.
	 */
	csr1 = addr->pcsr1;
	if (csr1 & 0xff60)
		printf("de%d: broken\n", ds->ds_device.dv_unit);
	else if (csr1 & 0x10)
		printf("de%d: delua\n", ds->ds_device.dv_unit);
	else
		printf("de%d: deuna\n", ds->ds_device.dv_unit);

	/*
	 * Reset the board and temporarily map
	 * the pcbb buffer onto the Unibus.
	 */
	addr->pcsr0 = 0;		/* reset INTE */
	DELAY(5000);
	addr->pcsr0 = PCSR0_RSET;
	(void)dewait(ds, "reset");

	ds->ds_ubaddr = uballoc(ds->ds_device.dv_parent->dv_unit,
	    (char *)&ds->ds_pcbb, sizeof (struct de_pcbb), 0);
	addr->pcsr2 = ds->ds_ubaddr & 0xffff;
	addr->pcsr3 = (ds->ds_ubaddr >> 16) & 0x3;
	addr->pclow = CMD_GETPCBB;
	(void)dewait(ds, "pcbb");

	ds->ds_pcbb.pcbb0 = FC_RDPHYAD;
	addr->pclow = CMD_GETCMD;
	(void)dewait(ds, "read addr ");

	ubarelse(ds->ds_device.dv_parent->dv_unit, &ds->ds_ubaddr);
 	bcopy((caddr_t)&ds->ds_pcbb.pcbb2, (caddr_t)ds->ds_addr,
	    sizeof (ds->ds_addr));
	printf("de%d: hardware address %s\n", ds->ds_device.dv_unit,
		ether_sprintf(ds->ds_addr));
	ifp->if_ioctl = deioctl;
	ifp->if_reset = dereset;
	ifp->if_start = destart;
	ds->ds_deuba.iff_flags = UBA_CANTWAIT;
#ifdef notdef
	/* CAN WE USE BDP's ??? */
	ds->ds_deuba.iff_flags |= UBA_NEEDBDP;
#endif
	if_attach(ifp);
	ether_ifattach(ifp);
}

/*
 * Reset of interface after UNIBUS reset.
 * If interface is on specified uba, reset its state.
 */
int
dereset(unit)
	int unit;
{
#if 0
	register struct uba_device *ui;

	if (unit >= NDE || (ui = deinfo[unit]) == 0 || ui->ui_alive == 0 ||
	    ui->ui_ubanum != uban)
		return;
	printf(" de%d", unit);
	de_softc[unit].ds_if.if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	de_softc[unit].ds_flags &= ~DSF_RUNNING;
	((struct dedevice *)ui->ui_addr)->pcsr0 = PCSR0_RSET;
	(void)dewait(ui, "reset");
	deinit(unit);
#endif
	return 0;
}

/*
 * Initialization of interface; clear recorded pending
 * operations, and reinitialize UNIBUS usage.
 */
void
deinit(unit)
	int unit;
{
	struct de_softc *ds;
	volatile struct dedevice *addr;
	struct ifrw *ifrw;
	struct ifxmt *ifxp;
	struct ifnet *ifp;
	struct de_ring *rp;
	int s,incaddr;

	ds = (struct de_softc *)decd.cd_devs[unit];
	ifp = &ds->ds_if;

	/* not yet, if address still unknown */
	if (ifp->if_addrlist.tqh_first == (struct ifaddr *)0)
		return;

	if (ds->ds_flags & DSF_RUNNING)
		return;
	if ((ifp->if_flags & IFF_RUNNING) == 0) {
		if (if_ubaminit(&ds->ds_deuba, ds->ds_device.dv_parent->dv_unit,
		    sizeof (struct ether_header), (int)btoc(ETHERMTU),
		    ds->ds_ifr, NRCV, ds->ds_ifw, NXMT) == 0) { 
			printf("de%d: can't initialize\n", unit);
			ds->ds_if.if_flags &= ~IFF_UP;
			return;
		}
		ds->ds_ubaddr = uballoc(ds->ds_device.dv_parent->dv_unit,
		    INCORE_BASE(ds), INCORE_SIZE(ds), 0);
	}
	addr = ds->ds_vaddr;

	/* set the pcbb block address */
	incaddr = ds->ds_ubaddr + PCBB_OFFSET(ds);
	addr->pcsr2 = incaddr & 0xffff;
	addr->pcsr3 = (incaddr >> 16) & 0x3;
	addr->pclow = 0;	/* reset INTE */
	DELAY(5000);
	addr->pclow = CMD_GETPCBB;
	(void)dewait(ds, "pcbb");

	/* set the transmit and receive ring header addresses */
	incaddr = ds->ds_ubaddr + UDBBUF_OFFSET(ds);
	ds->ds_pcbb.pcbb0 = FC_WTRING;
	ds->ds_pcbb.pcbb2 = incaddr & 0xffff;
	ds->ds_pcbb.pcbb4 = (incaddr >> 16) & 0x3;

	incaddr = ds->ds_ubaddr + XRENT_OFFSET(ds);
	ds->ds_udbbuf.b_tdrbl = incaddr & 0xffff;
	ds->ds_udbbuf.b_tdrbh = (incaddr >> 16) & 0x3;
	ds->ds_udbbuf.b_telen = sizeof (struct de_ring) / sizeof (short);
	ds->ds_udbbuf.b_trlen = NXMT;
	incaddr = ds->ds_ubaddr + RRENT_OFFSET(ds);
	ds->ds_udbbuf.b_rdrbl = incaddr & 0xffff;
	ds->ds_udbbuf.b_rdrbh = (incaddr >> 16) & 0x3;
	ds->ds_udbbuf.b_relen = sizeof (struct de_ring) / sizeof (short);
	ds->ds_udbbuf.b_rrlen = NRCV;

	addr->pclow = CMD_GETCMD;
	(void)dewait(ds, "wtring");

	/* initialize the mode - enable hardware padding */
	ds->ds_pcbb.pcbb0 = FC_WTMODE;
	/* let hardware do padding - set MTCH bit on broadcast */
	ds->ds_pcbb.pcbb2 = MOD_TPAD|MOD_HDX;
	addr->pclow = CMD_GETCMD;
	(void)dewait(ds, "wtmode");

	/* set up the receive and transmit ring entries */
	ifxp = &ds->ds_ifw[0];
	for (rp = &ds->ds_xrent[0]; rp < &ds->ds_xrent[NXMT]; rp++) {
		rp->r_segbl = ifxp->ifw_info & 0xffff;
		rp->r_segbh = (ifxp->ifw_info >> 16) & 0x3;
		rp->r_flags = 0;
		ifxp++;
	}
	ifrw = &ds->ds_ifr[0];
	for (rp = &ds->ds_rrent[0]; rp < &ds->ds_rrent[NRCV]; rp++) {
		rp->r_slen = sizeof (struct de_buf);
		rp->r_segbl = ifrw->ifrw_info & 0xffff;
		rp->r_segbh = (ifrw->ifrw_info >> 16) & 0x3;
		rp->r_flags = RFLG_OWN;		/* hang receive */
		ifrw++;
	}

	/* start up the board (rah rah) */
	s = splnet();
	ds->ds_rindex = ds->ds_xindex = ds->ds_xfree = ds->ds_nxmit = 0;
	ds->ds_if.if_flags |= IFF_RUNNING;
	addr->pclow = PCSR0_INTE;		/* avoid interlock */
	destart(&ds->ds_if);		/* queue output packets */
	ds->ds_flags |= DSF_RUNNING;		/* need before de_setaddr */
	if (ds->ds_flags & DSF_SETADDR)
		de_setaddr(ds->ds_addr, unit);
	addr->pclow = CMD_START | PCSR0_INTE;
	splx(s);
}

/*
 * Setup output on interface.
 * Get another datagram to send off of the interface queue,
 * and map it to the interface before starting the output.
 * Must be called from ipl >= our interrupt level.
 */
void
destart(ifp)
	struct ifnet *ifp;
{
        int len;
	int unit = ifp->if_unit;
	register struct de_softc *ds = decd.cd_devs[ifp->if_unit];
	volatile struct dedevice *addr = ds->ds_vaddr;
	register struct de_ring *rp;
	struct mbuf *m;
	register int nxmit;

	/*
	 * the following test is necessary, since
	 * the code is not reentrant and we have
	 * multiple transmission buffers.
	 */
	if (ds->ds_if.if_flags & IFF_OACTIVE)
		return;
	for (nxmit = ds->ds_nxmit; nxmit < NXMT; nxmit++) {
		IF_DEQUEUE(&ds->ds_if.if_snd, m);
		if (m == 0)
			break;
		rp = &ds->ds_xrent[ds->ds_xfree];
		if (rp->r_flags & XFLG_OWN)
			panic("deuna xmit in progress");
		len = if_ubaput(&ds->ds_deuba, &ds->ds_ifw[ds->ds_xfree], m);
		if (ds->ds_deuba.iff_flags & UBA_NEEDBDP)
			UBAPURGE(ds->ds_deuba.iff_uba,
			ds->ds_ifw[ds->ds_xfree].ifw_bdp);
		rp->r_slen = len;
		rp->r_tdrerr = 0;
		rp->r_flags = XFLG_STP|XFLG_ENP|XFLG_OWN;

		ds->ds_xfree++;
		if (ds->ds_xfree == NXMT)
			ds->ds_xfree = 0;
	}
	if (ds->ds_nxmit != nxmit) {
		ds->ds_nxmit = nxmit;
		if (ds->ds_flags & DSF_RUNNING)
			addr->pclow = PCSR0_INTE|CMD_PDMD;
	}
}

/*
 * Command done interrupt.
 */
void
deintr(unit)
	int	unit;
{
	volatile struct dedevice *addr;
	register struct de_softc *ds;
	register struct de_ring *rp;
	register struct ifxmt *ifxp;
	short csr0;

	ds = decd.cd_devs[unit];
	addr = ds->ds_vaddr;


	/* save flags right away - clear out interrupt bits */
	csr0 = addr->pcsr0;
	addr->pchigh = csr0 >> 8;


	ds->ds_if.if_flags |= IFF_OACTIVE;	/* prevent entering destart */
	/*
	 * if receive, put receive buffer on mbuf
	 * and hang the request again
	 */
	derecv(unit);

	/*
	 * Poll transmit ring and check status.
	 * Be careful about loopback requests.
	 * Then free buffer space and check for
	 * more transmit requests.
	 */
	for ( ; ds->ds_nxmit > 0; ds->ds_nxmit--) {
		rp = &ds->ds_xrent[ds->ds_xindex];
		if (rp->r_flags & XFLG_OWN)
			break;
		ds->ds_if.if_opackets++;
		ifxp = &ds->ds_ifw[ds->ds_xindex];
		/* check for unusual conditions */
		if (rp->r_flags & (XFLG_ERRS|XFLG_MTCH|XFLG_ONE|XFLG_MORE)) {
			if (rp->r_flags & XFLG_ERRS) {
				/* output error */
				ds->ds_if.if_oerrors++;
				if (dedebug)
			printf("de%d: oerror, flags=%b tdrerr=%b (len=%d)\n",
				    unit, rp->r_flags, XFLG_BITS,
				    rp->r_tdrerr, XERR_BITS, rp->r_slen);
			} else if (rp->r_flags & XFLG_ONE) {
				/* one collision */
				ds->ds_if.if_collisions++;
			} else if (rp->r_flags & XFLG_MORE) {
				/* more than one collision */
				ds->ds_if.if_collisions += 2;	/* guess */
			} else if (rp->r_flags & XFLG_MTCH) {
				/* received our own packet */
				ds->ds_if.if_ipackets++;
				deread(ds, &ifxp->ifrw,
				    rp->r_slen - sizeof (struct ether_header));
			}
		}
		if (ifxp->ifw_xtofree) {
			m_freem(ifxp->ifw_xtofree);
			ifxp->ifw_xtofree = 0;
		}
		/* check if next transmit buffer also finished */
		ds->ds_xindex++;
		if (ds->ds_xindex == NXMT)
			ds->ds_xindex = 0;
	}
	ds->ds_if.if_flags &= ~IFF_OACTIVE;
	destart(&ds->ds_if);

	if (csr0 & PCSR0_RCBI) {
		if (dedebug)
			log(LOG_WARNING, "de%d: buffer unavailable\n", unit);
		addr->pclow = PCSR0_INTE|CMD_PDMD;
	}
}

/*
 * Ethernet interface receiver interface.
 * If input error just drop packet.
 * Otherwise purge input buffered data path and examine 
 * packet to determine type.  If can't determine length
 * from type, then have to drop packet.  Othewise decapsulate
 * packet based on type and pass to type specific higher-level
 * input routine.
 */
void
derecv(unit)
	int unit;
{
	register struct de_softc *ds = decd.cd_devs[unit];
	register struct de_ring *rp;
	int len;

	rp = &ds->ds_rrent[ds->ds_rindex];
	while ((rp->r_flags & RFLG_OWN) == 0) {
		ds->ds_if.if_ipackets++;
		if (ds->ds_deuba.iff_flags & UBA_NEEDBDP)
			UBAPURGE(ds->ds_deuba.iff_uba,
			ds->ds_ifr[ds->ds_rindex].ifrw_bdp);
		len = (rp->r_lenerr&RERR_MLEN) - sizeof (struct ether_header)
			- 4;	/* don't forget checksum! */
		/* check for errors */
		if ((rp->r_flags & (RFLG_ERRS|RFLG_FRAM|RFLG_OFLO|RFLG_CRC)) ||
		    (rp->r_flags&(RFLG_STP|RFLG_ENP)) != (RFLG_STP|RFLG_ENP) ||
		    (rp->r_lenerr & (RERR_BUFL|RERR_UBTO|RERR_NCHN)) ||
		    len < ETHERMIN || len > ETHERMTU) {
			ds->ds_if.if_ierrors++;
			if (dedebug)
			printf("de%d: ierror, flags=%b lenerr=%b (len=%d)\n",
				unit, rp->r_flags, RFLG_BITS, rp->r_lenerr,
				RERR_BITS, len);
		} else
			deread(ds, &ds->ds_ifr[ds->ds_rindex], len);

		/* hang the receive buffer again */
		rp->r_lenerr = 0;
		rp->r_flags = RFLG_OWN;

		/* check next receive buffer */
		ds->ds_rindex++;
		if (ds->ds_rindex == NRCV)
			ds->ds_rindex = 0;
		rp = &ds->ds_rrent[ds->ds_rindex];
	}
}

/*
 * Pass a packet to the higher levels.
 * We deal with the trailer protocol here.
 */
void
deread(ds, ifrw, len)
	register struct de_softc *ds;
	struct ifrw *ifrw;
	int len;
{
	struct ether_header *eh;
    	struct mbuf *m;
	int s;
	register struct ifqueue *inq;

	/*
	 * Deal with trailer protocol: if type is trailer type
	 * get true type from first 16-bit word past data.
	 * Remember that type was trailer by setting off.
	 */
	eh = (struct ether_header *)ifrw->ifrw_addr;
	if (len == 0)
		return;

	/*
	 * Pull packet off interface.  Off is nonzero if packet
	 * has trailing header; if_ubaget will then force this header
	 * information to be at the front.
	 */
	m = if_ubaget(&ds->ds_deuba, ifrw, len, &ds->ds_if);
	if (m)
		ether_input(&ds->ds_if, eh, m);
}
/*
 * Process an ioctl request.
 */
int
deioctl(ifp, cmd, data)
	register struct ifnet *ifp;
	u_long	cmd;
	caddr_t data;
{
	register struct ifaddr *ifa = (struct ifaddr *)data;
	register struct de_softc *ds = decd.cd_devs[ifp->if_unit];
	int s = splnet(), error = 0;

	switch (cmd) {

	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		deinit(ifp->if_unit);

		switch (ifa->ifa_addr->sa_family) {
#ifdef INET
		case AF_INET:
			arp_ifinit(&ds->ds_ac, ifa);
			break;
#endif
#ifdef NS
		case AF_NS:
		    {
			register struct ns_addr *ina = &(IA_SNS(ifa)->sns_addr);
			
			if (ns_nullhost(*ina))
				ina->x_host = *(union ns_host *)(ds->ds_addr);
			else
				de_setaddr(ina->x_host.c_host,ifp->if_unit);
			break;
		    }
#endif
		}
		break;

	case SIOCSIFFLAGS:
		if ((ifp->if_flags & IFF_UP) == 0 &&
		    ds->ds_flags & DSF_RUNNING) {
			ds->ds_vaddr->pclow = 0;
			DELAY(5000);
			ds->ds_vaddr->pclow = PCSR0_RSET;
			ds->ds_flags &= ~DSF_RUNNING;
			ds->ds_if.if_flags &= ~IFF_OACTIVE;
		} else if (ifp->if_flags & IFF_UP &&
		    (ds->ds_flags & DSF_RUNNING) == 0)
			deinit(ifp->if_unit);
		break;

	default:
		error = EINVAL;
	}
	splx(s);
	return (error);
}

/*
 * set ethernet address for unit
 */
void
de_setaddr(physaddr, unit)
	u_char	*physaddr;
	int	unit;
{
	register struct de_softc *ds = decd.cd_devs[unit];
	volatile struct dedevice *addr= ds->ds_vaddr;
	
	if (! (ds->ds_flags & DSF_RUNNING))
		return;
		
	bcopy((caddr_t) physaddr, (caddr_t) &ds->ds_pcbb.pcbb2, 6);
	ds->ds_pcbb.pcbb0 = FC_WTPHYAD;
	addr->pclow = PCSR0_INTE|CMD_GETCMD;
	if (dewait(ds, "address change") == 0) {
		ds->ds_flags |= DSF_SETADDR;
		bcopy((caddr_t) physaddr, (caddr_t) ds->ds_addr, 6);
	}
}

/*
 * Await completion of the named function
 * and check for errors.
 */
int
dewait(ds, fn)
	register struct de_softc *ds;
	char *fn;
{
	volatile struct dedevice *addr = ds->ds_vaddr;
	register csr0;

	while ((addr->pcsr0 & PCSR0_INTR) == 0)
		;
	csr0 = addr->pcsr0;
	addr->pchigh = csr0 >> 8;
	if (csr0 & PCSR0_PCEI)
		printf("de%d: %s failed, csr0=%b csr1=%b\n", 
		    ds->ds_device.dv_unit, fn, csr0, PCSR0_BITS, 
		    addr->pcsr1, PCSR1_BITS);
	return (csr0 & PCSR0_PCEI);
}

int
dematch(parent, match, aux)
	struct	device *parent;
	void	*match, *aux;
{
	struct	de_softc *sc = match;
	struct	uba_attach_args *ua = aux;
	volatile struct	dedevice *addr = (struct dedevice *)ua->ua_addr;
	int	i;

	/*
	 * Make sure self-test is finished before we screw with the board.
	 * Self-test on a DELUA can take 15 seconds (argh).
	 */
	for (i = 0;
	     i < 160 &&
	     (addr->pcsr0 & PCSR0_FATI) == 0 &&
	     (addr->pcsr1 & PCSR1_STMASK) == STAT_RESET;
	     ++i)
		DELAY(50000);
	if ((addr->pcsr0 & PCSR0_FATI) != 0 ||
	    (addr->pcsr1 & PCSR1_STMASK) != STAT_READY &&
		(addr->pcsr1 & PCSR1_STMASK) != STAT_RUN)
		return(0);

	addr->pcsr0 = 0;
	DELAY(5000);
	addr->pcsr0 = PCSR0_RSET;
	while ((addr->pcsr0 & PCSR0_INTR) == 0)
		;
	/* make board interrupt by executing a GETPCBB command */
	addr->pcsr0 = PCSR0_INTE;
	addr->pcsr2 = 0;
	addr->pcsr3 = 0;
	addr->pcsr0 = PCSR0_INTE|CMD_GETPCBB;
	DELAY(50000);
	ua->ua_ivec = deintr;
	ua->ua_iarg = sc->ds_device.dv_unit;
	return 1;
}
