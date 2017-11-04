/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#ifndef STM_VALUE_LIST_HPP
#define STM_VALUE_LIST_HPP

/**
 *  We use the ValueList class to log address/value pairs for our
 *  value-based-validation implementations---NOrec and NOrecPrio currently. We
 *  generally log things at word granularity, and during validation we check to
 *  see if any of the bits in the word has changed since the word was originally
 *  read. If they have, then we have a conflict.
 *
 *  This word-granularity continues to be correct when we have enabled byte
 *  logging (because we're building for C++ TM compatibility), but it introduces
 *  the possibility of byte-level false conflicts. One of VBV's advantages is
 *  that there are no false conflicts. In order to preserve this behavior, we
 *  offer the user the option to use the byte-mask (which is already enabled for
 *  byte logging) to do byte-granularity validation. The disadvantage to this
 *  technique is that the read log entry size is increased by the size of the
 *  stored mask (we could optimize for 64-bit Linux and pack the mask into an
 *  unused part of the logged address, but we don't yet have this capability).
 *
 *  This file implements the value log given the current configuration settings
 *  in stm/config.h
 */
#include "stm/MiniVector.hpp"

namespace stm {
  /**
   *  When we're word logging we simply store address/value pairs in the
   *  ValueList.
   */
  class WordLoggingValueListEntry {
      void** addr;
      void* val;

    public:
      WordLoggingValueListEntry(void** a, void* v) : addr(a), val(v) {
      }

      /**
       *  When word logging, we can just check if the address still has the
       *  value that we read earlier.
       */
      bool isValid() const {
          return *addr == val;
      }
  };

  typedef WordLoggingValueListEntry ValueListEntry;

  struct ValueList : public MiniVector<ValueListEntry> {
      ValueList(const unsigned long cap) : MiniVector<ValueListEntry>(cap) {
      }
  };
  template void MiniVector<ValueListEntry>::expand();
}

#endif // STM_VALUE_LIST_HPP
