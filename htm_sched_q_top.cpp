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

#include <boost/lockfree/fifo.hpp>

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
      char pad[CACHELINE_BYTES-sizeof(uintptr_t)];
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
//00

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

//\
//		tx->undo_log[tx->undo_i].addr = (void **) &(_var);\
//		tx->undo_log[tx->undo_i].val.val = (void *) _val;\
//		tx->undo_i++;\
//		tx->p_wf.add(&(_var));\
//		_var = _val;

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

//USE THIS ONLY TO TEST YOUR FRAMEWORK QUICKLY WITH NO SHORT TX SUPPORT (NOT FOR REAL RESULST
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


//TODO partial abort and check if complete abort is necessary when xabort is called
//TODO Do partial validation "check_in_flight" (will need partial read signature also)
//TODO is false correct in built in expect!?
//TODO Do partial abort with xabort?
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
//============================================================
//int (*p[4]) (int x, int y);
//result = (*p[op]) (i, j);

//#define Q_MAX_SIZE 8
//
//void * (*queue[Q_MAX_SIZE][Q_MAX_SIZE]) (void *);
//jmp_buf * jmp_q[Q_MAX_SIZE][Q_MAX_SIZE];
//pad_word_t queue_s[Q_MAX_SIZE] = {{0}};
//pad_word_t queue_e[Q_MAX_SIZE] = {{0}};
//pad_word_t queue_l[Q_MAX_SIZE] = {{0}};
//
//
//FORCE_INLINE void enqueue(int index, void * (*v) (void *), jmp_buf * jmp_buff) {
////	printf("enq %d\n", index);
//	uintptr_t end = __sync_fetch_and_add(&queue_e[index].val, 1) % Q_MAX_SIZE;
//	queue[index][end] = v;
//	jmp_q[index][end] = jmp_buff;
//}
//
////FORCE_INLINE void dequeue(int index, void * (*&v) (void *), jmp_buf * &jmp_buff) {
////	//TODO do we need to check if queue is empty?
////	uintptr_t start = __sync_fetch_and_add(&queue_s[index].val, 1) % Q_MAX_SIZE;
////	v = queue[index][start];
////	jmp_buff = jmp_q[index][start];
////}
//
//FORCE_INLINE void lock_q_and_deq(int index, void * (*&v) (void *), jmp_buf * &jmp_buff) {
//	if (queue_l[index].val || queue_s[index].val == queue_e[index].val) {
//		v = NULL;
//		return;
//	}
//	if (!__sync_bool_compare_and_swap(&queue_l[index].val, 0, 1)) {
//		v = NULL;
//		return;
//	}
//	//now we are sure no race condition is their
//	if (queue_s[index].val == queue_e[index].val) {
//		queue_l[index].val = 0;
//		v = NULL;
//		return;
//	}
////	printf("deq %d\n", index);
//	uintptr_t start = queue_s[index].val % Q_MAX_SIZE;
//	queue_s[index].val ++;
//	v = queue[index][start];
//	if (v == NULL) printf("something horrible happened!!\n");
//	jmp_buff = jmp_q[index][start];
//}
//
//FORCE_INLINE void unlock_q(int index) {
//	queue_l[index].val = 0;
//}
//
//FORCE_INLINE bool is_q_locked(int index) {
//	return queue_l[index].val;
//}


#define Q_MAX_SIZE 64
#define Q_COUNTS 10

pad_word_t queue[Q_COUNTS][Q_MAX_SIZE];
volatile int queue_s[Q_COUNTS] = {0};
volatile int queue_e[Q_COUNTS] = {0};
pad_word_t queue_dl[Q_COUNTS] = {{0}};
pad_word_t queue_el[Q_COUNTS] = {{0}};

//lock is unlocked explicitly
FORCE_INLINE void enqueue(int index, int v) {
	bool again = false;
	do{
		again = false;
		while(queue_el[index].val);
		if (!__sync_bool_compare_and_swap(&queue_el[index].val, 0, 1)) {
			again = true;
		}
	} while (again);
	int end = queue_e[index] % Q_MAX_SIZE;
	queue[index][end].val = v;
	++queue_e[index];
}

