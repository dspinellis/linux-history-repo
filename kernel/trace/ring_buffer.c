/*
 * Generic ring buffer
 *
 * Copyright (C) 2008 Steven Rostedt <srostedt@redhat.com>
 */
#include <linux/ring_buffer.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/mutex.h>
#include <linux/sched.h>	/* used for sched_clock() (for now) */
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/fs.h>

/* Up this if you want to test the TIME_EXTENTS and normalization */
#define DEBUG_SHIFT 0

/* FIXME!!! */
u64 ring_buffer_time_stamp(int cpu)
{
	/* shift to debug/test normalization and TIME_EXTENTS */
	return sched_clock() << DEBUG_SHIFT;
}

void ring_buffer_normalize_time_stamp(int cpu, u64 *ts)
{
	/* Just stupid testing the normalize function and deltas */
	*ts >>= DEBUG_SHIFT;
}

#define RB_EVNT_HDR_SIZE (sizeof(struct ring_buffer_event))
#define RB_ALIGNMENT_SHIFT	2
#define RB_ALIGNMENT		(1 << RB_ALIGNMENT_SHIFT)
#define RB_MAX_SMALL_DATA	28

enum {
	RB_LEN_TIME_EXTEND = 8,
	RB_LEN_TIME_STAMP = 16,
};

/* inline for ring buffer fast paths */
static inline unsigned
rb_event_length(struct ring_buffer_event *event)
{
	unsigned length;

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		/* undefined */
		return -1;

	case RINGBUF_TYPE_TIME_EXTEND:
		return RB_LEN_TIME_EXTEND;

	case RINGBUF_TYPE_TIME_STAMP:
		return RB_LEN_TIME_STAMP;

	case RINGBUF_TYPE_DATA:
		if (event->len)
			length = event->len << RB_ALIGNMENT_SHIFT;
		else
			length = event->array[0];
		return length + RB_EVNT_HDR_SIZE;
	default:
		BUG();
	}
	/* not hit */
	return 0;
}

/**
 * ring_buffer_event_length - return the length of the event
 * @event: the event to get the length of
 */
unsigned ring_buffer_event_length(struct ring_buffer_event *event)
{
	return rb_event_length(event);
}

/* inline for ring buffer fast paths */
static inline void *
rb_event_data(struct ring_buffer_event *event)
{
	BUG_ON(event->type != RINGBUF_TYPE_DATA);
	/* If length is in len field, then array[0] has the data */
	if (event->len)
		return (void *)&event->array[0];
	/* Otherwise length is in array[0] and array[1] has the data */
	return (void *)&event->array[1];
}

/**
 * ring_buffer_event_data - return the data of the event
 * @event: the event to get the data from
 */
void *ring_buffer_event_data(struct ring_buffer_event *event)
{
	return rb_event_data(event);
}

#define for_each_buffer_cpu(buffer, cpu)		\
	for_each_cpu_mask(cpu, buffer->cpumask)

#define TS_SHIFT	27
#define TS_MASK		((1ULL << TS_SHIFT) - 1)
#define TS_DELTA_TEST	(~TS_MASK)

/*
 * This hack stolen from mm/slob.c.
 * We can store per page timing information in the page frame of the page.
 * Thanks to Peter Zijlstra for suggesting this idea.
 */
struct buffer_page {
	union {
		struct {
			unsigned long	 flags;		/* mandatory */
			atomic_t	 _count;	/* mandatory */
			u64		 time_stamp;	/* page time stamp */
			unsigned	 size;		/* size of page data */
			struct list_head list;		/* list of free pages */
		};
		struct page page;
	};
};

/*
 * Also stolen from mm/slob.c. Thanks to Mathieu Desnoyers for pointing
 * this issue out.
 */
static inline void free_buffer_page(struct buffer_page *bpage)
{
	reset_page_mapcount(&bpage->page);
	bpage->page.mapping = NULL;
	__free_page(&bpage->page);
}

/*
 * We need to fit the time_stamp delta into 27 bits.
 */
static inline int test_time_stamp(u64 delta)
{
	if (delta & TS_DELTA_TEST)
		return 1;
	return 0;
}

#define BUF_PAGE_SIZE PAGE_SIZE

/*
 * head_page == tail_page && head == tail then buffer is empty.
 */
struct ring_buffer_per_cpu {
	int				cpu;
	struct ring_buffer		*buffer;
	spinlock_t			lock;
	struct lock_class_key		lock_key;
	struct list_head		pages;
	unsigned long			head;	/* read from head */
	unsigned long			tail;	/* write to tail */
	unsigned long			reader;
	struct buffer_page		*head_page;
	struct buffer_page		*tail_page;
	struct buffer_page		*reader_page;
	unsigned long			overrun;
	unsigned long			entries;
	u64				write_stamp;
	u64				read_stamp;
	atomic_t			record_disabled;
};

struct ring_buffer {
	unsigned long			size;
	unsigned			pages;
	unsigned			flags;
	int				cpus;
	cpumask_t			cpumask;
	atomic_t			record_disabled;

	struct mutex			mutex;

	struct ring_buffer_per_cpu	**buffers;
};

struct ring_buffer_iter {
	struct ring_buffer_per_cpu	*cpu_buffer;
	unsigned long			head;
	struct buffer_page		*head_page;
	u64				read_stamp;
};

#define RB_WARN_ON(buffer, cond)			\
	if (unlikely(cond)) {				\
		atomic_inc(&buffer->record_disabled);	\
		WARN_ON(1);				\
		return -1;				\
	}

/**
 * check_pages - integrity check of buffer pages
 * @cpu_buffer: CPU buffer with pages to test
 *
 * As a safty measure we check to make sure the data pages have not
 * been corrupted.
 */
