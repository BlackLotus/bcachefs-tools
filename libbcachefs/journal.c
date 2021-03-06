/*
 * bcachefs journalling code, for btree insertions
 *
 * Copyright 2012 Google, Inc.
 */

#include "bcachefs.h"
#include "alloc.h"
#include "bkey_methods.h"
#include "btree_gc.h"
#include "buckets.h"
#include "journal.h"
#include "journal_io.h"
#include "journal_reclaim.h"
#include "journal_seq_blacklist.h"
#include "super-io.h"

#include <trace/events/bcachefs.h>

static bool journal_entry_is_open(struct journal *j)
{
	return j->reservations.cur_entry_offset < JOURNAL_ENTRY_CLOSED_VAL;
}

void bch2_journal_buf_put_slowpath(struct journal *j, bool need_write_just_set)
{
	struct journal_buf *w = journal_prev_buf(j);

	atomic_dec_bug(&journal_seq_pin(j, le64_to_cpu(w->data->seq))->count);

	if (!need_write_just_set &&
	    test_bit(JOURNAL_NEED_WRITE, &j->flags))
		bch2_time_stats_update(j->delay_time,
				       j->need_write_time);
#if 0
	closure_call(&j->io, bch2_journal_write, NULL, NULL);
#else
	/* Shut sparse up: */
	closure_init(&j->io, NULL);
	set_closure_fn(&j->io, bch2_journal_write, NULL);
	bch2_journal_write(&j->io);
#endif
}

static void journal_pin_new_entry(struct journal *j, int count)
{
	struct journal_entry_pin_list *p;

	/*
	 * The fifo_push() needs to happen at the same time as j->seq is
	 * incremented for journal_last_seq() to be calculated correctly
	 */
	atomic64_inc(&j->seq);
	p = fifo_push_ref(&j->pin);

	INIT_LIST_HEAD(&p->list);
	INIT_LIST_HEAD(&p->flushed);
	atomic_set(&p->count, count);
	p->devs.nr = 0;
}

static void bch2_journal_buf_init(struct journal *j)
{
	struct journal_buf *buf = journal_cur_buf(j);

	memset(buf->has_inode, 0, sizeof(buf->has_inode));

	memset(buf->data, 0, sizeof(*buf->data));
	buf->data->seq	= cpu_to_le64(journal_cur_seq(j));
	buf->data->u64s	= 0;
}

static inline size_t journal_entry_u64s_reserve(struct journal_buf *buf)
{
	return BTREE_ID_NR * (JSET_KEYS_U64s + BKEY_EXTENT_U64s_MAX);
}

static enum {
	JOURNAL_ENTRY_ERROR,
	JOURNAL_ENTRY_INUSE,
	JOURNAL_ENTRY_CLOSED,
	JOURNAL_UNLOCKED,
} journal_buf_switch(struct journal *j, bool need_write_just_set)
{
	struct bch_fs *c = container_of(j, struct bch_fs, journal);
	struct journal_buf *buf;
	union journal_res_state old, new;
	u64 v = atomic64_read(&j->reservations.counter);

	lockdep_assert_held(&j->lock);

	do {
		old.v = new.v = v;
		if (old.cur_entry_offset == JOURNAL_ENTRY_CLOSED_VAL)
			return JOURNAL_ENTRY_CLOSED;

		if (old.cur_entry_offset == JOURNAL_ENTRY_ERROR_VAL)
			return JOURNAL_ENTRY_ERROR;

		if (new.prev_buf_unwritten)
			return JOURNAL_ENTRY_INUSE;

		/*
		 * avoid race between setting buf->data->u64s and
		 * journal_res_put starting write:
		 */
		journal_state_inc(&new);

		new.cur_entry_offset = JOURNAL_ENTRY_CLOSED_VAL;
		new.idx++;
		new.prev_buf_unwritten = 1;

		BUG_ON(journal_state_count(new, new.idx));
	} while ((v = atomic64_cmpxchg(&j->reservations.counter,
				       old.v, new.v)) != old.v);

	clear_bit(JOURNAL_NEED_WRITE, &j->flags);

	buf = &j->buf[old.idx];
	buf->data->u64s		= cpu_to_le32(old.cur_entry_offset);

	j->prev_buf_sectors =
		vstruct_blocks_plus(buf->data, c->block_bits,
				    journal_entry_u64s_reserve(buf)) *
		c->opts.block_size;
	BUG_ON(j->prev_buf_sectors > j->cur_buf_sectors);

	bch2_journal_reclaim_fast(j);
	/* XXX: why set this here, and not in bch2_journal_write()? */
	buf->data->last_seq	= cpu_to_le64(journal_last_seq(j));

	journal_pin_new_entry(j, 1);

	bch2_journal_buf_init(j);

	cancel_delayed_work(&j->write_work);
	spin_unlock(&j->lock);

	if (c->bucket_journal_seq > 1 << 14) {
		c->bucket_journal_seq = 0;
		bch2_bucket_seq_cleanup(c);
	}

	c->bucket_journal_seq++;

	/* ugh - might be called from __journal_res_get() under wait_event() */
	__set_current_state(TASK_RUNNING);
	bch2_journal_buf_put(j, old.idx, need_write_just_set);

	return JOURNAL_UNLOCKED;
}