FORCE_INLINE void unlock_q_el(int index) {
	queue_el[index].val = 0;
}

FORCE_INLINE bool is_q_locked(int index) {
	return queue_dl[index].val;
}


FORCE_INLINE int check_q_top(int index) {
	return queue[index][queue_s[index]  % Q_MAX_SIZE].val;
}

//FORCE_INLINE void dequeue(int index) {
//	queue_s[index]++;
//}

FORCE_INLINE void lock_q_dl(int index) {
	bool again = false;
	do{
		again = false;
		while(queue_dl[index].val);
		if (!__sync_bool_compare_and_swap(&queue_dl[index].val, 0, 1)) {
			again = true;
		}
	} while (again);
}

//FORCE_INLINE void unlock_q_dl(int index) {
//	queue_dl[index].val = 0;
//}


FORCE_INLINE void deq_unlock_q_dl(int index) {
	queue_s[index]++;
	queue_dl[index].val = 0;
}
//FORCE_INLINE void dequeue(int index, void * (*&v) (void *), jmp_buf * &jmp_buff) {
//	//TODO do we need to check if queue is empty?
//	uintptr_t start = __sync_fetch_and_add(&queue_s[index].val, 1) % Q_MAX_SIZE;
//	v = queue[index][start];
//	jmp_buff = jmp_q[index][start];
//}

//FORCE_INLINE void lock_q_and_deq(int index, int &v) {
//	if (queue_l[index].val || queue_s[index].val == queue_e[index].val) {
//		v = -1;
//		return;
//	}
//	if (!__sync_bool_compare_and_swap(&queue_l[index].val, 0, 1)) {
//		v = -1;
//		return;
//	}
//	//now we are sure no race condition is their
//	if (queue_s[index].val == queue_e[index].val) {
//		queue_l[index].val = 0;
//		v = -1;
//		return;
//	}
////	printf("deq %d\n", index);
//	uintptr_t start = queue_s[index].val % Q_MAX_SIZE;
//	queue_s[index].val ++;
//	v = queue[index][start];
////	if (v == NULL) printf("something horrible happened!!\n");
////	jmp_buff = jmp_q[index][start];
//}
//
//FORCE_INLINE void unlock_q(int index) {
//	queue_l[index].val = 0;
//}
//
//FORCE_INLINE bool is_q_locked(int index) {
//	return queue_l[index].val;
//}
//
//FORCE_INLINE int q_top(int index) {
//	return queue[index][queue_s[index].val % Q_MAX_SIZE];
//}



int x[5000];

#define ACCOUT_NUM 8

pad_word_t accounts[ACCOUT_NUM] = {{100000}};

//int c1 = 0;
//void * callback1(void *arg) {
//	int id = (long)arg;
//	printf("in callback!!!\n");
//	enqueue(id, callback1);
//	void * (*ret) (void*);
//	lock_q_and_deq(id, ret);
//	unlock_q(id);
//	if (ret) {
//		ret(arg);
//	}
//
//	c1++;
//	if (c1< 10000)
//		callback1(arg);
//}

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
//			if (status & _XABORT_CONFLICT)
//				conf++;//printf("1 abort reason CONFLICT\n");
//			else if (status & _XABORT_CAPACITY)
//				cap++;//printf("1 abort reason CAPACITY\n");
//			else
//				other++;//printf("1 abort reason %d\n", status);
		}
	} while (more);


//	int status;
//	double interest;
//	unsigned int seed = (long)arg;
//	int id = seed;
////	printf("in callback %d \n", id);
//	int conf=0, cap = 0, other = 0;
//
//	int acc1 = rand_r_32(&seed) % (ACCOUT_NUM/2) + (id-1) * 2;
//	int acc2 = rand_r_32(&seed) % (ACCOUT_NUM/2) + (id-1) * 2;
//	if (acc1 == acc2)
//		acc2= (acc1 + 1) % (ACCOUT_NUM/2) + (id-1) * 2;
//	int type = rand_r_32(&seed) % 2;
//
//	int more = 0;
//	do{
//		more = 0;
//		if ((status = _xbegin()) == _XBEGIN_STARTED) {
//			if (type) { //write tx
//				accounts[acc1].val += 50;
//				accounts[acc2].val -= 50;
//			} else { //read tx
//				interest = (accounts[acc1].val + accounts[acc2].val) * 1.05;
//			}
//			_xend();
//		} else {
//			more = 1;
//			if (status & _XABORT_CONFLICT)
//				conf++;//printf("1 abort reason CONFLICT\n");
//			else if (status & _XABORT_CAPACITY)
//				cap++;//printf("1 abort reason CAPACITY\n");
//			else
//				other++;//printf("1 abort reason %d\n", status);
//		}
//	} while (more);

