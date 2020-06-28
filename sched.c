/* 
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is the Netscape Portable Runtime library.
 * 
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are 
 * Copyright (C) 1994-2000 Netscape Communications Corporation.  All
 * Rights Reserved.
 * 
 * Contributor(s):  Silicon Graphics, Inc.
 * 
 * Portions created by SGI are Copyright (C) 2000-2001 Silicon
 * Graphics, Inc.  All Rights Reserved.
 * 
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU General Public License Version 2 or later (the
 * "GPL"), in which case the provisions of the GPL are applicable 
 * instead of those above.  If you wish to allow use of your 
 * version of this file only under the terms of the GPL and not to
 * allow others to use your version of this file under the MPL,
 * indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by
 * the GPL.  If you do not delete the provisions above, a recipient
 * may use your version of this file under either the MPL or the
 * GPL.
 */

/*
 * This file is derived directly from Netscape Communications Corporation,
 * and consists of extensive modifications made during the year(s) 1999-2000.
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <wordexp.h>
#include <execinfo.h>
#include "common.h"

// #define ST_DEBUG_PRINTF(...)

/* Global data */
struct st_vp_desc st_vps[MAX_ST_VP];
int nb_worker_pthreads;

__thread int self_index;

pthread_t schedule_thread;

time_t _st_curr_time = 0; /* Current time as returned by time(2) */
st_utime_t _st_last_tset; /* Last time it was fetched */

int st_poll(struct pollfd *pds, int npds, st_utime_t timeout)
{
	struct pollfd *pd;
	struct pollfd *epd = pds + npds;
	_st_pollq_t pq;
	_st_thread_t *me = _ST_CURRENT_THREAD();
	int n;

	if (me->flags & _ST_FL_INTERRUPT) {
		me->flags &= ~_ST_FL_INTERRUPT;
		errno = EINTR;
		return -1;
	}

	if ((*_st_eventsys->pollset_add)(pds, npds) < 0)
		return -1;

	pq.pds = pds;
	pq.npds = npds;
	pq.thread = me;
	pq.on_ioq = 1;
	_ST_ADD_IOQ(pq);
	if (timeout != ST_UTIME_NO_TIMEOUT)
		_ST_ADD_SLEEPQ(me, timeout);
	me->state = _ST_ST_IO_WAIT;

	_ST_SWITCH_CONTEXT(me);

	n = 0;
	if (pq.on_ioq) {
		/* If we timed out, the pollq might still be on the ioq. Remove it */
		_ST_DEL_IOQ(pq);
		(*_st_eventsys->pollset_del)(pds, npds);
	} else {
		/* Count the number of ready descriptors */
		for (pd = pds; pd < epd; pd++) {
			if (pd->revents)
				n++;
		}
	}

	if (me->flags & _ST_FL_INTERRUPT) {
		me->flags &= ~_ST_FL_INTERRUPT;
		errno = EINTR;
		return -1;
	}

	return n;
}

_st_thread_t *get_first_thread_from_runq(_st_clist_t* l,
		st_pthread_spinlock *q_lock)
{
	_st_thread_t* th = NULL;
	st_pthread_spin_lock(q_lock);
	if (l->next != l) {
		_st_clist_t *next = l->next;
		th = _ST_THREAD_PTR(next);
		ST_DEBUG_PRINTF("before remove l->next is %p, %p, %p, l is %p self index %d\n", l->next, l->next->next, l->next->next->next, l, self_index);
		ST_REMOVE_LINK(next); ST_DEBUG_PRINTF("delete thread %p from runq %d l->next is %p l is %p\n", th, self_index, l->next, l);
	}
	st_pthread_spin_unlock(q_lock);
	return th;
}

_st_thread_t *get_last_thread_from_runq(_st_clist_t* l,
		st_pthread_spinlock *q_lock)
{
	_st_thread_t* th = NULL;
	st_pthread_spin_lock(q_lock);
	if (l->next != l) {
		_st_clist_t *prev = l->prev;
		th = _ST_THREAD_PTR(prev);
		ST_REMOVE_LINK(prev); ST_DEBUG_PRINTF("delete thread %p from runq %d\n", th, self_index);
	}
	st_pthread_spin_unlock(q_lock);
	return th;
}

