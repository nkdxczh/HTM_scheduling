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

//#define TM_READ(var)       tm_read(&var, tx)
//#define TM_SHORT_WRITE(var, val) tm_short_write(&var, val, tx)
//#define TM_WRITE(var, val) tm_write(&var, val, tx)


#define TM_READ(var)       var
#define TM_WRITE(var, val) var = val



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

#define  NUM_ITEMS  50
//1000; // Correct overall # of items: 100,000
#define  NUM_WAREHOUSES  20
#define  NUM_DISTRICTS  50
//10000; //4;
#define  NUM_CUSTOMERS_PER_D  50
//300; //30;
#define  NUM_ORDERS_PER_D  50

#define  NUM_LINES_PER_O  5
//3000; //30;
#define  MAX_CUSTOMER_NAMES  50

#define  NUM_HISTORY_PER_C 10
//1000; // 10;

#define  DEFAULT_LENGTH  6

uint32_t gseed = 459;

typedef struct customer{
	 int C_FIRST;
	 int C_MIDDLE;
	 int C_LAST;
	 int C_STREET_1;
	 int C_STREET_2;
	 int C_CITY;
	 int C_STATE;
	 int C_ZIP;
	 int C_PHONE;
	 int C_SINCE;
	 int C_CREDIT;
	 double C_CREDIT_LIM;
	 double C_DISCOUNT;
	 double C_BALANCE;
	 double C_YTD_PAYMENT;
	 int C_PAYMENT_CNT;
	 int C_DELIVERY_CNT;
	 int C_DATA;
	 customer() : C_CREDIT_LIM(50000.0), C_DISCOUNT(10.0), C_YTD_PAYMENT(10.0), C_PAYMENT_CNT(1), C_DELIVERY_CNT(0)
	     {
		 	int r = rand_r_32(&gseed) % 100;
		 	C_CREDIT = r < 90? 1: 2;
			C_DISCOUNT = (double)((rand_r_32(&gseed)%5000) * 0.0001);
	     }
} customer_t;

typedef struct district {
	int  D_NAME;
	int  D_STREET_1;
	int  D_STREET_2;
	int  D_CITY;
	int  D_STATE;
	int  D_ZIP;

	 double D_TAX;
	 double D_YTD;
	 int D_NEXT_O_ID;

	 district(): D_YTD(30000.0), D_NEXT_O_ID(3001)
	 {
		 D_TAX = ((rand_r_32(&gseed)%2000) * 0.0001);
	 }
} district_t;


typedef struct history {
	int H_C_ID;
	 int H_C_D_ID;
	 int H_C_W_ID;
	 int H_D_ID;
	 int H_W_ID;
	 int H_DATE;
	 double H_AMOUNT;
	 int H_DATA;
	 history(int c_id, int d_id): H_AMOUNT(10)
	 {
		 H_W_ID = rand_r_32(&gseed)%100;
		 H_D_ID = d_id;
		 H_C_ID = c_id;
	 }
	 history(): H_AMOUNT(10)
	 	 {
	 		 H_W_ID = rand_r_32(&gseed)%100;
	 		 H_D_ID = rand_r_32(&gseed)% NUM_DISTRICTS;
	 		 H_C_ID = rand_r_32(&gseed)% NUM_CUSTOMERS_PER_D;
	 	 }
} history_t;

typedef struct item {
	int I_IM_ID;
	 int I_NAME;
	 float I_PRICE;
	 int I_DATA;
	 item()
	 {
		 I_PRICE = (float) (rand_r_32(&gseed)%10000);
	 }
} item_t;

typedef struct order {
	int O_C_ID;
	 int O_ENTRY_D;
	 int O_CARRIER_ID;
	 int O_OL_CNT;
	 bool O_ALL_LOCAL;
	 order(): O_ALL_LOCAL(true)
	 {
		 O_C_ID = rand_r_32(&gseed)%100;
		 O_OL_CNT = 5 + rand_r_32(&gseed)%11;
	 }
} order_t;

typedef struct order_line {
	int OL_I_ID;
	 int OL_SUPPLY_W_ID;
	 int OL_DELIVERY_D;
	 int OL_QUANTITY;
	 int OL_AMOUNT;
	 int OL_DIST_INFO;
	 order_line() : OL_QUANTITY(5)
	 {
		 OL_I_ID = 1 + rand_r_32(&gseed)%100000;
		OL_SUPPLY_W_ID = rand_r_32(&gseed)%1000;
		int a = rand_r_32(&gseed)%3000;
		if (a < 2101)
			OL_AMOUNT = 0;
		else {
			OL_AMOUNT = (1 + rand_r_32(&gseed)%(999999));
		}
	 }
} order_line_t;

