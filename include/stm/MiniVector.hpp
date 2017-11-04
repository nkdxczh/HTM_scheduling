/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

/**
 *  A simple vector-like templated collection object.  The main difference
 *  from the STL vector is that we can force uncommon code (such as resize)
 *  to be a function call by putting instantiations of the expand() method
 *  into their own .o file.
 */

#ifndef MINIVECTOR_HPP__
#define MINIVECTOR_HPP__

#include <cassert>
#include <cstdlib>
#include <string.h>
//#include <common/platform.hpp>

namespace
{
  /**
   * We use malloc a couple of times here, and this makes it a bit easier
   */
  template <typename T>
  inline T* typed_malloc(size_t N)
  {
      return static_cast<T*>(malloc(sizeof(T) * N));
  }
}

namespace stm
{
  /***  Self-growing array */
  template <class T>
  class MiniVector
  {
      unsigned long m_cap;            // current vector capacity
      unsigned long m_size;           // current number of used elements
      T* m_elements;                  // the actual elements in the vector

      /*** double the size of the minivector */
      void expand();


    public:

      /*** Construct a minivector with a default size */
      MiniVector(const unsigned long capacity)
          : m_cap(capacity), m_size(0),
            m_elements(static_cast<T*>(malloc(sizeof(T)*m_cap)))
      {
          assert(m_elements);
      }

      ~MiniVector() { free(m_elements); }

      /*** Reset the vector without destroying the elements it holds */
      inline void reset() { m_size = 0; }

      /*** Insert an element into the minivector */
      inline void insert(T data)
      {
          // NB: There is a tradeoff here.  If we put the element into the list
          // first, we are going to have to copy one more object any time we
          // double the list.  However, by doing things in this order we avoid
          // constructing /data/ on the stack if (1) it has a simple
          // constructor and (2) /data/ isn't that big relative to the number
          // of available registers.

          // Push data onto the end of the array and increment the size
          m_elements[m_size++] = data;

          // If the list is full, double the list size, allocate a new array
          // of elements, bitcopy the old array into the new array, and free
          // the old array. No destructors are called.
          if (m_size != m_cap)
              return;
          //expand();
      }

      /*** Simple getter to determine the array size */
      inline unsigned long size() const { return m_size; }

      /*** iterator interface, just use a basic pointer */
      typedef T* iterator;

      /*** iterator to the start of the array */
      inline iterator begin() const { return m_elements; }

      /*** iterator to the end of the array */
      inline iterator end() const { return m_elements + m_size; }

  }; // class MiniVector


  template <class T>
    void MiniVector<T>::expand()
    {
        T* temp = m_elements;
        m_cap *= 2;
        m_elements = typed_malloc<T>(m_cap);
        assert(m_elements);
        memcpy(m_elements, temp, sizeof(T)*m_size);
        free(temp);
    }

    /***  must instantiate explicitly! */
//    template void MiniVector<void*>::expand();
//template void MiniVector<orec_t*>::expand();
//    template void MiniVector<bytelock_t*>::expand();
//    template void MiniVector<bitlock_t*>::expand();
//    template void MiniVector<WriteSetEntry>::expand();
//    template void MiniVector<rrec_t*>::expand();
//    template void MiniVector<nanorec_t>::expand();
//    template void MiniVector<qtable_t>::expand();

//    template void MiniVector<UndoLogEntry>::expand();

} // stm

#endif // MINIVECTOR_HPP__
