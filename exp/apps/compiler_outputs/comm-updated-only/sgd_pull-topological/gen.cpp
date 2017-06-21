/** SGD -*- C++ -*-
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
 * @section Description
 *
 * Compute SGD (matrix completion) on distributed Galois.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 */

#include <iostream>
#include <limits>
#include <cmath>
#include "Galois/Galois.h"
#include "Galois/gstl.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Runtime/CompilerHelperFunctions.h"

#include "Galois/Runtime/dGraph_edgeCut.h"
#include "Galois/Runtime/dGraph_cartesianCut.h"
#include "Galois/Runtime/dGraph_hybridCut.h"

#include "Galois/DistAccumulator.h"
#include "Galois/Runtime/Tracer.h"

enum VertexCut {
  PL_VCUT, CART_VCUT
};

#ifdef __GALOIS_HET_CUDA__
#include "gen_cuda.h"
struct CUDA_Context *cuda_ctx;

enum Personality {
   CPU, GPU_CUDA, GPU_OPENCL
};
std::string personality_str(Personality p) {
   switch (p) {
   case CPU:
      return "CPU";
   case GPU_CUDA:
      return "GPU_CUDA";
   case GPU_OPENCL:
      return "GPU_OPENCL";
   }
   assert(false&& "Invalid personality");
   return "";
}
#endif

static const char* const name = "SGD - Distributed Heterogeneous";
static const char* const desc = "SGD on Distributed Galois.";
static const char* const url = 0;

namespace cll = llvm::cl;
static cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file (Transpose graph)>"), cll::Required);
static cll::opt<std::string> partFolder("partFolder", cll::desc("path to partitionFolder"), cll::init(""));
static cll::opt<bool> transpose("transpose", cll::desc("transpose the graph in memory after partitioning"), cll::init(false));
static cll::opt<unsigned int> maxIterations("maxIterations", cll::desc("Maximum iterations: Default 10000"), cll::init(10000));
static cll::opt<unsigned int> src_node("srcNodeId", cll::desc("ID of the source node"), cll::init(0));
static cll::opt<bool> verify("verify", cll::desc("Verify ranks by printing to 'page_ranks.#hid.csv' file"), cll::init(false));

static cll::opt<bool> enableVCut("enableVertexCut", cll::desc("Use vertex cut for graph partitioning."), cll::init(false));
static cll::opt<bool> bipartite("bipartite", cll::desc("Is graph bipartite? if yes, it expects first N nodes to have edges."), cll::init(true));

static cll::opt<unsigned int> VCutThreshold("VCutThreshold", cll::desc("Threshold for high degree edges."), cll::init(100));
static cll::opt<VertexCut> vertexcut("vertexcut", cll::desc("Type of vertex cut."),
       cll::values(clEnumValN(PL_VCUT, "pl_vcut", "Powerlyra Vertex Cut"), clEnumValN(CART_VCUT , "cart_vcut", "Cartesian Vertex Cut"), clEnumValEnd),
       cll::init(PL_VCUT));

#ifdef __GALOIS_HET_CUDA__
static cll::opt<int> gpudevice("gpu", cll::desc("Select GPU to run on, default is to choose automatically"), cll::init(-1));
static cll::opt<Personality> personality("personality", cll::desc("Personality"),
      cll::values(clEnumValN(CPU, "cpu", "Galois CPU"), clEnumValN(GPU_CUDA, "gpu/cuda", "GPU/CUDA"), clEnumValN(GPU_OPENCL, "gpu/opencl", "GPU/OpenCL"), clEnumValEnd),
      cll::init(CPU));
static cll::opt<std::string> personality_set("pset", cll::desc("String specifying personality for each host. 'c'=CPU,'g'=GPU/CUDA and 'o'=GPU/OpenCL"), cll::init(""));
static cll::opt<unsigned> scalegpu("scalegpu", cll::desc("Scale GPU workload w.r.t. CPU, default is proportionally equal workload to CPU and GPU (1)"), cll::init(1));
static cll::opt<unsigned> scalecpu("scalecpu", cll::desc("Scale CPU workload w.r.t. GPU, default is proportionally equal workload to CPU and GPU (1)"), cll::init(1));
#endif

#define LATENT_VECTOR_SIZE 20
static const double LEARNING_RATE = 0.001; // GAMMA, Purdue: 0.01 Intel: 0.001
static const double DECAY_RATE = 0.9; // STEP_DEC, Purdue: 0.1 Intel: 0.9
static const double LAMBDA = 0.001; // Purdue: 1.0 Intel: 0.001


