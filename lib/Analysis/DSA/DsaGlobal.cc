#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetLibraryInfo.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Support/raw_ostream.h"

#include "seahorn/Analysis/DSA/Graph.hh"
#include "seahorn/Analysis/DSA/Global.hh"
#include "seahorn/Analysis/DSA/Local.hh"
#include "seahorn/Analysis/DSA/Cloner.hh"
#include "seahorn/Analysis/DSA/BottomUp.hh"
#include "seahorn/Analysis/DSA/CallSite.hh"

#include "ufo/Stats.hh"

#include "avy/AvyDebug.h"

#include "boost/range/iterator_range.hpp"

using namespace llvm;

namespace seahorn
{
  namespace dsa
  {

    void ContextInsensitiveGlobalAnalysis::
    resolveArguments (DsaCallSite &cs, Graph& g)
    {
      // unify return
      const Function &callee = *cs.getCallee ();
      if (g.hasRetCell(callee))
      {
        Cell &nc = g.mkCell(*cs.getInstruction(), Cell());
        const Cell& r = g.getRetCell(callee);
        Cell c (*r.getNode (), r.getOffset ());
        nc.unify(c);
      }

      // unify actuals and formals
      DsaCallSite::const_actual_iterator AI = cs.actual_begin(), AE = cs.actual_end();
      for (DsaCallSite::const_formal_iterator FI = cs.formal_begin(), FE = cs.formal_end();
           FI != FE && AI != AE; ++FI, ++AI) 
      {
        const Value *arg = (*AI).get();
        const Value *farg = &*FI;
        if (g.hasCell(*farg)) {
          Cell &c = g.mkCell(*arg, Cell());
          Cell &d = g.mkCell (*farg, Cell ());
          c.unify (d);
        }
      }
    }                                      

    bool ContextInsensitiveGlobalAnalysis::runOnModule (Module &M)
    {

      LOG("dsa-global", 
          errs () << "Started context-insensitive global analysis ... \n");

      ufo::Stats::resume ("CI-DsaAnalysis");

      m_graph.reset (new Graph (m_dl, m_setFactory));

      LocalAnalysis la (m_dl, m_tli);

      // -- bottom-up inlining of all graphs
      for (auto it = scc_begin (&m_cg); !it.isAtEnd (); ++it)
      {
        auto &scc = *it;

        // --- all scc members share the same local graph
        for (CallGraphNode *cgn : scc) 
        {
          Function *fn = cgn->getFunction ();
          if (!fn || fn->isDeclaration () || fn->empty ()) continue;
          
          // compute local graph
          Graph fGraph (m_dl, m_setFactory);
          la.runOnFunction (*fn, fGraph);

          m_fns.insert (fn);
          m_graph->import(fGraph, true);
        }

        // --- resolve callsites
        for (CallGraphNode *cgn : scc)
        {
          Function *fn = cgn->getFunction ();
          
          // XXX probably not needed since if the function is external
          // XXX it will have no call records
          if (!fn || fn->isDeclaration () || fn->empty ()) continue;
          
          // -- iterate over all call instructions of the current function fn
          // -- they are indexed in the CallGraphNode data structure
          for (auto &CallRecord : *cgn) 
          {
            ImmutableCallSite cs (CallRecord.first);
            DsaCallSite dsa_cs (cs);
            const Function *callee = dsa_cs.getCallee();
            // XXX We want to resolve external calls as well.
            // XXX By not resolving them, we pretend that they have no
            // XXX side-effects. This should be an option, not the only behavior
            if (callee && !callee->isDeclaration () && !callee->empty ())
            {
              assert (fn == dsa_cs.getCaller ());
              resolveArguments (dsa_cs, *m_graph);
            }
          }
        }
        m_graph->compress();
      }

      ufo::Stats::stop ("CI-DsaAnalysis");
      LOG("dsa-global", 
          errs () << "Finished context-insensitive global analysis.\n");

      return false;
    }


