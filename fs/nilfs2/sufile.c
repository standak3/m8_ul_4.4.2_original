/*
 * sufile.c - NILFS segment usage file.
 *
 * Copyright (C) 2006-2008 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Written by Koji Sato <koji@osrg.net>.
 * Revised by Ryusuke Konishi <ryusuke@osrg.net>.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/nilfs2_fs.h>
#include "mdt.h"
#include "sufile.h"


struct nilfs_sufile_info {
	struct nilfs_mdt_info mi;
	unsigned long ncleansegs;
	__u64 allocmin;		
	__u64 allocmax;		
};

static inline struct nilfs_sufile_info *NILFS_SUI(struct inode *sufile)
{
	return (struct nilfs_sufile_info *)NILFS_MDT(sufile);
}

static inline unsigned long
nilfs_sufile_segment_usages_per_block(const struct inode *sufile)
{
	return NILFS_MDT(sufile)->mi_entries_per_block;
}

static unsigned long
nilfs_sufile_get_blkoff(const struct inode *sufile, __u64 segnum)
{
	__u64 t = segnum + NILFS_MDT(sufile)->mi_first_entry_offset;
	do_div(t, nilfs_sufile_segment_usages_per_block(sufile));
	return (unsigned long)t;
}

static unsigned long
nilfs_sufile_get_offset(const struct inode *sufile, __u64 segnum)
{
	__u64 t = segnum + NILFS_MDT(sufile)->mi_first_entry_offset;
	return do_div(t, nilfs_sufile_segment_usages_per_block(sufile));
}

static unsigned long
nilfs_sufile_segment_usages_in_block(const struct inode *sufile, __u64 curr,
				     __u64 max)
{
	return min_t(unsigned long,
		     nilfs_sufile_segment_usages_per_block(sufile) -
		     nilfs_sufile_get_offset(sufile, curr),
		     max - curr + 1);
}

static struct nilfs_segment_usage *
nilfs_sufile_block_get_segment_usage(const struct inode *sufile, __u64 segnum,
				     struct buffer_head *bh, void *kaddr)
{
	return kaddr + bh_offset(bh) +
		nilfs_sufile_get_offset(sufile, segnum) *
		NILFS_MDT(sufile)->mi_entry_size;
}

static inline int nilfs_sufile_get_header_block(struct inode *sufile,
						struct buffer_head **bhp)
{
	return nilfs_mdt_get_block(sufile, 0, 0, NULL, bhp);
}

static inline int
nilfs_sufile_get_segment_usage_block(struct inode *sufile, __u64 segnum,
				     int create, struct buffer_head **bhp)
{
	return nilfs_mdt_get_block(sufile,
				   nilfs_sufile_get_blkoff(sufile, segnum),
				   create, NULL, bhp);
}

static int nilfs_sufile_delete_segment_usage_block(struct inode *sufile,
						   __u64 segnum)
{
	return nilfs_mdt_delete_block(sufile,
				      nilfs_sufile_get_blkoff(sufile, segnum));
}

static void nilfs_sufile_mod_counter(struct buffer_head *header_bh,
				     u64 ncleanadd, u64 ndirtyadd)
{
	struct nilfs_sufile_header *header;
	void *kaddr;

	kaddr = kmap_atomic(header_bh->b_page);
	header = kaddr + bh_offset(header_bh);
	le64_add_cpu(&header->sh_ncleansegs, ncleanadd);
	le64_add_cpu(&header->sh_ndirtysegs, ndirtyadd);
	kunmap_atomic(kaddr);

	mark_buffer_dirty(header_bh);
}

unsigned long nilfs_sufile_get_ncleansegs(struct inode *sufile)
{
	return NILFS_SUI(sufile)->ncleansegs;
}

int nilfs_sufile_updatev(struct inode *sufile, __u64 *segnumv, size_t nsegs,
			 int create, size_t *ndone,
			 void (*dofunc)(struct inode *, __u64,
					struct buffer_head *,
					struct buffer_head *))
{
	struct buffer_head *header_bh, *bh;
	unsigned long blkoff, prev_blkoff;
	__u64 *seg;
	size_t nerr = 0, n = 0;
	int ret = 0;

	if (unlikely(nsegs == 0))
		goto out;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	for (seg = segnumv; seg < segnumv + nsegs; seg++) {
		if (unlikely(*seg >= nilfs_sufile_get_nsegments(sufile))) {
			printk(KERN_WARNING
			       "%s: invalid segment number: %llu\n", __func__,
			       (unsigned long long)*seg);
			nerr++;
		}
	}
	if (nerr > 0) {
		ret = -EINVAL;
		goto out_sem;
	}

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	seg = segnumv;
	blkoff = nilfs_sufile_get_blkoff(sufile, *seg);
	ret = nilfs_mdt_get_block(sufile, blkoff, create, NULL, &bh);
	if (ret < 0)
		goto out_header;

	for (;;) {
		dofunc(sufile, *seg, header_bh, bh);

		if (++seg >= segnumv + nsegs)
			break;
		prev_blkoff = blkoff;
		blkoff = nilfs_sufile_get_blkoff(sufile, *seg);
		if (blkoff == prev_blkoff)
			continue;

		
		brelse(bh);
		ret = nilfs_mdt_get_block(sufile, blkoff, create, NULL, &bh);
		if (unlikely(ret < 0))
			goto out_header;
	}
	brelse(bh);

 out_header:
	n = seg - segnumv;
	brelse(header_bh);
 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
 out:
	if (ndone)
		*ndone = n;
	return ret;
}

int nilfs_sufile_update(struct inode *sufile, __u64 segnum, int create,
			void (*dofunc)(struct inode *, __u64,
				       struct buffer_head *,
				       struct buffer_head *))
{
	struct buffer_head *header_bh, *bh;
	int ret;

	if (unlikely(segnum >= nilfs_sufile_get_nsegments(sufile))) {
		printk(KERN_WARNING "%s: invalid segment number: %llu\n",
		       __func__, (unsigned long long)segnum);
		return -EINVAL;
	}
	down_write(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, create, &bh);
	if (!ret) {
		dofunc(sufile, segnum, header_bh, bh);
		brelse(bh);
	}
	brelse(header_bh);

 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

int nilfs_sufile_set_alloc_range(struct inode *sufile, __u64 start, __u64 end)
{
	struct nilfs_sufile_info *sui = NILFS_SUI(sufile);
	__u64 nsegs;
	int ret = -ERANGE;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	nsegs = nilfs_sufile_get_nsegments(sufile);

	if (start <= end && end < nsegs) {
		sui->allocmin = start;
		sui->allocmax = end;
		ret = 0;
	}
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

int nilfs_sufile_alloc(struct inode *sufile, __u64 *segnump)
{
	struct buffer_head *header_bh, *su_bh;
	struct nilfs_sufile_header *header;
	struct nilfs_segment_usage *su;
	struct nilfs_sufile_info *sui = NILFS_SUI(sufile);
	size_t susz = NILFS_MDT(sufile)->mi_entry_size;
	__u64 segnum, maxsegnum, last_alloc;
	void *kaddr;
	unsigned long nsegments, ncleansegs, nsus, cnt;
	int ret, j;

	down_write(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;
	kaddr = kmap_atomic(header_bh->b_page);
	header = kaddr + bh_offset(header_bh);
	ncleansegs = le64_to_cpu(header->sh_ncleansegs);
	last_alloc = le64_to_cpu(header->sh_last_alloc);
	kunmap_atomic(kaddr);

	nsegments = nilfs_sufile_get_nsegments(sufile);
	maxsegnum = sui->allocmax;
	segnum = last_alloc + 1;
	if (segnum < sui->allocmin || segnum > sui->allocmax)
		segnum = sui->allocmin;

	for (cnt = 0; cnt < nsegments; cnt += nsus) {
		if (segnum > maxsegnum) {
			if (cnt < sui->allocmax - sui->allocmin + 1) {
				segnum = sui->allocmin;
				maxsegnum = last_alloc;
			} else if (segnum > sui->allocmin &&
				   sui->allocmax + 1 < nsegments) {
				segnum = sui->allocmax + 1;
				maxsegnum = nsegments - 1;
			} else if (sui->allocmin > 0)  {
				segnum = 0;
				maxsegnum = sui->allocmin - 1;
			} else {
				break; 
			}
		}
		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 1,
							   &su_bh);
		if (ret < 0)
			goto out_header;
		kaddr = kmap_atomic(su_bh->b_page);
		su = nilfs_sufile_block_get_segment_usage(
			sufile, segnum, su_bh, kaddr);

		nsus = nilfs_sufile_segment_usages_in_block(
			sufile, segnum, maxsegnum);
		for (j = 0; j < nsus; j++, su = (void *)su + susz, segnum++) {
			if (!nilfs_segment_usage_clean(su))
				continue;
			
			nilfs_segment_usage_set_dirty(su);
			kunmap_atomic(kaddr);

			kaddr = kmap_atomic(header_bh->b_page);
			header = kaddr + bh_offset(header_bh);
			le64_add_cpu(&header->sh_ncleansegs, -1);
			le64_add_cpu(&header->sh_ndirtysegs, 1);
			header->sh_last_alloc = cpu_to_le64(segnum);
			kunmap_atomic(kaddr);

			sui->ncleansegs--;
			mark_buffer_dirty(header_bh);
			mark_buffer_dirty(su_bh);
			nilfs_mdt_mark_dirty(sufile);
			brelse(su_bh);
			*segnump = segnum;
			goto out_header;
		}

		kunmap_atomic(kaddr);
		brelse(su_bh);
	}

	
	ret = -ENOSPC;

 out_header:
	brelse(header_bh);

 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

void nilfs_sufile_do_cancel_free(struct inode *sufile, __u64 segnum,
				 struct buffer_head *header_bh,
				 struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	void *kaddr;

	kaddr = kmap_atomic(su_bh->b_page);
	su = nilfs_sufile_block_get_segment_usage(sufile, segnum, su_bh, kaddr);
	if (unlikely(!nilfs_segment_usage_clean(su))) {
		printk(KERN_WARNING "%s: segment %llu must be clean\n",
		       __func__, (unsigned long long)segnum);
		kunmap_atomic(kaddr);
		return;
	}
	nilfs_segment_usage_set_dirty(su);
	kunmap_atomic(kaddr);

	nilfs_sufile_mod_counter(header_bh, -1, 1);
	NILFS_SUI(sufile)->ncleansegs--;

	mark_buffer_dirty(su_bh);
	nilfs_mdt_mark_dirty(sufile);
}

void nilfs_sufile_do_scrap(struct inode *sufile, __u64 segnum,
			   struct buffer_head *header_bh,
			   struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	void *kaddr;
	int clean, dirty;

	kaddr = kmap_atomic(su_bh->b_page);
	su = nilfs_sufile_block_get_segment_usage(sufile, segnum, su_bh, kaddr);
	if (su->su_flags == cpu_to_le32(1UL << NILFS_SEGMENT_USAGE_DIRTY) &&
	    su->su_nblocks == cpu_to_le32(0)) {
		kunmap_atomic(kaddr);
		return;
	}
	clean = nilfs_segment_usage_clean(su);
	dirty = nilfs_segment_usage_dirty(su);

	
	su->su_lastmod = cpu_to_le64(0);
	su->su_nblocks = cpu_to_le32(0);
	su->su_flags = cpu_to_le32(1UL << NILFS_SEGMENT_USAGE_DIRTY);
	kunmap_atomic(kaddr);

	nilfs_sufile_mod_counter(header_bh, clean ? (u64)-1 : 0, dirty ? 0 : 1);
	NILFS_SUI(sufile)->ncleansegs -= clean;

	mark_buffer_dirty(su_bh);
	nilfs_mdt_mark_dirty(sufile);
}

void nilfs_sufile_do_free(struct inode *sufile, __u64 segnum,
			  struct buffer_head *header_bh,
			  struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	void *kaddr;
	int sudirty;

	kaddr = kmap_atomic(su_bh->b_page);
	su = nilfs_sufile_block_get_segment_usage(sufile, segnum, su_bh, kaddr);
	if (nilfs_segment_usage_clean(su)) {
		printk(KERN_WARNING "%s: segment %llu is already clean\n",
		       __func__, (unsigned long long)segnum);
		kunmap_atomic(kaddr);
		return;
	}
	WARN_ON(nilfs_segment_usage_error(su));
	WARN_ON(!nilfs_segment_usage_dirty(su));

	sudirty = nilfs_segment_usage_dirty(su);
	nilfs_segment_usage_set_clean(su);
	kunmap_atomic(kaddr);
	mark_buffer_dirty(su_bh);

	nilfs_sufile_mod_counter(header_bh, 1, sudirty ? (u64)-1 : 0);
	NILFS_SUI(sufile)->ncleansegs++;

	nilfs_mdt_mark_dirty(sufile);
}

int nilfs_sufile_mark_dirty(struct inode *sufile, __u64 segnum)
{
	struct buffer_head *bh;
	int ret;

	ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0, &bh);
	if (!ret) {
		mark_buffer_dirty(bh);
		nilfs_mdt_mark_dirty(sufile);
		brelse(bh);
	}
	return ret;
}

int nilfs_sufile_set_segment_usage(struct inode *sufile, __u64 segnum,
				   unsigned long nblocks, time_t modtime)
{
	struct buffer_head *bh;
	struct nilfs_segment_usage *su;
	void *kaddr;
	int ret;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0, &bh);
	if (ret < 0)
		goto out_sem;

	kaddr = kmap_atomic(bh->b_page);
	su = nilfs_sufile_block_get_segment_usage(sufile, segnum, bh, kaddr);
	WARN_ON(nilfs_segment_usage_error(su));
	if (modtime)
		su->su_lastmod = cpu_to_le64(modtime);
	su->su_nblocks = cpu_to_le32(nblocks);
	kunmap_atomic(kaddr);

	mark_buffer_dirty(bh);
	nilfs_mdt_mark_dirty(sufile);
	brelse(bh);

 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

int nilfs_sufile_get_stat(struct inode *sufile, struct nilfs_sustat *sustat)
{
	struct buffer_head *header_bh;
	struct nilfs_sufile_header *header;
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	void *kaddr;
	int ret;

	down_read(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	kaddr = kmap_atomic(header_bh->b_page);
	header = kaddr + bh_offset(header_bh);
	sustat->ss_nsegs = nilfs_sufile_get_nsegments(sufile);
	sustat->ss_ncleansegs = le64_to_cpu(header->sh_ncleansegs);
	sustat->ss_ndirtysegs = le64_to_cpu(header->sh_ndirtysegs);
	sustat->ss_ctime = nilfs->ns_ctime;
	sustat->ss_nongc_ctime = nilfs->ns_nongc_ctime;
	spin_lock(&nilfs->ns_last_segment_lock);
	sustat->ss_prot_seq = nilfs->ns_prot_seq;
	spin_unlock(&nilfs->ns_last_segment_lock);
	kunmap_atomic(kaddr);
	brelse(header_bh);

 out_sem:
	up_read(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

void nilfs_sufile_do_set_error(struct inode *sufile, __u64 segnum,
			       struct buffer_head *header_bh,
			       struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	void *kaddr;
	int suclean;

	kaddr = kmap_atomic(su_bh->b_page);
	su = nilfs_sufile_block_get_segment_usage(sufile, segnum, su_bh, kaddr);
	if (nilfs_segment_usage_error(su)) {
		kunmap_atomic(kaddr);
		return;
	}
	suclean = nilfs_segment_usage_clean(su);
	nilfs_segment_usage_set_error(su);
	kunmap_atomic(kaddr);

	if (suclean) {
		nilfs_sufile_mod_counter(header_bh, -1, 0);
		NILFS_SUI(sufile)->ncleansegs--;
	}
	mark_buffer_dirty(su_bh);
	nilfs_mdt_mark_dirty(sufile);
}

static int nilfs_sufile_truncate_range(struct inode *sufile,
				       __u64 start, __u64 end)
{
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	struct buffer_head *header_bh;
	struct buffer_head *su_bh;
	struct nilfs_segment_usage *su, *su2;
	size_t susz = NILFS_MDT(sufile)->mi_entry_size;
	unsigned long segusages_per_block;
	unsigned long nsegs, ncleaned;
	__u64 segnum;
	void *kaddr;
	ssize_t n, nc;
	int ret;
	int j;

	nsegs = nilfs_sufile_get_nsegments(sufile);

	ret = -EINVAL;
	if (start > end || start >= nsegs)
		goto out;

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out;

	segusages_per_block = nilfs_sufile_segment_usages_per_block(sufile);
	ncleaned = 0;

	for (segnum = start; segnum <= end; segnum += n) {
		n = min_t(unsigned long,
			  segusages_per_block -
				  nilfs_sufile_get_offset(sufile, segnum),
			  end - segnum + 1);
		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0,
							   &su_bh);
		if (ret < 0) {
			if (ret != -ENOENT)
				goto out_header;
			
			continue;
		}
		kaddr = kmap_atomic(su_bh->b_page);
		su = nilfs_sufile_block_get_segment_usage(
			sufile, segnum, su_bh, kaddr);
		su2 = su;
		for (j = 0; j < n; j++, su = (void *)su + susz) {
			if ((le32_to_cpu(su->su_flags) &
			     ~(1UL << NILFS_SEGMENT_USAGE_ERROR)) ||
			    nilfs_segment_is_active(nilfs, segnum + j)) {
				ret = -EBUSY;
				kunmap_atomic(kaddr);
				brelse(su_bh);
				goto out_header;
			}
		}
		nc = 0;
		for (su = su2, j = 0; j < n; j++, su = (void *)su + susz) {
			if (nilfs_segment_usage_error(su)) {
				nilfs_segment_usage_set_clean(su);
				nc++;
			}
		}
		kunmap_atomic(kaddr);
		if (nc > 0) {
			mark_buffer_dirty(su_bh);
			ncleaned += nc;
		}
		brelse(su_bh);

		if (n == segusages_per_block) {
			
			nilfs_sufile_delete_segment_usage_block(sufile, segnum);
		}
	}
	ret = 0;

out_header:
	if (ncleaned > 0) {
		NILFS_SUI(sufile)->ncleansegs += ncleaned;
		nilfs_sufile_mod_counter(header_bh, ncleaned, 0);
		nilfs_mdt_mark_dirty(sufile);
	}
	brelse(header_bh);
out:
	return ret;
}

int nilfs_sufile_resize(struct inode *sufile, __u64 newnsegs)
{
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	struct buffer_head *header_bh;
	struct nilfs_sufile_header *header;
	struct nilfs_sufile_info *sui = NILFS_SUI(sufile);
	void *kaddr;
	unsigned long nsegs, nrsvsegs;
	int ret = 0;

	down_write(&NILFS_MDT(sufile)->mi_sem);

	nsegs = nilfs_sufile_get_nsegments(sufile);
	if (nsegs == newnsegs)
		goto out;

	ret = -ENOSPC;
	nrsvsegs = nilfs_nrsvsegs(nilfs, newnsegs);
	if (newnsegs < nsegs && nsegs - newnsegs + nrsvsegs > sui->ncleansegs)
		goto out;

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out;

	if (newnsegs > nsegs) {
		sui->ncleansegs += newnsegs - nsegs;
	} else  {
		ret = nilfs_sufile_truncate_range(sufile, newnsegs, nsegs - 1);
		if (ret < 0)
			goto out_header;

		sui->ncleansegs -= nsegs - newnsegs;
	}

	kaddr = kmap_atomic(header_bh->b_page);
	header = kaddr + bh_offset(header_bh);
	header->sh_ncleansegs = cpu_to_le64(sui->ncleansegs);
	kunmap_atomic(kaddr);

	mark_buffer_dirty(header_bh);
	nilfs_mdt_mark_dirty(sufile);
	nilfs_set_nsegments(nilfs, newnsegs);

out_header:
	brelse(header_bh);
out:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

ssize_t nilfs_sufile_get_suinfo(struct inode *sufile, __u64 segnum, void *buf,
				unsigned sisz, size_t nsi)
{
	struct buffer_head *su_bh;
	struct nilfs_segment_usage *su;
	struct nilfs_suinfo *si = buf;
	size_t susz = NILFS_MDT(sufile)->mi_entry_size;
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	void *kaddr;
	unsigned long nsegs, segusages_per_block;
	ssize_t n;
	int ret, i, j;

	down_read(&NILFS_MDT(sufile)->mi_sem);

	segusages_per_block = nilfs_sufile_segment_usages_per_block(sufile);
	nsegs = min_t(unsigned long,
		      nilfs_sufile_get_nsegments(sufile) - segnum,
		      nsi);
	for (i = 0; i < nsegs; i += n, segnum += n) {
		n = min_t(unsigned long,
			  segusages_per_block -
				  nilfs_sufile_get_offset(sufile, segnum),
			  nsegs - i);
		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0,
							   &su_bh);
		if (ret < 0) {
			if (ret != -ENOENT)
				goto out;
			
			memset(si, 0, sisz * n);
			si = (void *)si + sisz * n;
			continue;
		}

		kaddr = kmap_atomic(su_bh->b_page);
		su = nilfs_sufile_block_get_segment_usage(
			sufile, segnum, su_bh, kaddr);
		for (j = 0; j < n;
		     j++, su = (void *)su + susz, si = (void *)si + sisz) {
			si->sui_lastmod = le64_to_cpu(su->su_lastmod);
			si->sui_nblocks = le32_to_cpu(su->su_nblocks);
			si->sui_flags = le32_to_cpu(su->su_flags) &
				~(1UL << NILFS_SEGMENT_USAGE_ACTIVE);
			if (nilfs_segment_is_active(nilfs, segnum + j))
				si->sui_flags |=
					(1UL << NILFS_SEGMENT_USAGE_ACTIVE);
		}
		kunmap_atomic(kaddr);
		brelse(su_bh);
	}
	ret = nsegs;

 out:
	up_read(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

int nilfs_sufile_read(struct super_block *sb, size_t susize,
		      struct nilfs_inode *raw_inode, struct inode **inodep)
{
	struct inode *sufile;
	struct nilfs_sufile_info *sui;
	struct buffer_head *header_bh;
	struct nilfs_sufile_header *header;
	void *kaddr;
	int err;

	sufile = nilfs_iget_locked(sb, NULL, NILFS_SUFILE_INO);
	if (unlikely(!sufile))
		return -ENOMEM;
	if (!(sufile->i_state & I_NEW))
		goto out;

	err = nilfs_mdt_init(sufile, NILFS_MDT_GFP, sizeof(*sui));
	if (err)
		goto failed;

	nilfs_mdt_set_entry_size(sufile, susize,
				 sizeof(struct nilfs_sufile_header));

	err = nilfs_read_inode_common(sufile, raw_inode);
	if (err)
		goto failed;

	err = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (err)
		goto failed;

	sui = NILFS_SUI(sufile);
	kaddr = kmap_atomic(header_bh->b_page);
	header = kaddr + bh_offset(header_bh);
	sui->ncleansegs = le64_to_cpu(header->sh_ncleansegs);
	kunmap_atomic(kaddr);
	brelse(header_bh);

	sui->allocmax = nilfs_sufile_get_nsegments(sufile) - 1;
	sui->allocmin = 0;

	unlock_new_inode(sufile);
 out:
	*inodep = sufile;
	return 0;
 failed:
	iget_failed(sufile);
	return err;
}