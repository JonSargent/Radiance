/* Copyright (c) 1991 Regents of the University of California */

#ifndef lint
static char SCCSid[] = "$SunId$ LBL";
#endif

/*
 * Fast malloc for memory hogs and VM environments.
 * Performs a minimum of searching through free lists.
 * malloc(), realloc() and free() use buckets
 *	containing blocks in powers of two, similar to CIT routines.
 * bfree() puts memory from any source into malloc free lists.
 * bmalloc() doesn't keep track of free lists -- it's usually
 *	just a buffered call to sbrk(2).  However, bmalloc will
 *	call mscrounge() if sbrk fails.
 * mscrounge() returns whatever free memory it can find to satisfy the
 *	request along with the number of bytes (modified).
 *
 *	Greg Ward	Lawrence Berkeley Laboratory
 */

#include  <errno.h>

#ifdef MSTATS
#include  <stdio.h>
static unsigned	b_nalloced = 0;
static unsigned	b_nfreed = 0;
static unsigned	b_nscrounged = 0;
static unsigned	m_nalloced = 0;
static unsigned	m_nfreed = 0;
static unsigned	m_nwasted = 0;
#else
#define  NULL		0
#endif

#ifndef ALIGN
#define  ALIGN		int			/* align type */
#endif
#define  BYTES_WORD	sizeof(ALIGN)

#define  MAXINCR	(1<<16)			/* largest sbrk(2) increment */

#ifdef  NOVMEM
#define  getpagesize()	1024
#endif
					/* malloc free lists */
typedef union m_head {
	union m_head	*next;
	struct {
		short		magic;
		short		bucket;
	}	a;
	ALIGN		dummy;
} M_HEAD;

#define MAGIC		0x1a2		/* magic number for allocated memory */

#define FIRSTBUCKET	3
#define NBUCKETS	30

static M_HEAD	*free_list[NBUCKETS];

static ALIGN	dummy_mem;

#define DUMMYLOC	((char *)&dummy_mem)


char *
mscrounge(np)		/* search free lists to satisfy request */
register unsigned	*np;
{
	char	*p;
	register int	bucket;
	register unsigned	bsiz;

	for (bucket = FIRSTBUCKET, bsiz = 1<<FIRSTBUCKET;
			bucket < NBUCKETS; bucket++, bsiz <<= 1)
		if (free_list[bucket] != NULL && bsiz+sizeof(M_HEAD) >= *np) {
			*np = bsiz+sizeof(M_HEAD);
			p = (char *)free_list[bucket];
			free_list[bucket] = free_list[bucket]->next;
#ifdef MSTATS
			b_nscrounged += *np;
#endif
			return(p);
		}
	*np = 0;
	return(NULL);
}


char *
bmalloc(n)		/* allocate a block of n bytes, sans header */
register unsigned  n;
{
	extern char  *sbrk();
	static unsigned  pagesz = 0;
	static unsigned  amnt;
	static char  *bpos;
	static unsigned  nrem;
	unsigned  thisamnt;
	register char	*p;

	if (pagesz == 0) {				/* initialize */
		pagesz = amnt = getpagesize();
		nrem = (int)sbrk(0);			/* page align break */
		nrem = pagesz - (nrem&(pagesz-1));
		bpos = sbrk(nrem);			/* word aligned! */
		if ((int)bpos == -1)
			return(NULL);
	}

	n = (n+(BYTES_WORD-1))&~(BYTES_WORD-1);		/* word align rqst. */

	if (n > nrem) {					/* need more core */
		if (n > amnt) {				/* big chunk */
			thisamnt = (n+(pagesz-1))&~(pagesz-1);
			if (thisamnt <= MAXINCR)	/* increase amnt */
				amnt = thisamnt;
		} else
			thisamnt = amnt;
		p = sbrk(thisamnt);
		if ((int)p == -1) {			/* uh-oh, ENOMEM */
			thisamnt = n;			/* search free lists */
			p = mscrounge(&thisamnt);
			if (p == NULL)			/* we're really out */
				return(NULL);
		}
		if (p != bpos+nrem) {			/* not an increment */
			bfree(bpos, nrem);		/* free what we have */
			bpos = p;
			nrem = thisamnt;
		} else					/* otherwise tack on */
			nrem += thisamnt;
	}
	p = bpos;
	bpos += n;					/* advance */
	nrem -= n;
#ifdef MSTATS
	b_nalloced += n;
#endif
	return(p);
}