const unsigned int infinity = std::numeric_limits<unsigned int>::max()/4;
unsigned iteration;

struct NodeData {
  std::array<double, LATENT_VECTOR_SIZE> latent_vector;
  //NodeData():latent_vector(LATENT_VECTOR_SIZE){}
  //std::vector<double> latent_vector;
  uint64_t updates;
  uint64_t edge_offset;
};


#if 0
typedef hGraph<NodeData, double> Graph;
typedef hGraph_edgeCut<NodeData, double> Graph_edgeCut;
typedef hGraph_vertexCut<NodeData, double> Graph_vertexCut;
typedef hGraph_cartesianCut<NodeData, double> Graph_cartesianCut;
#endif

typedef hGraph<NodeData, uint32_t> Graph;
typedef hGraph_edgeCut<NodeData, uint32_t> Graph_edgeCut;
typedef hGraph_vertexCut<NodeData, uint32_t> Graph_vertexCut;
typedef hGraph_cartesianCut<NodeData, uint32_t> Graph_cartesianCut;

typedef typename Graph::GraphNode GNode;

static double genRand () {
  // generate a random double in (-1,1)
  return 2.0 * ((double)std::rand () / (double)RAND_MAX) - 1.0;
}

double getstep_size(unsigned int round) {
  return LEARNING_RATE * 1.5 / (1.0 + DECAY_RATE * pow(round + 1, 1.5));
}

static const double MINVAL = -1e+100;
static const double MAXVAL = 1e+100;
double calcPrediction (const NodeData& movie_data, const NodeData& user_data) {

#if 0
	double init_value = 0.0;
	auto jj = user_data.latent_vector.begin();
	for(auto ii = movie_data.latent_vector.begin(); ii != movie_data.latent_vector.end(); ++ii, ++jj){
	      init_value += (*ii) * (*jj);
	    }
#endif

  double pred = Galois::innerProduct(movie_data.latent_vector, user_data.latent_vector, 0.0); //init_value; //(double) Galois::innerProduct(movie_data.latent_vector.begin(),  movie_data.latent_vector.begin(),user_data.latent_vector.begin(),0.0);
  double p = pred;

  pred = std::min (MAXVAL, pred);
  pred = std::max (MINVAL, pred);
  if (p != pred)
    std::cerr << "clamped " << p << " to " << pred << "\n";
  return pred;
}

struct InitializeGraph {
  Graph *graph;

  InitializeGraph(Graph* _graph) : graph(_graph){}