void bch2_journal_halt(struct journal *j)
{
	union journal_res_state old, new;
	u64 v = atomic64_read(&j->reservations.counter);

	do {
		old.v = new.v = v;
		if (old.cur_entry_offset == JOURNAL_ENTRY_ERROR_VAL)
			return;

		new.cur_entry_offset = JOURNAL_ENTRY_ERROR_VAL;
	} while ((v = atomic64_cmpxchg(&j->reservations.counter,
				       old.v, new.v)) != old.v);

	journal_wake(j);
	closure_wake_up(&journal_cur_buf(j)->wait);
	closure_wake_up(&journal_prev_buf(j)->wait);
}

/*
 * should _only_ called from journal_res_get() - when we actually want a
 * journal reservation - journal entry is open means journal is dirty:
 *
 * returns:
 * 1:		success
 * 0:		journal currently full (must wait)
 * -EROFS:	insufficient rw devices
 * -EIO:	journal error
 */
static int journal_entry_open(struct journal *j)
{
	struct journal_buf *buf = journal_cur_buf(j);
	union journal_res_state old, new;
	ssize_t u64s;
	int sectors;
	u64 v;

	lockdep_assert_held(&j->lock);
	BUG_ON(journal_entry_is_open(j));

	if (!fifo_free(&j->pin))
		return 0;

	sectors = bch2_journal_entry_sectors(j);
	if (sectors <= 0)
		return sectors;

	buf->disk_sectors	= sectors;

	sectors = min_t(unsigned, sectors, buf->size >> 9);
	j->cur_buf_sectors	= sectors;

	u64s = (sectors << 9) / sizeof(u64);

	/* Subtract the journal header */
	u64s -= sizeof(struct jset) / sizeof(u64);
	/*
	 * Btree roots, prio pointers don't get added until right before we do
	 * the write:
	 */
	u64s -= journal_entry_u64s_reserve(buf);
	u64s  = max_t(ssize_t, 0L, u64s);

	BUG_ON(u64s >= JOURNAL_ENTRY_CLOSED_VAL);

	if (u64s <= le32_to_cpu(buf->data->u64s))
		return 0;

	/*
	 * Must be set before marking the journal entry as open:
	 */
	j->cur_entry_u64s = u64s;

	v = atomic64_read(&j->reservations.counter);
	do {
		old.v = new.v = v;

		if (old.cur_entry_offset == JOURNAL_ENTRY_ERROR_VAL)
			return -EIO;

		/* Handle any already added entries */
		new.cur_entry_offset = le32_to_cpu(buf->data->u64s);
	} while ((v = atomic64_cmpxchg(&j->reservations.counter,
				       old.v, new.v)) != old.v);

	if (j->res_get_blocked_start)
		bch2_time_stats_update(j->blocked_time,
				       j->res_get_blocked_start);
	j->res_get_blocked_start = 0;

	mod_delayed_work(system_freezable_wq,
			 &j->write_work,
			 msecs_to_jiffies(j->write_delay_ms));
	journal_wake(j);
	return 1;
}

/*
 * returns true if there's nothing to flush and no journal write still in flight
 */
static bool journal_flush_write(struct journal *j)
{
	bool ret;

	spin_lock(&j->lock);
	ret = !j->reservations.prev_buf_unwritten;

	if (!journal_entry_is_open(j)) {
		spin_unlock(&j->lock);
		return ret;
	}

	set_bit(JOURNAL_NEED_WRITE, &j->flags);
	if (journal_buf_switch(j, false) == JOURNAL_UNLOCKED)
		ret = false;
	else
		spin_unlock(&j->lock);
	return ret;
}