static int rb_check_pages(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct list_head *head = &cpu_buffer->pages;
	struct buffer_page *page, *tmp;

	RB_WARN_ON(cpu_buffer, head->next->prev != head);
	RB_WARN_ON(cpu_buffer, head->prev->next != head);

	list_for_each_entry_safe(page, tmp, head, list) {
		RB_WARN_ON(cpu_buffer, page->list.next->prev != &page->list);
		RB_WARN_ON(cpu_buffer, page->list.prev->next != &page->list);
	}

	return 0;
}

static unsigned rb_head_size(struct ring_buffer_per_cpu *cpu_buffer)
{
	return cpu_buffer->head_page->size;
}

static int rb_allocate_pages(struct ring_buffer_per_cpu *cpu_buffer,
			     unsigned nr_pages)
{
	struct list_head *head = &cpu_buffer->pages;
	struct buffer_page *page, *tmp;
	unsigned long addr;
	LIST_HEAD(pages);
	unsigned i;

	for (i = 0; i < nr_pages; i++) {
		addr = __get_free_page(GFP_KERNEL);
		if (!addr)
			goto free_pages;
		page = (struct buffer_page *)virt_to_page(addr);
		list_add(&page->list, &pages);
	}

	list_splice(&pages, head);

	rb_check_pages(cpu_buffer);

	return 0;

 free_pages:
	list_for_each_entry_safe(page, tmp, &pages, list) {
		list_del_init(&page->list);
		free_buffer_page(page);
	}
	return -ENOMEM;
}

static struct ring_buffer_per_cpu *
rb_allocate_cpu_buffer(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long addr;
	int ret;

	cpu_buffer = kzalloc_node(ALIGN(sizeof(*cpu_buffer), cache_line_size()),
				  GFP_KERNEL, cpu_to_node(cpu));
	if (!cpu_buffer)
		return NULL;

	cpu_buffer->cpu = cpu;
	cpu_buffer->buffer = buffer;
	spin_lock_init(&cpu_buffer->lock);
	INIT_LIST_HEAD(&cpu_buffer->pages);

	addr = __get_free_page(GFP_KERNEL);
	if (!addr)
		goto fail_free_buffer;
	cpu_buffer->reader_page = (struct buffer_page *)virt_to_page(addr);
	INIT_LIST_HEAD(&cpu_buffer->reader_page->list);
	cpu_buffer->reader_page->size = 0;

	ret = rb_allocate_pages(cpu_buffer, buffer->pages);
	if (ret < 0)
		goto fail_free_reader;

	cpu_buffer->head_page
		= list_entry(cpu_buffer->pages.next, struct buffer_page, list);
	cpu_buffer->tail_page
		= list_entry(cpu_buffer->pages.next, struct buffer_page, list);

	return cpu_buffer;

 fail_free_reader:
	free_buffer_page(cpu_buffer->reader_page);

 fail_free_buffer:
	kfree(cpu_buffer);
	return NULL;
}

static void rb_free_cpu_buffer(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct list_head *head = &cpu_buffer->pages;
	struct buffer_page *page, *tmp;

	list_del_init(&cpu_buffer->reader_page->list);
	free_buffer_page(cpu_buffer->reader_page);

	list_for_each_entry_safe(page, tmp, head, list) {
		list_del_init(&page->list);
		free_buffer_page(page);
	}
	kfree(cpu_buffer);
}

/*
 * Causes compile errors if the struct buffer_page gets bigger
 * than the struct page.
 */
extern int ring_buffer_page_too_big(void);

/**
 * ring_buffer_alloc - allocate a new ring_buffer
 * @size: the size in bytes that is needed.
 * @flags: attributes to set for the ring buffer.
 *
 * Currently the only flag that is available is the RB_FL_OVERWRITE
 * flag. This flag means that the buffer will overwrite old data
 * when the buffer wraps. If this flag is not set, the buffer will
 * drop data when the tail hits the head.
 */
struct ring_buffer *ring_buffer_alloc(unsigned long size, unsigned flags)
{
	struct ring_buffer *buffer;
	int bsize;
	int cpu;

	/* Paranoid! Optimizes out when all is well */
	if (sizeof(struct buffer_page) > sizeof(struct page))
		ring_buffer_page_too_big();


	/* keep it in its own cache line */
	buffer = kzalloc(ALIGN(sizeof(*buffer), cache_line_size()),
			 GFP_KERNEL);
	if (!buffer)
		return NULL;

	buffer->pages = DIV_ROUND_UP(size, BUF_PAGE_SIZE);
	buffer->flags = flags;

	/* need at least two pages */
	if (buffer->pages == 1)
		buffer->pages++;

	buffer->cpumask = cpu_possible_map;
	buffer->cpus = nr_cpu_ids;

	bsize = sizeof(void *) * nr_cpu_ids;
	buffer->buffers = kzalloc(ALIGN(bsize, cache_line_size()),
				  GFP_KERNEL);
	if (!buffer->buffers)
		goto fail_free_buffer;

	for_each_buffer_cpu(buffer, cpu) {
		buffer->buffers[cpu] =
			rb_allocate_cpu_buffer(buffer, cpu);
		if (!buffer->buffers[cpu])
			goto fail_free_buffers;
	}

	mutex_init(&buffer->mutex);

	return buffer;

 fail_free_buffers:
	for_each_buffer_cpu(buffer, cpu) {
		if (buffer->buffers[cpu])
			rb_free_cpu_buffer(buffer->buffers[cpu]);
	}
	kfree(buffer->buffers);

 fail_free_buffer:
	kfree(buffer);
	return NULL;
}

/**
 * ring_buffer_free - free a ring buffer.
 * @buffer: the buffer to free.
 */
void
ring_buffer_free(struct ring_buffer *buffer)
{
	int cpu;

	for_each_buffer_cpu(buffer, cpu)
		rb_free_cpu_buffer(buffer->buffers[cpu]);

	kfree(buffer);
}

static void rb_reset_cpu(struct ring_buffer_per_cpu *cpu_buffer);