typedef struct stock {
	int S_QUANTITY;
	 int S_DIST_01;
	 int S_DIST_02;
	 int S_DIST_03;
	 int S_DIST_04;
	 int S_DIST_05;
	 int S_DIST_06;
	 int S_DIST_07;
	 int S_DIST_08;
	 int S_DIST_09;
	 int S_DIST_10;
	 int S_YTD;
	 int S_ORDER_CNT;
	 int S_REMOTE_CNT;
	 int S_DATA;

	 stock() : S_YTD(0), S_ORDER_CNT(0), S_REMOTE_CNT(0)
	 {
		 S_QUANTITY = 10 + rand_r_32(&gseed) %(91);
		 if (rand_r_32(&gseed)%(100) < 10) {
			S_DATA = 1000 + rand_r_32(&gseed)%100;//orginal
		} else {
			S_DATA = rand_r_32(&gseed)%(100);
		}
	 }
} stock_t;

typedef struct warehouse {
	int W_NAME;
	 int W_STREET_1;
	 int  W_STREET_2;
	 int  W_CITY;
	 int W_STATE;
	 int W_ZIP;
	 float W_TAX;
	 float W_YTD;
	 warehouse(): W_YTD(300000.0)
	 {
		W_TAX = (rand_r_32(&gseed)%(2000) * 0.0001);
	 }
} warehouse_t;



item_t items[NUM_ITEMS];

warehouse_t warehouses[NUM_WAREHOUSES];

stock_t stocks[NUM_WAREHOUSES][NUM_ITEMS];

district_t districts[NUM_WAREHOUSES][NUM_DISTRICTS];

customer_t customers[NUM_WAREHOUSES][NUM_DISTRICTS][NUM_CUSTOMERS_PER_D];

history_t histories[NUM_WAREHOUSES][NUM_DISTRICTS][NUM_CUSTOMERS_PER_D][NUM_HISTORY_PER_C];

order_t orders[NUM_WAREHOUSES][NUM_ORDERS_PER_D];

order_line_t order_lines[NUM_WAREHOUSES][NUM_ORDERS_PER_D][NUM_LINES_PER_O];