static void journal_write_work(struct work_struct *work)
{
	struct journal *j = container_of(work, struct journal, write_work.work);

	journal_flush_write(j);
}

/*
 * Given an inode number, if that inode number has data in the journal that
 * hasn't yet been flushed, return the journal sequence number that needs to be
 * flushed:
 */
u64 bch2_inode_journal_seq(struct journal *j, u64 inode)
{
	size_t h = hash_64(inode, ilog2(sizeof(j->buf[0].has_inode) * 8));
	u64 seq = 0;

	if (!test_bit(h, j->buf[0].has_inode) &&
	    !test_bit(h, j->buf[1].has_inode))
		return 0;

	spin_lock(&j->lock);
	if (test_bit(h, journal_cur_buf(j)->has_inode))
		seq = journal_cur_seq(j);
	else if (test_bit(h, journal_prev_buf(j)->has_inode))
		seq = journal_cur_seq(j) - 1;
	spin_unlock(&j->lock);

	return seq;
}

static int __journal_res_get(struct journal *j, struct journal_res *res,
			      unsigned u64s_min, unsigned u64s_max)
{
	struct bch_fs *c = container_of(j, struct bch_fs, journal);
	struct journal_buf *buf;
	int ret;
retry:
	ret = journal_res_get_fast(j, res, u64s_min, u64s_max);
	if (ret)
		return ret;

	spin_lock(&j->lock);
	/*
	 * Recheck after taking the lock, so we don't race with another thread
	 * that just did journal_entry_open() and call journal_entry_close()
	 * unnecessarily
	 */
	ret = journal_res_get_fast(j, res, u64s_min, u64s_max);
	if (ret) {
		spin_unlock(&j->lock);
		return 1;
	}

	/*
	 * If we couldn't get a reservation because the current buf filled up,
	 * and we had room for a bigger entry on disk, signal that we want to
	 * realloc the journal bufs:
	 */
	buf = journal_cur_buf(j);
	if (journal_entry_is_open(j) &&
	    buf->size >> 9 < buf->disk_sectors &&
	    buf->size < JOURNAL_ENTRY_SIZE_MAX)
		j->buf_size_want = max(j->buf_size_want, buf->size << 1);

	/*
	 * Close the current journal entry if necessary, then try to start a new
	 * one:
	 */
	switch (journal_buf_switch(j, false)) {
	case JOURNAL_ENTRY_ERROR:
		spin_unlock(&j->lock);
		return -EROFS;
	case JOURNAL_ENTRY_INUSE:
		/* haven't finished writing out the previous one: */
		spin_unlock(&j->lock);
		trace_journal_entry_full(c);
		goto blocked;
	case JOURNAL_ENTRY_CLOSED:
		break;
	case JOURNAL_UNLOCKED:
		goto retry;
	}

	/* We now have a new, closed journal buf - see if we can open it: */
	ret = journal_entry_open(j);
	spin_unlock(&j->lock);

	if (ret < 0)
		return ret;
	if (ret)
		goto retry;

	/* Journal's full, we have to wait */

	/*
	 * Direct reclaim - can't rely on reclaim from work item
	 * due to freezing..
	 */
	bch2_journal_reclaim_work(&j->reclaim_work.work);

	trace_journal_full(c);
blocked:
	if (!j->res_get_blocked_start)
		j->res_get_blocked_start = local_clock() ?: 1;
	return 0;
}

/*
 * Essentially the entry function to the journaling code. When bcachefs is doing
 * a btree insert, it calls this function to get the current journal write.
 * Journal write is the structure used set up journal writes. The calling
 * function will then add its keys to the structure, queuing them for the next
 * write.
 *
 * To ensure forward progress, the current task must not be holding any
 * btree node write locks.
 */
int bch2_journal_res_get_slowpath(struct journal *j, struct journal_res *res,
				 unsigned u64s_min, unsigned u64s_max)
{
	int ret;

	wait_event(j->wait,
		   (ret = __journal_res_get(j, res, u64s_min,
					    u64s_max)));
	return ret < 0 ? ret : 0;
}

u64 bch2_journal_last_unwritten_seq(struct journal *j)
{
	u64 seq;

	spin_lock(&j->lock);
	seq = journal_cur_seq(j);
	if (j->reservations.prev_buf_unwritten)
		seq--;
	spin_unlock(&j->lock);

	return seq;
}