static void
rb_remove_pages(struct ring_buffer_per_cpu *cpu_buffer, unsigned nr_pages)
{
	struct buffer_page *page;
	struct list_head *p;
	unsigned i;

	atomic_inc(&cpu_buffer->record_disabled);
	synchronize_sched();

	for (i = 0; i < nr_pages; i++) {
		BUG_ON(list_empty(&cpu_buffer->pages));
		p = cpu_buffer->pages.next;
		page = list_entry(p, struct buffer_page, list);
		list_del_init(&page->list);
		free_buffer_page(page);
	}
	BUG_ON(list_empty(&cpu_buffer->pages));

	rb_reset_cpu(cpu_buffer);

	rb_check_pages(cpu_buffer);

	atomic_dec(&cpu_buffer->record_disabled);

}

static void
rb_insert_pages(struct ring_buffer_per_cpu *cpu_buffer,
		struct list_head *pages, unsigned nr_pages)
{
	struct buffer_page *page;
	struct list_head *p;
	unsigned i;

	atomic_inc(&cpu_buffer->record_disabled);
	synchronize_sched();

	for (i = 0; i < nr_pages; i++) {
		BUG_ON(list_empty(pages));
		p = pages->next;
		page = list_entry(p, struct buffer_page, list);
		list_del_init(&page->list);
		list_add_tail(&page->list, &cpu_buffer->pages);
	}
	rb_reset_cpu(cpu_buffer);

	rb_check_pages(cpu_buffer);

	atomic_dec(&cpu_buffer->record_disabled);
}

/**
 * ring_buffer_resize - resize the ring buffer
 * @buffer: the buffer to resize.
 * @size: the new size.
 *
 * The tracer is responsible for making sure that the buffer is
 * not being used while changing the size.
 * Note: We may be able to change the above requirement by using
 *  RCU synchronizations.
 *
 * Minimum size is 2 * BUF_PAGE_SIZE.
 *
 * Returns -1 on failure.
 */
int ring_buffer_resize(struct ring_buffer *buffer, unsigned long size)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned nr_pages, rm_pages, new_pages;
	struct buffer_page *page, *tmp;
	unsigned long buffer_size;
	unsigned long addr;
	LIST_HEAD(pages);
	int i, cpu;

	size = DIV_ROUND_UP(size, BUF_PAGE_SIZE);
	size *= BUF_PAGE_SIZE;
	buffer_size = buffer->pages * BUF_PAGE_SIZE;

	/* we need a minimum of two pages */
	if (size < BUF_PAGE_SIZE * 2)
		size = BUF_PAGE_SIZE * 2;

	if (size == buffer_size)
		return size;

	mutex_lock(&buffer->mutex);

	nr_pages = DIV_ROUND_UP(size, BUF_PAGE_SIZE);

	if (size < buffer_size) {

		/* easy case, just free pages */
		BUG_ON(nr_pages >= buffer->pages);

		rm_pages = buffer->pages - nr_pages;

		for_each_buffer_cpu(buffer, cpu) {
			cpu_buffer = buffer->buffers[cpu];
			rb_remove_pages(cpu_buffer, rm_pages);
		}
		goto out;
	}

	/*
	 * This is a bit more difficult. We only want to add pages
	 * when we can allocate enough for all CPUs. We do this
	 * by allocating all the pages and storing them on a local
	 * link list. If we succeed in our allocation, then we
	 * add these pages to the cpu_buffers. Otherwise we just free
	 * them all and return -ENOMEM;
	 */
	BUG_ON(nr_pages <= buffer->pages);
	new_pages = nr_pages - buffer->pages;

	for_each_buffer_cpu(buffer, cpu) {
		for (i = 0; i < new_pages; i++) {
			addr = __get_free_page(GFP_KERNEL);
			if (!addr)
				goto free_pages;
			page = (struct buffer_page *)virt_to_page(addr);
			list_add(&page->list, &pages);
		}
	}

	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		rb_insert_pages(cpu_buffer, &pages, new_pages);
	}

	BUG_ON(!list_empty(&pages));

 out:
	buffer->pages = nr_pages;
	mutex_unlock(&buffer->mutex);

	return size;

 free_pages:
	list_for_each_entry_safe(page, tmp, &pages, list) {
		list_del_init(&page->list);
		free_buffer_page(page);
	}
	return -ENOMEM;
}

static inline int rb_per_cpu_empty(struct ring_buffer_per_cpu *cpu_buffer)
{
	return (cpu_buffer->reader == cpu_buffer->reader_page->size &&
		(cpu_buffer->tail_page == cpu_buffer->reader_page ||
		 (cpu_buffer->tail_page == cpu_buffer->head_page &&
		  cpu_buffer->head == cpu_buffer->tail)));
}

static inline int rb_null_event(struct ring_buffer_event *event)
{
	return event->type == RINGBUF_TYPE_PADDING;
}

static inline void *rb_page_index(struct buffer_page *page, unsigned index)
{
	void *addr = page_address(&page->page);

	return addr + index;
}

static inline struct ring_buffer_event *
rb_reader_event(struct ring_buffer_per_cpu *cpu_buffer)
{
	return rb_page_index(cpu_buffer->reader_page,
			     cpu_buffer->reader);
}

static inline struct ring_buffer_event *
rb_iter_head_event(struct ring_buffer_iter *iter)
{
	return rb_page_index(iter->head_page,
			     iter->head);
}

/*
 * When the tail hits the head and the buffer is in overwrite mode,
 * the head jumps to the next page and all content on the previous
 * page is discarded. But before doing so, we update the overrun
 * variable of the buffer.
 */
static void rb_update_overflow(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct ring_buffer_event *event;
	unsigned long head;

	for (head = 0; head < rb_head_size(cpu_buffer);
	     head += rb_event_length(event)) {

		event = rb_page_index(cpu_buffer->head_page, head);
		BUG_ON(rb_null_event(event));
		/* Only count data entries */
		if (event->type != RINGBUF_TYPE_DATA)
			continue;
		cpu_buffer->overrun++;
		cpu_buffer->entries--;
	}
}

static inline void rb_inc_page(struct ring_buffer_per_cpu *cpu_buffer,
			       struct buffer_page **page)
{
	struct list_head *p = (*page)->list.next;