/*** Run a bunch of increment transactions */
void *tx_fn_tpcc(void *arg) {
	int status;
	double interest;
	long acc_total;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0, cantfit=0, gl_count=0;
	while(barrier);


	for (int kk=0; kk<100000; kk++){
	    uint32_t val = rand_r_32(&seed) % 256;
	    uint32_t act = rand_r_32(&seed) % 100;
		int w_id = rand_r_32(&seed) %(NUM_WAREHOUSES);
		int o_id = rand_r_32(&seed) %(NUM_ORDERS_PER_D);
		int c_id = rand_r_32(&seed) %(NUM_CUSTOMERS_PER_D);
		int d_id = rand_r_32(&seed) %(NUM_DISTRICTS);
		int l_id = rand_r_32(&seed) %(NUM_LINES_PER_O);
		int orderLineCount = rand_r_32(&seed) % NUM_LINES_PER_O;
		int crtdate = rand_r_32(&seed) % 50000;
		float h_amount = (float)(rand_r_32(&seed) %(500000) * 0.01);

	    int retries = 0;
	    int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
				if(act < 4) {
			//		(READ_ONLY_TX);
			//		(TX_ORDER_STATUS);
					float dummy;
						dummy = TM_READ(warehouses[w_id].W_YTD);
						dummy = TM_READ(orders[w_id][o_id].O_CARRIER_ID);
						dummy = TM_READ(customers[w_id][d_id][c_id].C_BALANCE);

						float olsum = (float)0;
						int i = 1;

						while (i < orderLineCount) {
							olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
							dummy = TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
							i += 1;
						}
				} else if (act < 8) {
			//		(READ_ONLY_TX);
			//		(TX_STOCKLEVEL);
						//TODO I believe this method is wrong!!
					double dummy;

						dummy= TM_READ(warehouses[w_id].W_YTD);


						dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);



						dummy=TM_READ(order_lines[w_id][o_id][l_id].OL_QUANTITY);



						int k = 1;
						while (k <= 10) {
							int s_id = rand_r_32(&seed) %(NUM_ITEMS);
							dummy=TM_READ(stocks[w_id][s_id].S_QUANTITY);

							k += 1;
						}
				} else if (act < 12) {
			//		(READ_WRITE_TX);
			//		(TX_DELIVERY);
					double dummy;
						for (int d_id = 0; d_id < NUM_DISTRICTS; d_id++) {
							dummy=TM_READ(warehouses[w_id].W_YTD);
							dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);

							float olsum = (float)0;

							int i = 1;
							while (i < orderLineCount) {
								olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
								dummy=TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
								i += 1;
							}
							TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) + olsum);
							TM_WRITE(customers[w_id][d_id][c_id].C_DELIVERY_CNT, TM_READ(customers[w_id][d_id][c_id].C_DELIVERY_CNT) + 1);
						}

				} else if (act < 55) {
			//		(READ_WRITE_TX);
			//		(TX_PAYMENT);


						// Open Wairehouse Table


						TM_WRITE(warehouses[w_id].W_YTD, TM_READ(warehouses[w_id].W_YTD) + h_amount);

						// In DISTRICT table

						TM_WRITE(districts[w_id][d_id].D_YTD, TM_READ(districts[w_id][d_id].D_YTD) + h_amount);

						TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) - h_amount);
						TM_WRITE(customers[w_id][d_id][c_id].C_YTD_PAYMENT, TM_READ(customers[w_id][d_id][c_id].C_YTD_PAYMENT) + h_amount);
						TM_WRITE(customers[w_id][d_id][c_id].C_PAYMENT_CNT, TM_READ(customers[w_id][d_id][c_id].C_PAYMENT_CNT) + 1);
				} else {
			//			(READ_WRITE_TX);
			//			(TX_NEWORDER);


						double D_TAX = TM_READ(districts[w_id][d_id].D_TAX);
						int o_id = TM_READ(districts[w_id][d_id].D_NEXT_O_ID) %(NUM_ORDERS_PER_D);
						TM_WRITE(districts[w_id][d_id].D_NEXT_O_ID, o_id + 1);


						double C_DISCOUNT = TM_READ(customers[w_id][d_id][c_id].C_DISCOUNT);
						int C_LAST = TM_READ(customers[w_id][d_id][c_id].C_LAST);
						int C_CREDIT = TM_READ(customers[w_id][d_id][c_id].C_CREDIT);

						//TODO we don't have create, so simulate it with one write in the array in random index
						// Create entries in ORDER and NEW-ORDER

			//			order_t  norder;// = (order_t*)TM_ALLOC(sizeof(order_t));
			//			norder.O_C_ID = c_id;
			//			norder.O_CARRIER_ID = rand_r_32(seed) %(10); //1000
			//			norder.O_ALL_LOCAL = true;

						TM_WRITE(orders[w_id][o_id].O_CARRIER_ID, rand_r_32(&seed) %(10));

						int items_count = 1 + rand_r_32(&seed) %(5);

						for (int i = 0; i <= items_count; i++) {
							int i_id = rand_r_32(&seed) %(NUM_ITEMS);

							float I_PRICE = TM_READ(items[i_id].I_PRICE);
							int I_NAME = TM_READ(items[i_id].I_NAME);
							int I_DATA = TM_READ(items[i_id].I_DATA);



							//order_line_t orderLine;// = (order_line_t*)TM_ALLOC(sizeof(order_line_t));

							int l_id = rand_r_32(&seed) %(NUM_LINES_PER_O);

			//				orderLine.OL_QUANTITY = rand_r_32(seed) %(1000);
			//				orderLine.OL_I_ID = i_id;
			//				orderLine.OL_SUPPLY_W_ID = w_id;
			//				orderLine.OL_AMOUNT = (int)(orderLine.OL_QUANTITY  * I_PRICE);
			//				orderLine.OL_DELIVERY_D = 0;
			//				orderLine.OL_DIST_INFO = d_id;




							TM_WRITE(order_lines[w_id][o_id][l_id].OL_QUANTITY, rand_r_32(&seed) %(1000));

						}


				}
				if (tm_super_glock.val) _xabort(X_GL_EXISTS);
				_xend();
			} else {
				more = 1;
				retries++;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more && retries < 5);
		if (retries == 5) {
			gl_count++;
			do {
				while (tm_super_glock.val) pause();
			} while (!__sync_bool_compare_and_swap(&tm_super_glock.val, 0, 1));



			if(act < 4) {
		//		(READ_ONLY_TX);
		//		(TX_ORDER_STATUS);
				float dummy;
					dummy = TM_READ(warehouses[w_id].W_YTD);
					dummy = TM_READ(orders[w_id][o_id].O_CARRIER_ID);
					dummy = TM_READ(customers[w_id][d_id][c_id].C_BALANCE);

					float olsum = (float)0;
					int i = 1;

					while (i < orderLineCount) {
						olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
						dummy = TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
						i += 1;
					}
			}
			else if (act < 8) {
		//		(READ_ONLY_TX);
		//		(TX_STOCKLEVEL);
					//TODO I believe this method is wrong!!
				double dummy;

					dummy= TM_READ(warehouses[w_id].W_YTD);


					dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);



					dummy=TM_READ(order_lines[w_id][o_id][l_id].OL_QUANTITY);



					int k = 1;
					while (k <= 10) {
						int s_id = rand_r_32(&seed) %(NUM_ITEMS);
						dummy=TM_READ(stocks[w_id][s_id].S_QUANTITY);

						k += 1;
					}
			}
			else if (act < 12) {
		//		(READ_WRITE_TX);
		//		(TX_DELIVERY);
				double dummy;
					for (int d_id = 0; d_id < NUM_DISTRICTS; d_id++) {
						dummy=TM_READ(warehouses[w_id].W_YTD);
						dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);

						float olsum = (float)0;

						int i = 1;
						while (i < orderLineCount) {
							olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
							dummy=TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
							i += 1;
						}
						TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) + olsum);
						TM_WRITE(customers[w_id][d_id][c_id].C_DELIVERY_CNT, TM_READ(customers[w_id][d_id][c_id].C_DELIVERY_CNT) + 1);
					}

			}
			else if (act < 55) {
		//		(READ_WRITE_TX);
		//		(TX_PAYMENT);


					// Open Wairehouse Table


					TM_WRITE(warehouses[w_id].W_YTD, TM_READ(warehouses[w_id].W_YTD) + h_amount);

					// In DISTRICT table

					TM_WRITE(districts[w_id][d_id].D_YTD, TM_READ(districts[w_id][d_id].D_YTD) + h_amount);

					TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) - h_amount);
					TM_WRITE(customers[w_id][d_id][c_id].C_YTD_PAYMENT, TM_READ(customers[w_id][d_id][c_id].C_YTD_PAYMENT) + h_amount);
					TM_WRITE(customers[w_id][d_id][c_id].C_PAYMENT_CNT, TM_READ(customers[w_id][d_id][c_id].C_PAYMENT_CNT) + 1);
			}
			else {
		//			(READ_WRITE_TX);
		//			(TX_NEWORDER);


					double D_TAX = TM_READ(districts[w_id][d_id].D_TAX);
					int o_id = TM_READ(districts[w_id][d_id].D_NEXT_O_ID) %(NUM_ORDERS_PER_D);
					TM_WRITE(districts[w_id][d_id].D_NEXT_O_ID, o_id + 1);


					double C_DISCOUNT = TM_READ(customers[w_id][d_id][c_id].C_DISCOUNT);
					int C_LAST = TM_READ(customers[w_id][d_id][c_id].C_LAST);
					int C_CREDIT = TM_READ(customers[w_id][d_id][c_id].C_CREDIT);

					//TODO we don't have create, so simulate it with one write in the array in random index
					// Create entries in ORDER and NEW-ORDER

		//			order_t  norder;// = (order_t*)TM_ALLOC(sizeof(order_t));
		//			norder.O_C_ID = c_id;
		//			norder.O_CARRIER_ID = rand_r_32(seed) %(10); //1000
		//			norder.O_ALL_LOCAL = true;

					TM_WRITE(orders[w_id][o_id].O_CARRIER_ID, rand_r_32(&seed) %(10));

					int items_count = 1 + rand_r_32(&seed) %(5);

					for (int i = 0; i <= items_count; i++) {
						int i_id = rand_r_32(&seed) %(NUM_ITEMS);

						float I_PRICE = TM_READ(items[i_id].I_PRICE);
						int I_NAME = TM_READ(items[i_id].I_NAME);
						int I_DATA = TM_READ(items[i_id].I_DATA);



						//order_line_t orderLine;// = (order_line_t*)TM_ALLOC(sizeof(order_line_t));

						int l_id = rand_r_32(&seed) %(NUM_LINES_PER_O);

		//				orderLine.OL_QUANTITY = rand_r_32(seed) %(1000);
		//				orderLine.OL_I_ID = i_id;
		//				orderLine.OL_SUPPLY_W_ID = w_id;
		//				orderLine.OL_AMOUNT = (int)(orderLine.OL_QUANTITY  * I_PRICE);
		//				orderLine.OL_DELIVERY_D = 0;
		//				orderLine.OL_DIST_INFO = d_id;




						TM_WRITE(order_lines[w_id][o_id][l_id].OL_QUANTITY, rand_r_32(&seed) %(1000));

					}


			}


			tm_super_glock.val =0;
		}
		cantfit += retries;
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);
	printf("CANNOT FIT IN HTM = %d\n", cantfit);
	printf("GL = %d\n", gl_count);
}



