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

#define FORCE_INLINE __attribute__((always_inline)) inline
#define CACHELINE_BYTES 64

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
      void*  val;
  };

typedef BitFilter<1024> filter_t;
static const uint32_t RING_ELEMENTS = 1024;

pad_word_t timestamp = {0};
pad_word_t last_init     = {0};
pad_word_t last_complete = {0};

filter_t   ring_wf[RING_ELEMENTS] __attribute__((aligned(16)));



#define nop()               __asm__ volatile("nop")

#define NUM_STRIPES  1048576

unsigned long long get_real_time() {
	struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);

    return time.tv_sec * 1000000000L + time.tv_nsec;
}

#define ARRAY_SIZE 100000

#define SPLIT_SIZE 20

long* array;

#define NO_READWRITE 400

#define NO_TX 100000
//00

#define X_CONFLICT 1

filter_t write_locks;

volatile int barrier = 1;

void *tx_fn(void *arg) {
	filter_t rf;
	filter_t wf;
	filter_t p_wf;
	for (int i=0; i< ARRAY_SIZE; i++) {
		array[i] = i;
	}
	AddrVal* undo_log;
	undo_log = (AddrVal*) malloc(10000*sizeof(AddrVal));
	//prefetch memory
	for (int j=0; j<10000; j++) {
		undo_log[j].addr = (void **) 0;
		undo_log[j].val = (void *) 0;
	}
	int conflict_abort = 0, capacity_abort = 0, other_abort = 0;
	int htm_conflict_abort = 0, htm_a_conflict_abort =0, htm_capacity_abort = 0, htm_other_abort = 0;
	int undo_i;
	unsigned status;
	unsigned int seed = (long)arg;
	int tx_limit = 0;
	int loc[1024];

	while(barrier);

	unsigned long long time = get_real_time();
	while (tx_limit < NO_TX) {
		int tx_len = (rand_r_32(&seed) % NO_READWRITE) + 1;
		tx_limit++;
		for (int i = 0; i < tx_len /*NO_READWRITE*/; ++i) {
			loc[i] = rand_r_32(&seed) % ARRAY_SIZE;
		}
		//try as all HTM
		if ((status = _xbegin()) == _XBEGIN_STARTED) {
				for (int i= 0; i < tx_len/*NO_READWRITE*/; i++) {
					wf.add(&array[loc[i]]);
					rf.add(&array[loc[i]]);
					array[loc[i]] = array[loc[i]] + 1;
				}
				if (rf.intersect(&write_locks) || wf.intersect(&write_locks)) {
					_xabort(X_CONFLICT);
				}
			_xend();
			rf.clear();
		    wf.clear();
		} else {
			//TODO do we need some reties in all HTM
			//All HTM statistics
			if (status & _XABORT_CODE(X_CONFLICT)) {
				htm_a_conflict_abort++;
			} else if (status & _XABORT_CAPACITY)
				htm_capacity_abort++;
			else if (status & _XABORT_CONFLICT){
				htm_conflict_abort++;
			}
			else
				htm_other_abort++;

			//failed in all HTM, retry with partitioning

	//		if (45678 == (long)arg)
	//		printf("tx from %d\n", (long)arg);
			undo_i =0;

			bool more = true;
			int initial = 0;
			int retry_limit = 0;
			int count = 0;
			bool retry = false;
			do { //retry loop
	//			if (45678 == (long)arg)
	//					printf("tx\n");
				retry = false;
				uintptr_t start_time = last_complete.val;
				while (more) {
	//				if (45678 == (long)arg)
	//						printf("tx part\n");
					retry_limit++;
					int limit = initial + SPLIT_SIZE;
					more = false;
					p_wf.clear();
					if ((status = _xbegin()) == _XBEGIN_STARTED) {
						int i;
						for (i=initial; i < tx_len/*NO_READWRITE*/; i++) {
							if (i == limit) {initial = limit; more = true; break;}
							undo_log[undo_i].addr = (void **) &(array[loc[i]]);
							undo_log[undo_i].val = (void *) (array[loc[i]]);
							undo_i++;
	//						undo_log[undo_i++] = (uintptr_t)();
	//						undo_log[undo_i++] = (array[loc[i]]);
							p_wf.add(&array[loc[i]]);
							rf.add(&array[loc[i]]);
							array[loc[i]] = array[loc[i]] + 1;
		//					//array[loc[i]]++;
		//					int fff = loc[0];
		//					count += array[fff];
						}
						if (rf.intersect(&write_locks, wf) || p_wf.intersect(&write_locks, wf)) {
							_xabort(X_CONFLICT);
						}
						else {
							write_locks.unionwith(p_wf);
						}
						end2: _xend();
						wf.unionwith(p_wf);
						//TODO Do partial validation "check_in_flight" (will need partial read signature also)
						uintptr_t my_index = last_init.val;
						if (__builtin_expect(my_index != start_time, false)) { //TODO is false correct!?
							  // intersect against all new entries
							  for (uintptr_t i = my_index; i >= start_time + 1; i--)
								  if (ring_wf[i % RING_ELEMENTS].intersect(&rf)) { //full abort
									  //tx->tmabort(tx);
									  while (undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
										  undo_i--;
										  *(undo_log[undo_i].addr) = undo_log[undo_i].val;
										  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
										  //undo_i -= 2;
									  }
	//					        	  if (45678 == (long)arg)printf("abort5\n");//TODO undo locations
									  retry = true;
									  for (int i = 0; i < 64; i++)
											nop();
									  goto retry_lbl;
								  }
							  // wait for newest entry to be writeback-complete before returning
							  while (last_complete.val < my_index)
								  {for (int i = 0; i < 64; i++)
										  nop();}

							  // detect ring rollover: start.ts must not have changed
							  if (timestamp.val > (start_time + RING_ELEMENTS)) {
	//					          tx->tmabort(tx);
								  while (undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
									  undo_i--;
									  *(undo_log[undo_i].addr) = undo_log[undo_i].val;
									  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
									  //undo_i -= 2;
								  }
	//				        	  if (45678 == (long)arg)printf("abort62\n");//TODO undo locations
								  retry = true;
								  for (int i = 0; i < 64; i++)
										nop();
								  goto retry_lbl;
							  }

							  // ensure this tx doesn't look at this entry again
							  start_time = my_index;
						}
					} else {
	//					if (45678 == (long)arg)
	//						printf("tx aborted %d\n", status);
						//usleep(1);
						for (int i = 0; i < 64; i++)
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
							  while (undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
								  undo_i--;
								  *(undo_log[undo_i].addr) = undo_log[undo_i].val;
								  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
								  //undo_i -= 2;
							  }
	//			        	  if (45678 == (long)arg)printf("abort27\n");//TODO undo locations
							  retry = true;
	//						  for (int i = 0; i < 64; i++)
	//								nop();
							  goto retry_lbl;
						} else
							more = true;
						if (status & _XABORT_CAPACITY)
							capacity_abort++;
						else if (status & _XABORT_CONFLICT){
							//printf("Conflict\n");
							conflict_abort++;
						}
						else
							other_abort++;
					}
				}
				  uintptr_t commit_time;
				  do {
					  commit_time = timestamp.val;
					  // get the latest ring entry, return if we've seen it already
					  if (commit_time != start_time) {
						  // wait for the latest entry to be initialized
						  while (last_init.val < commit_time)
							  {for (int i = 0; i < 64; i++)
									  nop();}

						  // intersect against all new entries
						  for (uintptr_t i = commit_time; i >= start_time + 1; i--)
							  if (ring_wf[i % RING_ELEMENTS].intersect(&rf)) {
								  //tx->tmabort(tx);
								  while (undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
									  undo_i--;
									  *(undo_log[undo_i].addr) = undo_log[undo_i].val;
									  //*(undo_log[undo_i-2]) = undo_log[undo_i-1];
									  //undo_i -= 2;
								  }
	//							  if (45678 == (long)arg)printf("abort8\n");//TODO undo locations
								  retry = true;
								  for (int i = 0; i < 64; i++)
										nop();
								  goto retry_lbl;
							  }

						  // wait for newest entry to be wb-complete before continuing
						  while (last_complete.val < commit_time)
							  {for (int i = 0; i < 64; i++)
										nop();}

						  // detect ring rollover: start.ts must not have changed
						  if (timestamp.val > (start_time + RING_ELEMENTS)) {
							  while (undo_i > 0) { //TODO if size matter, we need to keep the size also or check how RSTM did it
								  undo_i--;
								  *(undo_log[undo_i].addr) = undo_log[undo_i].val;
	//							  *(undo_log[undo_i-2]) = undo_log[undo_i-1];
	//							  undo_i -= 2;
							  }
	//						  if (45678 == (long)arg)printf("abort92\n");//TODO undo locations
							  retry = true;
							  for (int i = 0; i < 64; i++)
									nop();
							  goto retry_lbl;
							  //tx->tmabort(tx);
						  }

						  // ensure this tx doesn't look at this entry again
						  start_time = commit_time;
					  }
				  } while (!__sync_bool_compare_and_swap(&timestamp.val, commit_time, commit_time + 1));

				  // copy the bits over (use SSE, not indirection)
				  ring_wf[(commit_time + 1) % RING_ELEMENTS].fastcopy(&wf);

				  // setting this says "the bits are valid"
				  last_init.val = commit_time + 1;

				  // we're committed... run redo log, then mark ring entry COMPLETE
				  //tx->writes.writeback(STM_WHEN_PROTECT_STACK(upper_stack_bound));
				  last_complete.val = commit_time + 1;
				  retry_lbl:
					  write_locks.differencewith(wf);
					  rf.clear();
					  wf.clear();
			} while (retry);
	//		printf("count = %d\n", count);
		} //partitioned htm block
	} //num of tx loop
	time = get_real_time() - time;
	printf("Time = %llu\n", time);

	printf("conflict = %d, capacity=%d, other=%d\n", conflict_abort, capacity_abort, other_abort);
	printf("HTM conflict = %d, our conflict = %d, capacity=%d, other=%d\n", htm_conflict_abort, htm_a_conflict_abort, htm_capacity_abort, htm_other_abort);
	printf("Last complete = %d\n", last_complete.val);
}

//_XABORT_CONFLICT
//_XABORT_CAPACITY

int main(void)
{

	array = (long*) malloc(ARRAY_SIZE * sizeof(long));
	//prefetch all memory pages
	int oo;
	for (int i = 0 ; i< ARRAY_SIZE; i++) {
		oo = array[i];
	}
  pthread_t thread1, thread2;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
//	int x[100000] = {1};
	int i;

	pthread_create(&thread1, &thread_attr, tx_fn, (void*)45678);
	pthread_create(&thread2, &thread_attr, tx_fn, (void*)4968147);

	unsigned long long time = get_real_time();
	barrier =0;

	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	time = get_real_time() - time;
	printf("Total Time = %llu\n", time);
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