  void static go(Graph& _graph) {
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		InitializeGraph_cuda(cuda_ctx);
    	} else if (personality == CPU)
    #endif
    	struct SyncerPull_0 {
    		static unsigned long extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_updates_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.updates;
    		}
    		static bool extract_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t *s, DataCommMode *data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_node_updates_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool extract_batch(unsigned from_id, unsigned long *y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_node_updates_cuda(cuda_ctx, from_id, y); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static void setVal (uint32_t node_id, struct NodeData & node, unsigned long y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_updates_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				node.updates = y;
    		}
    		static bool setVal_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t s, DataCommMode data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_set_node_updates_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		typedef unsigned long ValTy;
    	};
    	struct SyncerPull_1 {
    		static unsigned long extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_edge_offset_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.edge_offset;
    		}
    		static bool extract_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t *s, DataCommMode *data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_node_edge_offset_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool extract_batch(unsigned from_id, unsigned long *y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_node_edge_offset_cuda(cuda_ctx, from_id, y); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static void setVal (uint32_t node_id, struct NodeData & node, unsigned long y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_edge_offset_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				node.edge_offset = y;
    		}
    		static bool setVal_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t s, DataCommMode data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_set_node_edge_offset_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		typedef unsigned long ValTy;
    	};
    	struct SyncerPull_2 {
        //static std::vector<double> extract(uint32_t node_id, const struct NodeData & node) {
    		static std::array<double, LATENT_VECTOR_SIZE> extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_latent_vector_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.latent_vector;
    		}
    		static bool extract_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t *s, DataCommMode *data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool extract_batch(unsigned from_id, std::array<double, LATENT_VECTOR_SIZE> *y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_node_latent_vector_cuda(cuda_ctx, from_id, y); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static void setVal (uint32_t node_id, struct NodeData & node, std::array<double, LATENT_VECTOR_SIZE> y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_latent_vector_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				node.latent_vector = y;
            }
    		static bool setVal_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t s, DataCommMode data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_set_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		typedef std::array<double, LATENT_VECTOR_SIZE> ValTy;
    	};
    	struct Syncer_vertexCut_0 {
    		static unsigned long extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_updates_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.updates;
    		}
    		static bool extract_reset_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t *s, DataCommMode *data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_slave_node_updates_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool extract_reset_batch(unsigned from_id, unsigned long *y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_slave_node_updates_cuda(cuda_ctx, from_id, y); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool reduce (uint32_t node_id, struct NodeData & node, unsigned long y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_updates_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				{ Galois::set(node.updates, y);}
            return true;
    		}
    		static bool reduce_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t s, DataCommMode data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_set_node_updates_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static void reset (uint32_t node_id, struct NodeData & node ) {
    		}
    		typedef unsigned long ValTy;
    	};
    	struct Syncer_vertexCut_1 {
    		static unsigned long extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_edge_offset_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.edge_offset;
    		}
    		static bool extract_reset_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t *s, DataCommMode *data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_slave_node_edge_offset_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool extract_reset_batch(unsigned from_id, unsigned long *y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_slave_node_edge_offset_cuda(cuda_ctx, from_id, y); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool reduce (uint32_t node_id, struct NodeData & node, unsigned long y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_edge_offset_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				{ Galois::set(node.edge_offset, y); }
            return true;
    		}
    		static bool reduce_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, unsigned long *y, size_t s, DataCommMode data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_set_node_edge_offset_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static void reset (uint32_t node_id, struct NodeData & node ) {
    		}
    		typedef unsigned long ValTy;
    	};
    	struct Syncer_vertexCut_2 {
    		static std::array<double, LATENT_VECTOR_SIZE> extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_latent_vector_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.latent_vector;
    		}
    		static bool extract_reset_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t *s, DataCommMode *data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_slave_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool extract_reset_batch(unsigned from_id, std::array<double, LATENT_VECTOR_SIZE> *y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_get_slave_node_latent_vector_cuda(cuda_ctx, from_id, y); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static bool reduce (uint32_t node_id, struct NodeData & node, std::array<double, LATENT_VECTOR_SIZE> y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_latent_vector_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				{ Galois::set(node.latent_vector, y); }
          return true;
    		}
    		static bool reduce_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t s, DataCommMode data_mode) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) { batch_set_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
    			assert (personality == CPU);
    		#endif
    			return false;
    		}
    		static void reset (uint32_t node_id, struct NodeData & node ) {
    		}
    		typedef std::array<double, LATENT_VECTOR_SIZE> ValTy;
    	};
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		std::string impl_str("CUDA_DO_ALL_IMPL_InitializeGraph_" + (_graph.get_run_identifier()));
    		Galois::StatTimer StatTimer_cuda(impl_str.c_str());
    		StatTimer_cuda.start();
    		InitializeGraph_all_cuda(cuda_ctx);
    		StatTimer_cuda.stop();
    	} else if (personality == CPU)
    #endif

    	_graph.sync_forward<Syncer_vertexCut_0, SyncerPull_0>("InitializeGraph");
    	_graph.sync_forward<Syncer_vertexCut_1, SyncerPull_1>("InitializeGraph");
    	_graph.sync_forward<Syncer_vertexCut_2, SyncerPull_2>("InitializeGraph");

    Galois::do_all(_graph.begin(), _graph.end(), InitializeGraph {&_graph}, Galois::loopname("Init"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "updates" , "unsigned long" , "set",  ""), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "edge_offset" , "unsigned long" , "set",  ""), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "latent_vector" , "std::array<double, LATENT_VECTOR_SIZE>" , "set",  ""));
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.updates = 0;
    sdata.edge_offset = 0;

    for(int i = 0; i < LATENT_VECTOR_SIZE; i++) {
      sdata.latent_vector[i] = genRand();
      if(!std::isnormal(sdata.latent_vector[i]))
        std::cout << "GEN for " << i << sdata.latent_vector[i] << "\n";
    }
  }

};

struct SGD_doPartialGradientUpdate {
  Graph* graph;
  double step_size;