//	jmp_buf scope;
//	uint32_t abort_flags = _setjmp(scope);
//	int id = (long)arg;
//	printf("in callback!!!\n");
//	enqueue(id, callback11);
//	void * (*ret) (void*);
//	lock_q_and_deq(id, ret);
//	unlock_q(id);
//	if (ret) {
//		ret(arg);
//	}
//
//	c1++;
//	if (c1< 10000)
//		callback1(arg);
}

void * callback22(void *arg) {
//	uint32_t abort_flags = setjmp (tx->scope);
//	longjmp(scope, 1);
//	jmp_buf scope;
//
//	int id = (long)arg;
//	printf("in callback!!!\n");
//	enqueue(id, callback1);
//	void * (*ret) (void*);
//	lock_q_and_deq(id, ret);
//	unlock_q(id);
//	if (ret) {
//		ret(arg);
//	}
//
//	c1++;
//	if (c1< 10000)
//		callback1(arg);
}

int kk[8];
int accc1[8];
int accc2[8];
int idd[8];
void *tx_fn(void *arg) {
	int status;
	double interest;
	unsigned int seed = (long)arg;
	int id = seed;
	int conf=0, cap = 0, other = 0;
	while(barrier);

	idd[id-1] = id;
	for (kk[id-1]=0; kk[id-1]<1000; kk[id-1]++){
		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;
		int acc2 = rand_r_32(&seed) % ACCOUT_NUM;
		if (acc1 == acc2)
			acc2= (acc1 + 1) % ACCOUT_NUM;

		accc1[id-1] = acc1;
		accc2[id-1] = acc2;

		int type = rand_r_32(&seed) % 2;

		if (acc1 < acc2) {
			enqueue(acc1, id);
			enqueue(acc2, id);
			unlock_q_el(acc1);
			unlock_q_el(acc2);
//			printf("enqueue %d %d\n", acc1,acc2);
		} else {
			enqueue(acc2, id);
			enqueue(acc1, id);
			unlock_q_el(acc2);
			unlock_q_el(acc1);
//			printf("enqueue %d %d\n", acc2,acc1);
		}

//		bool again;

		printf("%d waiting for %d and %d\n", id, acc1,acc2);
		while(check_q_top(acc1) != id && check_q_top(acc2) != id);


//		do {
//			again = false;
////			printf("waiting for %d and %d\n", acc1, acc2);
//			while (is_q_locked(acc1) && is_q_locked(acc2));
////			printf("waiting for %d and %d OVER\n", acc1, acc2);
//			if (check_q_top(acc1) == id && check_q_top(acc2) == id) {
//				if (acc1 < acc2) {
//					lock_q_dl(acc1);
//					lock_q_dl(acc2);
//				} else {
//					lock_q_dl(acc2);
//					lock_q_dl(acc1);
//				}
//				printf("got locks for %d and %d\n", acc1, acc2);
//			} else {
//				again = true;
//			}
//		} while (again);
//		printf("%d executing %d for %d\n", kk, acc1, acc2);

		int more = 0;
		int retry = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
//				if (type) { //write tx
					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
//				} else { //read tx
//					interest = (accounts[acc1].val + accounts[acc2].val) * 1.05;
//				}
//				accounts[acc1].val += 50;
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
			retry++;
			if (retry > 20) {
				printf("retrying too much!\n");
			}
		} while (more);
		deq_unlock_q_dl(acc1);
		deq_unlock_q_dl(acc2);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);