    const Graph& ContextInsensitiveGlobalAnalysis::getGraph(const Function&) const
    { assert(m_graph); return *m_graph; }

    Graph& ContextInsensitiveGlobalAnalysis::getGraph(const Function&)
    { assert(m_graph); return *m_graph; }

    bool ContextInsensitiveGlobalAnalysis::hasGraph(const Function&fn) const 
    { return m_fns.count(&fn) > 0; }

      
    /// LLVM pass

    ContextInsensitiveGlobal::ContextInsensitiveGlobal () 
        : DsaGlobalPass (ID), m_ga (nullptr) {}
          
    void ContextInsensitiveGlobal::getAnalysisUsage (AnalysisUsage &AU) const 
    {
      AU.addRequired<DataLayoutPass> ();
      AU.addRequired<TargetLibraryInfo> ();
      AU.addRequired<CallGraphWrapperPass> ();
      AU.setPreservesAll ();
    }
        
    bool ContextInsensitiveGlobal::runOnModule (Module &M)
    {
      auto &dl = getAnalysis<DataLayoutPass>().getDataLayout ();
      auto &tli = getAnalysis<TargetLibraryInfo> ();
      auto &cg = getAnalysis<CallGraphWrapperPass> ().getCallGraph ();

      m_ga.reset (new ContextInsensitiveGlobalAnalysis (dl, tli, cg, m_setFactory));
      return m_ga->runOnModule (M);
    }

    const Graph& ContextInsensitiveGlobal::getGraph(const Function& fn) const
    { return m_ga->getGraph (fn); }

    Graph& ContextInsensitiveGlobal::getGraph(const Function& fn) 
    { return m_ga->getGraph (fn); }

    bool ContextInsensitiveGlobal::hasGraph(const Function& fn) const 
    { return m_ga->hasGraph (fn); }
  
  } // end namespace dsa
} // end namespace seahorn


namespace seahorn
{
  namespace dsa 
  {
    // Clone caller nodes into callee and resolve arguments
    // XXX: this code is pretty much symmetric to the one defined in
    // BottomUp. They should be merged at some point.
    static void cloneAndResolveArguments (const DsaCallSite &cs, 
                                          Graph& callerG, Graph& calleeG)
    {      
      Cloner C (calleeG);
      
      // clone and unify globals 
      for (auto &kv : boost::make_iterator_range (callerG.globals_begin (),
                                                  callerG.globals_end ()))
      {
        Node &n = C.clone (*kv.second->getNode ());
        Cell c (n, kv.second->getOffset ());
        Cell &nc = calleeG.mkCell (*kv.first, Cell ());
        nc.unify (c);
      }

      // clone and unify return
      const Function &callee = *cs.getCallee ();
      if (calleeG.hasRetCell (callee) && callerG.hasCell (*cs.getInstruction ()))
      {
        Node &n = C.clone (*callerG.getCell (*cs.getInstruction ()).getNode());
        Cell c (n, callerG.getCell (*cs.getInstruction()).getOffset ());
        Cell &nc = calleeG.getRetCell (callee);
        nc.unify (c);
      }

      // clone and unify actuals and formals
      DsaCallSite::const_actual_iterator AI = cs.actual_begin(), AE = cs.actual_end();
      for (DsaCallSite::const_formal_iterator FI = cs.formal_begin(), FE = cs.formal_end();
           FI != FE && AI != AE; ++FI, ++AI) 
      {
        const Value *arg = (*AI).get();
        const Value *fml = &*FI;
        if (callerG.hasCell (*arg) && calleeG.hasCell (*fml))
        {
          Node &n = C.clone (*callerG.getCell (*arg).getNode ());
          Cell c (n, callerG.getCell (*arg).getOffset ());
          Cell &nc = calleeG.mkCell (*fml, Cell ());
          nc.unify (c);
        }
      }
      calleeG.compress();
    }