  SGD_doPartialGradientUpdate(Graph* _graph, double _step_size) : graph(_graph), step_size(_step_size) {}
  void static go(Graph& _graph, double _step_size) {

#if 0
	  std::deque<GNode> Movies;
	     for (auto ii = g.begin(), ee = g.end(); ii != ee; ++ii)
	       if (g.edge_begin(*ii) != g.edge_end(*ii))
	         Movies.push_back(*ii);
#endif
      //Galois::do_all(_graph.begin(), _graph.end(), SGD_doPartialGradientUpdate { &_graph, _step_size }, Galois::loopname("SGD_doPartialGradientUpdate"));
      Galois::for_each(_graph.begin(), _graph.end(), SGD_doPartialGradientUpdate (&_graph, _step_size));
  }
  template<typename Context>
  void operator()(GNode src , Context& cnx) const {
    NodeData& sdata= graph->getData(src);
    auto& movie_node = sdata.latent_vector;

    for (auto jj = graph->edge_begin(src), ej = graph->edge_end(src); jj != ej; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& ddata = graph->getData(dst);
      auto& user_node = ddata.latent_vector;
      auto& sdata_up = sdata.updates;
      double edge_rating = graph->getEdgeData(dst);

      //doGradientUpdate
     double old_dp = Galois::innerProduct(user_node.begin(), user_node.end(), movie_node.begin(), 0.0);
     double cur_error = edge_rating - old_dp;
     if (Galois::Runtime::getSystemNetworkInterface().ID == 0) {
           //   std::cout << " Cur_error : " << cur_error << "\n";
      }
     assert(cur_error < 1000 && cur_error > -1000);
     for(int i = 0; i < LATENT_VECTOR_SIZE; ++i) {
       double prevUser = user_node[i];
       double prevMovie = movie_node[i];

       user_node[i] += step_size * (cur_error * prevMovie - LAMBDA * prevUser);
       assert(std::isnormal(user_node[i]));
       movie_node[i] += step_size * (cur_error * prevUser - LAMBDA * prevMovie);
       assert(std::isnormal(movie_node[i]));
     }
    }
  }
};


struct SGD {
  Graph* graph;
  double step_size;
  static Galois::DGAccumulator<double> DGAccumulator_accum;

