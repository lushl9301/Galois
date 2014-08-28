/** Kruskal MST -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2011, The University of Texas at Austin. All rights reserved.
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
 * @section Description
 *
 * Kruskal MST.
 *
 * @author <ahassaan@ices.utexas.edu>
 */

#ifndef KRUSKALSTRICTOBIM_H
#define KRUSKALSTRICTOBIM_H

#include "Galois/Graph/Graph.h"
#include "Galois/Runtime/LevelExecutor.h"
#include "Galois/WorkList/WorkListExperimental.h"

#include "Kruskal.h"
#include "KruskalParallel.h"


namespace kruskal {


class KruskalStrictOBIM: public Kruskal {
  protected:

  typedef Galois::Graph::FirstGraph<void,void,false> Graph;
  typedef Graph::GraphNode Lockable;
  typedef std::vector<Lockable> VecLocks;

  virtual const std::string getVersion () const { return "Parallel Kruskal using Strict OBIM"; }
  
  struct FindLoopSpec {

    Graph& graph;
    VecLocks& locks;
    VecRep& repVec;
    Accumulator& findIter;


    FindLoopSpec (
        Graph& graph,
        VecLocks& locks,
        VecRep& repVec,
        Accumulator& findIter)
      :
        graph (graph),
        locks (locks),
        repVec (repVec),
        findIter (findIter)

    {}


    template <typename C>
    void operator () (const Edge& e, C& ctx) {
      int repSrc = kruskal::findPCiter_int (e.src, repVec);
      int repDst = kruskal::findPCiter_int (e.dst, repVec);
      // int repSrc = kruskal::getRep_int (e.src, repVec);
      // int repDst = kruskal::getRep_int (e.dst, repVec);
      

      if (repSrc != repDst) {
        graph.getData (locks[repSrc]);
        graph.getData (locks[repDst]);
      }

      findIter += 1;
    }
  };


  struct LinkUpLoopSpec {

    static const unsigned CHUNK_SIZE = 64;

    VecRep& repVec;
    Accumulator& mstSum;
    Accumulator& linkUpIter;

    LinkUpLoopSpec (
        VecRep& repVec,
        Accumulator& mstSum,
        Accumulator& linkUpIter)
      :
        repVec (repVec),
        mstSum (mstSum),
        linkUpIter (linkUpIter)

    {}


    template <typename C>
    void operator () (const Edge& e, C& ctx) {
      int repSrc = kruskal::findPCiter_int (e.src, repVec);
      int repDst = kruskal::findPCiter_int (e.dst, repVec);

      // int repSrc = kruskal::getRep_int (e.src, repVec);
      // int repDst = kruskal::getRep_int (e.dst, repVec);

      if (repSrc != repDst) {
        unionByRank_int (repSrc, repDst, repVec);
        linkUpIter += 1;
        mstSum += e.weight;
      }

    }
  };

  struct GetWeight: public std::unary_function<Edge,Weight_ty> {
    Weight_ty operator () (const Edge& e) const {
      return e.weight;
    }
  };

  virtual void runMST (const size_t numNodes, VecEdge& edges,
      size_t& mstWeight, size_t& totalIter) {

    Graph graph;
    VecLocks locks;
    locks.resize(numNodes);
    Galois::do_all(boost::counting_iterator<size_t>(0), boost::counting_iterator<size_t>(numNodes), [&](size_t i) {
      locks[i] = graph.createNode();
      graph.addNode(locks[i]);
    });
      
    VecRep repVec (numNodes, -1);
    Accumulator findIter;
    Accumulator linkUpIter;
    Accumulator mstSum;

    FindLoopSpec findLoop (graph, locks, repVec, findIter);
    LinkUpLoopSpec linkUpLoop (repVec, mstSum, linkUpIter);

    Galois::TimeAccumulator runningTime;

    runningTime.start ();
    typedef Galois::WorkList::OrderedByIntegerMetric<GetWeight, Galois::WorkList::ChunkedFIFO<128>>::with_barrier<true>::type WL;
    Galois::for_each(edges.begin(), edges.end(),
        [&](const Edge& e, Galois::UserContext<Edge>& ctx) {
          findLoop(e, ctx);
          linkUpLoop(e, ctx);
    }, Galois::wl<WL>());
    //}, Galois::wl<Galois::WorkList::StableIterator<>>());
    //Galois::Runtime::for_each_ordered_level (
    //    Galois::Runtime::makeStandardRange (edges.begin (), edges.end ()),
    //    GetWeight (), std::less<Weight_ty> (), findLoop, linkUpLoop);

    runningTime.stop ();

    mstWeight = mstSum.reduce ();
    totalIter = findIter.reduce ();

    std::cout << "Number of FindLoop iterations = " << findIter.reduce () << std::endl;
    std::cout << "Number of LinkUpLoop iterations = " << linkUpIter.reduce () << std::endl;

    std::cout << "MST running time without initialization/destruction: " << runningTime.get () << std::endl;
  }
};



}// end namespace kruskal




#endif //  KRUSKAL_LEVEL_EXEC_H