    // Compute for each function the set of used/defined
    // callsites. All functions in the same SCC share the same set of
    // used/defined sets.
    void ContextSensitiveGlobalAnalysis::buildIndexes (CallGraph &cg){

      // --- compute immediate predecessors (callsites) for each
      //     function in the call graph (considering only direct
      //     calls).
      // 
      // XXX: CallGraph cannot be reversed and the CallGraph analysis
      // doesn't seem to compute predecessors so I do not know a
      // better way.
      boost::unordered_map<const Function*, InstSet> imm_preds;
      for (auto it = scc_begin (&cg); !it.isAtEnd (); ++it)
      {
        auto &scc = *it;
        for (CallGraphNode *cgn : scc)
        {
          const Function *fn = cgn->getFunction ();
          if (!fn || fn->isDeclaration () || fn->empty ()) continue;
          
          for (auto &callRecord : *cgn)
          { 
            ImmutableCallSite CS (callRecord.first);
            const Function *callee = CS.getCalledFunction ();
            if (!callee || callee->isDeclaration () || callee->empty ()) continue;

            auto predIt = imm_preds.find (callee);
            if (predIt != imm_preds.end ())
              predIt->second.insert (CS.getInstruction());
            else
            {
              InstSet s;
              s.insert (CS.getInstruction ());
              imm_preds.insert (std::make_pair(callee, s));
            }
          }
        }
      }


      // -- compute uses/defs sets
      for (auto it = scc_begin (&cg); !it.isAtEnd (); ++it)
      {
        auto &scc = *it;

        // compute uses and defs shared between all functions in the scc 
        auto uses = std::make_shared<InstSet>();
        auto defs = std::make_shared<InstSet>();

        for (CallGraphNode *cgn : scc)
        {
          const Function *fn = cgn->getFunction ();
          if (!fn || fn->isDeclaration () || fn->empty ()) continue;
          
          uses->insert(imm_preds [fn].begin(), imm_preds [fn].end());

          for (auto &callRecord : *cgn)
          {
            ImmutableCallSite CS (callRecord.first);
            defs->insert(CS.getInstruction());
          }
        }

        // store uses and defs 
        for (CallGraphNode *cgn : scc)
        {
          const Function *fn = cgn->getFunction ();
          if (!fn || fn->isDeclaration () || fn->empty ()) continue;

          m_uses[fn] = uses;
          m_defs[fn] = defs;
        }
      }
    }


    // Decide which kind of propagation (if any) is needed
    ContextSensitiveGlobalAnalysis::PropagationKind 
    ContextSensitiveGlobalAnalysis::decidePropagation 
    (const DsaCallSite& cs, Graph &calleeG, Graph& callerG) 
    {
      PropagationKind res = UP;
      SimulationMapper sm;
      if (Graph::computeCalleeCallerMapping(cs, calleeG, callerG, true, sm)) {
        if (sm.isFunction ())
          res = (sm.isInjective () ? NONE: DOWN);
      }

      return res;
    }

    template<typename WorkList>
    static void Insert (WorkList &w, const Instruction* I)
    {
      // XXX: this is a very inefficient way of breaking cycles
      if (std::find(w.begin(), w.end(), I) == w.end())
        w.push_back (I);
    }

    void ContextSensitiveGlobalAnalysis::
    propagateTopDown (const DsaCallSite& cs, Graph &callerG, Graph& calleeG) 
    {
      cloneAndResolveArguments (cs, callerG, calleeG);

      LOG("dsa-global",
          if (decidePropagation (cs, calleeG, callerG) == DOWN)
          {
            errs () << "Sanity check failed:"
                    << " we should not need more top-down propagation\n";
          });
            
      assert (decidePropagation (cs, calleeG, callerG) != DOWN);
    }