/**
 * bch2_journal_open_seq_async - try to open a new journal entry if @seq isn't
 * open yet, or wait if we cannot
 *
 * used by the btree interior update machinery, when it needs to write a new
 * btree root - every journal entry contains the roots of all the btrees, so it
 * doesn't need to bother with getting a journal reservation
 */
int bch2_journal_open_seq_async(struct journal *j, u64 seq, struct closure *parent)
{
	int ret;

	spin_lock(&j->lock);
	BUG_ON(seq > journal_cur_seq(j));

	if (seq < journal_cur_seq(j) ||
	    journal_entry_is_open(j)) {
		spin_unlock(&j->lock);
		return 1;
	}

	ret = journal_entry_open(j);
	if (!ret)
		closure_wait(&j->async_wait, parent);
	spin_unlock(&j->lock);

	if (!ret)
		bch2_journal_reclaim_work(&j->reclaim_work.work);

	return ret;
}

/**
 * bch2_journal_wait_on_seq - wait for a journal entry to be written
 *
 * does _not_ cause @seq to be written immediately - if there is no other
 * activity to cause the relevant journal entry to be filled up or flushed it
 * can wait for an arbitrary amount of time (up to @j->write_delay_ms, which is
 * configurable).
 */
void bch2_journal_wait_on_seq(struct journal *j, u64 seq, struct closure *parent)
{
	spin_lock(&j->lock);

	BUG_ON(seq > journal_cur_seq(j));

	if (bch2_journal_error(j)) {
		spin_unlock(&j->lock);
		return;
	}

	if (seq == journal_cur_seq(j)) {
		if (!closure_wait(&journal_cur_buf(j)->wait, parent))
			BUG();
	} else if (seq + 1 == journal_cur_seq(j) &&
		   j->reservations.prev_buf_unwritten) {
		if (!closure_wait(&journal_prev_buf(j)->wait, parent))
			BUG();

		smp_mb();

		/* check if raced with write completion (or failure) */
		if (!j->reservations.prev_buf_unwritten ||
		    bch2_journal_error(j))
			closure_wake_up(&journal_prev_buf(j)->wait);
	}

	spin_unlock(&j->lock);
}

/**
 * bch2_journal_flush_seq_async - wait for a journal entry to be written
 *
 * like bch2_journal_wait_on_seq, except that it triggers a write immediately if
 * necessary
 */
void bch2_journal_flush_seq_async(struct journal *j, u64 seq, struct closure *parent)
{
	struct journal_buf *buf;

	spin_lock(&j->lock);

	BUG_ON(seq > journal_cur_seq(j));

	if (bch2_journal_error(j)) {
		spin_unlock(&j->lock);
		return;
	}

	if (seq == journal_cur_seq(j)) {
		bool set_need_write = false;

		buf = journal_cur_buf(j);

		if (parent && !closure_wait(&buf->wait, parent))
			BUG();

		if (!test_and_set_bit(JOURNAL_NEED_WRITE, &j->flags)) {
			j->need_write_time = local_clock();
			set_need_write = true;
		}

		switch (journal_buf_switch(j, set_need_write)) {
		case JOURNAL_ENTRY_ERROR:
			if (parent)
				closure_wake_up(&buf->wait);
			break;
		case JOURNAL_ENTRY_CLOSED:
			/*
			 * Journal entry hasn't been opened yet, but caller
			 * claims it has something
			 */
			BUG();
		case JOURNAL_ENTRY_INUSE:
			break;
		case JOURNAL_UNLOCKED:
			return;
		}
	} else if (parent &&
		   seq + 1 == journal_cur_seq(j) &&
		   j->reservations.prev_buf_unwritten) {
		buf = journal_prev_buf(j);

		if (!closure_wait(&buf->wait, parent))
			BUG();

		smp_mb();

		/* check if raced with write completion (or failure) */
		if (!j->reservations.prev_buf_unwritten ||
		    bch2_journal_error(j))
			closure_wake_up(&buf->wait);
	}

	spin_unlock(&j->lock);
}