#define Q_MAX_SIZE 64
#define Q_COUNTS 50


pad_word_t enq_counter[Q_COUNTS] = {{0}};
pad_word_t deq_counter[Q_COUNTS] = {{0}};
pad_word_t order_lock[Q_COUNTS] = {{0}};

int q_top[Q_COUNTS];



void *tx_fn_tpcc_q(void *arg) {
	int status;
	double interest;
	long acc_total;
	int id = (long)arg;
	unsigned int seed = (unsigned int) get_real_time();
	int conf=0, cap = 0, other = 0, cantfit=0, gl_count=0;
	while(barrier);


	for (int kk=0; kk<100000; kk++){
	    uint32_t val = rand_r_32(&seed) % 256;
	    uint32_t act = rand_r_32(&seed) % 100;
		int w_id = rand_r_32(&seed) %(NUM_WAREHOUSES);
		int o_id = rand_r_32(&seed) %(NUM_ORDERS_PER_D);
		int c_id = rand_r_32(&seed) %(NUM_CUSTOMERS_PER_D);
		int d_id = rand_r_32(&seed) %(NUM_DISTRICTS);
		int l_id = rand_r_32(&seed) %(NUM_LINES_PER_O);
		int orderLineCount = rand_r_32(&seed) % NUM_LINES_PER_O;
		int crtdate = rand_r_32(&seed) % 50000;
		float h_amount = (float)(rand_r_32(&seed) %(500000) * 0.01);

		uintptr_t ticket = __sync_fetch_and_add(&enq_counter[w_id].val, 1);

//		bool success;
//		success = qs[acc1].enq(id);
//		if (!success) printf("enqueue failed!\n");
//		else  printf("%d enqueued %d!\n", id, acc1);

//		enq_l.val = 0;
		while(deq_counter[w_id].val != ticket){
			//if (ticket - deq_counter[acc1].val >= 2)
			if (withYield)
				sched_yield();
		}


	    int retries = 0;
	    int more = 0;
		do{
			more = 0;
			if ((status = _xbegin()) == _XBEGIN_STARTED) {
				if(act < 4) {
			//		(READ_ONLY_TX);
			//		(TX_ORDER_STATUS);
					float dummy;
						dummy = TM_READ(warehouses[w_id].W_YTD);
						dummy = TM_READ(orders[w_id][o_id].O_CARRIER_ID);
						dummy = TM_READ(customers[w_id][d_id][c_id].C_BALANCE);

						float olsum = (float)0;
						int i = 1;

						while (i < orderLineCount) {
							olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
							dummy = TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
							i += 1;
						}
				} else if (act < 8) {
			//		(READ_ONLY_TX);
			//		(TX_STOCKLEVEL);
						//TODO I believe this method is wrong!!
					double dummy;

						dummy= TM_READ(warehouses[w_id].W_YTD);


						dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);



						dummy=TM_READ(order_lines[w_id][o_id][l_id].OL_QUANTITY);



						int k = 1;
						while (k <= 10) {
							int s_id = rand_r_32(&seed) %(NUM_ITEMS);
							dummy=TM_READ(stocks[w_id][s_id].S_QUANTITY);

							k += 1;
						}
				} else if (act < 12) {
			//		(READ_WRITE_TX);
			//		(TX_DELIVERY);
					double dummy;
						for (int d_id = 0; d_id < NUM_DISTRICTS; d_id++) {
							dummy=TM_READ(warehouses[w_id].W_YTD);
							dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);

							float olsum = (float)0;

							int i = 1;
							while (i < orderLineCount) {
								olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
								dummy=TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
								i += 1;
							}
							TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) + olsum);
							TM_WRITE(customers[w_id][d_id][c_id].C_DELIVERY_CNT, TM_READ(customers[w_id][d_id][c_id].C_DELIVERY_CNT) + 1);
						}

				} else if (act < 55) {
			//		(READ_WRITE_TX);
			//		(TX_PAYMENT);


						// Open Wairehouse Table


						TM_WRITE(warehouses[w_id].W_YTD, TM_READ(warehouses[w_id].W_YTD) + h_amount);

						// In DISTRICT table

						TM_WRITE(districts[w_id][d_id].D_YTD, TM_READ(districts[w_id][d_id].D_YTD) + h_amount);

						TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) - h_amount);
						TM_WRITE(customers[w_id][d_id][c_id].C_YTD_PAYMENT, TM_READ(customers[w_id][d_id][c_id].C_YTD_PAYMENT) + h_amount);
						TM_WRITE(customers[w_id][d_id][c_id].C_PAYMENT_CNT, TM_READ(customers[w_id][d_id][c_id].C_PAYMENT_CNT) + 1);
				} else {
			//			(READ_WRITE_TX);
			//			(TX_NEWORDER);


						double D_TAX = TM_READ(districts[w_id][d_id].D_TAX);
						int o_id = TM_READ(districts[w_id][d_id].D_NEXT_O_ID) %(NUM_ORDERS_PER_D);
						TM_WRITE(districts[w_id][d_id].D_NEXT_O_ID, o_id + 1);


						double C_DISCOUNT = TM_READ(customers[w_id][d_id][c_id].C_DISCOUNT);
						int C_LAST = TM_READ(customers[w_id][d_id][c_id].C_LAST);
						int C_CREDIT = TM_READ(customers[w_id][d_id][c_id].C_CREDIT);

						//TODO we don't have create, so simulate it with one write in the array in random index
						// Create entries in ORDER and NEW-ORDER

			//			order_t  norder;// = (order_t*)TM_ALLOC(sizeof(order_t));
			//			norder.O_C_ID = c_id;
			//			norder.O_CARRIER_ID = rand_r_32(seed) %(10); //1000
			//			norder.O_ALL_LOCAL = true;

						TM_WRITE(orders[w_id][o_id].O_CARRIER_ID, rand_r_32(&seed) %(10));

						int items_count = 1 + rand_r_32(&seed) %(5);

						for (int i = 0; i <= items_count; i++) {
							int i_id = rand_r_32(&seed) %(NUM_ITEMS);

							float I_PRICE = TM_READ(items[i_id].I_PRICE);
							int I_NAME = TM_READ(items[i_id].I_NAME);
							int I_DATA = TM_READ(items[i_id].I_DATA);



							//order_line_t orderLine;// = (order_line_t*)TM_ALLOC(sizeof(order_line_t));

							int l_id = rand_r_32(&seed) %(NUM_LINES_PER_O);

			//				orderLine.OL_QUANTITY = rand_r_32(seed) %(1000);
			//				orderLine.OL_I_ID = i_id;
			//				orderLine.OL_SUPPLY_W_ID = w_id;
			//				orderLine.OL_AMOUNT = (int)(orderLine.OL_QUANTITY  * I_PRICE);
			//				orderLine.OL_DELIVERY_D = 0;
			//				orderLine.OL_DIST_INFO = d_id;




							TM_WRITE(order_lines[w_id][o_id][l_id].OL_QUANTITY, rand_r_32(&seed) %(1000));

						}


				}
				//TODO add this