    void ContextSensitiveGlobalAnalysis::
    propagateBottomUp (const DsaCallSite& cs, Graph &calleeG, Graph& callerG) 
    {
      BottomUpAnalysis::cloneAndResolveArguments (cs, calleeG, callerG);
      
      LOG("dsa-global",
          if (decidePropagation (cs, calleeG, callerG) == UP)
          {
            errs () << "Sanity check failed:"
                    << " we should not need more bottom-up propagation\n";
          });
      
      assert (decidePropagation (cs, calleeG, callerG) != UP);
    }


    bool ContextSensitiveGlobalAnalysis::runOnModule (Module &M) 
    {

      LOG("dsa-global", errs () << "Started context-sensitive global analysis ... \n");

      ufo::Stats::resume ("CS-DsaAnalysis");

      for (auto &F: M)
      { 
        GraphRef fGraph = std::make_shared<Graph> (m_dl, m_setFactory);
        m_graphs[&F] = fGraph;
      }

      // -- Run bottom up analysis on the whole call graph 
      //    and initialize worklist
      BottomUpAnalysis bu (m_dl, m_tli, m_cg);
      bu.runOnModule (M, m_graphs);

      // build for each function the set of used/defined callsites
      buildIndexes (m_cg);

      /// push in the worklist callsites for which two different
      /// callee nodes are mapped to the same caller node
      Worklist worklist;
      for (auto &kv: boost::make_iterator_range (bu.callee_caller_mapping_begin (),
                                                 bu.callee_caller_mapping_end ()))
      {
        auto const &simMapper = *(kv.second);
        if (!simMapper.isInjective ()) 
          worklist.push_back (kv.first);  // they do need top-down
      }

      /// -- top-down/bottom-up propagation until no change

      unsigned td_props = 0;
      unsigned bu_props = 0;
      while (!worklist.empty()) 
      {
        const Instruction* I = I = worklist.back();
        worklist.pop_back();

        if (const CallInst *CI = dyn_cast<CallInst> (I))
          if (CI->isInlineAsm())
            continue;

        ImmutableCallSite CS (I);
        DsaCallSite dsaCS (CS);

        auto fn = dsaCS.getCallee();
        if (!fn || fn->isDeclaration() || fn->empty())
          continue;

        assert (m_graphs.count (dsaCS.getCaller ()) > 0);
        assert (m_graphs.count (dsaCS.getCallee ()) > 0);
        
        Graph &callerG = *(m_graphs.find (dsaCS.getCaller())->second);
        Graph &calleeG = *(m_graphs.find (dsaCS.getCallee())->second);

        // -- find out which propagation is needed if any
        auto propKind = decidePropagation  (dsaCS, calleeG, callerG);
        if (propKind == DOWN)
        {
          propagateTopDown (dsaCS, callerG, calleeG); 
          td_props++;
          if (const Function *callee = dsaCS.getCallee ())
          {
            for (const Instruction *cs: *(m_uses [callee]))
              Insert(worklist, cs); // they might need bottom-up
            for (const Instruction *cs: *(m_defs [callee]))
              Insert(worklist, cs); // they might need top-down
          }
        }
        else if (propKind == UP)
        {
          propagateBottomUp (dsaCS, calleeG, callerG);
          bu_props++;
          if (const Function *caller = dsaCS.getCaller ())
          {
            for (const Instruction *cs: *(m_uses [caller]))
              Insert (worklist, cs);  // they might need bottom-up
            for (const Instruction *cs: *(m_defs [caller]))
              Insert (worklist, cs);  // they might need top-down
          }
        }
      }

      LOG("dsa-global", 
          errs () << "-- Number of top-down propagations=" << td_props << "\n";
          errs () << "-- Number of bottom-up propagations=" << bu_props << "\n";);

      LOG("dsa-global", checkNoMorePropagation ());

      assert (checkNoMorePropagation ());

      LOG("dsa-global", errs () << "Finished context-sensitive global analysis\n");

      ufo::Stats::stop ("CS-DsaAnalysis");          

      return false;
    }