	if (p == &cpu_buffer->pages)
		p = p->next;

	*page = list_entry(p, struct buffer_page, list);
}

static inline void
rb_add_stamp(struct ring_buffer_per_cpu *cpu_buffer, u64 *ts)
{
	cpu_buffer->tail_page->time_stamp = *ts;
	cpu_buffer->write_stamp = *ts;
}

static void rb_reset_head_page(struct ring_buffer_per_cpu *cpu_buffer)
{
	cpu_buffer->head = 0;
}

static void rb_reset_reader_page(struct ring_buffer_per_cpu *cpu_buffer)
{
	cpu_buffer->read_stamp = cpu_buffer->reader_page->time_stamp;
	cpu_buffer->reader = 0;
}

static inline void rb_inc_iter(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;

	/*
	 * The iterator could be on the reader page (it starts there).
	 * But the head could have moved, since the reader was
	 * found. Check for this case and assign the iterator
	 * to the head page instead of next.
	 */
	if (iter->head_page == cpu_buffer->reader_page)
		iter->head_page = cpu_buffer->head_page;
	else
		rb_inc_page(cpu_buffer, &iter->head_page);

	iter->read_stamp = iter->head_page->time_stamp;
	iter->head = 0;
}

/**
 * ring_buffer_update_event - update event type and data
 * @event: the even to update
 * @type: the type of event
 * @length: the size of the event field in the ring buffer
 *
 * Update the type and data fields of the event. The length
 * is the actual size that is written to the ring buffer,
 * and with this, we can determine what to place into the
 * data field.
 */
static inline void
rb_update_event(struct ring_buffer_event *event,
			 unsigned type, unsigned length)
{
	event->type = type;

	switch (type) {

	case RINGBUF_TYPE_PADDING:
		break;

	case RINGBUF_TYPE_TIME_EXTEND:
		event->len =
			(RB_LEN_TIME_EXTEND + (RB_ALIGNMENT-1))
			>> RB_ALIGNMENT_SHIFT;
		break;

	case RINGBUF_TYPE_TIME_STAMP:
		event->len =
			(RB_LEN_TIME_STAMP + (RB_ALIGNMENT-1))
			>> RB_ALIGNMENT_SHIFT;
		break;

	case RINGBUF_TYPE_DATA:
		length -= RB_EVNT_HDR_SIZE;
		if (length > RB_MAX_SMALL_DATA) {
			event->len = 0;
			event->array[0] = length;
		} else
			event->len =
				(length + (RB_ALIGNMENT-1))
				>> RB_ALIGNMENT_SHIFT;
		break;
	default:
		BUG();
	}
}

static inline unsigned rb_calculate_event_length(unsigned length)
{
	struct ring_buffer_event event; /* Used only for sizeof array */

	/* zero length can cause confusions */
	if (!length)
		length = 1;

	if (length > RB_MAX_SMALL_DATA)
		length += sizeof(event.array[0]);

	length += RB_EVNT_HDR_SIZE;
	length = ALIGN(length, RB_ALIGNMENT);

	return length;
}

static struct ring_buffer_event *
__rb_reserve_next(struct ring_buffer_per_cpu *cpu_buffer,
		  unsigned type, unsigned long length, u64 *ts)
{
	struct buffer_page *tail_page, *head_page, *reader_page;
	unsigned long tail;
	struct ring_buffer *buffer = cpu_buffer->buffer;
	struct ring_buffer_event *event;

	/* No locking needed for tail page */
	tail_page = cpu_buffer->tail_page;
	tail = cpu_buffer->tail;

	if (tail + length > BUF_PAGE_SIZE) {
		struct buffer_page *next_page = tail_page;

		spin_lock(&cpu_buffer->lock);
		rb_inc_page(cpu_buffer, &next_page);

		head_page = cpu_buffer->head_page;
		reader_page = cpu_buffer->reader_page;

		/* we grabbed the lock before incrementing */
		WARN_ON(next_page == reader_page);

		if (next_page == head_page) {
			if (!(buffer->flags & RB_FL_OVERWRITE)) {
				spin_unlock(&cpu_buffer->lock);
				return NULL;
			}

			/* count overflows */
			rb_update_overflow(cpu_buffer);

			rb_inc_page(cpu_buffer, &head_page);
			cpu_buffer->head_page = head_page;
			rb_reset_head_page(cpu_buffer);
		}

		if (tail != BUF_PAGE_SIZE) {
			event = rb_page_index(tail_page, tail);
			/* page padding */
			event->type = RINGBUF_TYPE_PADDING;
		}

		tail_page->size = tail;
		tail_page = next_page;
		tail_page->size = 0;
		tail = 0;
		cpu_buffer->tail_page = tail_page;
		cpu_buffer->tail = tail;
		rb_add_stamp(cpu_buffer, ts);
		spin_unlock(&cpu_buffer->lock);
	}

	BUG_ON(tail + length > BUF_PAGE_SIZE);

	event = rb_page_index(tail_page, tail);
	rb_update_event(event, type, length);

	return event;
}

static int
rb_add_time_stamp(struct ring_buffer_per_cpu *cpu_buffer,
		  u64 *ts, u64 *delta)
{
	struct ring_buffer_event *event;
	static int once;

	if (unlikely(*delta > (1ULL << 59) && !once++)) {
		printk(KERN_WARNING "Delta way too big! %llu"
		       " ts=%llu write stamp = %llu\n",
		       *delta, *ts, cpu_buffer->write_stamp);
		WARN_ON(1);
	}

	/*
	 * The delta is too big, we to add a
	 * new timestamp.
	 */
	event = __rb_reserve_next(cpu_buffer,
				  RINGBUF_TYPE_TIME_EXTEND,
				  RB_LEN_TIME_EXTEND,
				  ts);
	if (!event)
		return -1;

	/* check to see if we went to the next page */
	if (cpu_buffer->tail) {
		/* Still on same page, update timestamp */
		event->time_delta = *delta & TS_MASK;
		event->array[0] = *delta >> TS_SHIFT;
		/* commit the time event */
		cpu_buffer->tail +=
			rb_event_length(event);
		cpu_buffer->write_stamp = *ts;
		*delta = 0;
	}

	return 0;
}

