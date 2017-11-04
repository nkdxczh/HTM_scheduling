#include "tsx-assert.h"
#include "rtm.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

int foo = 100;

void *bla(void *arg) {
	int *passedLocal = (long)arg;
	int kk;
	for (kk=0; kk<100; kk++){
		unsigned status;
		int x[100000] = {1};
		int i;
		if ((status = _xbegin()) == _XBEGIN_STARTED) {
			//*passedLocal = 54;
			for (i=0; i<4000;i++) {
				x[i]=i;
			}
			_xend();
			printf("2 worked\n");
		}
		if (status == _XABORT_CONFLICT)
			printf("2 abort reason CONFLICT\n");
		else if (status == _XABORT_CAPACITY)
			printf("2 abort reason CAPACITY\n");
		else  if (status != _XBEGIN_STARTED)
			printf("2 abort reason %d\n", status);
	}
}

//_XABORT_CONFLICT
//_XABORT_CAPACITY

int main(void)
{
  pthread_t thread;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);

	unsigned status;
	int x[100000] = {1};
	int i;

//	pthread_create(&thread, &thread_attr, bla, (void*)&x[10]);

int kk;

for (kk=0; kk<1000; kk++){
	if ((status = _xbegin()) == _XBEGIN_STARTED) { 
	//	tsx_assert(foo);
	//	x[5] = 10;
	//	int y[1000] = {0};
	//	foo = 1;
	//	_xabort(0);
		//4760.
//		malloc(1);
		int ll;
		for (i=0; i<400;i++) {
			x[i] = i;
		}
		for (i=100000 - 400; i<100000;i++) {
			x[i] = i;
		}
		_xend();
		//printf("1 Worked!\n");
	} else {
		if (status == _XABORT_CONFLICT)
			printf("1 abort reason CONFLICT\n");
		else if (status == _XABORT_CAPACITY)
			printf("1 abort reason CAPACITY\n");
		else
			printf("1 abort reason %u\n", status);
	}
//	printf("i final value = %d\n", i);
//	printf ("foo =%d, x=%d\n", foo, x[10]);
	//usleep(100);
}
	//bla();
	return 0;
}
