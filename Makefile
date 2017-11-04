CFLAGS := -I include -I .
CPPFLAGS := -I include -I .
CLEAN := ignore-xend.so assert-test assert-test.o has-tsx tsx-assert.o

all: htm htm_conflict htm_sched htm_bank htm_rob_sched TPCcHTM

ignore-xend.so: ignore-xend.c
	gcc -fPIC -shared -g -o ignore-xend.so ignore-xend.c

assert-test: assert-test.o tsx-assert.o
	gcc -o assert-test assert-test.o tsx-assert.o -lpthread
	
htm: htm.o
	g++ -o htm htm.o -lpthread -O3 -ggdb

htm_conflict: htm_conflict.o
	g++ -o htm_conflict htm_conflict.o -lpthread -O3 -ggdb
	
htm_sched: htm_sched.o
	g++ -o htm_sched htm_sched.o -lpthread -lboost_thread -lboost_system -ggdb

htm_rob_sched: htm_rob_sched.o
	g++ -o htm_rob_sched htm_rob_sched.o -lpthread -lboost_thread -lboost_system -ggdb

htm_bank: htm_bank.o
	g++ -o htm_bank htm_bank.o -lpthread

TPCcHTM: TPCcHTM.o
	g++ -o TPCcHTM TPCcHTM.o -lpthread -O3 -g

clean:
	rm -f ${CLEAN}

