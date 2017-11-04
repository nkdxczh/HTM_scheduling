#include "tsx-assert.h"
#include "rtm.h"
#include "rand_r_32.h"
#include "stm/ValueList.hpp"
#include "stm/BitFilter.hpp"
#include "stm/WriteSet.hpp"
#include "stm/UndoLog.hpp"
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <setjmp.h>
#include <unistd.h>
#include <sched.h>

#include <boost/lockfree/fifo.hpp>
#include <boost/lockfree/ringbuffer.hpp>

bool withYield = false;
int percentage = 50;

boost::lockfree::fifo<void * (*) (void *)> lf_q[8];
boost::lockfree::fifo<jmp_buf*> lf_qb[8];

#define FORCE_INLINE __attribute__((always_inline)) inline
#define CACHELINE_BYTES 64
#define CFENCE              __asm__ volatile ("":::"memory")

using stm::ValueList;
using stm::ValueListEntry;
using stm::BitFilter;
using stm::WriteSetEntry;
using stm::WriteSet;
using stm::UndoLogEntry;
using stm::UndoLog;

struct pad_word_t
  {
      volatile uintptr_t val;
      volatile uintptr_t val2;
      char pad[CACHELINE_BYTES-(2*sizeof(uintptr_t))];
  };

struct AddrVal
  {
      void** addr;
      union {
    	  void*  val;
    	  int i;
    	  long l;
    	  char b;
    	  short s;
      } val;
  };

typedef BitFilter<1024> filter_t;
static const uint32_t RING_ELEMENTS = 1024;

pad_word_t timestamp = {0};
pad_word_t last_init     = {0};
pad_word_t last_complete = {0};

filter_t   ring_wf[RING_ELEMENTS] __attribute__((aligned(16)));



#define nop()       __asm__ volatile("nop")
#define pause()		__asm__ ( "pause;" )

#define NUM_STRIPES  1048576

unsigned long long get_real_time() {
	struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);

    return time.tv_sec * 1000000000L + time.tv_nsec;
}


#define SPLIT_SIZE 20

long* array;


#define NO_TX 100000

#define X_CONFLICT 1
#define X_GL_EXISTS  2

int ARRAY_SIZE = 100000;

int NO_READWRITE = 400;

#define RANDOMIZE 0

filter_t write_locks;

volatile int barrier = 1;

//============================================================
#define STATISTICS
int RETRY_SHORT = 1;

//Tx context
struct Tx_Context {
	filter_t rf;
	filter_t wf;
	filter_t p_wf;
	AddrVal* undo_log;
	int undo_i;
	jmp_buf scope;
	int nesting_depth = 0;
	int backoff;
	uintptr_t start_time;
	bool read_only;
#ifdef STATISTICS
	int conflict_abort = 0, capacity_abort = 0, other_abort = 0;
	int htm_conflict_abort = 0, htm_a_conflict_abort =0, htm_capacity_abort = 0, htm_other_abort = 0;
	int sw_c = 0, sw_abort=0;
	int gl_c = 0;
#endif
};

__thread Tx_Context* Self;

#define TM_TX_VAR	Tx_Context* tx = (Tx_Context*)Self;

template <typename T>
FORCE_INLINE T tm_read(T* addr, Tx_Context* tx)
{
	tx->rf.add(addr);
    return *addr;//reinterpret_cast<T>(*addr);
}

template <typename T>
FORCE_INLINE void tm_short_write(T* addr, T val, Tx_Context* tx)
{
	tx->wf.add(addr);
    *addr = val;
}


//TODO support types in the undo log
template <typename T>
FORCE_INLINE void tm_write(T* addr, T val, Tx_Context* tx)
{
	tx->undo_log[tx->undo_i].addr = (void **) addr;
	tx->undo_log[tx->undo_i].val.val = (void *) *addr;
	tx->undo_i++;
	tx->p_wf.add(addr);
    *addr = val;
}

#define TM_READ(var)       tm_read(&var, tx)
#define TM_SHORT_WRITE(var, val) tm_short_write(&var, val, tx)
#define TM_WRITE(var, val) tm_write(&var, val, tx)

FORCE_INLINE void spin64() {
	for (int i = 0; i < 64; i++)
		nop();
}