//	callback1(arg);
//	int status;
//	double interest;
//	unsigned int seed = (long)arg;
//	int conf=0, cap = 0, other = 0;
//	while(barrier);
//
//	for (int kk=0; kk<1000000; kk++){
////		fifo1.enqueue(callback1);
////		void * (*ret) (void*);
////		fifo1.dequeue(ret);
//		int acc1 = rand_r_32(&seed) % ACCOUT_NUM;
//		int acc2 = rand_r_32(&seed) % ACCOUT_NUM;
//		if (acc1 == acc2)
//			acc2= (acc1 + 1) % ACCOUT_NUM;
//		int type = rand_r_32(&seed) % 2;
//
//		int more = 0;
//		do{
//			more = 0;
//			if ((status = _xbegin()) == _XBEGIN_STARTED) {
//				if (type) { //write tx
//					accounts[acc1].val += 50;
//					accounts[acc2].val -= 50;
//				} else { //read tx
//					interest = (accounts[acc1].val + accounts[acc2].val) * 1.05;
//				}
//				_xend();
//			} else {
//				more = 1;
//				if (status & _XABORT_CONFLICT)
//					conf++;//printf("1 abort reason CONFLICT\n");
//				else if (status & _XABORT_CAPACITY)
//					cap++;//printf("1 abort reason CAPACITY\n");
//				else
//					other++;//printf("1 abort reason %d\n", status);
//			}
//		} while (more);
//	}
//	printf("1 abort reason CONFLICT = %d\n", conf);
//	printf("1 abort reason CAPACITY = %d\n", cap);
//	printf("1 abort reason OTHER = %d\n", other);
}



jmp_buf scope[2] = {{0}};
volatile int jmp_ready[2] = {0};

