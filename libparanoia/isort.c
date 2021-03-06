/* @(#)isort.c	1.20 19/04/03 J. Schilling from cdparanoia-III-alpha9.8 */
#include <schily/mconfig.h>
#ifndef lint
static	UConst char sccsid[] =
"@(#)isort.c	1.20 19/04/03 J. Schilling from cdparanoia-III-alpha9.8";

#endif
/*
 * CopyPolicy: GNU Lesser General Public License v2.1 applies
 * Copyright (C) 1997-2001,2008 by Monty (xiphmont@mit.edu)
 * Copyright (C) 2002-2019 by J. Schilling
 *
 * sorted vector abstraction for paranoia
 *
 */

/*
 * Old isort got a bit complex.  This re-constrains complexity to
 * give a go at speed through a more alpha-6-like mechanism.
 */

/*
 * "Sort" is a bit of a misnomer in this implementation.  It's actually
 * basically a hash table of sample values (with a linked-list collision
 * resolution), which lets you quickly determine where in a vector a
 * particular sample value occurs.
 *
 * Collisions aren't due to hash collisions, as the table has one bucket
 * for each possible sample value.  Instead, the "collisions" represent
 * multiple occurrences of a given value.
 */

#include <schily/stdlib.h>
#include <schily/standard.h>
#include <schily/utypes.h>
#include <schily/string.h>
#include "p_block.h"
#include "isort.h"
#include "pmalloc.h"

EXPORT	sort_info	*sort_alloc	__PR((long size));
EXPORT	void		sort_unsortall	__PR((sort_info * i));
EXPORT	void		sort_free	__PR((sort_info * i));
LOCAL	void		sort_sort	__PR((sort_info * i,
						long sortlo, long sorthi));
EXPORT	void		sort_setup	__PR((sort_info * i,
						Int16_t * vector,
						long *abspos, long size,
						long sortlo, long sorthi));
EXPORT	sort_link	*sort_getmatch	__PR((sort_info * i,
						long post, long overlap,
						int value));
EXPORT	sort_link	*sort_nextmatch	__PR((sort_info * i, sort_link * prev));


/*
 * sort_alloc()
 *
 * Allocates and initializes a new, empty sort_info object, which can be
 * used to index up to (size) samples from a vector.
 */
EXPORT sort_info *
sort_alloc(size)
	long	size;
{
	sort_info	*ret = _pcalloc(1, sizeof (sort_info));

	DBG_MALLOC_MARK(ret);
	ret->vector = NULL;
	ret->sortbegin = -1;
	ret->size = -1;
	ret->maxsize = size;

	ret->head = _pcalloc(65536, sizeof (sort_link *));
	DBG_MALLOC_MARK(ret->head);
	ret->bucketusage = _pmalloc(65536 * sizeof (long));
	DBG_MALLOC_MARK(ret->bucketusage);
	ret->revindex = _pcalloc(size, sizeof (sort_link));
	DBG_MALLOC_MARK(ret->revindex);
	ret->lastbucket = 0;

	return (ret);
}

/*
 * sort_unsortall() (internal)
 *
 * This function resets the index for further use with a different vector
 * or range, without the overhead of an unnecessary free/alloc.
 */
EXPORT void
sort_unsortall(i)
	sort_info	*i;
{
	/*
	 * If there were few enough different samples encountered (and hence few
	 * enough buckets used), we can just zero out those buckets.  If there
	 * were many (2000 is picked somewhat arbitrarily), it's faster simply to
	 * zero out all buckets with a memset() rather than walking the data
	 * structure and zeroing them out one by one.
	 */
	if (i->lastbucket > 2000) {	/* a guess */
		memset(i->head, 0, 65536 * sizeof (sort_link *));
	} else {
		long	b;

		for (b = 0; b < i->lastbucket; b++)
			i->head[i->bucketusage[b]] = NULL;
	}

	i->lastbucket = 0;
	i->sortbegin = -1;

	/*
	 * Curiously, this function preserves the vector association created
	 * by sort_setup(), but it is used only internally by sort_setup, so
	 * preserving this association is unnecessary.
	 */
}

/*
 * sort_free()
 *
 * Releases all memory consumed by a sort_info object.
 */
EXPORT void
sort_free(i)
	sort_info	*i;
{
	_pfree(i->revindex);
	_pfree(i->head);
	_pfree(i->bucketusage);
	_pfree(i);
}

/*
 * sort_sort() (internal)
 *
 * This function builds the index to allow for fast searching for sample
 * values within a portion (sortlo - sorthi) of the object's associated
 * vector.  It is called internally and only when needed.
 */
