/*
 * OS Abstraction Layer
 *
 * $Copyright Open Broadcom Corporation$
 *
 * $Id: osl.h 320905 2012-03-13 15:33:25Z $
 */

#ifndef _osl_h_
#define _osl_h_


typedef struct osl_info osl_t;
typedef struct osl_dmainfo osldma_t;

#define OSL_PKTTAG_SZ	32 


typedef void (*pktfree_cb_fn_t)(void *ctx, void *pkt, unsigned int status);


typedef unsigned int (*osl_rreg_fn_t)(void *ctx, volatile void *reg, unsigned int size);
typedef void  (*osl_wreg_fn_t)(void *ctx, volatile void *reg, unsigned int val, unsigned int size);


#include <linux_osl.h>

#ifndef PKTDBG_TRACE
#define PKTDBG_TRACE(osh, pkt, bit)
#endif

#define PKTCTFMAP(osh, p)



#define	SET_REG(osh, r, mask, val)	W_REG((osh), (r), ((R_REG((osh), r) & ~(mask)) | (val)))

#ifndef AND_REG
#define AND_REG(osh, r, v)		W_REG(osh, (r), R_REG(osh, r) & (v))
#endif   

#ifndef OR_REG
#define OR_REG(osh, r, v)		W_REG(osh, (r), R_REG(osh, r) | (v))
#endif   

#if !defined(OSL_SYSUPTIME)
#define OSL_SYSUPTIME() (0)
#define OSL_SYSUPTIME_SUPPORT FALSE
#else
#define OSL_SYSUPTIME_SUPPORT TRUE
#endif 

#if !defined(PKTC)
#define	PKTCCNT(skb)		(0)
#define	PKTCLEN(skb)		(0)
#define	PKTCFLAGS(skb)		(0)
#define	PKTCSETCNT(skb, c)
#define	PKTCSETLEN(skb, l)
#define	PKTCSETFLAG(skb, fb)
#define	PKTCCLRFLAG(skb, fb)
#define	PKTCLINK(skb)		PKTLINK(skb)
#define	PKTSETCLINK(skb, x)	PKTSETLINK((skb), (x))
#define	PKTISCHAINED(skb)	FALSE
#define FOREACH_CHAINED_PKT(skb, nskb) \
	for ((nskb) = NULL; (skb) != NULL; (skb) = (nskb))
#define	PKTCFREE		PKTFREE
#endif


#endif	