static struct ring_buffer_event *
rb_reserve_next_event(struct ring_buffer_per_cpu *cpu_buffer,
		      unsigned type, unsigned long length)
{
	struct ring_buffer_event *event;
	u64 ts, delta;

	ts = ring_buffer_time_stamp(cpu_buffer->cpu);

	if (cpu_buffer->tail) {
		delta = ts - cpu_buffer->write_stamp;

		if (test_time_stamp(delta)) {
			int ret;

			ret = rb_add_time_stamp(cpu_buffer, &ts, &delta);
			if (ret < 0)
				return NULL;
		}
	} else {
		spin_lock(&cpu_buffer->lock);
		rb_add_stamp(cpu_buffer, &ts);
		spin_unlock(&cpu_buffer->lock);
		delta = 0;
	}

	event = __rb_reserve_next(cpu_buffer, type, length, &ts);
	if (!event)
		return NULL;

	/* If the reserve went to the next page, our delta is zero */
	if (!cpu_buffer->tail)
		delta = 0;

	event->time_delta = delta;

	return event;
}

/**
 * ring_buffer_lock_reserve - reserve a part of the buffer
 * @buffer: the ring buffer to reserve from
 * @length: the length of the data to reserve (excluding event header)
 * @flags: a pointer to save the interrupt flags
 *
 * Returns a reseverd event on the ring buffer to copy directly to.
 * The user of this interface will need to get the body to write into
 * and can use the ring_buffer_event_data() interface.
 *
 * The length is the length of the data needed, not the event length
 * which also includes the event header.
 *
 * Must be paired with ring_buffer_unlock_commit, unless NULL is returned.
 * If NULL is returned, then nothing has been allocated or locked.
 */
struct ring_buffer_event *
ring_buffer_lock_reserve(struct ring_buffer *buffer,
			 unsigned long length,
			 unsigned long *flags)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	int cpu;

	if (atomic_read(&buffer->record_disabled))
		return NULL;

	local_irq_save(*flags);
	cpu = raw_smp_processor_id();

	if (!cpu_isset(cpu, buffer->cpumask))
		goto out;

	cpu_buffer = buffer->buffers[cpu];

	if (atomic_read(&cpu_buffer->record_disabled))
		goto out;

	length = rb_calculate_event_length(length);
	if (length > BUF_PAGE_SIZE)
		return NULL;

	event = rb_reserve_next_event(cpu_buffer, RINGBUF_TYPE_DATA, length);
	if (!event)
		goto out;

	return event;

 out:
	local_irq_restore(*flags);
	return NULL;
}

static void rb_commit(struct ring_buffer_per_cpu *cpu_buffer,
		      struct ring_buffer_event *event)
{
	cpu_buffer->tail += rb_event_length(event);
	cpu_buffer->tail_page->size = cpu_buffer->tail;
	cpu_buffer->write_stamp += event->time_delta;
	cpu_buffer->entries++;
}

/**
 * ring_buffer_unlock_commit - commit a reserved
 * @buffer: The buffer to commit to
 * @event: The event pointer to commit.
 * @flags: the interrupt flags received from ring_buffer_lock_reserve.
 *
 * This commits the data to the ring buffer, and releases any locks held.
 *
 * Must be paired with ring_buffer_lock_reserve.
 */
int ring_buffer_unlock_commit(struct ring_buffer *buffer,
			      struct ring_buffer_event *event,
			      unsigned long flags)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	int cpu = raw_smp_processor_id();

	cpu_buffer = buffer->buffers[cpu];

	rb_commit(cpu_buffer, event);

	local_irq_restore(flags);

	return 0;
}

/**
 * ring_buffer_write - write data to the buffer without reserving
 * @buffer: The ring buffer to write to.
 * @length: The length of the data being written (excluding the event header)
 * @data: The data to write to the buffer.
 *
 * This is like ring_buffer_lock_reserve and ring_buffer_unlock_commit as
 * one function. If you already have the data to write to the buffer, it
 * may be easier to simply call this function.
 *
 * Note, like ring_buffer_lock_reserve, the length is the length of the data
 * and not the length of the event which would hold the header.
 */
int ring_buffer_write(struct ring_buffer *buffer,
			unsigned long length,
			void *data)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	unsigned long event_length, flags;
	void *body;
	int ret = -EBUSY;
	int cpu;

	if (atomic_read(&buffer->record_disabled))
		return -EBUSY;

	local_irq_save(flags);
	cpu = raw_smp_processor_id();

	if (!cpu_isset(cpu, buffer->cpumask))
		goto out;

	cpu_buffer = buffer->buffers[cpu];

	if (atomic_read(&cpu_buffer->record_disabled))
		goto out;

	event_length = rb_calculate_event_length(length);
	event = rb_reserve_next_event(cpu_buffer,
				      RINGBUF_TYPE_DATA, event_length);
	if (!event)
		goto out;

	body = rb_event_data(event);

	memcpy(body, data, length);

	rb_commit(cpu_buffer, event);

	ret = 0;
 out:
	local_irq_restore(flags);

	return ret;
}

/**
 * ring_buffer_record_disable - stop all writes into the buffer
 * @buffer: The ring buffer to stop writes to.
 *
 * This prevents all writes to the buffer. Any attempt to write
 * to the buffer after this will fail and return NULL.
 *
 * The caller should call synchronize_sched() after this.
 */
void ring_buffer_record_disable(struct ring_buffer *buffer)
{
	atomic_inc(&buffer->record_disabled);
}

/**
 * ring_buffer_record_enable - enable writes to the buffer
 * @buffer: The ring buffer to enable writes
 *
 * Note, multiple disables will need the same number of enables
 * to truely enable the writing (much like preempt_disable).
 */