void _st_vp_schedule(void)
{
	_st_thread_t *thread;

	if (!_ST_IS_EMPTY) {
		/* Pull thread off of the run queue */
		thread = get_first_thread_from_runq(&_ST_RUNQ(self_index),
				&_ST_RUNQ_LOCK(self_index));
		if (!thread) {
			thread = _st_this_vp.idle_thread;
		}
	} else {
		/* If there are no threads to run, switch to the idle thread */
		thread = _st_this_vp.idle_thread;
	}

	ST_ASSERT(thread->state == _ST_ST_RUNNABLE);

	/* Resume the thread */
	thread->state = _ST_ST_RUNNING;
	_ST_RESTORE_CONTEXT(thread);
}

void clear_switch_q()
{
	if (ST_CLIST_IS_EMPTY(&_st_this_vp.switching_q)) {
		return;
	}
	while (!ST_CLIST_IS_EMPTY(&_st_this_vp.switching_q)) {
		_st_clist_t* l = _st_this_vp.switching_q.next;
		ST_REMOVE_LINK(l)
		_st_thread_t *thread = _ST_THREAD_PTR(l);
		atomic_inc(&the_vp(thread->vp_index)._st_active_count);
		_ST_ADD_RUNQ(thread)
	}
}

void schedule_to_vp(int index)
{
	index %= nb_worker_pthreads;
	if (index == self_index) {
		return;
	}
	ST_DEBUG_PRINTF("schedule from %d to %d\n", self_index, index);
	_st_thread_t *me = _ST_CURRENT_THREAD();

	_ST_DEL_THREADQ(me);
	me->vp_index = index;
	_ST_ADD_THREADQ(me);

	_ST_ADD_SWITCHQ(me);
	atomic_dec(&this_vp._st_active_count);
	_ST_SWITCH_CONTEXT(me);
}

static void* event_pass_fun(void* arg)
{
	int eventfd = _st_this_vp.eventfd;
	while(1) {
		struct pollfd pd;
		pd.fd = eventfd;
		pd.revents = 0;
		pd.events = POLLIN;
		int ret = st_poll(&pd, 1, ST_UTIME_NO_TIMEOUT);
		if (ret < 0) {
			break;
		}
		uint64_t data;
		ret = read(eventfd, &data, sizeof(data));
		(void)ret;
	}
	ST_DEBUG_PRINTF("event pass fun exit");
	return NULL;
}

uint64_t interrute_count;

void interrupt_vp(int vp_index)
{
	ST_DEBUG_PRINTF("self vp %d interrupt vp %d\n", self_index, vp_index);
	int efd = _st_the_vp(vp_index).eventfd;
	uint64_t data = 1;
	int ret = write(efd, &data, sizeof(data));
	(void) ret;
	interrute_count++;
}

/*
 * Initialize this Virtual Processor
 */
int st_init_pthread()
{
	_st_thread_t *thread;

	self_index = atomic_inc(&nb_worker_pthreads);
	this_vp.self_pthread_id = pthread_self();

	memset(&_st_this_vp, 0, sizeof(_st_vp_t));

	ST_INIT_CLIST(&_ST_RUNQ(self_index));
	ST_INIT_CLIST(&_ST_IOQ);
	ST_INIT_CLIST(&_ST_ZOMBIEQ);
	ST_INIT_CLIST(&_ST_SWITCHQ(self_index));
	ST_INIT_CLIST(&_ST_THREADQ(self_index));
	ST_INIT_CLIST(&_ST_MUTEXQ(self_index));

	if ((*_st_eventsys->init)() < 0)
		return -1;

	_st_this_vp.pagesize = getpagesize();
	_st_this_vp.last_clock = st_utime();
	st_pthread_spin_init(&_st_this_vp.run_q_lock);
	st_pthread_spin_init(&_st_this_vp._sleep_q_lock);
	st_pthread_spin_init(&_st_this_vp.thread_q_lock);
	st_pthread_spin_init(&_st_this_vp.mutex_q_lock);

	/*
	 * Create idle thread
	 */
	_st_this_vp.idle_thread = st_thread_create(_st_idle_thread_start,
			NULL, 0, 0);
	if (!_st_this_vp.idle_thread) {
		return -1;
	}

	_st_this_vp.idle_thread->flags = _ST_FL_IDLE_THREAD;
	atomic_dec(&this_vp._st_active_count);
	_ST_DEL_RUNQ(_st_this_vp.idle_thread);

	/*
	 * create event thread for multi pthread
	 */
	_st_this_vp.eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	_st_this_vp.event_thread = st_thread_create(event_pass_fun, NULL, 0, 0);
	if (!_st_this_vp.event_thread) {
		return -1;
	}
	_st_this_vp.event_thread->flags = _ST_FL_PTREAD_SPIN;

	/*
	 * Initialize primordial thread
	 */
	thread = (_st_thread_t *) calloc(1,
			sizeof(_st_thread_t) + (ST_KEYS_MAX * sizeof(void *)));
	if (!thread)
		return -1;
	thread->private_data = (void **) (thread + 1);
	thread->state = _ST_ST_RUNNING;
	thread->flags = _ST_FL_PRIMORDIAL;
	thread->vp_index = self_index;
	_ST_SET_CURRENT_THREAD(thread);
	atomic_inc(&this_vp._st_active_count);
	_st_this_vp.main_thread = st_thread_self();
	_ST_ADD_THREADQ(thread);
	return 0;
}