    // Perform some sanity checks:
    // 1) each callee node can be simulated by its corresponding caller node.
    // 2) no two callee nodes are mapped to the same caller node.
    bool ContextSensitiveGlobalAnalysis::checkNoMorePropagation() 
    {
      for (auto it = scc_begin (&m_cg); !it.isAtEnd (); ++it)
      {
        auto &scc = *it;
        for (CallGraphNode *cgn : scc)
        {
          Function *fn = cgn->getFunction ();
          if (!fn || fn->isDeclaration () || fn->empty ()) continue;
          
          for (auto &callRecord : *cgn)
          {
            ImmutableCallSite CS (callRecord.first);
            DsaCallSite cs (CS);

            const Function *callee = cs.getCallee ();
            if (!callee || callee->isDeclaration () || callee->empty ()) continue;

            assert (m_graphs.count (cs.getCaller ()) > 0);
            assert (m_graphs.count (cs.getCallee ()) > 0);
        
            Graph &callerG = *(m_graphs.find (cs.getCaller())->second);
            Graph &calleeG = *(m_graphs.find (cs.getCallee())->second);
            PropagationKind pkind = decidePropagation (cs, calleeG, callerG);
            if (pkind != NONE)
            {
              auto pkind_str = (pkind==UP)? "bottom-up": "top-down";
              errs () << "Sanity check failed:" 
                      << *(cs.getInstruction ()) << " requires " 
                      << pkind_str << " propagation.\n";
              return false;
            }
          }
        }
      }
      errs () << "Sanity check succeed: global propagation completed!\n";
      return true;
    }

    const Graph &ContextSensitiveGlobalAnalysis::getGraph (const Function &fn) const
    { return *(m_graphs.find (&fn)->second); }

    Graph &ContextSensitiveGlobalAnalysis::getGraph (const Function &fn)
    { return *(m_graphs.find (&fn)->second); }

    bool ContextSensitiveGlobalAnalysis::hasGraph (const Function &fn) const
    { return m_graphs.count (&fn) > 0; }


    /// LLVM pass

    ContextSensitiveGlobal::ContextSensitiveGlobal () 
        : DsaGlobalPass (ID), m_ga (nullptr) {}
          
    void ContextSensitiveGlobal::getAnalysisUsage (AnalysisUsage &AU) const 
    {
      AU.addRequired<DataLayoutPass> ();
      AU.addRequired<TargetLibraryInfo> ();
      AU.addRequired<CallGraphWrapperPass> ();
      AU.setPreservesAll ();
    }

    bool ContextSensitiveGlobal::runOnModule (Module &M)
    {
      auto &dl = getAnalysis<DataLayoutPass>().getDataLayout ();
      auto &tli = getAnalysis<TargetLibraryInfo> ();
      auto &cg = getAnalysis<CallGraphWrapperPass> ().getCallGraph ();

      m_ga.reset (new ContextSensitiveGlobalAnalysis (dl, tli, cg, m_setFactory));
      return m_ga->runOnModule (M);
    }

    const Graph& ContextSensitiveGlobal::getGraph(const Function& fn) const
    { return m_ga->getGraph (fn); }

    Graph& ContextSensitiveGlobal::getGraph(const Function& fn) 
    { return m_ga->getGraph (fn); }

    bool ContextSensitiveGlobal::hasGraph(const Function& fn) const 
    { return m_ga->hasGraph (fn); }

  }
}

char seahorn::dsa::ContextInsensitiveGlobal::ID = 0;

char seahorn::dsa::ContextSensitiveGlobal::ID = 0;

static llvm::RegisterPass<seahorn::dsa::ContextInsensitiveGlobal> 
X ("dsa-global", "Context-insensitive Dsa analysis");

static llvm::RegisterPass<seahorn::dsa::ContextSensitiveGlobal> 
Y ("dsa-cs-global", "Context-sensitive Dsa analysis");