  SGD(Graph* _graph, double _step_size) : graph(_graph),step_size(_step_size) {}
  void static go(Graph& _graph){
    iteration = 0;
    double rms_normalized = 0.0;
    do{
      auto step_size = getstep_size(iteration);
      SGD_doPartialGradientUpdate::go(_graph,step_size);
      DGAccumulator_accum.reset();
      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		int __retval = 0;
      		SGD_cuda(__retval, cuda_ctx);
      		DGAccumulator_accum += __retval;
      	} else if (personality == CPU)
      #endif
      	struct SyncerPull_0 {
      		static std::array<double, LATENT_VECTOR_SIZE> extract(uint32_t node_id, const struct NodeData & node) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) return get_node_latent_vector_cuda(cuda_ctx, node_id);
      			assert (personality == CPU);
      		#endif
      			//std::cout << "SGD:: SyncerPull_0\n";
      			return node.latent_vector;
      		}
      		static bool extract_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t *s, DataCommMode *data_mode) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) { batch_get_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
      			assert (personality == CPU);
      		#endif
      			return false;
      		}
      		static bool extract_batch(unsigned from_id, std::array<double, LATENT_VECTOR_SIZE> *y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) { batch_get_node_latent_vector_cuda(cuda_ctx, from_id, y); return true; }
      			assert (personality == CPU);
      		#endif
      			return false;
      		}
      		static void setVal (uint32_t node_id, struct NodeData & node, std::array<double, LATENT_VECTOR_SIZE> y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) set_node_latent_vector_cuda(cuda_ctx, node_id, y);
      			else if (personality == CPU)
      		#endif
      				node.latent_vector = y;
      		}
      		static bool setVal_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t s, DataCommMode data_mode) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) { batch_set_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
      			assert (personality == CPU);
      		#endif
      			return false;
      		}
      		typedef std::array<double, LATENT_VECTOR_SIZE> ValTy;
      	};

      	struct Syncer_vertexCut_2 {
      		static std::array<double, LATENT_VECTOR_SIZE> extract(uint32_t node_id, const struct NodeData & node) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) return get_node_latent_vector_cuda(cuda_ctx, node_id);
      			assert (personality == CPU);
      		#endif
      			return node.latent_vector;
      		}
      		static bool extract_reset_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t *s, DataCommMode *data_mode) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) { batch_get_reset_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode, {Galois::resetVec(node.latent_vector); }); return true; }
      			assert (personality == CPU);
      		#endif
      			return false;
      		}
      		static bool extract_reset_batch(unsigned from_id, std::array<double, LATENT_VECTOR_SIZE> *y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) { batch_get_reset_node_latent_vector_cuda(cuda_ctx, from_id, y, {Galois::resetVec(node.latent_vector); }); return true; }
      			assert (personality == CPU);
      		#endif
      			return false;
      		}
      		static bool reduce (uint32_t node_id, struct NodeData & node, std::array<double, LATENT_VECTOR_SIZE> y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) {Galois::pairWiseAvg_vec(node.latent_vector, y); }_node_latent_vector_cuda(cuda_ctx, node_id, y);
      			else if (personality == CPU)
      		#endif
      				{ { Galois::pairWiseAvg_vec(node.latent_vector, y); } }
              return true;
      		}
      		static bool reduce_batch(unsigned from_id, unsigned long long int *b, unsigned int *o, std::array<double, LATENT_VECTOR_SIZE> *y, size_t s, DataCommMode data_mode) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) { batch_{Galois::pairWiseAvg_vec(node.latent_vector, y); }_node_latent_vector_cuda(cuda_ctx, from_id, b, o, y, s, data_mode); return true; }
      			assert (personality == CPU);
      		#endif
      			return false;
      		}
      		static void reset (uint32_t node_id, struct NodeData & node ) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) set_node_latent_vector_cuda(cuda_ctx, node_id, {Galois::resetVec(node.latent_vector); });
      			else if (personality == CPU)
      		#endif
      				{ Galois::resetVec(node.latent_vector); }
      		}
      		typedef std::array<double, LATENT_VECTOR_SIZE> ValTy;
      	};
      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		std::string impl_str("CUDA_DO_ALL_IMPL_SGD_" + (_graph.get_run_identifier()));
      		Galois::StatTimer StatTimer_cuda(impl_str.c_str());
      		StatTimer_cuda.start();
      		int __retval = 0;
      		SGD_all_cuda(__retval, cuda_ctx);
      		DGAccumulator_accum += __retval;
      		StatTimer_cuda.stop();
      	} else if (personality == CPU)
      #endif
      

      	//_graph.sync_forward<Syncer_vertexCut_2, SyncerPull_0>("SGD");
      	_graph.sync_exchange<Syncer_vertexCut_2, SyncerPull_0>("SGD");


      Galois::do_all(_graph.begin(), _graph.end(), SGD { &_graph, step_size }, Galois::loopname("SGD"), Galois::write_set("sync_push", "this->graph", "struct NodeData &", "std::vector<std::array<double, LATENT_VECTOR_SIZE>>" , "latent_vector", "std::array<double, LATENT_VECTOR_SIZE>" , "{Galois::pairWiseAvg_vec(node.latent_vector, y); }",  "{Galois::resetVec(node.latent_vector); }"), Galois::write_set("sync_push", "this->graph", "struct NodeData &", "std::vector<std::array<double, LATENT_VECTOR_SIZE>>" , "latent_vector", "std::array<double, LATENT_VECTOR_SIZE>" , "{Galois::pairWiseAvg_vec(node.latent_vector, y); }",  "{Galois::resetVec(node.latent_vector); }"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "latent_vector" , "std::array<double, LATENT_VECTOR_SIZE>" , "{Galois::pairWiseAvg_vec(node.latent_vector, y); }",  "{Galois::resetVec(node.latent_vector); }"));
      ++iteration;
      rms_normalized = std::sqrt(DGAccumulator_accum.reduce()/_graph.get_totalEdges());
      std::cout << " RMS Normalized  : " << rms_normalized << "\n";
    }while((iteration < maxIterations) && (rms_normalized > 10));

    if (Galois::Runtime::getSystemNetworkInterface().ID == 0) {
      Galois::Runtime::reportStat("(NULL)", "NUM_ITERATIONS_" + std::to_string(_graph.get_run_num()), (unsigned long)iteration, 0);
    }
  }



  void operator()(GNode src) const {
    NodeData& sdata= graph->getData(src);
    auto& movie_node = sdata.latent_vector;

    for (auto jj = graph->edge_begin(src), ej = graph->edge_end(src); jj != ej; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& ddata = graph->getData(dst);
      auto& user_node = ddata.latent_vector;
      double edge_rating = graph->getEdgeData(dst);

     double cur_error2 = edge_rating - calcPrediction(sdata, ddata);
     DGAccumulator_accum += (cur_error2*cur_error2);
    }
  }
};
Galois::DGAccumulator<double>  SGD::DGAccumulator_accum;