static int journal_seq_flushed(struct journal *j, u64 seq)
{
	struct journal_buf *buf;
	int ret = 1;

	spin_lock(&j->lock);
	BUG_ON(seq > journal_cur_seq(j));

	if (seq == journal_cur_seq(j)) {
		bool set_need_write = false;

		ret = 0;

		buf = journal_cur_buf(j);

		if (!test_and_set_bit(JOURNAL_NEED_WRITE, &j->flags)) {
			j->need_write_time = local_clock();
			set_need_write = true;
		}

		switch (journal_buf_switch(j, set_need_write)) {
		case JOURNAL_ENTRY_ERROR:
			ret = -EIO;
			break;
		case JOURNAL_ENTRY_CLOSED:
			/*
			 * Journal entry hasn't been opened yet, but caller
			 * claims it has something
			 */
			BUG();
		case JOURNAL_ENTRY_INUSE:
			break;
		case JOURNAL_UNLOCKED:
			return 0;
		}
	} else if (seq + 1 == journal_cur_seq(j) &&
		   j->reservations.prev_buf_unwritten) {
		ret = bch2_journal_error(j);
	}

	spin_unlock(&j->lock);

	return ret;
}

int bch2_journal_flush_seq(struct journal *j, u64 seq)
{
	u64 start_time = local_clock();
	int ret, ret2;

	ret = wait_event_killable(j->wait, (ret2 = journal_seq_flushed(j, seq)));

	bch2_time_stats_update(j->flush_seq_time, start_time);

	return ret ?: ret2 < 0 ? ret2 : 0;
}

/**
 * bch2_journal_meta_async - force a journal entry to be written
 */
void bch2_journal_meta_async(struct journal *j, struct closure *parent)
{
	struct journal_res res;
	unsigned u64s = jset_u64s(0);

	memset(&res, 0, sizeof(res));

	bch2_journal_res_get(j, &res, u64s, u64s);
	bch2_journal_res_put(j, &res);

	bch2_journal_flush_seq_async(j, res.seq, parent);
}

int bch2_journal_meta(struct journal *j)
{
	struct journal_res res;
	unsigned u64s = jset_u64s(0);
	int ret;

	memset(&res, 0, sizeof(res));

	ret = bch2_journal_res_get(j, &res, u64s, u64s);
	if (ret)
		return ret;

	bch2_journal_res_put(j, &res);

	return bch2_journal_flush_seq(j, res.seq);
}

/*
 * bch2_journal_flush_async - if there is an open journal entry, or a journal
 * still being written, write it and wait for the write to complete
 */
void bch2_journal_flush_async(struct journal *j, struct closure *parent)
{
	u64 seq, journal_seq;

	spin_lock(&j->lock);
	journal_seq = journal_cur_seq(j);

	if (journal_entry_is_open(j)) {
		seq = journal_seq;
	} else if (journal_seq) {
		seq = journal_seq - 1;
	} else {
		spin_unlock(&j->lock);
		return;
	}
	spin_unlock(&j->lock);

	bch2_journal_flush_seq_async(j, seq, parent);
}

int bch2_journal_flush(struct journal *j)
{
	u64 seq, journal_seq;

	spin_lock(&j->lock);
	journal_seq = journal_cur_seq(j);

	if (journal_entry_is_open(j)) {
		seq = journal_seq;
	} else if (journal_seq) {
		seq = journal_seq - 1;
	} else {
		spin_unlock(&j->lock);
		return 0;
	}
	spin_unlock(&j->lock);

	return bch2_journal_flush_seq(j, seq);
}

/* allocate journal on a device: */