void ring_buffer_record_enable(struct ring_buffer *buffer)
{
	atomic_dec(&buffer->record_disabled);
}

/**
 * ring_buffer_record_disable_cpu - stop all writes into the cpu_buffer
 * @buffer: The ring buffer to stop writes to.
 * @cpu: The CPU buffer to stop
 *
 * This prevents all writes to the buffer. Any attempt to write
 * to the buffer after this will fail and return NULL.
 *
 * The caller should call synchronize_sched() after this.
 */
void ring_buffer_record_disable_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpu_isset(cpu, buffer->cpumask))
		return;

	cpu_buffer = buffer->buffers[cpu];
	atomic_inc(&cpu_buffer->record_disabled);
}

/**
 * ring_buffer_record_enable_cpu - enable writes to the buffer
 * @buffer: The ring buffer to enable writes
 * @cpu: The CPU to enable.
 *
 * Note, multiple disables will need the same number of enables
 * to truely enable the writing (much like preempt_disable).
 */
void ring_buffer_record_enable_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpu_isset(cpu, buffer->cpumask))
		return;

	cpu_buffer = buffer->buffers[cpu];
	atomic_dec(&cpu_buffer->record_disabled);
}

/**
 * ring_buffer_entries_cpu - get the number of entries in a cpu buffer
 * @buffer: The ring buffer
 * @cpu: The per CPU buffer to get the entries from.
 */
unsigned long ring_buffer_entries_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpu_isset(cpu, buffer->cpumask))
		return 0;

	cpu_buffer = buffer->buffers[cpu];
	return cpu_buffer->entries;
}

/**
 * ring_buffer_overrun_cpu - get the number of overruns in a cpu_buffer
 * @buffer: The ring buffer
 * @cpu: The per CPU buffer to get the number of overruns from
 */
unsigned long ring_buffer_overrun_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpu_isset(cpu, buffer->cpumask))
		return 0;

	cpu_buffer = buffer->buffers[cpu];
	return cpu_buffer->overrun;
}

/**
 * ring_buffer_entries - get the number of entries in a buffer
 * @buffer: The ring buffer
 *
 * Returns the total number of entries in the ring buffer
 * (all CPU entries)
 */
unsigned long ring_buffer_entries(struct ring_buffer *buffer)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long entries = 0;
	int cpu;

	/* if you care about this being correct, lock the buffer */
	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		entries += cpu_buffer->entries;
	}

	return entries;
}

/**
 * ring_buffer_overrun_cpu - get the number of overruns in buffer
 * @buffer: The ring buffer
 *
 * Returns the total number of overruns in the ring buffer
 * (all CPU entries)
 */
unsigned long ring_buffer_overruns(struct ring_buffer *buffer)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long overruns = 0;
	int cpu;

	/* if you care about this being correct, lock the buffer */
	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		overruns += cpu_buffer->overrun;
	}

	return overruns;
}

/**
 * ring_buffer_iter_reset - reset an iterator
 * @iter: The iterator to reset
 *
 * Resets the iterator, so that it will start from the beginning
 * again.
 */
void ring_buffer_iter_reset(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;

	/* Iterator usage is expected to have record disabled */
	if (list_empty(&cpu_buffer->reader_page->list)) {
		iter->head_page = cpu_buffer->head_page;
		iter->head = cpu_buffer->head;
	} else {
		iter->head_page = cpu_buffer->reader_page;
		iter->head = cpu_buffer->reader;
	}
	if (iter->head)
		iter->read_stamp = cpu_buffer->read_stamp;
	else
		iter->read_stamp = iter->head_page->time_stamp;
}

/**
 * ring_buffer_iter_empty - check if an iterator has no more to read
 * @iter: The iterator to check
 */
int ring_buffer_iter_empty(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	cpu_buffer = iter->cpu_buffer;

	return iter->head_page == cpu_buffer->tail_page &&
		iter->head == cpu_buffer->tail;
}

static void
rb_update_read_stamp(struct ring_buffer_per_cpu *cpu_buffer,
		     struct ring_buffer_event *event)
{
	u64 delta;

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		return;

	case RINGBUF_TYPE_TIME_EXTEND:
		delta = event->array[0];
		delta <<= TS_SHIFT;
		delta += event->time_delta;
		cpu_buffer->read_stamp += delta;
		return;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		return;

	case RINGBUF_TYPE_DATA:
		cpu_buffer->read_stamp += event->time_delta;
		return;

	default:
		BUG();
	}
	return;
}

static void
rb_update_iter_read_stamp(struct ring_buffer_iter *iter,
			  struct ring_buffer_event *event)
{
	u64 delta;

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		return;

	case RINGBUF_TYPE_TIME_EXTEND:
		delta = event->array[0];
		delta <<= TS_SHIFT;
		delta += event->time_delta;
		iter->read_stamp += delta;
		return;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		return;

	case RINGBUF_TYPE_DATA:
		iter->read_stamp += event->time_delta;
		return;

	default:
		BUG();
	}
	return;
}

static struct buffer_page *
rb_get_reader_page(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct buffer_page *reader = NULL;
	unsigned long flags;

	spin_lock_irqsave(&cpu_buffer->lock, flags);

 again:
	reader = cpu_buffer->reader_page;

	/* If there's more to read, return this page */
	if (cpu_buffer->reader < reader->size)
		goto out;

	/* Never should we have an index greater than the size */
	WARN_ON(cpu_buffer->reader > reader->size);

	/* check if we caught up to the tail */
	reader = NULL;
	if (cpu_buffer->tail_page == cpu_buffer->reader_page)
		goto out;

	/*
	 * Splice the empty reader page into the list around the head.
	 * Reset the reader page to size zero.
	 */

	reader = cpu_buffer->head_page;
	cpu_buffer->reader_page->list.next = reader->list.next;
	cpu_buffer->reader_page->list.prev = reader->list.prev;
	cpu_buffer->reader_page->size = 0;

	/* Make the reader page now replace the head */
	reader->list.prev->next = &cpu_buffer->reader_page->list;
	reader->list.next->prev = &cpu_buffer->reader_page->list;

	/*
	 * If the tail is on the reader, then we must set the head
	 * to the inserted page, otherwise we set it one before.
	 */
	cpu_buffer->head_page = cpu_buffer->reader_page;

	if (cpu_buffer->tail_page != reader)
		rb_inc_page(cpu_buffer, &cpu_buffer->head_page);

	/* Finally update the reader page to the new head */
	cpu_buffer->reader_page = reader;
	rb_reset_reader_page(cpu_buffer);

	goto again;

 out:
	spin_unlock_irqrestore(&cpu_buffer->lock, flags);

	return reader;
}

