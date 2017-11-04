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

using stm::ValueList;
using stm::ValueListEntry;
using stm::BitFilter;
using stm::WriteSetEntry;
using stm::WriteSet;
using stm::UndoLogEntry;
using stm::UndoLog;

typedef BitFilter<1024> filter_t;

#define NUM_STRIPES  1048576
int* write_locks;

__attribute__((always_inline))
inline int get_lock(void* addr)
{
    uintptr_t index = reinterpret_cast<uintptr_t>(addr);
    return write_locks[(index>>3) % NUM_STRIPES];
}

//__attribute__((always_inline))
inline void set_lock(void* addr)
{
    //uintptr_t index = 4445634345635337734657634LL;//(uintptr_t) addr; //reinterpret_cast<uintptr_t>(addr);
    long index = reinterpret_cast<long>(addr);
    uintptr_t i = (index>>3) % NUM_STRIPES;
  /*  if (write_locks[i])
    	_xabort(0);
    else*/
    	write_locks[i] = 1;
}

__attribute__((always_inline))
inline void release_lock(void* addr)
{
    uintptr_t index = reinterpret_cast<uintptr_t>(addr);
    write_locks[(index>>3) % NUM_STRIPES] = 0;
}

unsigned long long get_real_time() {
	struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);

    return time.tv_sec * 1000000000L + time.tv_nsec;
}

#define LIST_SIZE 100000

#define SPLIT_SIZE 50

int list[LIST_SIZE];


void *tx_fn(void *arg) {
	//ValueList      vlist(100000);
	filter_t rf;
	filter_t wf;
	//TODO we are using hash based undo log to make search and duplicates effecient for HTM. Try the trivial  undo log and compare
	//WriteSet undo_log(10000);
	//UndoLog undo_log(10000);
	UndoLogEntry* undo_log;

	undo_log = (UndoLogEntry*) malloc(10000*sizeof(UndoLogEntry));

	int undo_i;
	unsigned status;
	unsigned int seed = 78909;
	int limit = 0;
	unsigned long long time = get_real_time();
	while (limit < 10000) {
		limit++;
		//vlist.reset();
		rf.clear();
		wf.clear();
		undo_i =0;
		int val = rand_r_32(&seed) % LIST_SIZE;
		int act = rand_r_32(&seed) % 100;

		if (act < 33) {
			int more = 1;
			int initial = 0;
			while (more) {
				//vlist.reset();
				int limit = initial + SPLIT_SIZE;
				more = 0;
				if ((status = _xbegin()) == _XBEGIN_STARTED) {
					//malloc(1);
					//new int;
					int found = 0;
					int i;
					for (i=initial; i<LIST_SIZE; i++) {
						if (i == limit) {initial = limit; more = 1; break;}
						rf.add(&list[i]);
						//vlist.insert(ValueListEntry((void**) (&list[i]), (void*)list[i]));
						if ((list[i]) >= val)
							break;
					}
					if (!more) {
						found = ((i != LIST_SIZE) && ((list[i]) == val));
					}
					end1: _xend();
					//printf("Worked\n");
				} else {
					more = 1;
					//printf("status %d\n", status);
				}/* else {
					if (status == _XABORT_CONFLICT)
						printf("1 abort reason CONFLICT\n");
					else if (status == _XABORT_CAPACITY)
						printf("1 abort reason CAPACITY %d\n", val);
				}*/
			}
		}
		else if (act < 66) {
			int more = 1;
			int initial = 0;
			//int retry_limit =0;
			while (more) {
//				vlist.reset();
				int limit = initial + SPLIT_SIZE;
				more = 0;
				if ((status = _xbegin()) == _XBEGIN_STARTED) {
					// traverse the list to find the insertion point
					int i;
					for (i=initial; i<LIST_SIZE; i++) {
						if (i == limit) {initial = limit; more = 1; break;}
//						vlist.insert(ValueListEntry((void**) (&list[i]), (void*)list[i]));
						rf.add(&list[i]);
						if ((list[i]) >= val)
							break;
					}
					if (!more && i != LIST_SIZE) {
						wf.add(&list[i]);
						//TODO optimize in  one function call
				        //WriteSetEntry log(STM_WRITE_SET_ENTRY((void**)&list[i], NULL, mask));
				        //bool found = undo_log.find(log);
				        //if (!found) {
				        	//undo_log.insert(UndoLogEntry((void**)&list[i], (void*) list[i]));
				        	undo_log[undo_i++] = UndoLogEntry((void**)&list[i], (void*) list[i]);
				        	//set_lock(&list[i]);
				        //}
						//set_lock(&list[i]);
						//release_lock(&list[i]);
						list[i] =  val;//we overwrite to simulation linked list behavior
					}
					end2: _xend();
				} else {
					more = 1;
					//retry_limit++;
					//if (retry_limit > 10) break;
					//printf("status %d\n", status);
//					if (status == _XABORT_CONFLICT)
//											printf("2 abort reason CONFLICT\n");
//										else if (status & _XABORT_CAPACITY)
//											printf("2 abort reason CAPACITY\n");
//					printf("%u\n", status);
				}/* else {
					if (status == _XABORT_CONFLICT)
						printf("2 abort reason CONFLICT\n");
					else if (status == _XABORT_CAPACITY)
						printf("2 abort reason CAPACITY\n");
				}*/
			}
			//printf("limit %d\n", retry_limit);
		}
		else {
			int more = 1;
			int initial = 0;
			while (more) {
//				vlist.reset();
				int limit = initial + SPLIT_SIZE;
				more = 0;
				if ((status = _xbegin()) == _XBEGIN_STARTED) {
					// traverse the list to find the insertion point
					int i;
					for (i=initial; i<LIST_SIZE; i++) {
						if (i == limit) {initial = limit; more = 1; break;}
//						vlist.insert(ValueListEntry((void**) (&list[i]), (void*)list[i]));
						rf.add(&list[i]);
						if ((list[i]) >= val)
							break;
					}
					if (!more && i != LIST_SIZE) {
						wf.add(&list[i]);
						//set_lock(&list[i]);
						release_lock(&list[i]);
						list[i] =  i;//we do not delete to simulation linked list behavior
					}
					end3: _xend();
				} else {
					more = 1;
					//printf("status %d\n", status);
				}/* else {
					if (status == _XABORT_CONFLICT)
						printf("3 abort reason CONFLICT\n");
					else if (status == _XABORT_CAPACITY)
						printf("3 abort reason CAPACITY\n");
				}*/
			}
		}
	}
	time = get_real_time() - time;
	printf("Time = %llu\n", time);
}

//_XABORT_CONFLICT
//_XABORT_CAPACITY

int main(void)
{
	write_locks = new int[NUM_STRIPES];
	memset(write_locks, 0, sizeof(int)*NUM_STRIPES);

	int j;
	for (j=0; j<LIST_SIZE;j++)
		list[j] = j;

  pthread_t thread;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
	int x[100000] = {1};
	int i;

	pthread_create(&thread, &thread_attr, tx_fn, (void*)&x[10]);

	pthread_join(thread, NULL);
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