static int __bch2_set_nr_journal_buckets(struct bch_dev *ca, unsigned nr,
					 bool new_fs, struct closure *cl)
{
	struct bch_fs *c = ca->fs;
	struct journal_device *ja = &ca->journal;
	struct bch_sb_field_journal *journal_buckets;
	u64 *new_bucket_seq = NULL, *new_buckets = NULL;
	int ret = 0;

	/* don't handle reducing nr of buckets yet: */
	if (nr <= ja->nr)
		return 0;

	ret = -ENOMEM;
	new_buckets	= kzalloc(nr * sizeof(u64), GFP_KERNEL);
	new_bucket_seq	= kzalloc(nr * sizeof(u64), GFP_KERNEL);
	if (!new_buckets || !new_bucket_seq)
		goto err;

	journal_buckets = bch2_sb_resize_journal(&ca->disk_sb,
				nr + sizeof(*journal_buckets) / sizeof(u64));
	if (!journal_buckets)
		goto err;

	if (c)
		spin_lock(&c->journal.lock);

	memcpy(new_buckets,	ja->buckets,	ja->nr * sizeof(u64));
	memcpy(new_bucket_seq,	ja->bucket_seq,	ja->nr * sizeof(u64));
	swap(new_buckets,	ja->buckets);
	swap(new_bucket_seq,	ja->bucket_seq);

	if (c)
		spin_unlock(&c->journal.lock);

	while (ja->nr < nr) {
		struct open_bucket *ob = NULL;
		long bucket;

		if (new_fs) {
			bucket = bch2_bucket_alloc_new_fs(ca);
			if (bucket < 0) {
				ret = -ENOSPC;
				goto err;
			}
		} else {
			int ob_idx = bch2_bucket_alloc(c, ca, RESERVE_ALLOC, false, cl);
			if (ob_idx < 0) {
				ret = cl ? -EAGAIN : -ENOSPC;
				goto err;
			}

			ob = c->open_buckets + ob_idx;
			bucket = sector_to_bucket(ca, ob->ptr.offset);
		}

		if (c)
			spin_lock(&c->journal.lock);

		__array_insert_item(ja->buckets,		ja->nr, ja->last_idx);
		__array_insert_item(ja->bucket_seq,		ja->nr, ja->last_idx);
		__array_insert_item(journal_buckets->buckets,	ja->nr, ja->last_idx);

		ja->buckets[ja->last_idx] = bucket;
		ja->bucket_seq[ja->last_idx] = 0;
		journal_buckets->buckets[ja->last_idx] = cpu_to_le64(bucket);

		if (ja->last_idx < ja->nr) {
			if (ja->cur_idx >= ja->last_idx)
				ja->cur_idx++;
			ja->last_idx++;
		}
		ja->nr++;

		if (c)
			spin_unlock(&c->journal.lock);

		bch2_mark_metadata_bucket(c, ca, bucket, BCH_DATA_JOURNAL,
				ca->mi.bucket_size,
				gc_phase(GC_PHASE_SB),
				new_fs
				? BCH_BUCKET_MARK_MAY_MAKE_UNAVAILABLE
				: 0);

		if (!new_fs)
			bch2_open_bucket_put(c, ob);
	}

	ret = 0;
err:
	kfree(new_bucket_seq);
	kfree(new_buckets);

	return ret;
}

/*
 * Allocate more journal space at runtime - not currently making use if it, but
 * the code works:
 */
int bch2_set_nr_journal_buckets(struct bch_fs *c, struct bch_dev *ca,
				unsigned nr)
{
	struct journal_device *ja = &ca->journal;
	struct closure cl;
	unsigned current_nr;
	int ret;

	closure_init_stack(&cl);

	do {
		struct disk_reservation disk_res = { 0, 0 };

		closure_sync(&cl);

		mutex_lock(&c->sb_lock);
		current_nr = ja->nr;

		/*
		 * note: journal buckets aren't really counted as _sectors_ used yet, so
		 * we don't need the disk reservation to avoid the BUG_ON() in buckets.c
		 * when space used goes up without a reservation - but we do need the
		 * reservation to ensure we'll actually be able to allocate:
		 */

		if (bch2_disk_reservation_get(c, &disk_res,
				bucket_to_sector(ca, nr - ja->nr), 1, 0)) {
			mutex_unlock(&c->sb_lock);
			return -ENOSPC;
		}

		ret = __bch2_set_nr_journal_buckets(ca, nr, false, &cl);

		bch2_disk_reservation_put(c, &disk_res);

		if (ja->nr != current_nr)
			bch2_write_super(c);
		mutex_unlock(&c->sb_lock);
	} while (ret == -EAGAIN);

	return ret;
}

int bch2_dev_journal_alloc(struct bch_dev *ca)
{
	unsigned nr;

	if (dynamic_fault("bcachefs:add:journal_alloc"))
		return -ENOMEM;

	/*
	 * clamp journal size to 1024 buckets or 512MB (in sectors), whichever
	 * is smaller:
	 */
	nr = clamp_t(unsigned, ca->mi.nbuckets >> 8,
		     BCH_JOURNAL_BUCKETS_MIN,
		     min(1 << 10,
			 (1 << 20) / ca->mi.bucket_size));

	return __bch2_set_nr_journal_buckets(ca, nr, true, NULL);
}

/* startup/shutdown: */