int main(int argc, char** argv) {
  try {
    LonestarStart(argc, argv, name, desc, url);
    Galois::StatManager statManager;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    Galois::StatTimer StatTimer_init("TIMER_GRAPH_INIT"), StatTimer_total("TIMER_TOTAL"), StatTimer_hg_init("TIMER_HG_INIT");

    StatTimer_total.start();

    std::vector<unsigned> scalefactor;
#ifdef __GALOIS_HET_CUDA__
    const unsigned my_host_id = Galois::Runtime::getHostID();
    int gpu_device = gpudevice;
    //Parse arg string when running on multiple hosts and update/override personality
    //with corresponding value.
    if (personality_set.length() == Galois::Runtime::NetworkInterface::Num) {
      switch (personality_set.c_str()[my_host_id]) {
      case 'g':
        personality = GPU_CUDA;
        break;
      case 'o':
        assert(0);
        personality = GPU_OPENCL;
        break;
      case 'c':
      default:
        personality = CPU;
        break;
      }
#ifdef __GALOIS_SINGLE_HOST_MULTIPLE_GPUS__
      if (gpu_device == -1) {
        gpu_device = 0;
        for (unsigned i = 0; i < my_host_id; ++i) {
          if (personality_set.c_str()[i] != 'c') ++gpu_device;
        }
      }
#endif
      for (unsigned i=0; i<personality_set.length(); ++i) {
        if (personality_set.c_str()[i] == 'c') 
          scalefactor.push_back(scalecpu);
        else
          scalefactor.push_back(scalegpu);
      }
    }
#endif


   StatTimer_hg_init.start();
   Graph* hg;
   if(enableVCut){
      if(vertexcut == CART_VCUT)
        hg = new Graph_cartesianCut(inputFile,partFolder, net.ID, net.Num, scalefactor, transpose);
      else if(vertexcut == PL_VCUT)
        hg = new Graph_vertexCut(inputFile,partFolder, net.ID, net.Num, scalefactor, transpose, VCutThreshold, bipartite);
   }
   else {
    hg = new Graph_edgeCut(inputFile,partFolder, net.ID, net.Num, scalefactor, transpose, bipartite);
   }

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      cuda_ctx = get_CUDA_context(my_host_id);
      if (!init_CUDA_context(cuda_ctx, gpu_device))
        return -1;
      MarshalGraph m = (*hg).getMarshalGraph(my_host_id);
      load_graph_CUDA(cuda_ctx, m, net.Num);
    } else if (personality == GPU_OPENCL) {
      //Galois::OpenCL::cl_env.init(cldevice.Value);
    }
#endif
    StatTimer_hg_init.stop();

    std::cout << "[" << net.ID << "] InitializeGraph::go called\n";
    StatTimer_init.start();
      InitializeGraph::go((*hg));
    StatTimer_init.stop();

    for(auto run = 0; run < numRuns; ++run){
      std::cout << "[" << net.ID << "] SGD::go run " << run << " called\n";
      std::string timer_str("TIMER_" + std::to_string(run));
      Galois::StatTimer StatTimer_main(timer_str.c_str());

      (*hg).reset_num_iter(run);

      StatTimer_main.start();
        SGD::go((*hg));
      StatTimer_main.stop();

      if((run + 1) != numRuns){
        Galois::Runtime::getHostBarrier().wait();
        (*hg).reset_num_iter(run);
        InitializeGraph::go((*hg));
      }
    }

   StatTimer_total.stop();

    // Verify
    if(verify){
#ifdef __GALOIS_HET_CUDA__
      if (personality == CPU) { 
#endif
        for(auto ii = (*hg).begin(); ii != (*hg).end(); ++ii) {
          for(auto i = 0; i < LATENT_VECTOR_SIZE; ++i)
            Galois::Runtime::printOutput("% %\n", (*hg).getGID(*ii), (*hg).getData(*ii).latent_vector[i]);
        }
#ifdef __GALOIS_HET_CUDA__
      } else if(personality == GPU_CUDA)  {
        for(auto ii = (*hg).begin(); ii != (*hg).end(); ++ii) {
          Galois::Runtime::printOutput("% %\n", (*hg).getGID(*ii), get_node_dist_current_cuda(cuda_ctx, *ii));
        }
      }
#endif
    }

    return 0;
  } catch(const char* c) {
    std::cerr << "Error: " << c << "\n";
      return 1;
  }
}