void *tx_fn2(void *arg) {

	int status;
	double interest;
	unsigned int seed = (long)arg;
	int id = seed;
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
//				if (type) { //write tx
					accounts[acc1].val += 50;
					accounts[acc2].val -= 50;
//				} else { //read tx
//					interest = (accounts[acc1].val + accounts[acc2].val) * 1.05;
//				}
//				accounts[acc1].val += 50;
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


	//	callback1(arg);
//	int status;
//	double interest;
//	unsigned int seed = (long)arg;
//	int id = seed;
//	int conf=0, cap = 0, other = 0;
//	while(barrier);
//
//	for (int k=0; k<100;k++) {
//		uint32_t abort_flags = _setjmp(scope[id-1]);
//		jmp_ready[id-1] = 1;
//		if (!abort_flags) {
//			printf("this is %d, jumping now\n", id);
//			int other = id == 1? 1: 0;//0:1;
//			while (!jmp_ready[other]);
//			jmp_ready[other] = 0;
//			longjmp(scope[other], 10);
//		}
//	}

//	for (int k=0; k<100;k++) {
//		uint32_t abort_flags = _setjmp(scope[id-1]);
//		if (!abort_flags) {
////			printf("jmp flag=%d k=%d\n", abort_flags, k);
//			int acc = rand_r_32(&seed) % ACCOUT_NUM;
//			enqueue(acc, callback11, &(scope[id-1]));
////			printf("enq %d \n", acc);
//			bool found = false;
//			do{
//				//find a queue to execute it
////				int i = id ==1?1:0;
//				for (int i=0; i<2; i++) {
//					if (!is_q_locked(i)) {
//						void * (*ret) (void*);
//						jmp_buf * following;
//						lock_q_and_deq(i, ret, following);
////						printf("dec %d \n", ret);
//						if (ret) {
////								printf("going to call %d %d", id, i);
//							ret((void*)i);
//							unlock_q(i);
//							longjmp(*following, 10);
//							//anyway it will not be executed but to show the logic
//							found = true;
//						}
//					}
//				}
//			} while(!found);
//		}
//	}
	/*for (int kk=0; kk<1000000; kk++){
//		fifo1.enqueue(callback1);
//		void * (*ret) (void*);
//		fifo1.dequeue(ret);
		enqueue(id, callback1);
		void * (*ret) (void*);
		dequeue(id, ret);

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

*/
//	thread_init();
//
//	int notRetried = 0;
//
//	unsigned int seed = (long)arg;
//	int threadId = seed;
//	int tx_limit = 0;
//	int loc[1024];
//
//	while(barrier);
//
//
//	unsigned long long time = get_real_time();
//	bool do_gl_test = true;
//
//	while (tx_limit < NO_TX) {
//
//		int tx_len = RANDOMIZE? (rand_r_32(&seed) % NO_READWRITE) + 1 : NO_READWRITE;
//		tx_limit++;
//		for (int i = 0; i < tx_len; ++i) {
////			loc[i] = tx_len*seed + i;//rand_r_32(&seed) % ARRAY_SIZE;
//			loc[i] = rand_r_32(&seed) % ARRAY_SIZE;
//		}
//
////		TM_SHORT_BEGIN
////				for (int i= 0; i < tx_len; i+=2) {
////					if (TM_READ(array[loc[i]]) > 50) {
////						TM_SHORT_WRITE(array[loc[i+1]], array[loc[i+1]] + 50);
////						TM_SHORT_WRITE(array[loc[i]], array[loc[i]] - 50);
////					}
////				}
////		TM_SHORT_END
//			//failed in all HTM, retry with partitioning
//		TM_NO_SHORT_BEGIN
//
//		TM_BEGIN
//
//				int initial = 0;
//				do{
//					int limit = initial + SPLIT_SIZE > tx_len? tx_len: initial + SPLIT_SIZE;
//					TM_PART_BEGIN
//						if (do_gl_test) {
//							do_gl_test = false;
//							malloc(100000);
////							printf("in=====================================\n");
//						}
//						int i;
//						for (i=initial; i < limit; i+=2) {
//							if (TM_READ(array[loc[i]]) > 50) {
//								TM_WRITE(array[loc[i+1]], array[loc[i+1]] + 50);
//								TM_WRITE(array[loc[i]], array[loc[i]] - 50);
//							}
//						}
//					TM_PART_END
//					initial = limit;
//				} while (initial!=tx_len);
//		TM_END
//	} //num of tx loop
//	time = get_real_time() - time;
//	printf("Time = %llu\n", time);
//	TM_TX_VAR
//	printf("conflict = %d, capacity=%d, other=%d\n", tx->conflict_abort, tx->capacity_abort, tx->other_abort);
//	printf("HTM conflict = %d, our conflict = %d, capacity=%d, other=%d\n", tx->htm_conflict_abort, tx->htm_a_conflict_abort, tx->htm_capacity_abort, tx->htm_other_abort);
//	printf("Last complete = %d, SW count = %d, abort count = %d\n", last_complete.val, tx->sw_c, tx->sw_abort);
//	printf("global locking = %d\n", tx->gl_c);
//	printf("seed final = %d\n", seed);
//	write_locks.print();
}

//_XABORT_CONFLICT
//_XABORT_CAPACITY