FORCE_INLINE void thread_init() {
	if (!Self) {
		Self = new Tx_Context();
		Tx_Context* tx = (Tx_Context*)Self;
//		printf("tx pointer %d\n", tx);
		tx->undo_log = (AddrVal*) malloc(10000*sizeof(AddrVal));
		//prefetch memory
		for (int j=0; j<10000; j++) {
			tx->undo_log[j].addr = (void **) 0;
			tx->undo_log[j].val.val = (void *) 0;
		}
	}
}

#define SUPER_GL

#ifdef SUPER_GL

#define SUPER_GL_LIMIT	10

pad_word_t active_tx = {0};
pad_word_t tm_super_glock = {0};

#define SUPER_GL_SHORT_CHK \
	if (tm_super_glock.val) _xabort(X_GL_EXISTS);

#define SUPER_GL_ACQUIRE 													\
	while (tm_super_glock.val) pause();										\
	if (go_super_gl) {														\
		do {																\
			while (tm_super_glock.val) pause();								\
		} while (!__sync_bool_compare_and_swap(&tm_super_glock.val, 0, 1));	\
		while(active_tx.val) pause();										\
		tx->gl_c++;															\
	}																		\
	__sync_fetch_and_add(&active_tx.val, 1);								\


#define SUPER_GL_INIT int other_abort_num = 0;

