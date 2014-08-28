/** Stable Iterator worklist -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section description
 * This dereferences iterators lazily.  This is only safe if they are not
 * invalidated by the operator
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#ifndef GALOIS_WORKLIST_STABLEITERATOR_H
#define GALOIS_WORKLIST_STABLEITERATOR_H

#include "Galois/gstl.h"
#include "Galois/WorkList/Chunked.h"

namespace Galois {
namespace WorkList {

/**
 * Low-overhead worklist when initial range is not invalidated by the
 * operator.
 *
 * @tparam Steal     Try workstealing on initial ranges
 * @tparam Container Worklist to manage work enqueued by the operator
 * @tparam Iterator  (inferred by library)
 */
template<bool Steal = false, typename Container = dChunkedFIFO<>, typename Iterator=int*>
struct StableIterator {
  typedef typename std::iterator_traits<Iterator>::value_type value_type;
  typedef Iterator iterator;

  //! change the concurrency flag
  template<bool _concurrent>
  struct rethread { typedef StableIterator<Steal, Container, Iterator> type; };
  
  //! change the type the worklist holds
  template<typename _T>
  struct retype { typedef StableIterator<Steal, typename Container::template retype<_T>::type, Iterator> type; };

  template<typename _iterator>
  struct with_iterator { typedef StableIterator<Steal, Container, _iterator> type; };

  template<bool _steal>
  struct with_steal { typedef StableIterator<_steal, Container, Iterator> type; };

  template<typename _container>
  struct with_container { typedef StableIterator<Steal, _container, Iterator> type; };

private:
  struct shared_state {
    Iterator stealBegin;
    Iterator stealEnd;
    Runtime::LL::SimpleLock stealLock;
    bool stealAvail;
  };

  struct state {
    Runtime::LL::CacheLineStorage<shared_state> stealState;
    Iterator localBegin;
    Iterator localEnd;
    unsigned int nextVictim;
    unsigned int numStealFailures;
    
    void populateSteal() {
      if (Steal && localBegin != localEnd) {
	shared_state& s = stealState.data;
	s.stealLock.lock();
	s.stealEnd = localEnd;
	s.stealBegin = localEnd = Galois::split_range(localBegin, localEnd);
	if (s.stealBegin != s.stealEnd)
          s.stealAvail = true;
	s.stealLock.unlock();
      }
    }
  };

  Runtime::PerThreadStorage<state> TLDS;
  Container inner;

  bool doSteal(state& dst, state& src, bool wait) {
    shared_state& s = src.stealState.data;
    if (s.stealAvail) {
      if (wait) {
        s.stealLock.lock();
      } else if (!s.stealLock.try_lock()) {
        return false;
      }
      if (s.stealBegin != s.stealEnd) {
	dst.localBegin = s.stealBegin;
	s.stealBegin = dst.localEnd = Galois::split_range(s.stealBegin, s.stealEnd);
        s.stealAvail = s.stealBegin != s.stealEnd;
      }
      s.stealLock.unlock();
    }
    return dst.localBegin != dst.localEnd;
  }

  //pop already failed, try again with stealing
  Galois::optional<value_type> pop_steal(state& data) {
    //always try stealing self
    if (doSteal(data, data, true))
      return *data.localBegin++;
    //only try stealing one other
    if (doSteal(data, *TLDS.getRemote(data.nextVictim), false)) {
      //share the wealth
      if (data.nextVictim != Runtime::LL::getTID())
	data.populateSteal();
      return *data.localBegin++;
    }
    ++data.nextVictim;
    ++data.numStealFailures;
    data.nextVictim %= Runtime::activeThreads;
    return Galois::optional<value_type>();
  }

public:
  //! push initial range onto the queue
  //! called with the same b and e on each thread
  template<typename RangeTy>
  void push_initial(const RangeTy& r) {
    state& data = *TLDS.getLocal();
    auto lp = r.local_pair();
    data.localBegin = lp.first;
    data.localEnd = lp.second;
    data.nextVictim = Runtime::LL::getTID();
    data.numStealFailures = 0;
    data.populateSteal();
  }

  //! pop a value from the queue.
  Galois::optional<value_type> pop() {
    state& data = *TLDS.getLocal();
    if (data.localBegin != data.localEnd)
      return *data.localBegin++;

    Galois::optional<value_type> item;
    if (Steal && 2 * data.numStealFailures > Runtime::activeThreads)
      if ((item = pop_steal(data)))
        return item;
    if ((item = inner.pop()))
      return item;
    if (Steal)
      return pop_steal(data);
    return item;
  }

  void push(const value_type& val) {
    inner.push(val);
  }

  template<typename Iter>
  void push(Iter b, Iter e) {
    while (b != e)
      push(*b++);
  }
};
GALOIS_WLCOMPILECHECK(StableIterator)

}
}
#endif