int main(int argc, char* argv[])
{
//	if (argc < 4) {
//		printf("Usage #_of_randomly_accessed_element  Array_size #_of_short_tx_retrials\n");
//		exit(0);
//	}
//	NO_READWRITE = atoi(argv[1]);
//	ARRAY_SIZE = atoi(argv[2]);
//	RETRY_SHORT = atoi(argv[3]);

//	char* huge_buffer = (char*) malloc(5000000);
//
//	huge_buffer += 100;
//
//	array = (long*) huge_buffer;
	array = (long*) malloc(ARRAY_SIZE * sizeof(long));
//	printf("%d %d\n", sizeof(int), sizeof(long));
	//prefetch all memory pages
	long oo=0;
	for (int i = 0 ; i< ARRAY_SIZE; i++) {
		array[i]=1000;
	}
        for (int i = 0 ; i< ARRAY_SIZE; i++) {
                oo += array[i];
        }
        printf("Inital sum = %d\n", oo);


//    	AddrVal* undo_log;
//    	undo_log = (AddrVal*) malloc(10000*sizeof(AddrVal));
//    	int undo_i=0;
//
//		undo_log[undo_i].addr = (void **) &(array[0]);
//		undo_log[undo_i].val.val = (void *) (array[0]);
//		undo_i++;
//		undo_log[undo_i].addr = (void **) &(array[1]);
//		undo_log[undo_i].val.val = (void *) (array[1]);
//		undo_i++;
//		undo_log[undo_i].addr = (void **) &(array[10]);
//		undo_log[undo_i].val.val = (void *) (array[10]);
//		undo_i++;
//
//
//		  while (undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
//				  undo_i--;
//				  printf("o = %d\n", undo_log[undo_i].val);
//				  (*((int*)undo_log[undo_i].addr)) = undo_log[undo_i].val.i;
////							  *(undo_log[undo_i-2]) = undo_log[undo_i-1];
////							  undo_i -= 2;
//			  }
//		  for (int i =0; i<20 ; i++) {
//			  printf("%d =  %d \n", i, array[i]);
//		  }


  pthread_t thread1, thread2, thread3, thread4, thread5, thread6, thread7, thread8;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
//	int x[100000] = {1};
	int i;

	pthread_create(&thread1, &thread_attr, tx_fn, (void*)1);
	pthread_create(&thread2, &thread_attr, tx_fn, (void*)2/*4968147*/);
	pthread_create(&thread3, &thread_attr, tx_fn, (void*)3);
	pthread_create(&thread4, &thread_attr, tx_fn, (void*)4);
//	pthread_create(&thread5, &thread_attr, tx_fn, (void*)5);
//	pthread_create(&thread6, &thread_attr, tx_fn, (void*)6/*4968147*/);
//	pthread_create(&thread7, &thread_attr, tx_fn, (void*)7);
//	pthread_create(&thread8, &thread_attr, tx_fn, (void*)8);

//	pthread_create(&thread1, &thread_attr, tx_fn2, (void*)1);
//	pthread_create(&thread2, &thread_attr, tx_fn2, (void*)2/*4968147*/);
//	pthread_create(&thread3, &thread_attr, tx_fn2, (void*)3);
//	pthread_create(&thread4, &thread_attr, tx_fn2, (void*)4);
//	pthread_create(&thread5, &thread_attr, tx_fn2, (void*)5);
//	pthread_create(&thread6, &thread_attr, tx_fn2, (void*)6/*4968147*/);
//	pthread_create(&thread7, &thread_attr, tx_fn2, (void*)7);
//	pthread_create(&thread8, &thread_attr, tx_fn2, (void*)8);


//	pthread_create(&thread1, &thread_attr, tx_fn, (void*)1);
//	pthread_create(&thread2, &thread_attr, tx_fn, (void*)2);
//	pthread_create(&thread3, &thread_attr, tx_fn, (void*)3);
//	pthread_create(&thread4, &thread_attr, tx_fn, (void*)4);


	unsigned long long time = get_real_time();
	barrier =0;

	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	pthread_join(thread3, NULL);
	pthread_join(thread4, NULL);
//	pthread_join(thread5, NULL);
//	pthread_join(thread6, NULL);
//	pthread_join(thread7, NULL);
//	pthread_join(thread8, NULL);
	time = get_real_time() - time;
	printf("Total Time = %llu\n", time);
	for (int i=0; i<ACCOUT_NUM;i++) {
		printf("account %d = %lld\n", i, accounts[i].val);
	}
	oo=0;
	for (int i = 0 ; i< ARRAY_SIZE; i++) {
                oo += array[i];
//                printf("I%d = %d  ", i, array[i]);
//                if (i%10 == 0)
//                	printf("\n");
        }
	printf("Final sum = %d\n", oo);
/*
int kk;

for (kk=0; kk<100; kk++){
	if ((status = _xbegin()) == _XBEGIN_STARTED) {
	//	tsx_assert(foo);
	//	x[5] = 10;
	//	int y[1000] = {0};
	//	foo = 1;
	//	_xabort(0);
		//4760
		for (i=0; i<4000;i++) {
			x[i]=i;
		}
		_xend();
		printf("1 Worked!\n");
	}
	if (status == _XABORT_CONFLICT)
		printf("1 abort reason CONFLICT\n");
	else if (status == _XABORT_CAPACITY)
		printf("1 abort reason CAPACITY\n");
	else if (status != _XBEGIN_STARTED)
		printf("1 abort reason %d\n", status);

	printf("i final value = %d\n", i);
	//printf ("foo =%d, x=%d\n", foo, x[10]);
}*/
	return 0;
}