LOCAL void
sort_sort(i, sortlo, sorthi)
	sort_info	*i;
	long		sortlo;
	long		sorthi;
{
	long	j;

	/*
	 * We walk backward through the range to index because we insert new
	 * samples at the head of each bucket's list.  At the end, they'll be
	 * sorted from first to last occurrence.
	 */
	for (j = sorthi - 1; j >= sortlo; j--) {
		/*
		 * i->vector[j] = the signed 16-bit sample to index.
		 * hv		= pointer to the head of the sorted list of occurences
		 *		  of this sample
		 * l		= the node to associate with this sample
		 *
		 * We add 32768 to convert the signed 16-bit integer to an unsigned
		 * range from 0 to 65535.
		 *
		 * Note that l is located within i->revindex at a position
		 * corresponding to the sample's position in the vector.  This allows
		 * ipos() to determine the sample position from a returned sort_link.
		 */
		sort_link	**hv = i->head + i->vector[j] + 32768;
		sort_link	 *l = i->revindex + j;

		/*
		 * If this is the first time we've encountered this sample, add its
		 * bucket to the list of buckets used.  This list is used only for
		 * resetting the index quickly.
		 */
		if (*hv == NULL) {
			i->bucketusage[i->lastbucket] = i->vector[j] + 32768;
			i->lastbucket++;
		}
		/*
		 * Point the new node at the old head, then assign the new node as
		 * the new head.
		 */
		l->next = *hv;
		*hv = l;
	}
	/*
	 * Mark the index as initialized.
	 */
	i->sortbegin = 0;
}

/*
 * sort_setup()
 *
 * This function initializes a previously allocated sort_info_t.  The
 * sort_info_t is associated with a vector of samples of length
 * (size), whose position begins at (*abspos) within the CD's stream
 * of samples.  Only the range of samples between (sortlo, sorthi)
 * will eventually be indexed for fast searching.  (sortlo, sorthi)
 * are absolute sample positions.
 *
 * Note: size *must* be <= i->maxsize given to the preceding sort_alloc(),
 * but no error checking is done here.
 */
EXPORT void
sort_setup(i, vector, abspos, size, sortlo, sorthi)
	sort_info	*i;
	Int16_t		*vector;
	long		*abspos;
	long		size;
	long		sortlo;
	long		sorthi;
{
	/*
	 * Reset the index if it has already been built.
	 */
	if (i->sortbegin != -1)
		sort_unsortall(i);

	i->vector = vector;
	i->size = size;
	i->abspos = abspos;

	/*
	 * Convert the absolute (sortlo, sorthi) to offsets within the vector.
	 * Note that the index will not be built until sort_getmatch() is called.
	 * Here we're simply hanging on to the range to index until then.
	 */
	i->lo = min(size, max(sortlo - *abspos, 0));
	i->hi = max(0, min(sorthi - *abspos, size));
}

/*
 * sort_getmatch()
 *
 * This function returns a sort_link_t pointer which refers to the
 * first sample equal to (value) in the vector.  It only searches for
 * hits within (overlap) samples of (post), where (post) is an offset
 * within the vector.  The caller can determine the position of the
 * matched sample using ipos(sort_info *, sort_link *).
 *
 * This function returns NULL if no matches were found.
 */
EXPORT sort_link *
sort_getmatch(i, post, overlap, value)
	sort_info	*i;
	long		post;
	long		overlap;
	int		value;
{
	sort_link	*ret;

	/*
	 * If the vector hasn't been indexed yet, index it now.
	 */
	if (i->sortbegin == -1)
		sort_sort(i, i->lo, i->hi);
	/*
	 * Now we reuse lo and hi
	 *
	 * We'll only return samples within (overlap) samples of (post).
	 * Clamp the boundaries to search to the boundaries of the array,
	 * convert the signed sample to an unsigned offset, and store the
	 * state so that future calls to sort_nextmatch do the right thing.
	 *
	 * Reusing lo and hi this way is awful.
	 */
	post = max(0, min(i->size, post));
	i->val = value + 32768;
	i->lo = max(0, post - overlap);		/* absolute position */
	i->hi = min(i->size, post + overlap);	/* absolute position */

	/*
	 * Walk through the linked list of samples with this value, until
	 * we find the first one within the bounds specified.  If there
	 * aren't any, return NULL.
	 */
	ret = i->head[i->val];
	while (ret) {
		/*
		 * ipos() calculates the offset (in terms of the original vector)
		 * of this hit.
		 */
		if (ipos(i, ret) < i->lo) {
			ret = ret->next;
		} else {
			if (ipos(i, ret) >= i->hi)
				ret = NULL;
			break;
		}
	}
/*	i->head[i->val]=ret; */
	return (ret);
}

/*
 * sort_nextmatch()
 *
 * This function returns a sort_link_t pointer which refers to the next sample
 * matching the criteria previously passed to sort_getmatch().  See
 * sort_getmatch() for details.
 *
 * This function returns NULL if no further matches were found.
 */
EXPORT sort_link *
sort_nextmatch(i, prev)
	sort_info	*i;
	sort_link	*prev;
{
	sort_link	*ret = prev->next;

	/*
	 * If there aren't any more hits, or we've passed the boundary requested
	 * of sort_getmatch(), we're done.
	 */
	if (!ret || ipos(i, ret) >= i->hi)
		return (NULL);
	return (ret);
}