static void rb_advance_reader(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct ring_buffer_event *event;
	struct buffer_page *reader;
	unsigned length;

	reader = rb_get_reader_page(cpu_buffer);

	/* This function should not be called when buffer is empty */
	BUG_ON(!reader);

	event = rb_reader_event(cpu_buffer);

	if (event->type == RINGBUF_TYPE_DATA)
		cpu_buffer->entries--;

	rb_update_read_stamp(cpu_buffer, event);

	length = rb_event_length(event);
	cpu_buffer->reader += length;
}

static void rb_advance_iter(struct ring_buffer_iter *iter)
{
	struct ring_buffer *buffer;
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	unsigned length;

	cpu_buffer = iter->cpu_buffer;
	buffer = cpu_buffer->buffer;

	/*
	 * Check if we are at the end of the buffer.
	 */
	if (iter->head >= iter->head_page->size) {
		BUG_ON(iter->head_page == cpu_buffer->tail_page);
		rb_inc_iter(iter);
		return;
	}

	event = rb_iter_head_event(iter);

	length = rb_event_length(event);

	/*
	 * This should not be called to advance the header if we are
	 * at the tail of the buffer.
	 */
	BUG_ON((iter->head_page == cpu_buffer->tail_page) &&
	       (iter->head + length > cpu_buffer->tail));

	rb_update_iter_read_stamp(iter, event);

	iter->head += length;

	/* check for end of page padding */
	if ((iter->head >= iter->head_page->size) &&
	    (iter->head_page != cpu_buffer->tail_page))
		rb_advance_iter(iter);
}

/**
 * ring_buffer_peek - peek at the next event to be read
 * @buffer: The ring buffer to read
 * @cpu: The cpu to peak at
 * @ts: The timestamp counter of this event.
 *
 * This will return the event that will be read next, but does
 * not consume the data.
 */
struct ring_buffer_event *
ring_buffer_peek(struct ring_buffer *buffer, int cpu, u64 *ts)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	struct buffer_page *reader;

	if (!cpu_isset(cpu, buffer->cpumask))
		return NULL;

	cpu_buffer = buffer->buffers[cpu];

 again:
	reader = rb_get_reader_page(cpu_buffer);
	if (!reader)
		return NULL;

	event = rb_reader_event(cpu_buffer);

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		WARN_ON(1);
		rb_advance_reader(cpu_buffer);
		return NULL;

	case RINGBUF_TYPE_TIME_EXTEND:
		/* Internal data, OK to advance */
		rb_advance_reader(cpu_buffer);
		goto again;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		rb_advance_reader(cpu_buffer);
		goto again;

	case RINGBUF_TYPE_DATA:
		if (ts) {
			*ts = cpu_buffer->read_stamp + event->time_delta;
			ring_buffer_normalize_time_stamp(cpu_buffer->cpu, ts);
		}
		return event;

	default:
		BUG();
	}

	return NULL;
}

/**
 * ring_buffer_iter_peek - peek at the next event to be read
 * @iter: The ring buffer iterator
 * @ts: The timestamp counter of this event.
 *
 * This will return the event that will be read next, but does
 * not increment the iterator.
 */
struct ring_buffer_event *
ring_buffer_iter_peek(struct ring_buffer_iter *iter, u64 *ts)
{
	struct ring_buffer *buffer;
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;

	if (ring_buffer_iter_empty(iter))
		return NULL;

	cpu_buffer = iter->cpu_buffer;
	buffer = cpu_buffer->buffer;

 again:
	if (rb_per_cpu_empty(cpu_buffer))
		return NULL;

	event = rb_iter_head_event(iter);

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		rb_inc_iter(iter);
		goto again;

	case RINGBUF_TYPE_TIME_EXTEND:
		/* Internal data, OK to advance */
		rb_advance_iter(iter);
		goto again;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		rb_advance_iter(iter);
		goto again;

	case RINGBUF_TYPE_DATA:
		if (ts) {
			*ts = iter->read_stamp + event->time_delta;
			ring_buffer_normalize_time_stamp(cpu_buffer->cpu, ts);
		}
		return event;

	default:
		BUG();
	}

	return NULL;
}

/**
 * ring_buffer_consume - return an event and consume it
 * @buffer: The ring buffer to get the next event from
 *
 * Returns the next event in the ring buffer, and that event is consumed.
 * Meaning, that sequential reads will keep returning a different event,
 * and eventually empty the ring buffer if the producer is slower.
 */
struct ring_buffer_event *
ring_buffer_consume(struct ring_buffer *buffer, int cpu, u64 *ts)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;

	if (!cpu_isset(cpu, buffer->cpumask))
		return NULL;

	event = ring_buffer_peek(buffer, cpu, ts);
	if (!event)
		return NULL;

	cpu_buffer = buffer->buffers[cpu];
	rb_advance_reader(cpu_buffer);

	return event;
}

/**
 * ring_buffer_read_start - start a non consuming read of the buffer
 * @buffer: The ring buffer to read from
 * @cpu: The cpu buffer to iterate over
 *
 * This starts up an iteration through the buffer. It also disables
 * the recording to the buffer until the reading is finished.
 * This prevents the reading from being corrupted. This is not
 * a consuming read, so a producer is not expected.
 *
 * Must be paired with ring_buffer_finish.
 */