static pthread_barrier_t init_barrier;

void* _st_worker_fun(void* arg)
{
	int ret = st_init_pthread();
	if (ret < 0) {
		return NULL;
	}
	pthread_barrier_wait(&init_barrier);
	st_sleep(-1);
	ST_DEBUG_PRINTF("pthread gone first");
	return NULL;
}

void* _st_schedule_fun(void* arg)
{
	pthread_barrier_wait(&init_barrier);
	while (1) {
		usleep(100 * 1000 * 1000);
	}
	return NULL;
}

extern pthread_spinlock_t free_stack_lock;

int st_init(int pthread_worker_nb)
{
	if (nb_worker_pthreads) {
		/* Already initialized */
		return 0;
	}
	if(pthread_worker_nb+1 > MAX_ST_VP){
		pthread_worker_nb = MAX_ST_VP-1;
	}

	pthread_barrier_init(&init_barrier, NULL, pthread_worker_nb + 2);

	/* We can ignore return value here */
	st_set_eventsys(ST_EVENTSYS_ALT);

	pthread_spin_init(&free_stack_lock, 0);

	if (_st_io_init() < 0) {
		return -1;
	}

	int ret = st_init_pthread();
	if (ret < 0) {
		return ret;
	}
	int i = 0;
	for (i = 0; i < pthread_worker_nb; i++) {
		pthread_t threadid;
		ret = pthread_create(&threadid, 0, _st_worker_fun, (void*) (uint64_t) i);
		if (ret < 0) {
			return ret;
		}
	}

	ret = pthread_create(&schedule_thread, 0, _st_schedule_fun, NULL);
	if (ret < 0) {
		return ret;
	}
	pthread_barrier_wait(&init_barrier);

	return 0;
}

#ifdef ST_SWITCH_CB
st_switch_cb_t st_set_switch_in_cb(st_switch_cb_t cb)
{
	st_switch_cb_t ocb = _st_this_vp.switch_in_cb;
	_st_this_vp.switch_in_cb = cb;
	return ocb;
}

st_switch_cb_t st_set_switch_out_cb(st_switch_cb_t cb)
{
	st_switch_cb_t ocb = _st_this_vp.switch_out_cb;
	_st_this_vp.switch_out_cb = cb;
	return ocb;
}
#endif

/*
 * Start function for the idle thread
 */
/* ARGSUSED */
void *_st_idle_thread_start(void *arg)
{
	_st_thread_t *me = _ST_CURRENT_THREAD();

	while (this_vp._st_active_count > 0) {
		/* Idle vp till I/O is ready or the smallest timeout expired */
		_ST_VP_IDLE();

		/* Check sleep queue for expired threads */
		_st_vp_check_clock();

		me->state = _ST_ST_RUNNABLE;
		_ST_SWITCH_CONTEXT(me);
	}

	ST_DEBUG_PRINTF("this vp active count is %d\n", this_vp._st_active_count);
	/* No more threads */
	exit(0);

	/* NOTREACHED */
	return NULL;
}

