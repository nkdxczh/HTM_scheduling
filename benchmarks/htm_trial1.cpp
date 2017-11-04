#include "tsx-assert.h"
#include "rtm.h"
#include "rand_r_32.h"
#include "stm/BitFilter.hpp"
#include "stm/ThreadLocal.hpp"
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <setjmp.h>


//==============Part HTM Globals=======================================
#define STATISTICS
#define FORCE_INLINE __attribute__((always_inline)) inline
#define CACHELINE_BYTES 64
#define CFENCE              __asm__ volatile ("":::"memory")

using stm::BitFilter;

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

#define nop()               __asm__ volatile("nop")

#define X_CONFLICT 1

filter_t write_locks;

//Tx context

struct Tx_Context {
	filter_t rf;
	filter_t wf;
	filter_t p_wf;
	AddrVal* undo_log;
	int undo_i;
	jmp_buf* scope;
	int nesting_depth = 0;
	int backoff = 1;
	uintptr_t start_time;
#ifdef STATISTICS
	int conflict_abort = 0, capacity_abort = 0, other_abort = 0;
	int htm_conflict_abort = 0, htm_a_conflict_abort =0, htm_capacity_abort = 0, htm_other_abort = 0;
	int sw_c = 0, sw_abort=0;
#endif
};

__thread Tx_Context* Self;

int RETRY_SHORT = 1;


FORCE_INLINE void spin64() {
	for (int i = 0; i < 64; i++)
		nop();
}

FORCE_INLINE void begin(Tx_Context* tx, jmp_buf* s, uint32_t /*abort_flags*/)
{
//    if (++tx->nesting_depth > 1)
//        return;

    // NB: this CAS fails on a transaction restart... is that too expensive?
	__sync_val_compare_and_swap((volatile uintptr_t*)&tx->scope, (uintptr_t)0, (uintptr_t)s);
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
	tx->undo_log[tx->undo_i].val.val = (void *) val;
	tx->undo_i++;
	tx->p_wf.add(addr);
    *addr = val;
}

#define TM_READ(var)       tm_read(&var, tx)
#define TM_SHORT_WRITE(var, val) tm_short_write(&var, val, tx)
#define TM_WRITE(var, val) tm_write(&var, val, tx)


FORCE_INLINE void tm_abort(Tx_Context* tx)
{
	  while (tx->undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
		  tx->undo_i--;
		  *(tx->undo_log[tx->undo_i].addr) = tx->undo_log[tx->undo_i].val.val;
	  }
//	  printf("backoff %d\n", tx->backoff);
	  for (int i = 0; i < 64*tx->backoff; i++)
			nop();

	  tx->backoff ++;//<<=1; //TODO exp backoff??
	  write_locks.differencewith(tx->wf);
	  tx->rf.clear();
	  tx->wf.clear();
//	  printf("aborting\n");
	  //restart the tx
#ifdef STATISTICS
	  tx->sw_abort++;
#endif
      longjmp(*(tx->scope), 1);
}