//				if (tm_super_glock.val) _xabort(X_GL_EXISTS);
				_xend();
			} else {
				more = 1;
				retries++;
				if (status & _XABORT_CONFLICT)
					conf++;//printf("1 abort reason CONFLICT\n");
				else if (status & _XABORT_CAPACITY)
					cap++;//printf("1 abort reason CAPACITY\n");
				else
					other++;//printf("1 abort reason %d\n", status);
			}
		} while (more && retries < 5);
		if (retries == 5) {
			gl_count++;
			do {
				while (tm_super_glock.val) pause();
			} while (!__sync_bool_compare_and_swap(&tm_super_glock.val, 0, 1));



			if(act < 4) {
		//		(READ_ONLY_TX);
		//		(TX_ORDER_STATUS);
				float dummy;
					dummy = TM_READ(warehouses[w_id].W_YTD);
					dummy = TM_READ(orders[w_id][o_id].O_CARRIER_ID);
					dummy = TM_READ(customers[w_id][d_id][c_id].C_BALANCE);

					float olsum = (float)0;
					int i = 1;

					while (i < orderLineCount) {
						olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
						dummy = TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
						i += 1;
					}
			}
			else if (act < 8) {
		//		(READ_ONLY_TX);
		//		(TX_STOCKLEVEL);
					//TODO I believe this method is wrong!!
				double dummy;

					dummy= TM_READ(warehouses[w_id].W_YTD);


					dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);



					dummy=TM_READ(order_lines[w_id][o_id][l_id].OL_QUANTITY);



					int k = 1;
					while (k <= 10) {
						int s_id = rand_r_32(&seed) %(NUM_ITEMS);
						dummy=TM_READ(stocks[w_id][s_id].S_QUANTITY);

						k += 1;
					}
			}
			else if (act < 12) {
		//		(READ_WRITE_TX);
		//		(TX_DELIVERY);
				double dummy;
					for (int d_id = 0; d_id < NUM_DISTRICTS; d_id++) {
						dummy=TM_READ(warehouses[w_id].W_YTD);
						dummy=TM_READ(orders[w_id][o_id].O_CARRIER_ID);

						float olsum = (float)0;

						int i = 1;
						while (i < orderLineCount) {
							olsum += TM_READ(order_lines[w_id][o_id][i].OL_AMOUNT);
							dummy=TM_READ(order_lines[w_id][o_id][i].OL_QUANTITY);
							i += 1;
						}
						TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) + olsum);
						TM_WRITE(customers[w_id][d_id][c_id].C_DELIVERY_CNT, TM_READ(customers[w_id][d_id][c_id].C_DELIVERY_CNT) + 1);
					}

			}
			else if (act < 55) {
		//		(READ_WRITE_TX);
		//		(TX_PAYMENT);


					// Open Wairehouse Table


					TM_WRITE(warehouses[w_id].W_YTD, TM_READ(warehouses[w_id].W_YTD) + h_amount);

					// In DISTRICT table

					TM_WRITE(districts[w_id][d_id].D_YTD, TM_READ(districts[w_id][d_id].D_YTD) + h_amount);

					TM_WRITE(customers[w_id][d_id][c_id].C_BALANCE, TM_READ(customers[w_id][d_id][c_id].C_BALANCE) - h_amount);
					TM_WRITE(customers[w_id][d_id][c_id].C_YTD_PAYMENT, TM_READ(customers[w_id][d_id][c_id].C_YTD_PAYMENT) + h_amount);
					TM_WRITE(customers[w_id][d_id][c_id].C_PAYMENT_CNT, TM_READ(customers[w_id][d_id][c_id].C_PAYMENT_CNT) + 1);
			}
			else {
		//			(READ_WRITE_TX);
		//			(TX_NEWORDER);


					double D_TAX = TM_READ(districts[w_id][d_id].D_TAX);
					int o_id = TM_READ(districts[w_id][d_id].D_NEXT_O_ID) %(NUM_ORDERS_PER_D);
					TM_WRITE(districts[w_id][d_id].D_NEXT_O_ID, o_id + 1);


					double C_DISCOUNT = TM_READ(customers[w_id][d_id][c_id].C_DISCOUNT);
					int C_LAST = TM_READ(customers[w_id][d_id][c_id].C_LAST);
					int C_CREDIT = TM_READ(customers[w_id][d_id][c_id].C_CREDIT);

					//TODO we don't have create, so simulate it with one write in the array in random index
					// Create entries in ORDER and NEW-ORDER

		//			order_t  norder;// = (order_t*)TM_ALLOC(sizeof(order_t));
		//			norder.O_C_ID = c_id;
		//			norder.O_CARRIER_ID = rand_r_32(seed) %(10); //1000
		//			norder.O_ALL_LOCAL = true;

					TM_WRITE(orders[w_id][o_id].O_CARRIER_ID, rand_r_32(&seed) %(10));

					int items_count = 1 + rand_r_32(&seed) %(5);

					for (int i = 0; i <= items_count; i++) {
						int i_id = rand_r_32(&seed) %(NUM_ITEMS);

						float I_PRICE = TM_READ(items[i_id].I_PRICE);
						int I_NAME = TM_READ(items[i_id].I_NAME);
						int I_DATA = TM_READ(items[i_id].I_DATA);



						//order_line_t orderLine;// = (order_line_t*)TM_ALLOC(sizeof(order_line_t));

						int l_id = rand_r_32(&seed) %(NUM_LINES_PER_O);

		//				orderLine.OL_QUANTITY = rand_r_32(seed) %(1000);
		//				orderLine.OL_I_ID = i_id;
		//				orderLine.OL_SUPPLY_W_ID = w_id;
		//				orderLine.OL_AMOUNT = (int)(orderLine.OL_QUANTITY  * I_PRICE);
		//				orderLine.OL_DELIVERY_D = 0;
		//				orderLine.OL_DIST_INFO = d_id;




						TM_WRITE(order_lines[w_id][o_id][l_id].OL_QUANTITY, rand_r_32(&seed) %(1000));

					}


			}


			tm_super_glock.val =0;
		}
		cantfit += retries;

		__sync_fetch_and_add(&deq_counter[w_id].val, 1);
	}
	printf("1 abort reason CONFLICT = %d\n", conf);
	printf("1 abort reason CAPACITY = %d\n", cap);
	printf("1 abort reason OTHER = %d\n", other);
	printf("CANNOT FIT IN HTM = %d\n", cantfit);
	printf("GL = %d\n", gl_count);
}