void st_thread_exit(void *retval)
{
	_st_thread_t *thread = _ST_CURRENT_THREAD();

	thread->retval = retval;
	_st_thread_cleanup(thread);
	atomic_dec(&this_vp._st_active_count);
	if (thread->term) {
		/* Put thread on the zombie queue */
		st_mutex_lock(thread->term_mutex);

		thread->state = _ST_ST_ZOMBIE;
		_ST_ADD_ZOMBIEQ(thread);

		/* Notify on our termination condition variable */
		st_cond_signal(thread->term);

		st_mutex_unlock(thread->term_mutex);
		/* Switch context and come back later */
		_ST_SWITCH_CONTEXT(thread);

		/* Continue the cleanup */
		st_cond_destroy(thread->term);
		st_mutex_destroy(thread->term_mutex);
		thread->term = NULL;
	}

	_ST_DEL_THREADQ(thread);

	if (!(thread->flags & _ST_FL_PRIMORDIAL))
		_st_stack_free(thread->stack);

	/* Find another thread to run */
	_ST_SWITCH_CONTEXT(thread);
	/* Not going to land here */
}

int st_thread_join(_st_thread_t *thread, void **retvalp)
{
	_st_cond_t *term = thread->term;

	/* Can't join a non-joinable thread */
	if (term == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (_ST_CURRENT_THREAD() == thread) {
		errno = EDEADLK;
		return -1;
	}

	/* Multiple threads can't wait on the same joinable thread */
	if (term->wait_q.next != &term->wait_q) {
		errno = EINVAL;
		return -1;
	}

	st_mutex_lock(thread->term_mutex);
	while (thread->state != _ST_ST_ZOMBIE) {
		if (st_cond_timedwait(term, ST_UTIME_NO_TIMEOUT, thread->term_mutex) != 0){
			st_mutex_unlock(thread->term_mutex);
			return -1;
		}
	}
	st_mutex_unlock(thread->term_mutex);

	if (retvalp)
		*retvalp = thread->retval;

	/*
	 * Remove target thread from the zombie queue and make it runnable.
	 * When it gets scheduled later, it will do the clean up.
	 */
	_ST_DEL_ZOMBIEQ(thread);
	_ST_ADD_RUNQ(thread);

	return 0;
}

void _st_thread_main(void)
{
	_st_thread_t *thread = _ST_CURRENT_THREAD();

	/*
	 * Cap the stack by zeroing out the saved return address register
	 * value. This allows some debugging/profiling tools to know when
	 * to stop unwinding the stack. It's a no-op on most platforms.
	 */
	MD_CAP_STACK(&thread);

	/* Run thread main */
	thread->retval = (*thread->start)(thread->arg);

	/* All done, time to go away */
	st_thread_exit(thread->retval);
}

/*
 * Insert "thread" into the timeout heap, in the position
 * specified by thread->heap_index.  See docs/timeout_heap.txt
 * for details about the timeout heap.
 */
static _st_thread_t **heap_insert(_st_thread_t *thread)
{
	int target = thread->heap_index;
	int s = target;
	_st_thread_t **p = &_ST_SLEEPQ(thread->vp_index);
	int bits = 0;
	int bit;
	int index = 1;

	while (s) {
		s >>= 1;
		bits++;
	}
	for (bit = bits - 2; bit >= 0; bit--) {
		if (thread->due < (*p)->due) {
			_st_thread_t *t = *p;
			thread->left = t->left;
			thread->right = t->right;
			*p = thread;
			thread->heap_index = index;
			thread = t;
		}
		index <<= 1;
		if (target & (1 << bit)) {
			p = &((*p)->right);
			index |= 1;
		} else {
			p = &((*p)->left);
		}
	}
	thread->heap_index = index;
	*p = thread;
	thread->left = thread->right = NULL;
	return p;
}

/*
 * Delete "thread" from the timeout heap.
 */
static void heap_delete(_st_thread_t *thread)
{
	_st_thread_t *t, **p;
	int bits = 0;
	int s, bit;

	/* First find and unlink the last heap element */
	p = &_ST_SLEEPQ(thread->vp_index);
	s = _ST_SLEEPQ_SIZE(thread->vp_index);
	while (s) {
		s >>= 1;
		bits++;
	}
	for (bit = bits - 2; bit >= 0; bit--) {
		if (_ST_SLEEPQ_SIZE(thread->vp_index) & (1 << bit)) {
			p = &((*p)->right);
		} else {
			p = &((*p)->left);
		}
	}
	t = *p;
	*p = NULL;
	--_ST_SLEEPQ_SIZE(thread->vp_index);
	if (t != thread) {
		/*
		 * Insert the unlinked last element in place of the element we are deleting
		 */
		t->heap_index = thread->heap_index;
		p = heap_insert(t);
		t = *p;
		t->left = thread->left;
		t->right = thread->right;

		/*
		 * Reestablish the heap invariant.
		 */
		for (;;) {
			_st_thread_t *y; /* The younger child */
			int index_tmp;
			if (t->left == NULL)
				break;
			else if (t->right == NULL)
				y = t->left;
			else if (t->left->due < t->right->due)
				y = t->left;
			else
				y = t->right;
			if (t->due > y->due) {
				_st_thread_t *tl = y->left;
				_st_thread_t *tr = y->right;
				*p = y;
				if (y == t->left) {
					y->left = t;
					y->right = t->right;
					p = &y->left;
				} else {
					y->left = t->left;
					y->right = t;
					p = &y->right;
				}
				t->left = tl;
				t->right = tr;
				index_tmp = t->heap_index;
				t->heap_index = y->heap_index;
				y->heap_index = index_tmp;
			} else {
				break;
			}
		}
	}
	thread->left = thread->right = NULL;
}

void lock_sleep_q(_st_thread_t *thread)
{
	st_pthread_spin_lock(&_st_the_vp(thread->vp_index)._sleep_q_lock);
}

void unlock_sleep_q(_st_thread_t *thread)
{
	st_pthread_spin_unlock(&_st_the_vp(thread->vp_index)._sleep_q_lock);
}

void _st_add_sleep_q(_st_thread_t *thread, st_utime_t timeout)
{
	ST_DEBUG_PRINTF("add sleep q thread %p timeout %d now %d last clock %d\n",
			thread, timeout, st_utime(), _ST_LAST_CLOCK);
	lock_sleep_q(thread);
	thread->due = _ST_LAST_CLOCK + timeout;
	thread->flags |= _ST_FL_ON_SLEEPQ;
	ST_ASSERT(self_index == thread->vp_index);
	thread->heap_index = ++_ST_SLEEPQ_SIZE(self_index);
	heap_insert(thread);
	unlock_sleep_q(thread);
}

void _st_del_sleep_q(_st_thread_t *thread)
{
	ST_DEBUG_PRINTF("del sleep q thread %p\n", thread);
	lock_sleep_q(thread);
	if (thread->flags & _ST_FL_ON_SLEEPQ) {
		heap_delete(thread);
		thread->flags &= ~_ST_FL_ON_SLEEPQ;
	}
	unlock_sleep_q(thread);
}

void _st_vp_check_clock(void)
{
	_st_thread_t *thread;
	st_utime_t elapsed, now;

	now = st_utime();
	elapsed = now - _ST_LAST_CLOCK;
	(void) elapsed;
	_ST_LAST_CLOCK = now;

	if (_st_curr_time && now - _st_last_tset > 999000) {
		_st_curr_time = time(NULL);
		_st_last_tset = now;
	}

	while (_ST_SLEEPQ(self_index) != NULL) {
		thread = _ST_SLEEPQ(self_index);
		ST_ASSERT(thread->flags & _ST_FL_ON_SLEEPQ);
		if (thread->due > now)
			break;
		_ST_DEL_SLEEPQ(thread);

		/* If thread is waiting on condition variable, set the time out flag */
		if (thread->state == _ST_ST_COND_WAIT)
			thread->flags |= _ST_FL_TIMEDOUT;

		/* Make thread runnable */
		ST_ASSERT(!(thread->flags & _ST_FL_IDLE_THREAD));
		_ST_ADD_RUNQ(thread);
	}
}

void st_thread_interrupt(_st_thread_t *thread)
{
	/* If thread is already dead */
	if (thread->state == _ST_ST_ZOMBIE)
		return;

	thread->flags |= _ST_FL_INTERRUPT;

	if (thread->state == _ST_ST_RUNNING || thread->state == _ST_ST_RUNNABLE)
		return;

	_ST_DEL_SLEEPQ(thread);

	/* Make thread runnable */
	_ST_ADD_RUNQ(thread);
}

_st_thread_t *st_thread_create_vp(void *(*start)(void *arg), void *arg,
		int joinable, int stk_size, int _vp_index)
{
	_st_thread_t *thread;
	_st_stack_t *stack;
	void **ptds;
	char *sp;
#ifdef __ia64__
	char *bsp;
#endif

	/* Adjust stack size */
	if (stk_size == 0)
		stk_size = ST_DEFAULT_STACK_SIZE;
	stk_size = ((stk_size + _ST_PAGE_SIZE - 1) / _ST_PAGE_SIZE) * _ST_PAGE_SIZE;
	stack = _st_stack_new(stk_size);
	if (!stack)
		return NULL;

	/* Allocate thread object and per-thread data off the stack */
#if defined (MD_STACK_GROWS_DOWN)
	sp = stack->stk_top;
#ifdef __ia64__
	/*
	 * The stack segment is split in the middle. The upper half is used
	 * as backing store for the register stack which grows upward.
	 * The lower half is used for the traditional memory stack which
	 * grows downward. Both stacks start in the middle and grow outward
	 * from each other.
	 */
	sp -= (stk_size >> 1);
	bsp = sp;
	/* Make register stack 64-byte aligned */
	if ((unsigned long)bsp & 0x3f)
	bsp = bsp + (0x40 - ((unsigned long)bsp & 0x3f));
	stack->bsp = bsp + _ST_STACK_PAD_SIZE;
#endif
	sp = sp - (ST_KEYS_MAX * sizeof(void *));
	ptds = (void **) sp;
	sp = sp - sizeof(_st_thread_t);
	thread = (_st_thread_t *) sp;

	/* Make stack 64-byte aligned */
	if ((unsigned long) sp & 0x3f)
		sp = sp - ((unsigned long) sp & 0x3f);
	stack->sp = sp - _ST_STACK_PAD_SIZE;
#elif defined (MD_STACK_GROWS_UP)
	sp = stack->stk_bottom;
	thread = (_st_thread_t *) sp;
	sp = sp + sizeof(_st_thread_t);
	ptds = (void **) sp;
	sp = sp + (ST_KEYS_MAX * sizeof(void *));

	/* Make stack 64-byte aligned */
	if ((unsigned long)sp & 0x3f)
	sp = sp + (0x40 - ((unsigned long)sp & 0x3f));
	stack->sp = sp + _ST_STACK_PAD_SIZE;
#else
#error Unknown OS
#endif

	memset(thread, 0, sizeof(_st_thread_t));
	memset(ptds, 0, ST_KEYS_MAX * sizeof(void *));

	/* Initialize thread */
	thread->private_data = ptds;
	thread->stack = stack;
	thread->start = start;
	thread->arg = arg;
	thread->vp_index = _vp_index;

#ifndef __ia64__
	_ST_INIT_CONTEXT(thread, stack->sp, _st_thread_main);
#else
	_ST_INIT_CONTEXT(thread, stack->sp, stack->bsp, _st_thread_main);
#endif

	/* If thread is joinable, allocate a termination condition variable */
	if (joinable) {
		thread->term = st_cond_new();
		thread->term_mutex = st_mutex_new();
		if (thread->term == NULL) {
			_st_stack_free(thread->stack);
			return NULL;
		}
	}

	/* Make thread runnable */
	atomic_inc(&the_vp(thread->vp_index)._st_active_count);
	_ST_ADD_RUNQ(thread);
	_ST_ADD_THREADQ(thread);

	return thread;
}

int st_get_thread_vp(_st_thread_t* thread)
{
	return thread->vp_index;
}

_st_thread_t *st_thread_get_waked_by(void)
{
	return _ST_CURRENT_THREAD()->debug_waked_by;
}

_st_thread_t *st_thread_create(void *(*start)(void *arg), void *arg,
		int joinable, int stk_size)
{
	return st_thread_create_vp(start, arg, joinable, stk_size, self_index);
}

_st_thread_t *st_thread_create_loop(void *(*start)(void *arg), void *arg,
		int joinable, int stk_size)
{
	static int index = 0;
	int choose_index = atomic_inc(&index);
	choose_index %= nb_worker_pthreads;
	return st_thread_create(start, arg, joinable, stk_size);
}

_st_thread_t *st_thread_self(void)
{
	return _ST_CURRENT_THREAD();
}

static __thread char* local_buf = NULL;
static __thread int len_local_buf = 0;
static __thread int size_local_buf = 0;

static void local_stack_printf(const char *format, ...)
{
	if(!local_buf){
		len_local_buf = 0;
		size_local_buf = 5*1024*1024;
		local_buf = malloc(size_local_buf);
	}
	va_list arglist;
	va_start(arglist, format);
	len_local_buf += vsprintf(local_buf+len_local_buf, format, arglist);
	if(len_local_buf + 1024 > size_local_buf){
		size_local_buf *= 2;
		local_buf = realloc(local_buf, size_local_buf);
	}
	va_end(arglist);
}

/* To be set from debugger */
char _st_program_name[256];
st_printf _st_stack_func = local_stack_printf;
st_print _st_stack_output_func = NULL;

static void eat_new_line(char* buf)
{
	int len = strlen(buf);
	if(buf[len-1] == '\n'){
		buf[len-1] = '\0';
	}
}

/* ARGSUSED */
void _st_show_thread_stack(_st_thread_t * thread)
{
#define MAX_THREAD_STACK 100
	void *buffer[MAX_THREAD_STACK];
	int i = 0, trace_size = 0;
	_st_stack_func("\n");
	trace_size = backtrace(buffer, MAX_THREAD_STACK);
	for (i = 0; i < trace_size; i++) {
		char sys_command[256] = "";
		char function_name[256] = "";
		char file_line[256] = "";
		FILE *pp;
		memset(sys_command, 0, sizeof sys_command);
		memset(function_name, 0, sizeof function_name);
		memset(file_line, 0, sizeof file_line);
		sprintf(sys_command, "addr2line %p -e %s -f", buffer[i],
				_st_program_name);
		if (NULL == (pp = popen(sys_command, "r"))) {
			return;
		}
		char *cret = fgets(function_name, sizeof function_name, pp);
		(void) cret;
		cret = fgets(file_line, sizeof file_line, pp);
		(void) cret;
		eat_new_line(function_name);
		_st_stack_func("#%02d %d.%p in %s() at %s", i, self_index, thread,
				function_name, file_line);
		pclose(pp);
	}
#undef MAX_THREAD_STACK
}

void mutex_callback(_st_mutex_t* mu)
{
	if(mu->owner) {
		_st_stack_func("mutex %p, owner is %p", mu, mu->owner);
		for(_st_clist_t* i = mu->wait_q.next; i != &mu->wait_q; i = i->next){
			_st_thread_t* th = _ST_THREAD_WAITQ_PTR(i);
			_st_stack_func("thread %p is waiting on mutex %p", th, mu);
		}
	}
}

#define ITERATE_FLAG(index) (st_vps[index]._st_iterate_threads_flag)

void _st_iterate_threads(void)
{
	static __thread _st_thread_t *thread = NULL;
	static __thread jmp_buf orig_jb, save_jb;
	_st_clist_t *q;

	if (!ITERATE_FLAG(self_index)) {
		if (thread) {
			_st_stack_func("here is impossible.\n");
			memcpy(thread->context, save_jb, sizeof(jmp_buf));
			MD_LONGJMP(orig_jb, 1);
		}
		return;
	}

	if (thread) {
		memcpy(thread->context, save_jb, sizeof(jmp_buf));
		_st_show_thread_stack(thread);
	} else {
		_st_stack_func("start ####################################################################\n");
		if(self_index == 0) {
			// iterate_all_mutex(&mutex_callback);
		}
		if (MD_SETJMP(orig_jb)) {
			ITERATE_FLAG(self_index) = 0;
			_st_stack_func("\n");
			_st_stack_func("end ##################################################################\n");
			thread = NULL;
			_st_stack_output_func(local_buf);
			free(local_buf); local_buf = NULL;
			return;
		}
		thread = _ST_CURRENT_THREAD();
		_st_show_thread_stack(thread);
	}

	q = thread->tlink.next;
	if (q == &_ST_THREADQ(self_index))
		q = q->next;
	ST_ASSERT(q != &_ST_THREADQ(self_index));
	thread = _ST_THREAD_THREADQ_PTR(q);
	if (thread == _ST_CURRENT_THREAD())
		MD_LONGJMP(orig_jb, 1);
	memcpy(save_jb, thread->context, sizeof(jmp_buf));
	MD_LONGJMP(thread->context, 1);
}

void st_print_threads_stack(const char *exeFile, st_print new_printf)
{
	int i = 0;
	_st_stack_output_func = new_printf;
	for (i = 0; i < nb_worker_pthreads; i++) {
		ITERATE_FLAG(i) = 1;
		interrupt_vp(i);
	}
	strcpy(_st_program_name, exeFile);
}