bfree(p, n)			/* free n bytes of random memory */
register char  *p;
register unsigned  n;
{
	register int	bucket;
	register unsigned	bsiz;

#ifdef MSTATS
	b_nfreed += n;
#endif
					/* align pointer */
	bsiz = BYTES_WORD - ((unsigned)p&(BYTES_WORD-1));
	if (bsiz < BYTES_WORD) {
		p += bsiz;
		n -= bsiz;
	}
					/* fill big buckets first */
	for (bucket = NBUCKETS-1, bsiz = 1<<(NBUCKETS-1);
			bucket >= FIRSTBUCKET; bucket--, bsiz >>= 1)
		if (n >= bsiz+sizeof(M_HEAD)) {
			((M_HEAD *)p)->next = free_list[bucket];
			free_list[bucket] = (M_HEAD *)p;
			p += bsiz+sizeof(M_HEAD);
			n -= bsiz+sizeof(M_HEAD);
		}
}


char *
malloc(n)			/* allocate n bytes of memory */
unsigned	n;
{
	extern int  errno;
	register M_HEAD	*mp;
	register int	bucket;
	register unsigned	bsiz;
					/* don't return NULL on 0 request */
	if (n == 0)
		return(DUMMYLOC);
					/* find first bucket that fits */
	for (bucket = FIRSTBUCKET, bsiz = 1<<FIRSTBUCKET;
			bucket < NBUCKETS; bucket++, bsiz <<= 1)
		if (bsiz >= n)
			break;
	if (bucket >= NBUCKETS) {
		errno = EINVAL;
		return(NULL);
	}
	if (free_list[bucket] == NULL) {	/* need more core */
		mp = (M_HEAD *)bmalloc(bsiz+sizeof(M_HEAD));
		if (mp == NULL)
			return(NULL);
	} else {				/* got it */
		mp = free_list[bucket];
		free_list[bucket] = mp->next;
	}
#ifdef MSTATS
	m_nalloced += bsiz + sizeof(M_HEAD);
	m_nwasted += bsiz + sizeof(M_HEAD) - n;
#endif
	mp->a.magic = MAGIC;			/* tag block */
	mp->a.bucket = bucket;
	return((char *)(mp+1));
}


char *
realloc(op, n)			/* reallocate memory using malloc() */
register char	*op;
unsigned	n;
{
	char	*p;
	register unsigned	on;
					/* get old size */
	if (op != NULL && op != DUMMYLOC && ((M_HEAD *)op-1)->a.magic == MAGIC)
		on = 1 << ((M_HEAD *)op-1)->a.bucket;
	else
		on = 0;
	if (n <= on && (n > on>>1 || on == 1<<FIRSTBUCKET))
		return(op);		/* same bucket */
	if ((p = malloc(n)) == NULL)
		return(NULL);
	if (on) {
#ifdef  BSD
		bcopy(op, p, n>on ? on : n);
#else
		(void)memcpy(p, op, n>on ? on : n);
#endif
		free(op);
	}
	return(p);
}


free(p)			/* free memory allocated by malloc */
char	*p;
{
	register M_HEAD	*mp;
	register int	bucket;

	if (p == NULL || p == DUMMYLOC)
		return(1);
	mp = (M_HEAD *)p - 1;
	if (mp->a.magic != MAGIC)		/* sanity check */
		return(0);
	bucket = mp->a.bucket;
	mp->next = free_list[bucket];
	free_list[bucket] = mp;
#ifdef MSTATS
	m_nfreed += (1 << bucket) + sizeof(M_HEAD);
#endif
	return(1);
}


#ifndef NOVMEM
#ifndef BSD
#include <sys/var.h>
int
getpagesize()			/* use SYSV var structure to get page size */
{
	struct var  v;

	uvar(&v);
	return(1 << v.v_pageshift);
}
#endif
#endif


#ifdef MSTATS
printmemstats(fp)		/* print memory statistics to stream */
FILE	*fp;
{
	register int	i;
	int	n;
	register M_HEAD	*mp;
	unsigned int	total = 0;

	fprintf(fp, "Memory statistics:\n");
	fprintf(fp, "\tbmalloc: %d bytes allocated\n", b_nalloced);
	fprintf(fp, "\tbmalloc: %d bytes freed\n", b_nfreed);
	fprintf(fp, "\tbmalloc: %d bytes scrounged\n", b_nscrounged);
	fprintf(fp, "\tmalloc: %d bytes allocated\n", m_nalloced);
	fprintf(fp, "\tmalloc: %d bytes wasted (%.1f%%)\n", m_nwasted,
			100.0*m_nwasted/m_nalloced);
	fprintf(fp, "\tmalloc: %d bytes freed\n", m_nfreed);
	for (i = NBUCKETS-1; i >= FIRSTBUCKET; i--) {
		n = 0;
		for (mp = free_list[i]; mp != NULL; mp = mp->next)
			n++;
		if (n) {
			fprintf(fp, "\t%d * %u\n", n, 1<<i);
			total += n * ((1<<i) + sizeof(M_HEAD));
		}
	}
	fprintf(fp, "\t %u total bytes in free list\n", total);
}
#endif