int main(int argc, char* argv[])
{

	if (argc < 3) {
		printf("Usage type #_of_threads\n");
		exit(0);
	}

	int type = atoi(argv[1]);

	int thread_count = atoi(argv[2]);
	if (thread_count > 8)
		withYield = true;

//	int exp = atoi(argv[3]);


//	if (argc == 5) {
//		percentage = atoi(argv[4]);
//	}

//	long count = 0;
//	for (int o=0;o<ACCOUT_NUM; o++) {
//		accounts[o].val = 10000;
//		count += accounts[o].val;
//	}
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
//	for (int i = 0 ; i< ARRAY_SIZE; i++) {
//		array[i]=1000;
//	}
//        for (int i = 0 ; i< ARRAY_SIZE; i++) {
//                oo += array[i];
//        }
//        printf("Inital sum = %d\n", count);


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


//  pthread_t thread1, thread2, thread3, thread4, thread5, thread6, thread7, thread8;
  pthread_t threads[200];
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
//	int x[100000] = {1};
	int i;

	if (type == 1) {
		for (i=0; i< thread_count; i++) {
			pthread_create(&threads[i], &thread_attr, tx_fn_tpcc, (void*)(i+1));
		}
	} else if (type == 2){
		for (i=0; i< thread_count; i++) {
			pthread_create(&threads[i], &thread_attr, tx_fn_tpcc_q, (void*)(i+1));
		}
	}


//	if (type == 1) {
//		if (exp ==1) {
//			for (i=0; i< thread_count; i++) {
//				pthread_create(&threads[i], &thread_attr, tx_fn2_one, (void*)(i+1));
//			}
//		} else if (exp ==2){
//			for (i=0; i< thread_count; i++) {
//				pthread_create(&threads[i], &thread_attr, tx_fn2, (void*)(i+1));
//			}
//		} else {
//			for (i=0; i< thread_count; i++) {
//				pthread_create(&threads[i], &thread_attr, tx_fn2_full, (void*)(i+1));
//			}
//		}
//	} else {
//		if (exp ==1) {
//			for (i=0; i< thread_count; i++) {
//				pthread_create(&threads[i], &thread_attr, tx_fn_one_q, (void*)(i+1));
//			}
//		} else if (exp ==2){
//			for (i=0; i< thread_count; i++) {
//				pthread_create(&threads[i], &thread_attr, tx_fn, (void*)(i+1));
//			}
//		} else {
//			for (i=0; i< thread_count; i++) {
//				pthread_create(&threads[i], &thread_attr, tx_fn_full_q, (void*)(i+1));
//			}
//		}
//	}

//	pthread_create(&thread1, &thread_attr, tx_fn, (void*)1);
//	pthread_create(&thread2, &thread_attr, tx_fn, (void*)2/*4968147*/);
//	pthread_create(&thread3, &thread_attr, tx_fn, (void*)3);
//	pthread_create(&thread4, &thread_attr, tx_fn, (void*)4);
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

	for (i=0; i< thread_count; i++) {
		pthread_join(threads[i], NULL);
	}
//	pthread_join(thread1, NULL);
//	pthread_join(thread2, NULL);
//	pthread_join(thread3, NULL);
//	pthread_join(thread4, NULL);
//	pthread_join(thread5, NULL);
//	pthread_join(thread6, NULL);
//	pthread_join(thread7, NULL);
//	pthread_join(thread8, NULL);
	time = get_real_time() - time;
	printf("Total Time = %llu\n", time);
//	for (int i=0; i<ACCOUT_NUM;i++) {
//		printf("account %d = %lld\n", i, accounts[i].val);
//	}
//	oo=0;
//	for (int i = 0 ; i< ARRAY_SIZE; i++) {
//                oo += array[i];
////                printf("I%d = %d  ", i, array[i]);
////                if (i%10 == 0)
////                	printf("\n");
//        }

//	count = 0;
//		for (int o=0;o<ACCOUT_NUM; o++) {
//			count += accounts[o].val;
//		}
//	for (int i=0; i< Q_COUNTS; i++) {
//		//printf("q %d size is %d\n", i, qs[i].getSize());
//		printf("%d: enq val %d & val2 %d\n", i, enq_counter[i].val, enq_counter[i].val2);
//	}
//
//	printf("Final sum = %d\n", count);
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