FORCE_INLINE void tm_commit(Tx_Context* tx)
{
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

	// we're committed... run redo log, then mark ring entry COMPLETE
	//tx->writes.writeback(STM_WHEN_PROTECT_STACK(upper_stack_bound));
	last_complete.val = commit_time + 1;
#ifdef STATISTICS
	tx->sw_c++;
#endif
	write_locks.differencewith(tx->wf);
	tx->rf.clear();
	tx->wf.clear();
	tx->backoff = 1;
//	printf("committed\n");
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

#define TM_TX_VAR	Tx_Context* tx = (Tx_Context*)Self;

#define TM_SHORT_BEGIN										\
	{														\
	Tx_Context* tx = (Tx_Context*)Self;          			\
	unsigned status;										\
	int htm_retry = 0;										\
	bool htm_again;											\
	do {													\
		htm_again = false;									\
		if ((status = _xbegin()) == _XBEGIN_STARTED) {



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
		if (htm_retry < RETRY_SHORT) {											\
			htm_again = true;														\
			continue;																\
		}

//{                                                       \
//Tx_Context* tx = (Tx_Context*)Self;          			\

#define TM_BEGIN		                                    \
    jmp_buf _jmpbuf;                                        \
    uint32_t abort_flags = setjmp(_jmpbuf);                 \
    begin(tx, &_jmpbuf, abort_flags);                  		\
    CFENCE;                                                 \
    {														\
    	tx->undo_i =0;										\
		bool more = true;									\
		tx->start_time = last_complete.val;

#define TM_PART_BEGIN		                                \
	while (more) {		                                    \
		/*tx->backoff++;*/\
		more = false;	                                    \
		tx->p_wf.clear();	                                \
		if ((status = _xbegin()) == _XBEGIN_STARTED) {

//TODO Do partial validation "check_in_flight" (will need partial read signature also)
//TODO is false correct in built in expect!?
//TODO Do partial abort with xabort?
#define TM_PART_END					                                									\
		if (tx->rf.intersect(&write_locks, tx->wf) || tx->p_wf.intersect(&write_locks, tx->wf)) {		\
			_xabort(X_CONFLICT);					                                					\
		}								                                								\
		else {							                                				\
			write_locks.unionwith(tx->p_wf);					                        \
		}					                                							\
		_xend();					                                					\
		tx->wf.unionwith(tx->p_wf);			                                			\
		uintptr_t my_index = last_init.val;					                            \
		if (__builtin_expect(my_index != tx->start_time, false)) { 						\
			  for (uintptr_t i = my_index; i >= tx->start_time + 1; i--)				\
				  if (ring_wf[i % RING_ELEMENTS].intersect(&tx->rf)) {					\
					  tm_abort(tx);														\
				  }				                                						\
			  while (last_complete.val < my_index)              						\
				  spin64();							              						\
			  if (timestamp.val > (tx->start_time + RING_ELEMENTS)) {					\
				  tm_abort(tx);															\
			  }																			\
			  tx->start_time = my_index;												\
		}																				\
	} else {																			\
		if (status & _XABORT_CODE(X_CONFLICT)) { 										\
			tm_abort(tx);																\
		} else 																			\
			more = true; 																\
		PART_HTM_STAT			 														\
		for (int i = 0; i < 64*tx->backoff; i++)										\
			nop();																		\
		tx->backoff++;																	\
	}																					\
}

/**
 *  This is the way to commit a transaction.  Note that these macros weakly
 *  enforce lexical scoping
 */
#define TM_END                                  \
		}                                  		\
		tm_commit(tx);                          \
	  }											\
	} while (htm_again);						\
 }

//==============END Part HTM Globals===================================


//==============Part HTM Macros========================================










//==============END Part HTM Macros====================================

unsigned long long get_real_time() {
	struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);

    return time.tv_sec * 1000000000L + time.tv_nsec;
}


#define SPLIT_SIZE 20

long* array;


#define NO_TX 100000

int ARRAY_SIZE = 100;

int NO_READWRITE = 10;

#define RANDOMIZE 0


volatile int barrier = 1;









