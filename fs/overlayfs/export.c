/*
 * Overlayfs NFS export support.
 *
 * Amir Goldstein <amir73il@gmail.com>
 *
 * Copyright (C) 2017-2018 CTERA Networks. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/exportfs.h>
#include <linux/ratelimit.h>
#include "overlayfs.h"

/*
 * We only need to encode origin if there is a chance that the same object was
 * encoded pre copy up and then we need to stay consistent with the same
 * encoding also after copy up. If non-pure upper is not indexed, then it was
 * copied up before NFS export was enabled. In that case we don't need to worry
 * about staying consistent with pre copy up encoding and we encode an upper
 * file handle. Overlay root dentry is a private case of non-indexed upper.
 *
 * The following table summarizes the different file handle encodings used for
 * different overlay object types:
 *
 *  Object type		| Encoding
 * --------------------------------
 *  Pure upper		| U
 *  Non-indexed upper	| U
 *  Indexed upper	| L
 *  Non-upper		| L
 *
 * U = upper file handle
 * L = lower file handle
 */
static bool ovl_should_encode_origin(struct dentry *dentry)
{
	if (!ovl_dentry_lower(dentry))
		return false;

	/* Decoding a non-indexed upper from origin is not implemented */
	if (ovl_dentry_upper(dentry) &&
	    !ovl_test_flag(OVL_INDEX, d_inode(dentry)))
		return false;

	return true;
}

static int ovl_d_to_fh(struct dentry *dentry, char *buf, int buflen)
{
	struct dentry *upper = ovl_dentry_upper(dentry);
	struct dentry *origin = ovl_dentry_lower(dentry);
	struct ovl_fh *fh = NULL;
	int err;

	if (!ovl_should_encode_origin(dentry))
		origin = NULL;

	err = -EACCES;
	if (!upper || origin)
		goto fail;

	/* TODO: encode non pure-upper by origin */
	fh = ovl_encode_fh(upper, true);

	err = -EOVERFLOW;
	if (fh->len > buflen)
		goto fail;

	memcpy(buf, (char *)fh, fh->len);
	err = fh->len;

out:
	kfree(fh);
	return err;

fail:
	pr_warn_ratelimited("overlayfs: failed to encode file handle (%pd2, err=%i, buflen=%d, len=%d, type=%d)\n",
			    dentry, err, buflen, fh ? (int)fh->len : 0,
			    fh ? fh->type : 0);
	goto out;
}

static int ovl_dentry_to_fh(struct dentry *dentry, u32 *fid, int *max_len)
{
	int res, len = *max_len << 2;

	res = ovl_d_to_fh(dentry, (char *)fid, len);
	if (res <= 0)
		return FILEID_INVALID;

	len = res;

	/* Round up to dwords */
	*max_len = (len + 3) >> 2;
	return OVL_FILEID;
}

static int ovl_encode_inode_fh(struct inode *inode, u32 *fid, int *max_len,
			       struct inode *parent)
{
	struct dentry *dentry;
	int type;

	/* TODO: encode connectable file handles */
	if (parent)
		return FILEID_INVALID;

	dentry = d_find_any_alias(inode);
	if (WARN_ON(!dentry))
		return FILEID_INVALID;

	type = ovl_dentry_to_fh(dentry, fid, max_len);

	dput(dentry);
	return type;
}

const struct export_operations ovl_export_operations = {
	.encode_fh	= ovl_encode_inode_fh,
};
