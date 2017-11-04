//  Copyright (C) 2009 Tim Blechmann
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//[fifo_example
#include <boost/thread/thread.hpp>
#include <boost/atomic.hpp>
#include <boost/lockfree/fifo.hpp>
#include <iostream>
#include <pthread.h>

boost::atomic_int producer_count(0);
boost::atomic_int consumer_count(0);

boost::lockfree::fifo<int> fifo;

const int iterations = 10000000;
const int producer_thread_count = 4;
const int consumer_thread_count = 4;

void producer(void)
{
    for (int i = 0; i != iterations; ++i) {
        int value = ++producer_count;
        while (!fifo.enqueue(value))
            ;
    }
}

boost::atomic<bool> done (false);
void consumer(void)
{
    int value;
    while (!done) {
        while (fifo.dequeue(value))
            ++consumer_count;
    }

    while (fifo.dequeue(value))
        ++consumer_count;
}

int main(int argc, char* argv[])
{
    using namespace std;
    cout << "boost::lockfree::fifo is ";
    if (!fifo.is_lock_free())
        cout << "not ";
    cout << "lockfree" << endl;

//    pthread_t thread1, thread2, thread3, thread4;
//    pthread_t thread11, thread22, thread33, thread44;
//     pthread_attr_t thread_attr;
//     pthread_attr_init(&thread_attr);
//
//   	pthread_create(&thread1, &thread_attr, producer, (void*)45678);
//   	pthread_create(&thread2, &thread_attr, producer, (void*)4968147);
//   	pthread_create(&thread3, &thread_attr, producer, (void*)49147);
//   	pthread_create(&thread4, &thread_attr, producer, (void*)4967);
//
//   	pthread_create(&thread11, &thread_attr, consumer, (void*)45678);
//   	pthread_create(&thread22, &thread_attr, consumer, (void*)4968147);
//   	pthread_create(&thread33, &thread_attr, consumer, (void*)49147);
//   	pthread_create(&thread44, &thread_attr, consumer, (void*)4967);
//
//
//
//   	pthread_join(thread1, NULL);
//   	pthread_join(thread2, NULL);
//   	pthread_join(thread3, NULL);
//   	pthread_join(thread4, NULL);

    boost::thread_group producer_threads, consumer_threads;

    for (int i = 0; i != producer_thread_count; ++i)
        producer_threads.create_thread(producer);

    for (int i = 0; i != consumer_thread_count; ++i)
        consumer_threads.create_thread(consumer);

    producer_threads.join_all();
    done = true;

    consumer_threads.join_all();

//   	pthread_join(thread11, NULL);
//   	pthread_join(thread22, NULL);
//   	pthread_join(thread33, NULL);
//   	pthread_join(thread44, NULL);


    cout << "produced " << producer_count << " objects." << endl;
    cout << "consumed " << consumer_count << " objects." << endl;
}
//]