void *tx_fn(void *arg) {

	thread_init();

	unsigned int seed = (long)arg;
	int tx_limit = 0;
	int loc[1024];

	while(barrier);

	unsigned long long time = get_real_time();
	while (tx_limit < NO_TX) {
		int tx_len = RANDOMIZE? (rand_r_32(&seed) % NO_READWRITE) + 1 : NO_READWRITE;
		tx_limit++;
		for (int i = 0; i < tx_len; ++i) {
			loc[i] = rand_r_32(&seed) % ARRAY_SIZE;
		}

		TM_SHORT_BEGIN
				for (int i= 0; i < tx_len; i+=2) {
//					if (array[loc[i]] > 50) {
//						array[loc[i+1]] += 50;
//						array[loc[i]] -= 50;
//						tx->wf.add(&array[loc[i]]);
//						tx->wf.add(&array[loc[i+1]]);
//					} else {
//						tx->rf.add(&array[loc[i]]);
//					}
					if (TM_READ(array[loc[i]]) > 50) {
						TM_SHORT_WRITE(array[loc[i+1]], array[loc[i+1]] + 50);
						TM_SHORT_WRITE(array[loc[i]], array[loc[i]] - 50);
					}
				}
		TM_SHORT_END
		int initial =0;
		TM_BEGIN



		while (more) {
							tx->backoff++;
			//				if (45678 == (long)arg)
			//						printf("tx part\n");
							int limit = initial + SPLIT_SIZE;
							more = false;
							tx->p_wf.clear();
							if ((status = _xbegin()) == _XBEGIN_STARTED) {
								int i;
								for (i=initial; i < tx_len/*NO_READWRITE*/; i+=2) {
									if (i == limit) {initial = limit; more = true; break;}

									if (array[loc[i]] > 50) {
										tx->undo_log[tx->undo_i].addr = (void **) &(array[loc[i]]);
										tx->undo_log[tx->undo_i].val.val = (void *) (array[loc[i]]);
										tx->undo_i++;
										tx->undo_log[tx->undo_i].addr = (void **) &(array[loc[i + 1]]);
										tx->undo_log[tx->undo_i].val.val = (void *) (array[loc[i + 1]]);
										tx->undo_i++;
										array[loc[i + 1]] += 50;
										array[loc[i]] -= 50;
										tx->p_wf.add(&array[loc[i]]);
										tx->p_wf.add(&array[loc[i + 1]]);
									} else {
										tx->rf.add(&array[loc[i]]);
									}

		/*							undo_log[undo_i].addr = (void **) &(array[loc[i]]);
									undo_log[undo_i].val = (void *) (array[loc[i]]);
									undo_i++;
			//						undo_log[undo_i++] = (uintptr_t)();
			//						undo_log[undo_i++] = (array[loc[i]]);
									p_wf.add(&array[loc[i]]);
									rf.add(&array[loc[i]]);
									array[loc[i]] = array[loc[i]] + 1;
				//					//array[loc[i]]++;
				//					int fff = loc[0];
				//					count += array[fff];*/
								}
								if (tx->rf.intersect(&write_locks, tx->wf) || tx->p_wf.intersect(&write_locks, tx->wf)) {
									_xabort(X_CONFLICT);
								}
								else {
									write_locks.unionwith(tx->p_wf);
								}
								end2: _xend();
								tx->wf.unionwith(tx->p_wf);
								//TODO Do partial validation "check_in_flight" (will need partial read signature also)
								uintptr_t my_index = last_init.val;
								if (__builtin_expect(my_index != tx->start_time, false)) { //TODO is false correct!?
									  // intersect against all new entries
									  for (uintptr_t i = my_index; i >= tx->start_time + 1; i--)
										  if (ring_wf[i % RING_ELEMENTS].intersect(&tx->rf)) { //full abort
											  //tx->tmabort(tx);
											  while (tx->undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
												  tx->undo_i--;
												  *(tx->undo_log[tx->undo_i].addr) = tx->undo_log[tx->undo_i].val.val;
												  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
												  //undo_i -= 2;
											  }
			//					        	  if (45678 == (long)arg)printf("abort5\n");//TODO undo locations
											  for (int i = 0; i < 64*tx->backoff; i++)
													nop();
											  write_locks.differencewith(tx->wf);
											  	  tx->rf.clear();
											  	  tx->wf.clear();
											  //	  printf("aborting\n");
											  	  //restart the tx
											        longjmp(*(tx->scope), 1);
										  }
									  // wait for newest entry to be writeback-complete before returning
									  while (last_complete.val < my_index)
										  {for (int i = 0; i < 64*tx->backoff; i++)
												  nop();}

									  // detect ring rollover: start.ts must not have changed
									  if (timestamp.val > (tx->start_time + RING_ELEMENTS)) {
			//					          tx->tmabort(tx);
										  while (tx->undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
											  tx->undo_i--;
											  *(tx->undo_log[tx->undo_i].addr) = tx->undo_log[tx->undo_i].val.val;
											  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
											  //undo_i -= 2;
										  }
			//				        	  if (45678 == (long)arg)printf("abort62\n");//TODO undo locations
										  for (int i = 0; i < 64*tx->backoff; i++)
												nop();
										  write_locks.differencewith(tx->wf);
										  											  	  tx->rf.clear();
										  											  	  tx->wf.clear();
										  											  //	  printf("aborting\n");
										  											  	  //restart the tx
										  											        longjmp(*(tx->scope), 1);
									  }

									  // ensure this tx doesn't look at this entry again
									  tx->start_time = my_index;
								}
							} else {
			//					if (45678 == (long)arg)
			//						printf("tx aborted %d\n", status);
								//usleep(1);
								for (int i = 0; i < 64*tx->backoff; i++)
									nop();

								//printf("abort! %d\n", status);
				//				for (int j = 0; j < NO_READWRITE; j++) {
				//					printf ("loc %d= %d\n", j, loc[j]);
				//				}
								//if (retry_limit < 10000)
								if (status & _XABORT_CODE(X_CONFLICT)) { //TODO check this!!
									//TODO Do partial abort?
									//printf("Conflict");
									//more = false; //abort the entire tx

									//limit--;
									//more = true;
									//TODO or abort the entire tx?
									  while (tx->undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
										  tx->undo_i--;
										  *(tx->undo_log[tx->undo_i].addr) = tx->undo_log[tx->undo_i].val.val;
										  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
										  //undo_i -= 2;
									  }
			//			        	  if (45678 == (long)arg)printf("abort27\n");//TODO undo locations
			//						  for (int i = 0; i < 64; i++)
			//								nop();
									  write_locks.differencewith(tx->wf);
									  											  	  tx->rf.clear();
									  											  	  tx->wf.clear();
									  											  //	  printf("aborting\n");
									  											  	  //restart the tx
									  											        longjmp(*(tx->scope), 1);
								} else
									more = true;
								if (status & _XABORT_CAPACITY)
									tx->capacity_abort++;
								else if (status & _XABORT_CONFLICT){
									//printf("Conflict\n");
									tx->conflict_abort++;
								}
								else
									tx->other_abort++;
							}
						}

//			int initial = 0;
//
//			do {
//				int limit = initial + SPLIT_SIZE > tx_len? tx_len: initial + SPLIT_SIZE;
////				printf("split size: %d\n", limit);
//				TM_PART_BEGIN
//					int i;
//					for (i=initial; i < limit; i+=2) {
////						if (array[loc[i]] > 50) {
////							tx->undo_log[tx->undo_i].addr = (void **) &(array[loc[i]]);
////							tx->undo_log[tx->undo_i].val.val = (void *) (array[loc[i]]);
////							tx->undo_i++;
////							tx->undo_log[tx->undo_i].addr = (void **) &(array[loc[i + 1]]);
////							tx->undo_log[tx->undo_i].val.val = (void *) (array[loc[i + 1]]);
////							tx->undo_i++;
////							array[loc[i + 1]] += 50;
////							array[loc[i]] -= 50;
////							tx->p_wf.add(&array[loc[i]]);
////							tx->p_wf.add(&array[loc[i + 1]]);
////						} else {
////							tx->rf.add(&array[loc[i]]);
////						}
//						if (TM_READ(array[loc[i]]) > 50) {
//							TM_WRITE(array[loc[i+1]], array[loc[i+1]] + 50);
//							TM_WRITE(array[loc[i]], array[loc[i]] - 50);
//						}
//					}
//				TM_PART_END
//				initial = limit;
////				printf("end size: %d\n", initial);
//			} while (initial!=tx_len);
		TM_END
	} //num of tx loop
	time = get_real_time() - time;
	printf("Time = %llu\n", time);

	TM_TX_VAR

	printf("conflict = %d, capacity=%d, other=%d\n", tx->conflict_abort, tx->capacity_abort, tx->other_abort);
	printf("HTM conflict = %d, our conflict = %d, capacity=%d, other=%d\n", tx->htm_conflict_abort, tx->htm_a_conflict_abort, tx->htm_capacity_abort, tx->htm_other_abort);
	printf("Last complete = %d, SW count = %d, SW Aborts = %d\n", last_complete.val, tx->sw_c, tx->sw_abort);
}

//_XABORT_CONFLICT
//_XABORT_CAPACITY

int main(int argc, char* argv[])
{
	if (argc < 4) {
		printf("Usage #_of_randomly_accessed_element  Array_size #_of_short_tx_retrials\n");
		exit(0);
	}
	NO_READWRITE = atoi(argv[1]);
	ARRAY_SIZE = atoi(argv[2]);
	RETRY_SHORT = atoi(argv[3]);

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


  pthread_t thread1, thread2, thread3, thread4;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
//	int x[100000] = {1};
	int i;

	pthread_create(&thread1, &thread_attr, tx_fn, (void*)45678);
	pthread_create(&thread2, &thread_attr, tx_fn, (void*)4968147);
	pthread_create(&thread3, &thread_attr, tx_fn, (void*)49147);
	pthread_create(&thread4, &thread_attr, tx_fn, (void*)4967);

	unsigned long long time = get_real_time();
	barrier =0;

	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	pthread_join(thread3, NULL);
	pthread_join(thread4, NULL);
	time = get_real_time() - time;
	printf("Total Time = %llu\n", time);
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