static bool bch2_journal_writing_to_device(struct journal *j, unsigned dev_idx)
{
	union journal_res_state state;
	struct journal_buf *w;
	bool ret;

	spin_lock(&j->lock);
	state = READ_ONCE(j->reservations);
	w = j->buf + !state.idx;

	ret = state.prev_buf_unwritten &&
		bch2_extent_has_device(bkey_i_to_s_c_extent(&w->key), dev_idx);
	spin_unlock(&j->lock);

	return ret;
}

void bch2_dev_journal_stop(struct journal *j, struct bch_dev *ca)
{
	spin_lock(&j->lock);
	bch2_extent_drop_device(bkey_i_to_s_extent(&j->key), ca->dev_idx);
	spin_unlock(&j->lock);

	wait_event(j->wait, !bch2_journal_writing_to_device(j, ca->dev_idx));
}

void bch2_fs_journal_stop(struct journal *j)
{
	wait_event(j->wait, journal_flush_write(j));

	cancel_delayed_work_sync(&j->write_work);
	cancel_delayed_work_sync(&j->reclaim_work);
}

void bch2_fs_journal_start(struct journal *j)
{
	struct journal_seq_blacklist *bl;
	u64 blacklist = 0;

	list_for_each_entry(bl, &j->seq_blacklist, list)
		blacklist = max(blacklist, bl->end);

	spin_lock(&j->lock);

	set_bit(JOURNAL_STARTED, &j->flags);

	while (journal_cur_seq(j) < blacklist)
		journal_pin_new_entry(j, 0);

	/*
	 * journal_buf_switch() only inits the next journal entry when it
	 * closes an open journal entry - the very first journal entry gets
	 * initialized here:
	 */
	journal_pin_new_entry(j, 1);
	bch2_journal_buf_init(j);

	spin_unlock(&j->lock);

	/*
	 * Adding entries to the next journal entry before allocating space on
	 * disk for the next journal entry - this is ok, because these entries
	 * only have to go down with the next journal entry we write:
	 */
	bch2_journal_seq_blacklist_write(j);

	queue_delayed_work(system_freezable_wq, &j->reclaim_work, 0);
}

/* init/exit: */

void bch2_dev_journal_exit(struct bch_dev *ca)
{
	kfree(ca->journal.bio);
	kfree(ca->journal.buckets);
	kfree(ca->journal.bucket_seq);

	ca->journal.bio		= NULL;
	ca->journal.buckets	= NULL;
	ca->journal.bucket_seq	= NULL;
}

int bch2_dev_journal_init(struct bch_dev *ca, struct bch_sb *sb)
{
	struct journal_device *ja = &ca->journal;
	struct bch_sb_field_journal *journal_buckets =
		bch2_sb_get_journal(sb);
	unsigned i;

	ja->nr = bch2_nr_journal_buckets(journal_buckets);

	ja->bucket_seq = kcalloc(ja->nr, sizeof(u64), GFP_KERNEL);
	if (!ja->bucket_seq)
		return -ENOMEM;

	ca->journal.bio = bio_kmalloc(GFP_KERNEL,
			DIV_ROUND_UP(JOURNAL_ENTRY_SIZE_MAX, PAGE_SIZE));
	if (!ca->journal.bio)
		return -ENOMEM;

	ja->buckets = kcalloc(ja->nr, sizeof(u64), GFP_KERNEL);
	if (!ja->buckets)
		return -ENOMEM;

	for (i = 0; i < ja->nr; i++)
		ja->buckets[i] = le64_to_cpu(journal_buckets->buckets[i]);

	return 0;
}

void bch2_fs_journal_exit(struct journal *j)
{
	kvpfree(j->buf[1].data, j->buf[1].size);
	kvpfree(j->buf[0].data, j->buf[0].size);
	free_fifo(&j->pin);
}

