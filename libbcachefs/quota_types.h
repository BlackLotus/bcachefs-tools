#ifndef _BCACHEFS_QUOTA_TYPES_H
#define _BCACHEFS_QUOTA_TYPES_H

#include <linux/generic-radix-tree.h>

struct bch_qid {
	u32		q[QTYP_NR];
};

struct memquota_counter {
	u64				v;
	u64				hardlimit;
	u64				softlimit;
	s64				timer;
	int				warns;
	int				warning_issued;
};

struct bch_memquota {
	struct memquota_counter		c[Q_COUNTERS];
};

typedef GENRADIX(struct bch_memquota)	bch_memquota_table;

struct quota_limit {
	u32				timelimit;
	u32				warnlimit;
};

struct bch_memquota_type {
	struct quota_limit		limits[Q_COUNTERS];
	bch_memquota_table		table;
	struct mutex			lock;
};

#endif /* _BCACHEFS_QUOTA_TYPES_H */