#define SUPER_GL_TX_BEGIN \
		if (go_super_gl || (status = _xbegin()) == _XBEGIN_STARTED) {

#define SUPER_GL_CHECK \
	if (!go_super_gl) {

#define SUPER_GL_CHECK_END }

#define SUPER_GL_COND \
	if (!status) other_abort_num++;													\
	if (other_abort_num > SUPER_GL_LIMIT) {											\
		/*printf("abort due to others is high %d %d %d\n", threadId, tm_super_glock, go_super_gl);*/\
		go_super_gl = true;															\
		tm_abort(tx);																\
	}

#define SUPER_GL_TX_EXIST \
	__sync_fetch_and_add(&active_tx.val, -1);

#define SUPER_GL_UNLOCK \
	if (go_super_gl) {						\
		go_super_gl = false;				\
		tm_super_glock.val = 0;				\
	}										\
	SUPER_GL_TX_EXIST



#else
#define SUPER_GL_SHORT_CHK
#define SUPER_GL_ACQUIRE
#define SUPER_GL_INIT
#define SUPER_GL_TX_BEGIN \
		if ((status = _xbegin()) == _XBEGIN_STARTED) {

#define SUPER_GL_CHECK
#define SUPER_GL_CHECK_END
#define SUPER_GL_COND
#define SUPER_GL_UNLOCK
#define SUPER_GL_TX_EXIST
#endif


FORCE_INLINE void tm_abort(Tx_Context* tx)
{
	if (!tx->read_only){
	  while (tx->undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
		  tx->undo_i--;
		  *(tx->undo_log[tx->undo_i].addr) = tx->undo_log[tx->undo_i].val.val;
	  }

	  write_locks.differencewith(tx->wf);
	  tx->wf.clear();
	}
	tx->rf.clear();

	for (int i = 0; i < 64*tx->backoff; i++)
		nop();

#ifdef STATISTICS
	tx->sw_abort++;
#endif
	SUPER_GL_TX_EXIST
	//restart the tx
    longjmp(tx->scope, 1);
}

FORCE_INLINE void tm_commit(Tx_Context* tx)
{
	if (tx->read_only) {
		tx->rf.clear();
	} else {
		uintptr_t commit_time;
		do {
			  commit_time = timestamp.val;
			  // get the latest ring entry, return if we've seen it already
			  if (commit_time != tx->start_time) {
				  // wait for the latest entry to be initialized
				  while (last_init.val < commit_time)
					  spin64();

				  // intersect against all new entries
				  for (uintptr_t i = commit_time; i >= tx->start_time + 1; i--)
					  if (ring_wf[i % RING_ELEMENTS].intersect(&tx->rf)) {
						  tm_abort(tx);
					  }

				  // wait for newest entry to be wb-complete before continuing
				  while (last_complete.val < commit_time)
					  spin64();

				  // detect ring rollover: start.ts must not have changed
				  if (timestamp.val > (tx->start_time + RING_ELEMENTS)) {
					  tm_abort(tx);
				  }

				  // ensure this tx doesn't look at this entry again
				  tx->start_time = commit_time;
			  }
		} while (!__sync_bool_compare_and_swap(&timestamp.val, commit_time, commit_time + 1));

		// copy the bits over (use SSE, not indirection)
		ring_wf[(commit_time + 1) % RING_ELEMENTS].fastcopy(&tx->wf);

		// setting this says "the bits are valid"
		last_init.val = commit_time + 1;
		//write through so mark ring entry COMPLETE
		last_complete.val = commit_time + 1;
#ifdef STATISTICS
		tx->sw_c++;
#endif
		write_locks.differencewith(tx->wf);
		CFENCE;
		tx->rf.clear();
		tx->wf.clear();
	}
}

FORCE_INLINE void tm_check_partition(Tx_Context* tx)
{
	//TODO Do partial validation "check_in_flight" (will need partial read signature also)
	uintptr_t my_index = last_init.val;
	if (__builtin_expect(my_index != tx->start_time, true)) { //TODO is false correct!?
		  // intersect against all new entries
		  for (uintptr_t i = my_index; i >= tx->start_time + 1; i--)
			  if (ring_wf[i % RING_ELEMENTS].intersect(&tx->rf)) { //full abort
				  tm_abort(tx);
			  }
		  // wait for newest entry to be writeback-complete before returning
		  while (last_complete.val < my_index)
			  spin64();

		  // detect ring rollover: start.ts must not have changed
		  if (timestamp.val > (tx->start_time + RING_ELEMENTS)) {
			  tm_abort(tx);
		  }

		  // ensure this tx doesn't look at this entry again
		  tx->start_time = my_index;
	}
}


#ifdef	STATISTICS
#define ALL_HTM_STAT															\
	if (status & _XABORT_CODE(X_CONFLICT)) {									\
		tx->htm_a_conflict_abort++;												\
	} else if (status & _XABORT_CAPACITY)										\
		tx->htm_capacity_abort++;												\
	else if (status & _XABORT_CONFLICT){										\
		tx->htm_conflict_abort++;												\
	}																			\
	else																		\
		tx->htm_other_abort++;

#define PART_HTM_STAT															\
	if (status & _XABORT_CAPACITY) 												\
		tx->capacity_abort++; 													\
	else if (status & _XABORT_CONFLICT) 										\
		tx->conflict_abort++; 													\
	else 																		\
		tx->other_abort++;

#else
#define ALL_HTM_STAT
#define PART_HTM_STAT
#endif

//USE THIS ONLY TO TEST YOUR FRAMEWORK QUICKLY WITH NO SHORT TX SUPPORT (NOT FOR REAL RESULSTS)
#define TM_NO_SHORT_BEGIN									\
	{														\
	Tx_Context* tx = (Tx_Context*)Self;          			\
	unsigned status;										\
	int htm_retry = 0;										\
	bool htm_again;											\
	do {													\
		htm_again = false;									\
		{


#define TM_SHORT_BEGIN										\
	{														\
	Tx_Context* tx = (Tx_Context*)Self;          			\
	unsigned status;										\
	int htm_retry = 0;										\
	bool htm_again;											\
	do {													\
		htm_again = false;									\
		if ((status = _xbegin()) == _XBEGIN_STARTED) {		\
			SUPER_GL_SHORT_CHK


#define TM_SHORT_END																\
		if (tx->rf.intersect(&write_locks) || tx->wf.intersect(&write_locks)) {		\
			_xabort(X_CONFLICT);													\
		}																			\
		_xend();																	\
		tx->rf.clear();																\
		tx->wf.clear();																\
	} else {																		\
		ALL_HTM_STAT																\
		htm_retry++;																\
		if (htm_retry < RETRY_SHORT) {												\
			htm_again = true;														\
			continue;																\
		}


//#define GL_1ST_LEVEL

#define PART_RETRY_LIMIT 10
#define TM_RETRY_LIMIT 10

#ifdef GL_1ST_LEVEL

volatile int tm_glock = 0;

#define GL_1ST_LEVEL_VAR	\
	int part_retry = 0;		\
	int tm_retry = 0;		\
	bool lock_owner = false;

#define GL_1ST_LEVEL_BLOCK												\
	if (!lock_owner) {													\
		while (tm_glock) pause();										\
		tm_retry++;														\
		if (tm_retry > TM_RETRY_LIMIT) {								\
			do {														\
				while (tm_glock) pause();								\
			} while (!__sync_bool_compare_and_swap(&tm_glock, 0, 1));	\
			lock_owner = true;											\
		}																\
	}

#define GL_1ST_LEVEL_PRT_INIT	part_retry = 0;

#define GL_1ST_LEVEL_PRT_CHECK										\
	part_retry++;													\
	if (part_retry > PART_RETRY_LIMIT) {							\
		tm_abort(tx);												\
	}

#define GL_1ST_LEVEL_UNLCK				\
	if (lock_owner) {					\
		lock_owner = false;				\
		tm_glock = 0;					\
    }									\

#else

#define GL_1ST_LEVEL_BLOCK
#define GL_1ST_LEVEL_VAR
#define GL_1ST_LEVEL_PRT_INIT
#define GL_1ST_LEVEL_PRT_CHECK
#define GL_1ST_LEVEL_UNLCK
#endif



#define TM_BEGIN		                                    \
	GL_1ST_LEVEL_VAR										\
	bool go_super_gl = false;								\
	tx->backoff=0;											\
	uint32_t abort_flags = _setjmp (tx->scope);				\
	{														\
		SUPER_GL_ACQUIRE 									\
		tx->read_only = true;								\
		tx->undo_i =0;										\
		bool more = true;									\
		GL_1ST_LEVEL_BLOCK									\
		tx->start_time = last_complete.val;


#define TM_PART_BEGIN		                                \
	SUPER_GL_INIT											\
	GL_1ST_LEVEL_PRT_INIT									\
	while (more) {		                                    \
		tx->backoff++;										\
		GL_1ST_LEVEL_PRT_CHECK								\
		more = false;	                                    \
		tx->p_wf.clear();	                                \
		SUPER_GL_TX_BEGIN

#define TM_PART_END					                                									\
		SUPER_GL_CHECK																					\
		if (tx->rf.intersect(&write_locks, tx->wf) || tx->p_wf.intersect(&write_locks, tx->wf)) {		\
			_xabort(X_CONFLICT);					                                					\
		}								                                								\
		else if (tx->undo_i){							                                				\
			tx->read_only = false;														\
			write_locks.unionwith(tx->p_wf);					                        \
		}					                                							\
		_xend();					                                					\
		tx->wf.unionwith(tx->p_wf);			                                			\
		tm_check_partition(tx);															\
		SUPER_GL_CHECK_END																\
	} else {																			\
		if (status & _XABORT_CODE(X_CONFLICT)) { 										\
			tm_abort(tx);																\
		} else 																			\
			more = true; 																\
		SUPER_GL_COND																	\
		PART_HTM_STAT			 														\
		for (int i = 0; i < 64*tx->backoff; i++)										\
			nop();																		\
	}																					\
}


#define TM_END                                  		\
				SUPER_GL_CHECK							\
				tm_commit(tx);                          \
				SUPER_GL_CHECK_END						\
				GL_1ST_LEVEL_UNLCK						\
				SUPER_GL_UNLOCK							\
			  }											\
			}	                                  		\
		} while (htm_again);							\
	}

#define Q_MAX_SIZE 64
#define Q_COUNTS 10


pad_word_t enq_counter[Q_COUNTS] = {{0}};
pad_word_t deq_counter[Q_COUNTS] = {{0}};
pad_word_t order_lock[Q_COUNTS] = {{0}};

int q_top[Q_COUNTS];

class Queue {
	int head, tail;
	int * items;
//	pthread_mutex_t lock;
	pad_word_t lock;
	int size;
public:
	Queue(int capacity=1024) {
	head = 0; tail = 0;
	lock.val = 0;
	items = new int[capacity];
	size = capacity;
	}

	bool enq(int x) {
		bool again = false;
		do{
			again = false;
			while(lock.val) usleep(100);
			if (!__sync_bool_compare_and_swap(&lock.val, 0, 1)) {
				again = true;
				usleep(100);
			}
		} while (again);
//		pthread_mutex_lock(&lock);
		if (tail - head == size) {
//			pthread_mutex_unlock(&lock);
			lock.val = 0;
			return false;
		}
		items[tail % size] = x;
		tail++;
		CFENCE;
		lock.val = 0;
//		pthread_mutex_unlock(&lock);
		return true;
	}
	bool deq(int &x){
		bool again = false;
		do{
			again = false;
			while(lock.val) usleep(100);
			if (!__sync_bool_compare_and_swap(&lock.val, 0, 1)) {
				again = true;
				usleep(100);
			}
		} while (again);
//		pthread_mutex_lock(&lock);
		if (tail == head) {
			lock.val = 0;
//			pthread_mutex_unlock(&lock);
			return false;
		}
		x = items[head % size];
		head++;
		CFENCE;
		lock.val = 0;
//		pthread_mutex_unlock(&lock);
		return true;
	}
	//TODO unsafe?
	bool top(int &x){
		if (tail == head) {
			return false;
		}
		x = items[head % size];
		return true;
	}
	int getSize() {
		return tail - head;
	}
};


Queue qs[Q_COUNTS];

pad_word_t enq_l = {0};
pad_word_t deq_l = {0};

int x[5000];

#define ACCOUT_NUM 8

pad_word_t accounts[ACCOUT_NUM] = {{100000}};

int k[8] = {0};

void * callback11(void *arg) {
	int status;
	double interest;
	unsigned int index = (long)arg;
//	printf("index %d\n", index);
	int more = 0;
	do{
		more = 0;
		if ((status = _xbegin()) == _XBEGIN_STARTED) {
			accounts[index].val += 50;
			_xend();
		} else {
			more = 1;
		}
	} while (more);
}

void * callback22(void *arg) {

}

FORCE_INLINE void acquire_lock(int index, bool isReadOnly = false) {
	bool again = false;
	do{
		again = false;
		if (order_lock[index].val == 2 && isReadOnly) {
			__sync_fetch_and_add(&order_lock[index].val2, 1);
			if (order_lock[index].val != 2) {
				__sync_fetch_and_add(&order_lock[index].val2, -1);
			} else {
				return;
			}
		}
		if (!__sync_bool_compare_and_swap(&order_lock[index].val, 0, isReadOnly? 2: 1)) {
			again = true;
			if (withYield)
				sched_yield();
		} else if (isReadOnly) {
			__sync_fetch_and_add(&order_lock[index].val2, 1);
		}
	} while (again);
}

FORCE_INLINE void release_lock(int index, bool isReadOnly = false) {
	if (isReadOnly) {
		uintptr_t oldVal = __sync_fetch_and_add(&order_lock[index].val2, -1);
		if (oldVal == 1) {
			order_lock[index].val = 0;
		}
	} else {
		order_lock[index].val = 0;
	}
}


//Transactional Process
//Uses Scheduling
//With queues
void *tx_fn_one_q(void *arg) {
	int status;
	double interest;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0;
	while(barrier);

	for (int kk=0; kk<100000; kk++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;

		uintptr_t ticket = __sync_fetch_and_add(&enq_counter[acc1].val, 1);

		while(deq_counter[acc1].val != ticket){
			//if (ticket - deq_counter[acc1].val >= 2)
			if (withYield)
				sched_yield();
		}

		int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
					accounts[acc1].val += 50;
				_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
		__sync_fetch_and_add(&deq_counter[acc1].val, 1);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);

}

//Transactional Process
//Uses Scheduling
//Full Transactions with Queues
void *tx_fn_full_q(void *arg) {
	int status;
	double interest;
	long acc_total;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0;
	while(barrier);

	for (int kk=0; kk<100000; kk++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;
		int acc2 = rand_r_32(&seed) % ACCOUT_NUM;
		if (acc1 == acc2)
			acc2= (acc1 + 1) % ACCOUT_NUM;
		bool type = (rand_r_32(&seed) % 100) < percentage;

		int more = 0;

		uintptr_t ticket1, ticket2;
		uintptr_t ticket1_w, ticket2_w;

		if (acc1 > acc2) {
			acquire_lock(acc1);
			acquire_lock(acc2);
		} else {
			acquire_lock(acc2);
			acquire_lock(acc1);
		}

		ticket1 = enq_counter[acc1].val;
		ticket1_w = enq_counter[acc1].val2;
		if (type)
			enq_counter[acc1].val++;
		else
			enq_counter[acc1].val2++;
		ticket2 = enq_counter[acc2].val;
		ticket2_w = enq_counter[acc2].val2;
		if (type)
			enq_counter[acc2].val++;
		else
			enq_counter[acc2].val2++;

		release_lock(acc1);
		release_lock(acc2);


		bool again;

		do {
			again = false;
			if (type) {
				if (ticket1 != deq_counter[acc1].val || ticket2 != deq_counter[acc2].val
						|| ticket1_w != deq_counter[acc1].val2 || ticket2_w != deq_counter[acc2].val2) {
					again = true;
					if (withYield)
						sched_yield();
				}
			} else {
				if (ticket1 != deq_counter[acc1].val || ticket2 != deq_counter[acc2].val) {
					again = true;
					if (withYield)
						sched_yield();
				}
			}
		} while (again);

		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
				if (type) { //write tx
					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
				} else { //read tx
					acc_total = accounts[acc1].val + accounts[acc2].val;
				}
				_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
		if (type) {
			__sync_fetch_and_add(&deq_counter[acc1].val, 1);
			__sync_fetch_and_add(&deq_counter[acc2].val, 1);
		} else {
			__sync_fetch_and_add(&deq_counter[acc1].val2, 1);
			__sync_fetch_and_add(&deq_counter[acc2].val2, 1);
		}
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);

}

//Transactional Process
//Uses Scheduling
//No Queues
void *tx_fn(void *arg) {
	int status;
	double interest;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0;
	while(barrier);


	for (int kk=0; kk<100000; kk++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;
		int acc2 = rand_r_32(&seed) % ACCOUT_NUM;
		if (acc1 == acc2)
			acc2= (acc1 + 1) % ACCOUT_NUM;

		int type = rand_r_32(&seed) % 2;

		uintptr_t ticket1, ticket2;
		if (acc1 > acc2) {
			acquire_lock(acc1);
			acquire_lock(acc2);
		} else {
			acquire_lock(acc2);
			acquire_lock(acc1);
		}

		ticket1 = enq_counter[acc1].val;
		enq_counter[acc1].val++;
		ticket2 = enq_counter[acc2].val;
		enq_counter[acc2].val++;

		release_lock(acc1);
		release_lock(acc2);

		bool again = false;

		int more = 0;

		do {
			again = false;
			if (ticket1 != deq_counter[acc1].val || ticket2 != deq_counter[acc2].val) {
				again = true;
				if (withYield)
					sched_yield();
			}

		} while (again);

		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {

					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
					_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
		__sync_fetch_and_add(&deq_counter[acc1].val, 1);
		__sync_fetch_and_add(&deq_counter[acc2].val, 1);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);
}



jmp_buf scope[2] = {{0}};
volatile int jmp_ready[2] = {0};

//Transactional Process
//No Scheduling
//No Queues
void *tx_fn2(void *arg) {

	int status;
	double interest;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0;
	while(barrier);

	for (int kk=0; kk<100000; kk++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;
		int acc2 = rand_r_32(&seed) % ACCOUT_NUM;
		if (acc1 == acc2)
			acc2= (acc1 + 1) % ACCOUT_NUM;
		int type = rand_r_32(&seed) % 2;

		int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {

					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
					_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);
	

		int acc1 = rand_r_32(&seed) % (ACCOUT_NUM/2) + (id-1) * 2;
		int acc2 = rand_r_32(&seed) % (ACCOUT_NUM/2) + (id-1) * 2;
		if (acc1 == acc2)
			acc2= (acc1 + 1) % (ACCOUT_NUM/2) + (id-1) * 2;
		int type = rand_r_32(&seed) % 2;

		int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
				if (type) { //write tx
					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
				} else { //read tx
					interest = (accounts[acc1].val + accounts[acc2].val) * 1.05;
				}
				_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);
}

//Transactional Process
//No Scheduling
//Only one transactions (shorter)
void *tx_fn2_one(void *arg) {
	int status;
	double interest;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0;
	while(barrier);

	for (int kk=0; kk<100000; kk++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;

		int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
				accounts[acc1].val += 50;
				_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);
}

//Transactional Process
//No scheduling
//Full transactions
void *tx_fn2_full(void *arg) {
	int status;
	double interest;
	long acc_total;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0;
	while(barrier);

	for (int kk=0; kk<100000; kk++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;
		int acc2 = rand_r_32(&seed) % ACCOUT_NUM;
		if (acc1 == acc2)
			acc2= (acc1 + 1) % ACCOUT_NUM;
		bool type = (rand_r_32(&seed) % 100) < percentage;

		int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
				if (type) { //write tx
					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
				} else { //read tx
					acc_total = accounts[acc1].val + accounts[acc2].val;
				}
				_xend();
			} else {
				more = 1;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);

}

int main(int argc, char* argv[])
{

	//Checks for correct usage of the program (must have at least 3 arguments)
	//Thread Count, Type of Thread and EXP
	if (argc < 4) {
		printf("Usage #_of_threads type exper\n");
		exit(0);
	}

	//Sets the thread attributes
	int thread_count = atoi(argv[1]);
	if (thread_count > 8)
		withYield = true;
	int type = atoi(argv[2]);
	int exp = atoi(argv[3]);

	//Checks if a percentage has been asserted
	if (argc == 5) {
		percentage = atoi(argv[4]);
	}

	//Establishes the accounts for the bank benchmark
	long count = 0;
	for (int o=0;o<ACCOUT_NUM; o++) {
		accounts[o].val = 10000;
		count += accounts[o].val;
	}

	//Allocates an array for the accounts
	array = (long*) malloc(ARRAY_SIZE * sizeof(long));

	//prefetch all memory pages
	long oo=0;
    printf("Inital sum = %d\n", count);

//  pthread_t thread1, thread2, thread3, thread4, thread5, thread6, thread7, thread8;
  pthread_t threads[200];
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
	int i;

	//If type is equal to 1, then implement the transactions without the scheduler 
	if (type == 1) {
		if (exp ==1) {
			for (i=0; i< thread_count; i++) {
				pthread_create(&threads[i], &thread_attr, tx_fn2_one, (void*)(i+1));
			}
		} else if (exp ==2){
			for (i=0; i< thread_count; i++) {
				pthread_create(&threads[i], &thread_attr, tx_fn2, (void*)(i+1));
			}
		} else {
			for (i=0; i< thread_count; i++) {
				pthread_create(&threads[i], &thread_attr, tx_fn2_full, (void*)(i+1));
			}
		}

	//If type is equal to 0, then implement the transactions with the scheduler 
	} else {
		if (exp ==1) {
			for (i=0; i< thread_count; i++) {
				pthread_create(&threads[i], &thread_attr, tx_fn_one_q, (void*)(i+1));
			}
		} else if (exp ==2){
			for (i=0; i< thread_count; i++) {
				pthread_create(&threads[i], &thread_attr, tx_fn, (void*)(i+1));
			}
		} else {
			for (i=0; i< thread_count; i++) {
				pthread_create(&threads[i], &thread_attr, tx_fn_full_q, (void*)(i+1));
			}
		}
	}

	//Gets the time to evaluate the transactions
	unsigned long long time = get_real_time();
	barrier =0;

	//Joins the threads used in the transactions
	for (i=0; i< thread_count; i++) {
		pthread_join(threads[i], NULL);
	}

	//Total time it took to join the threads
	time = get_real_time() - time;
	printf("Total Time = %llu\n", time);

	//Prints the values of each account
	for (int i=0; i<ACCOUT_NUM;i++) {
		printf("account %d = %lld\n", i, accounts[i].val);
	}


	//Sums the values of each account into a total called count
	count = 0;
		for (int o=0;o<ACCOUT_NUM; o++) {
			count += accounts[o].val;
		}

    //prints both values of the queue counter
	for (int i=0; i< Q_COUNTS; i++) {
		printf("%d: enq val %d & val2 %d\n", i, enq_counter[i].val, enq_counter[i].val2);
	}

	//Prints the final sum of all the accounts values
	printf("Final sum = %d\n", count);
	return 0;
}