int bch2_fs_journal_init(struct journal *j)
{
	struct bch_fs *c = container_of(j, struct bch_fs, journal);
	static struct lock_class_key res_key;
	int ret = 0;

	pr_verbose_init(c->opts, "");

	spin_lock_init(&j->lock);
	spin_lock_init(&j->err_lock);
	init_waitqueue_head(&j->wait);
	INIT_DELAYED_WORK(&j->write_work, journal_write_work);
	INIT_DELAYED_WORK(&j->reclaim_work, bch2_journal_reclaim_work);
	mutex_init(&j->blacklist_lock);
	INIT_LIST_HEAD(&j->seq_blacklist);
	mutex_init(&j->reclaim_lock);

	lockdep_init_map(&j->res_map, "journal res", &res_key, 0);

	j->buf[0].size		= JOURNAL_ENTRY_SIZE_MIN;
	j->buf[1].size		= JOURNAL_ENTRY_SIZE_MIN;
	j->write_delay_ms	= 1000;
	j->reclaim_delay_ms	= 100;

	bkey_extent_init(&j->key);

	atomic64_set(&j->reservations.counter,
		((union journal_res_state)
		 { .cur_entry_offset = JOURNAL_ENTRY_CLOSED_VAL }).v);

	if (!(init_fifo(&j->pin, JOURNAL_PIN, GFP_KERNEL)) ||
	    !(j->buf[0].data = kvpmalloc(j->buf[0].size, GFP_KERNEL)) ||
	    !(j->buf[1].data = kvpmalloc(j->buf[1].size, GFP_KERNEL))) {
		ret = -ENOMEM;
		goto out;
	}

	j->pin.front = j->pin.back = 1;
out:
	pr_verbose_init(c->opts, "ret %i", ret);
	return ret;
}

/* debug: */

ssize_t bch2_journal_print_debug(struct journal *j, char *buf)
{
	struct bch_fs *c = container_of(j, struct bch_fs, journal);
	union journal_res_state *s = &j->reservations;
	struct bch_dev *ca;
	unsigned iter;
	ssize_t ret = 0;

	rcu_read_lock();
	spin_lock(&j->lock);

	ret += scnprintf(buf + ret, PAGE_SIZE - ret,
			 "active journal entries:\t%llu\n"
			 "seq:\t\t\t%llu\n"
			 "last_seq:\t\t%llu\n"
			 "last_seq_ondisk:\t%llu\n"
			 "reservation count:\t%u\n"
			 "reservation offset:\t%u\n"
			 "current entry u64s:\t%u\n"
			 "io in flight:\t\t%i\n"
			 "need write:\t\t%i\n"
			 "dirty:\t\t\t%i\n"
			 "replay done:\t\t%i\n",
			 fifo_used(&j->pin),
			 journal_cur_seq(j),
			 journal_last_seq(j),
			 j->last_seq_ondisk,
			 journal_state_count(*s, s->idx),
			 s->cur_entry_offset,
			 j->cur_entry_u64s,
			 s->prev_buf_unwritten,
			 test_bit(JOURNAL_NEED_WRITE,	&j->flags),
			 journal_entry_is_open(j),
			 test_bit(JOURNAL_REPLAY_DONE,	&j->flags));

	for_each_member_device_rcu(ca, c, iter,
				   &c->rw_devs[BCH_DATA_JOURNAL]) {
		struct journal_device *ja = &ca->journal;

		if (!ja->nr)
			continue;

		ret += scnprintf(buf + ret, PAGE_SIZE - ret,
				 "dev %u:\n"
				 "\tnr\t\t%u\n"
				 "\tcur_idx\t\t%u (seq %llu)\n"
				 "\tlast_idx\t%u (seq %llu)\n",
				 iter, ja->nr,
				 ja->cur_idx,	ja->bucket_seq[ja->cur_idx],
				 ja->last_idx,	ja->bucket_seq[ja->last_idx]);
	}

	spin_unlock(&j->lock);
	rcu_read_unlock();

	return ret;
}

ssize_t bch2_journal_print_pins(struct journal *j, char *buf)
{
	struct journal_entry_pin_list *pin_list;
	struct journal_entry_pin *pin;
	ssize_t ret = 0;
	u64 i;

	spin_lock(&j->lock);
	fifo_for_each_entry_ptr(pin_list, &j->pin, i) {
		ret += scnprintf(buf + ret, PAGE_SIZE - ret,
				 "%llu: count %u\n",
				 i, atomic_read(&pin_list->count));

		list_for_each_entry(pin, &pin_list->list, list)
			ret += scnprintf(buf + ret, PAGE_SIZE - ret,
					 "\t%p %pf\n",
					 pin, pin->flush);

		if (!list_empty(&pin_list->flushed))
			ret += scnprintf(buf + ret, PAGE_SIZE - ret,
					 "flushed:\n");

		list_for_each_entry(pin, &pin_list->flushed, list)
			ret += scnprintf(buf + ret, PAGE_SIZE - ret,
					 "\t%p %pf\n",
					 pin, pin->flush);
	}
	spin_unlock(&j->lock);

	return ret;
}