struct ring_buffer_iter *
ring_buffer_read_start(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_iter *iter;
	unsigned long flags;

	if (!cpu_isset(cpu, buffer->cpumask))
		return NULL;

	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return NULL;

	cpu_buffer = buffer->buffers[cpu];

	iter->cpu_buffer = cpu_buffer;

	atomic_inc(&cpu_buffer->record_disabled);
	synchronize_sched();

	spin_lock_irqsave(&cpu_buffer->lock, flags);
	ring_buffer_iter_reset(iter);
	spin_unlock_irqrestore(&cpu_buffer->lock, flags);

	return iter;
}

/**
 * ring_buffer_finish - finish reading the iterator of the buffer
 * @iter: The iterator retrieved by ring_buffer_start
 *
 * This re-enables the recording to the buffer, and frees the
 * iterator.
 */
void
ring_buffer_read_finish(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;

	atomic_dec(&cpu_buffer->record_disabled);
	kfree(iter);
}

/**
 * ring_buffer_read - read the next item in the ring buffer by the iterator
 * @iter: The ring buffer iterator
 * @ts: The time stamp of the event read.
 *
 * This reads the next event in the ring buffer and increments the iterator.
 */
struct ring_buffer_event *
ring_buffer_read(struct ring_buffer_iter *iter, u64 *ts)
{
	struct ring_buffer_event *event;

	event = ring_buffer_iter_peek(iter, ts);
	if (!event)
		return NULL;

	rb_advance_iter(iter);

	return event;
}

/**
 * ring_buffer_size - return the size of the ring buffer (in bytes)
 * @buffer: The ring buffer.
 */
unsigned long ring_buffer_size(struct ring_buffer *buffer)
{
	return BUF_PAGE_SIZE * buffer->pages;
}

static void
rb_reset_cpu(struct ring_buffer_per_cpu *cpu_buffer)
{
	cpu_buffer->head_page
		= list_entry(cpu_buffer->pages.next, struct buffer_page, list);
	cpu_buffer->head_page->size = 0;
	cpu_buffer->tail_page = cpu_buffer->head_page;
	cpu_buffer->tail_page->size = 0;
	INIT_LIST_HEAD(&cpu_buffer->reader_page->list);
	cpu_buffer->reader_page->size = 0;

	cpu_buffer->head = cpu_buffer->tail = cpu_buffer->reader = 0;

	cpu_buffer->overrun = 0;
	cpu_buffer->entries = 0;
}

/**
 * ring_buffer_reset_cpu - reset a ring buffer per CPU buffer
 * @buffer: The ring buffer to reset a per cpu buffer of
 * @cpu: The CPU buffer to be reset
 */
void ring_buffer_reset_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer = buffer->buffers[cpu];
	unsigned long flags;

	if (!cpu_isset(cpu, buffer->cpumask))
		return;

	spin_lock_irqsave(&cpu_buffer->lock, flags);

	rb_reset_cpu(cpu_buffer);

	spin_unlock_irqrestore(&cpu_buffer->lock, flags);
}

/**
 * ring_buffer_reset - reset a ring buffer
 * @buffer: The ring buffer to reset all cpu buffers
 */
void ring_buffer_reset(struct ring_buffer *buffer)
{
	int cpu;

	for_each_buffer_cpu(buffer, cpu)
		ring_buffer_reset_cpu(buffer, cpu);
}

/**
 * rind_buffer_empty - is the ring buffer empty?
 * @buffer: The ring buffer to test
 */
int ring_buffer_empty(struct ring_buffer *buffer)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	int cpu;

	/* yes this is racy, but if you don't like the race, lock the buffer */
	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		if (!rb_per_cpu_empty(cpu_buffer))
			return 0;
	}
	return 1;
}

/**
 * ring_buffer_empty_cpu - is a cpu buffer of a ring buffer empty?
 * @buffer: The ring buffer
 * @cpu: The CPU buffer to test
 */
int ring_buffer_empty_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpu_isset(cpu, buffer->cpumask))
		return 1;

	cpu_buffer = buffer->buffers[cpu];
	return rb_per_cpu_empty(cpu_buffer);
}

/**
 * ring_buffer_swap_cpu - swap a CPU buffer between two ring buffers
 * @buffer_a: One buffer to swap with
 * @buffer_b: The other buffer to swap with
 *
 * This function is useful for tracers that want to take a "snapshot"
 * of a CPU buffer and has another back up buffer lying around.
 * it is expected that the tracer handles the cpu buffer not being
 * used at the moment.
 */
int ring_buffer_swap_cpu(struct ring_buffer *buffer_a,
			 struct ring_buffer *buffer_b, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer_a;
	struct ring_buffer_per_cpu *cpu_buffer_b;

	if (!cpu_isset(cpu, buffer_a->cpumask) ||
	    !cpu_isset(cpu, buffer_b->cpumask))
		return -EINVAL;

	/* At least make sure the two buffers are somewhat the same */
	if (buffer_a->size != buffer_b->size ||
	    buffer_a->pages != buffer_b->pages)
		return -EINVAL;

	cpu_buffer_a = buffer_a->buffers[cpu];
	cpu_buffer_b = buffer_b->buffers[cpu];

	/*
	 * We can't do a synchronize_sched here because this
	 * function can be called in atomic context.
	 * Normally this will be called from the same CPU as cpu.
	 * If not it's up to the caller to protect this.
	 */
	atomic_inc(&cpu_buffer_a->record_disabled);
	atomic_inc(&cpu_buffer_b->record_disabled);

	buffer_a->buffers[cpu] = cpu_buffer_b;
	buffer_b->buffers[cpu] = cpu_buffer_a;

	cpu_buffer_b->buffer = buffer_a;
	cpu_buffer_a->buffer = buffer_b;

	atomic_dec(&cpu_buffer_a->record_disabled);
	atomic_dec(&cpu_buffer_b->record_disabled);

	return 0;
}

