//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"

#include "Context.h"
#include "CoreStats.h"
#include "ExecutionState.h"
#include "ExternalDispatcher.h"
#include "GetElementPtrTypeIterator.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"

#include "klee/ADT/RNG.h"
#include "klee/Config/Version.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/ArrayExprOptimizer.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprPPrinter.h"
#include "klee/Expr/ExprSMTLIBPrinter.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Solver/Common.h"
#include "klee/ADT/KTest.h"
#include "klee/Support/OptionCategories.h"
#include "klee/Statistics/TimerStatIncrementer.h"
#include "klee/Module/Cell.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Solver/SolverStats.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"
#include "klee/Support/FloatEvaluation.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/System/MemoryUsage.h"
#include "klee/System/Time.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cxxabi.h>
#include <fstream>
#include <iomanip>
#include <iosfwd>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <vector>

//Haoxin for AEG
//add head file of elf_parser
#include "./elf-parser/elf_parser.hpp"

using namespace llvm;
using namespace klee;

namespace klee {
cl::OptionCategory DebugCat("Debugging options",
                            "These are debugging options.");

cl::OptionCategory ExtCallsCat("External call policy options",
                               "These options impact external calls.");

cl::OptionCategory SeedingCat(
    "Seeding options",
    "These options are related to the use of seeds to start exploration.");

cl::OptionCategory
    TerminationCat("State and overall termination options",
                   "These options control termination of the overall KLEE "
                   "execution and of individual states.");

cl::OptionCategory TestGenCat("Test generation options",
                              "These options impact test generation.");

cl::opt<std::string> MaxTime(
    "max-time",
    cl::desc("Halt execution after the specified duration.  "
             "Set to 0s to disable (default=0s)"),
    cl::init("0s"),
    cl::cat(TerminationCat));
} // namespace klee

namespace {

/*** Test generation options ***/

cl::opt<bool> DumpStatesOnHalt(
    "dump-states-on-halt",
    cl::init(true),
    cl::desc("Dump test cases for all active states on exit (default=true)"),
    cl::cat(TestGenCat));

cl::opt<bool> OnlyOutputStatesCoveringNew(
    "only-output-states-covering-new",
    cl::init(false),
    cl::desc("Only output test cases covering new code (default=false)"),
    cl::cat(TestGenCat));

cl::opt<bool> EmitAllErrors(
    "emit-all-errors", cl::init(false),
    cl::desc("Generate tests cases for all errors "
             "(default=false, i.e. one per (error,instruction) pair)"),
    cl::cat(TestGenCat));


/* Constraint solving options */

cl::opt<unsigned> MaxSymArraySize(
    "max-sym-array-size",
    cl::desc(
        "If a symbolic array exceeds this size (in bytes), symbolic addresses "
        "into this array are concretized.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(SolvingCat));

cl::opt<bool>
    SimplifySymIndices("simplify-sym-indices",
                       cl::init(false),
                       cl::desc("Simplify symbolic accesses using equalities "
                                "from other constraints (default=false)"),
                       cl::cat(SolvingCat));

cl::opt<bool>
    EqualitySubstitution("equality-substitution", cl::init(true),
                         cl::desc("Simplify equality expressions before "
                                  "querying the solver (default=true)"),
                         cl::cat(SolvingCat));


/*** External call policy options ***/

enum class ExternalCallPolicy {
  None,     // No external calls allowed
  Concrete, // Only external calls with concrete arguments allowed
  All,      // All external calls allowed
};

cl::opt<ExternalCallPolicy> ExternalCalls(
    "external-calls",
    cl::desc("Specify the external call policy"),
    cl::values(
        clEnumValN(
            ExternalCallPolicy::None, "none",
            "No external function calls are allowed.  Note that KLEE always "
            "allows some external calls with concrete arguments to go through "
            "(in particular printf and puts), regardless of this option."),
        clEnumValN(ExternalCallPolicy::Concrete, "concrete",
                   "Only external function calls with concrete arguments are "
                   "allowed (default)"),
        clEnumValN(ExternalCallPolicy::All, "all",
                   "All external function calls are allowed.  This concretizes "
                   "any symbolic arguments in calls to external functions.")
            KLEE_LLVM_CL_VAL_END),
    cl::init(ExternalCallPolicy::Concrete),
    cl::cat(ExtCallsCat));

cl::opt<bool> SuppressExternalWarnings(
    "suppress-external-warnings",
    cl::init(false),
    cl::desc("Supress warnings about calling external functions."),
    cl::cat(ExtCallsCat));

cl::opt<bool> AllExternalWarnings(
    "all-external-warnings",
    cl::init(false),
    cl::desc("Issue a warning everytime an external call is made, "
             "as opposed to once per function (default=false)"),
    cl::cat(ExtCallsCat));


/*** Seeding options ***/

cl::opt<bool> AlwaysOutputSeeds(
    "always-output-seeds",
    cl::init(true),
    cl::desc(
        "Dump test cases even if they are driven by seeds only (default=true)"),
    cl::cat(SeedingCat));

cl::opt<bool> OnlyReplaySeeds(
    "only-replay-seeds",
    cl::init(false),
    cl::desc("Discard states that do not have a seed (default=false)."),
    cl::cat(SeedingCat));

cl::opt<bool> OnlySeed("only-seed",
                       cl::init(false),
                       cl::desc("Stop execution after seeding is done without "
                                "doing regular search (default=false)."),
                       cl::cat(SeedingCat));

cl::opt<bool>
    AllowSeedExtension("allow-seed-extension",
                       cl::init(false),
                       cl::desc("Allow extra (unbound) values to become "
                                "symbolic during seeding (default=false)."),
                       cl::cat(SeedingCat));

cl::opt<bool> ZeroSeedExtension(
    "zero-seed-extension",
    cl::init(false),
    cl::desc(
        "Use zero-filled objects if matching seed not found (default=false)"),
    cl::cat(SeedingCat));

cl::opt<bool> AllowSeedTruncation(
    "allow-seed-truncation",
    cl::init(false),
    cl::desc("Allow smaller buffers than in seeds (default=false)."),
    cl::cat(SeedingCat));

cl::opt<bool> NamedSeedMatching(
    "named-seed-matching",
    cl::init(false),
    cl::desc("Use names to match symbolic objects to inputs (default=false)."),
    cl::cat(SeedingCat));

cl::opt<std::string>
    SeedTime("seed-time",
             cl::desc("Amount of time to dedicate to seeds, before normal "
                      "search (default=0s (off))"),
             cl::cat(SeedingCat));


/*** Termination criteria options ***/

cl::list<Executor::TerminateReason> ExitOnErrorType(
    "exit-on-error-type",
    cl::desc(
        "Stop execution after reaching a specified condition (default=false)"),
    cl::values(
        clEnumValN(Executor::Abort, "Abort", "The program crashed"),
        clEnumValN(Executor::Assert, "Assert", "An assertion was hit"),
        clEnumValN(Executor::BadVectorAccess, "BadVectorAccess",
                   "Vector accessed out of bounds"),
        clEnumValN(Executor::Exec, "Exec",
                   "Trying to execute an unexpected instruction"),
        clEnumValN(Executor::External, "External",
                   "External objects referenced"),
        clEnumValN(Executor::Free, "Free", "Freeing invalid memory"),
        clEnumValN(Executor::Model, "Model", "Memory model limit hit"),
        clEnumValN(Executor::Overflow, "Overflow", "An overflow occurred"),
        clEnumValN(Executor::Ptr, "Ptr", "Pointer error"),
        clEnumValN(Executor::ReadOnly, "ReadOnly", "Write to read-only memory"),
        clEnumValN(Executor::ReportError, "ReportError",
                   "klee_report_error called"),
        clEnumValN(Executor::User, "User", "Wrong klee_* functions invocation"),
        clEnumValN(Executor::Unhandled, "Unhandled",
                   "Unhandled instruction hit") KLEE_LLVM_CL_VAL_END),
    cl::ZeroOrMore,
    cl::cat(TerminationCat));

cl::opt<unsigned long long> MaxInstructions(
    "max-instructions",
    cl::desc("Stop execution after this many instructions.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(TerminationCat));

cl::opt<unsigned>
    MaxForks("max-forks",
             cl::desc("Only fork this many times.  Set to -1 to disable (default=-1)"),
             cl::init(~0u),
             cl::cat(TerminationCat));

cl::opt<unsigned> MaxDepth(
    "max-depth",
    cl::desc("Only allow this many symbolic branches.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(TerminationCat));

cl::opt<unsigned> MaxMemory("max-memory",
                            cl::desc("Refuse to fork when above this amount of "
                                     "memory (in MB) (default=2000)"),
                            cl::init(2000),
                            cl::cat(TerminationCat));

cl::opt<bool> MaxMemoryInhibit(
    "max-memory-inhibit",
    cl::desc(
        "Inhibit forking at memory cap (vs. random terminate) (default=true)"),
    cl::init(true),
    cl::cat(TerminationCat));

cl::opt<unsigned> RuntimeMaxStackFrames(
    "max-stack-frames",
    cl::desc("Terminate a state after this many stack frames.  Set to 0 to "
             "disable (default=8192)"),
    cl::init(8192),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticForkPct(
    "max-static-fork-pct", cl::init(1.),
    cl::desc("Maximum percentage spent by an instruction forking out of the "
             "forking of all instructions (default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticSolvePct(
    "max-static-solve-pct", cl::init(1.),
    cl::desc("Maximum percentage of solving time that can be spent by a single "
             "instruction over total solving time for all instructions "
             "(default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticCPForkPct(
    "max-static-cpfork-pct", cl::init(1.),
    cl::desc("Maximum percentage spent by an instruction of a call path "
             "forking out of the forking of all instructions in the call path "
             "(default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticCPSolvePct(
    "max-static-cpsolve-pct", cl::init(1.),
    cl::desc("Maximum percentage of solving time that can be spent by a single "
             "instruction of a call path over total solving time for all "
             "instructions (default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<std::string> TimerInterval(
    "timer-interval",
    cl::desc("Minimum interval to check timers. "
             "Affects -max-time, -istats-write-interval, -stats-write-interval, and -uncovered-update-interval (default=1s)"),
    cl::init("1s"),
    cl::cat(TerminationCat));


/*** Debugging options ***/

/// The different query logging solvers that can switched on/off
enum PrintDebugInstructionsType {
  STDERR_ALL, ///
  STDERR_SRC,
  STDERR_COMPACT,
  FILE_ALL,    ///
  FILE_SRC,    ///
  FILE_COMPACT ///
};

llvm::cl::bits<PrintDebugInstructionsType> DebugPrintInstructions(
    "debug-print-instructions",
    llvm::cl::desc("Log instructions during execution."),
    llvm::cl::values(
        clEnumValN(STDERR_ALL, "all:stderr",
                   "Log all instructions to stderr "
                   "in format [src, inst_id, "
                   "llvm_inst]"),
        clEnumValN(STDERR_SRC, "src:stderr",
                   "Log all instructions to stderr in format [src, inst_id]"),
        clEnumValN(STDERR_COMPACT, "compact:stderr",
                   "Log all instructions to stderr in format [inst_id]"),
        clEnumValN(FILE_ALL, "all:file",
                   "Log all instructions to file "
                   "instructions.txt in format [src, "
                   "inst_id, llvm_inst]"),
        clEnumValN(FILE_SRC, "src:file",
                   "Log all instructions to file "
                   "instructions.txt in format [src, "
                   "inst_id]"),
        clEnumValN(FILE_COMPACT, "compact:file",
                   "Log all instructions to file instructions.txt in format "
                   "[inst_id]") KLEE_LLVM_CL_VAL_END),
    llvm::cl::CommaSeparated,
    cl::cat(DebugCat));

#ifdef HAVE_ZLIB_H
cl::opt<bool> DebugCompressInstructions(
    "debug-compress-instructions", cl::init(false),
    cl::desc(
        "Compress the logged instructions in gzip format (default=false)."),
    cl::cat(DebugCat));
#endif

cl::opt<bool> DebugCheckForImpliedValues(
    "debug-check-for-implied-values", cl::init(false),
    cl::desc("Debug the implied value optimization"),
    cl::cat(DebugCat));

} // namespace

namespace klee {
  RNG theRNG;
}

// XXX hack
extern "C" unsigned dumpStates, dumpPTree;
unsigned dumpStates = 0, dumpPTree = 0;

const char *Executor::TerminateReasonNames[] = {
  [ Abort ] = "abort",
  [ Assert ] = "assert",
  [ BadVectorAccess ] = "bad_vector_access",
  [ Exec ] = "exec",
  [ External ] = "external",
  [ Free ] = "free",
  [ Model ] = "model",
  [ Overflow ] = "overflow",
  [ Ptr ] = "ptr",
  [ ReadOnly ] = "readonly",
  [ ReportError ] = "reporterror",
  [ User ] = "user",
  [ Unhandled ] = "xxx",
};

/* Jiaqi */
extern struct kn_indica* kn_indicator;//They are defined in lib/Core/ExecutionState.h
extern struct HeapAlloc* nme_buf;
extern unsigned long n_heap_l;
extern unsigned long n_heap_h;
extern FILE* req_dump_fp;
extern int heap_idx;

//for explot generation
extern struct of_k* oflow_k;
extern struct of_n* oflow_n;
extern uint8_t* nme_store;//nme returned content array for an overflow read operation

ExecutionState* last_state;

void emulate_nme_req (ExecutionState* state, bool new_alloc)
{
    unsigned long addr = n_heap_l + 0x10*(heap_idx*2);//one for data, one for next meta data
    // state->heap_allocs.back().nativeAddress = n_heap_l + 0x10*heap_idx + 0x10;
    state->heap_allocs.back().nativeAddress = addr;
    heap_idx++;
    last_state = state;
    printf ("state: %p, return native address: %lx. \n", state, addr);
    return;
}

static __attribute__ ((noinline)) unsigned long long rdtsc(void)
{
    unsigned hi, lo;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    // asm volatile ("int $3;\n\t");
    return ((unsigned long long) lo | ((unsigned long long) hi << 32));
}
unsigned long long t0, t1, t;

// no need to differentiate re-execution and fresh execution in KLEE side, since the size para in HeapAlloc already differentiate them.
// NME checks size para to tell fixed addr allocation.
void nme_req (ExecutionState* state, bool new_alloc)
{
    // state->heap_allocs.back().nativeAddress = n_heap_l + 0xa0*heap_idx;
    // printf ("state: %p, size: %lx. \n", state, state->heap_allocs.back().size);
    // // heap_idx += state->heap_allocs.back().size;
    // heap_idx ++;
    // return;

    std::vector<HeapAlloc> v;
    int i;
    int j;
    printf ("state: %p ; last_state: %p. \n", state, last_state);
    if (state)
    {
        printf ("state heap_allocs size: %d. \n", state->heap_allocs.size());
    }
    if (last_state)
    {
        printf ("last state heap_allocs size: %d. \n", last_state->heap_allocs.size());
    }
    /* /debug */

    if (state == last_state || last_state == NULL)
    {
        v.push_back(state->heap_allocs.back());
        // printf ("last_state: %p. \n");
    }
    else//execution has switched to a different state
    {
        int k = state->heap_allocs.size() - 2; //up to state.heap_allocs[k] have been executed natively since in interactive mode.
        int q = last_state->heap_allocs.size() - 1;//all reqs (total size q+1) in last_state have been executed natively.
        for (i = 0; i < last_state->heap_allocs.size(); i ++)
        {
            if (i == k)
                break;
            if (state->heap_allocs[i] != last_state->heap_allocs[i])
                break;
        }
        if (i == k)//up to k, state->heap_allocs == last_state->heap_allocs && k+1 < q+1. // This is not gonna to happen since the last req in current state shouldn't have been execute natively.
        {
            //roll back to k
            for (j = q; j > k; j --)
            {
                HeapAlloc my_heap_alloc = last_state->heap_allocs[j];
                /* flip the request# here */
                if (my_heap_alloc.req == 1)
                    my_heap_alloc.req = 2;
                else
                    my_heap_alloc.req = 1;
                v.push_back(my_heap_alloc);
            }
            //append the last req in state
            HeapAlloc my_heap_alloc = state->heap_allocs.back();
            v.push_back(my_heap_alloc);
        }
        // else if (i == last_state->heap_allocs.size())//up to last_state->native_idx, heap_allocs in two states are the same
        else if (i == q)//up to last_state->native_idx, heap_allocs in two states are the same
        {
            //re-execute reqs in state
            // for (j = last_state->heap_allocs.size(); j < state->heap_allocs.size(); j ++)
            for (j = q+1; j < state->heap_allocs.size(); j ++)
            {
                HeapAlloc my_heap_alloc = state->heap_allocs[j];
                v.push_back(my_heap_alloc);
            }
        }
        else // mismatch point i < last_state
        {
            //roll back from last_state->native_idx to i
            for (j = q; j >= i; j --)
            {
                HeapAlloc my_heap_alloc = last_state->heap_allocs[j];
                /* flip the request# */
                if (my_heap_alloc.req == 1)
                    my_heap_alloc.req = 2;
                else
                    my_heap_alloc.req = 1;
                v.push_back(my_heap_alloc);
            }
            //forward execute from state->native_idx to state->heap_allocs.size()
            for (j = i; j < state->heap_allocs.size(); j ++)
            {
                HeapAlloc my_heap_alloc = state->heap_allocs[j];
                v.push_back(my_heap_alloc);
            }
        }
    }

    if (!new_alloc)
    {
        v.erase(v.begin());//remove the first nme req which is thought to be the new allocation
    }

    memcpy(nme_buf, &v[0], v.size()*sizeof(struct HeapAlloc));

    printf ("v.size: %d. \n", v.size());
    // printf ("req: %d, size: %lx. \n", nme_buf[0].req, nme_buf[0].size);

    // printf ("addr of nme_buf: %p. \n", nme_buf);
    for (i = 0; i < v.size(); i ++)
    {
        printf ("req: %d. size: %d, mo: %p. nativeaddress: %lx. \n", v[i].req, v[i].size, v[i].mo, v[i].nativeAddress);
    }
    // shar_mem->end += sizeof(v);
    // int req_num = v.size();//number of HeapAlloc reqs
    // kn_indicator->num = req_num;
    kn_indicator->num = v.size();
    kn_indicator->flag = 1;

    asm volatile("mfence; \n\t");
    t0 = rdtsc();

    //fetch the responses; and update the NativeAddress in MemoryObject.
    do{
        // printf ("wait for nme response. \n");
        asm volatile("mfence; \n\t");
    } while(kn_indicator->flag != 0);
    t1 = rdtsc();
    t = t1-t0;
    printf ("t0: %llx, t1: %llx, t: %d. \n", t0, t1, t);

    printf ("return from onsite. \n");

    if (new_alloc)
    {
        // only the last req in state.heap_allocs has not been natively executed.
        printf ("update nativeAddress as: %lx. \n", nme_buf[(kn_indicator->num)-1].nativeAddress);
        state->heap_allocs.back().nativeAddress = nme_buf[(kn_indicator->num)-1].nativeAddress;
    }

    for (i = 0; i < v.size(); i ++ )
    {
        fprintf (req_dump_fp, "state: %p. new_alloc: %x. \n %d th request, req#:%d, size: 0x%lx,  mo: %p, nativeAddress: %lx. \n", &state, new_alloc, i, nme_buf[i].req, nme_buf[i].size, nme_buf[i].mo, nme_buf[i].nativeAddress);
        // HeapAlloc ha = nme_buf[req_num];
        // if (ha.mo == state->heap_allocs.back().mo && ha.req == state->heap_allocs.back().req)
        // {
        //     state->heap_allocs.back().nativeAddress = ha.nativeAddress;//update the nativeAddress no matter it is a malloc or free requests.
        //     break;
        // }
    }

    last_state = state;
    return;
}

/* /Jiaqi */

Executor::Executor(LLVMContext &ctx, const InterpreterOptions &opts,
                   InterpreterHandler *ih)
    : Interpreter(opts), interpreterHandler(ih), searcher(0),
      externalDispatcher(new ExternalDispatcher(ctx)), statsTracker(0),
      pathWriter(0), symPathWriter(0), specialFunctionHandler(0), timers{time::Span(TimerInterval)},
      replayKTest(0), replayPath(0), usingSeeds(0),
      atMemoryLimit(false), inhibitForking(false), haltExecution(false),
      ivcEnabled(false), debugLogBuffer(debugBufferString) {


  const time::Span maxTime{MaxTime};
  if (maxTime) timers.add(
        std::make_unique<Timer>(maxTime, [&]{
        klee_message("HaltTimer invoked");
        setHaltExecution(true);
      }));

  coreSolverTimeout = time::Span{MaxCoreSolverTime};
  if (coreSolverTimeout) UseForkedCoreSolver = true;
  Solver *coreSolver = klee::createCoreSolver(CoreSolverToUse);
  if (!coreSolver) {
    klee_error("Failed to create core solver\n");
  }

  Solver *solver = constructSolverChain(
      coreSolver,
      interpreterHandler->getOutputFilename(ALL_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(ALL_QUERIES_KQUERY_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_KQUERY_FILE_NAME));

  this->solver = new TimingSolver(solver, EqualitySubstitution);
  memory = new MemoryManager(&arrayCache);

  initializeSearchOptions();

  if (OnlyOutputStatesCoveringNew && !StatsTracker::useIStats())
    klee_error("To use --only-output-states-covering-new, you need to enable --output-istats.");

  if (DebugPrintInstructions.isSet(FILE_ALL) ||
      DebugPrintInstructions.isSet(FILE_COMPACT) ||
      DebugPrintInstructions.isSet(FILE_SRC)) {
    std::string debug_file_name =
        interpreterHandler->getOutputFilename("instructions.txt");
    std::string error;
#ifdef HAVE_ZLIB_H
    if (!DebugCompressInstructions) {
#endif
      debugInstFile = klee_open_output_file(debug_file_name, error);
#ifdef HAVE_ZLIB_H
    } else {
      debug_file_name.append(".gz");
      debugInstFile = klee_open_compressed_output_file(debug_file_name, error);
    }
#endif
    if (!debugInstFile) {
      klee_error("Could not open file %s : %s", debug_file_name.c_str(),
                 error.c_str());
    }
  }
}

llvm::Module *
Executor::setModule(std::vector<std::unique_ptr<llvm::Module>> &modules,
                    const ModuleOptions &opts) {
  assert(!kmodule && !modules.empty() &&
         "can only register one module"); // XXX gross

  kmodule = std::unique_ptr<KModule>(new KModule());

  // Preparing the final module happens in multiple stages

  // Link with KLEE intrinsics library before running any optimizations
  SmallString<128> LibPath(opts.LibraryDir);
  llvm::sys::path::append(LibPath, "libkleeRuntimeIntrinsic.bca");
  std::string error;
  if (!klee::loadFile(LibPath.str(), modules[0]->getContext(), modules,
                      error)) {
    klee_error("Could not load KLEE intrinsic file %s", LibPath.c_str());
  }

  // 1.) Link the modules together
  while (kmodule->link(modules, opts.EntryPoint)) {
    // 2.) Apply different instrumentation
    kmodule->instrument(opts);
  }

  // 3.) Optimise and prepare for KLEE

  // Create a list of functions that should be preserved if used
  std::vector<const char *> preservedFunctions;
  specialFunctionHandler = new SpecialFunctionHandler(*this);
  specialFunctionHandler->prepare(preservedFunctions);

  preservedFunctions.push_back(opts.EntryPoint.c_str());

  // Preserve the free-standing library calls
  preservedFunctions.push_back("memset");
  preservedFunctions.push_back("memcpy");
  preservedFunctions.push_back("memcmp");
  preservedFunctions.push_back("memmove");

  kmodule->optimiseAndPrepare(opts, preservedFunctions);
  kmodule->checkModule();

  // 4.) Manifest the module
  kmodule->manifest(interpreterHandler, StatsTracker::useStatistics());

  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics() || userSearcherRequiresMD2U()) {
    statsTracker =
      new StatsTracker(*this,
                       interpreterHandler->getOutputFilename("assembly.ll"),
                       userSearcherRequiresMD2U());
  }

  // Initialize the context.
  DataLayout *TD = kmodule->targetData.get();
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width)TD->getPointerSizeInBits());

  return kmodule->module.get();
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  delete specialFunctionHandler;
  delete statsTracker;
  delete solver;
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c,
                                      unsigned offset) {
  const auto targetData = kmodule->targetData.get();
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i),
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t) 0);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i),
			     offset + i*elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i),
			     offset + sl->getElementOffset(i));
  } else if (const ConstantDataSequential *cds =
               dyn_cast<ConstantDataSequential>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cds->getElementType());
    for (unsigned i=0, e=cds->getNumElements(); i != e; ++i)
      initializeGlobalObject(state, os, cds->getElementAsConstant(i),
                             offset + i*elementSize);
  } else if (!isa<UndefValue>(c) && !isa<MetadataAsValue>(c)) {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject * Executor::addExternalObject(ExecutionState &state,
                                           void *addr, unsigned size,
                                           bool isReadOnly) {
  auto mo = memory->allocateFixed(reinterpret_cast<std::uint64_t>(addr),
                                  size, nullptr);
  ObjectState *os = bindObjectInState(state, mo, false);
  for(unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t*)addr)[i]);
  if(isReadOnly)
    os->setReadOnly(true);
  return mo;
}


extern void *__dso_handle __attribute__ ((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
    Module *m = kmodule->module.get();

    if (m->getModuleInlineAsm() != "")
        klee_warning("executable has module level assembly (ignoring)");
    // represent function globals using the address of the actual llvm function
    // object. given that we use malloc to allocate memory in states this also
    // ensures that we won't conflict. we don't need to allocate a memory object
    // since reading/writing via a function pointer is unsupported anyway.
    for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
        Function *f = &*i;
        ref<ConstantExpr> addr(0);

        // If the symbol has external weak linkage then it is implicitly
        // not defined in this module; if it isn't resolvable then it
        // should be null.
        if (f->hasExternalWeakLinkage() &&
                !externalDispatcher->resolveSymbol(f->getName())) {
            addr = Expr::createPointer(0);
        } else {
            addr = Expr::createPointer(reinterpret_cast<std::uint64_t>(f));
            legalFunctions.insert(reinterpret_cast<std::uint64_t>(f));
        }

        globalAddresses.insert(std::make_pair(f, addr));

	// Haoxin for AEG
    	FunctionCalls.insert(std::make_pair(f->getName().str(), addr->getZExtValue()));
    }

#ifndef WINDOWS
    int *errno_addr = getErrnoLocation(state);
    MemoryObject *errnoObj =
        addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false);
    // Copy values from and to program space explicitly
    errnoObj->isUserSpecified = true;
#endif

    // Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
    /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
    const uint16_t **addr = __ctype_b_loc();
    addExternalObject(state, const_cast<uint16_t*>(*addr-128),
            384 * sizeof **addr, true);
    addExternalObject(state, addr, sizeof(*addr), true);

    const int32_t **lower_addr = __ctype_tolower_loc();
    addExternalObject(state, const_cast<int32_t*>(*lower_addr-128),
            384 * sizeof **lower_addr, true);
    addExternalObject(state, lower_addr, sizeof(*lower_addr), true);

    const int32_t **upper_addr = __ctype_toupper_loc();
    addExternalObject(state, const_cast<int32_t*>(*upper_addr-128),
            384 * sizeof **upper_addr, true);
    addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif

    // allocate and initialize globals, done in two passes since we may
    // need address of a global in order to initialize some other one.

    // allocate memory objects for all globals
    for (Module::const_global_iterator i = m->global_begin(),
            e = m->global_end();
            i != e; ++i) {
        const GlobalVariable *v = &*i;
	    // Haoxin for AEG start
	    //printf("GlobalVariable = %s\n", v->getGlobalIdentifier().c_str());
        // find the dest and value
        //KInstruction *ki_temp = state.stack[state.stack.size()-1].kf->instrunction
        //printf("dest = %d\t", state.pc->dest);
        //printf("operand = %d\n", *(state.pc->operands));
    	std::string g_name = v->getGlobalIdentifier();
        // Haoxin for AEG End

        size_t globalObjectAlignment = getAllocationAlignment(v);
        if (i->isDeclaration()) {
            // FIXME: We have no general way of handling unknown external
            // symbols. If we really cared about making external stuff work
            // better we could support user definition, or use the EXE style
            // hack where we check the object file information.

            Type *ty = i->getType()->getElementType();
            uint64_t size = 0;
            if (ty->isSized()) {
                size = kmodule->targetData->getTypeStoreSize(ty);
            } else {
                klee_warning("Type for %.*s is not sized", (int)i->getName().size(),
                        i->getName().data());
            }

            // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
            if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
                size = 0x2C;
            } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
                size = 0x2C;
            } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
                size = 0x2C;
            }
#endif

            if (size == 0) {
                klee_warning("Unable to find size for global variable: %.*s (use will result in out of bounds access)",
                        (int)i->getName().size(), i->getName().data());
            }

            MemoryObject *mo = memory->allocate(size, /*isLocal=*/false,
                    /*isGlobal=*/true, /*allocSite=*/v,
                    /*alignment=*/globalObjectAlignment);
            ObjectState *os = bindObjectInState(state, mo, false);
            globalObjects.insert(std::make_pair(v, mo));
            globalAddresses.insert(std::make_pair(v, mo->getBaseExpr()));

	        // Haoxin for AEG
            //printf("address = %lld\n", mo->address);
      	    FunctionCalls.insert(std::make_pair(g_name, mo->address));

     	    if (g_name.find("_Z4func") != std::string::npos){
            	printf("mo for func : \n");
          	printf("  mo->address = %d\n", mo->address);
          	printf("  mo->name = %s\n", mo->name.c_str());
          	//printf("  os->concreteStore = %d\n", os->concreteStore);
      	    }
            // Program already running = object already initialized.  Read
            // concrete value and write it to our copy.
            if (size) {
                void *addr;
                if (i->getName() == "__dso_handle") {
                    addr = &__dso_handle; // wtf ?
                } else {
                    addr = externalDispatcher->resolveSymbol(i->getName());
                }
                if (!addr)
                    klee_error("unable to load symbol(%s) while initializing globals.",
                            i->getName().data());

                for (unsigned offset=0; offset<mo->size; offset++)
                    os->write8(offset, ((unsigned char*)addr)[offset]);
            }
        } else {
            Type *ty = i->getType()->getElementType();
            uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
            MemoryObject *mo = memory->allocate(size, /*isLocal=*/false,
                    /*isGlobal=*/true, /*allocSite=*/v,
                    /*alignment=*/globalObjectAlignment);
            if (!mo)
                llvm::report_fatal_error("out of memory");
            ObjectState *os = bindObjectInState(state, mo, false);
            globalObjects.insert(std::make_pair(v, mo));
            globalAddresses.insert(std::make_pair(v, mo->getBaseExpr()));

            //printf("address = %lld\n", mo->address);
      	    FunctionCalls.insert(std::make_pair(g_name, mo->address));
            if (!i->hasInitializer())
                os->initializeToRandom();
        }
    }

    // link aliases to their definitions (if bound)
    for (auto i = m->alias_begin(), ie = m->alias_end(); i != ie; ++i) {
        // Map the alias to its aliasee's address. This works because we have
        // addresses for everything, even undefined functions.

        // Alias may refer to other alias, not necessarily known at this point.
        // Thus, resolve to real alias directly.
        const GlobalAlias *alias = &*i;
        while (const auto *ga = dyn_cast<GlobalAlias>(alias->getAliasee())) {
            assert(ga != alias && "alias pointing to itself");
            alias = ga;
        }

        globalAddresses.insert(std::make_pair(&*i, evalConstant(alias->getAliasee())));
    }

    // once all objects are allocated, do the actual initialization
    // remember constant objects to initialise their counter part for external
    // calls
    std::vector<ObjectState *> constantObjects;
    for (Module::const_global_iterator i = m->global_begin(),
            e = m->global_end();
            i != e; ++i) {
        if (i->hasInitializer()) {
            const GlobalVariable *v = &*i;
            MemoryObject *mo = globalObjects.find(v)->second;
            const ObjectState *os = state.addressSpace.findObject(mo);
            assert(os);
            ObjectState *wos = state.addressSpace.getWriteable(mo, os);

            initializeGlobalObject(state, wos, i->getInitializer(), 0);
            if (i->isConstant())
                constantObjects.emplace_back(wos);
        }
    }

    // initialise constant memory that is potentially used with external calls
    if (!constantObjects.empty()) {
        // initialise the actual memory with constant values
        state.addressSpace.copyOutConcretes();

        // mark constant objects as read-only
        for (auto obj : constantObjects)
            obj->setReadOnly(true);
    }
}

void Executor::branch(ExecutionState &state,
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result) {
    TimerStatIncrementer timer(stats::forkTime);
    unsigned N = conditions.size();
    assert(N);

    if (MaxForks!=~0u && stats::forks >= MaxForks) {
        unsigned next = theRNG.getInt32() % N;
        for (unsigned i=0; i<N; ++i) {
            if (i == next) {
                result.push_back(&state);
            } else {
                result.push_back(NULL);
            }
        }
    } else {
        stats::forks += N-1;

        // XXX do proper balance or keep random?
        result.push_back(&state);
        for (unsigned i=1; i<N; ++i) {
            ExecutionState *es = result[theRNG.getInt32() % i];
            ExecutionState *ns = es->branch();
            addedStates.push_back(ns);
            result.push_back(ns);
            processTree->attach(es->ptreeNode, ns, es);
        }
    }

    // If necessary redistribute seeds to match conditions, killing
    // states if necessary due to OnlyReplaySeeds (inefficient but
    // simple).

    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
        seedMap.find(&state);
    if (it != seedMap.end()) {
        std::vector<SeedInfo> seeds = it->second;
        seedMap.erase(it);

        // Assume each seed only satisfies one condition (necessarily true
        // when conditions are mutually exclusive and their conjunction is
        // a tautology).
        for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
                siie = seeds.end(); siit != siie; ++siit) {
            unsigned i;
            for (i=0; i<N; ++i) {
                ref<ConstantExpr> res;
                bool success =
                    solver->getValue(state, siit->assignment.evaluate(conditions[i]),
                            res);
                assert(success && "FIXME: Unhandled solver failure");
                (void) success;
                if (res->isTrue())
                    break;
            }

            // If we didn't find a satisfying condition randomly pick one
            // (the seed will be patched).
            if (i==N)
                i = theRNG.getInt32() % N;

            // Extra check in case we're replaying seeds with a max-fork
            if (result[i])
                seedMap[result[i]].push_back(*siit);
        }

        if (OnlyReplaySeeds) {
            for (unsigned i=0; i<N; ++i) {
                if (result[i] && !seedMap.count(result[i])) {
                    terminateState(*result[i]);
                    result[i] = NULL;
                }
            }
        }
    }

    for (unsigned i=0; i<N; ++i)
        if (result[i])
            addConstraint(*result[i], conditions[i]);
}

Executor::StatePair
Executor::fork(ExecutionState &current, ref<Expr> condition, bool isInternal) {
    Solver::Validity res;
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
        seedMap.find(&current);
    bool isSeeding = it != seedMap.end();

    if (!isSeeding && !isa<ConstantExpr>(condition) &&
            (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
             MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
            statsTracker->elapsed() > time::seconds(60)) {
        StatisticManager &sm = *theStatisticManager;
        CallPathNode *cpn = current.stack.back().callPathNode;
        if ((MaxStaticForkPct<1. &&
                    sm.getIndexedValue(stats::forks, sm.getIndex()) >
                    stats::forks*MaxStaticForkPct) ||
                (MaxStaticCPForkPct<1. &&
                 cpn && (cpn->statistics.getValue(stats::forks) >
                     stats::forks*MaxStaticCPForkPct)) ||
                (MaxStaticSolvePct<1 &&
                 sm.getIndexedValue(stats::solverTime, sm.getIndex()) >
                 stats::solverTime*MaxStaticSolvePct) ||
                (MaxStaticCPForkPct<1. &&
                 cpn && (cpn->statistics.getValue(stats::solverTime) >
                     stats::solverTime*MaxStaticCPSolvePct))) {
            ref<ConstantExpr> value;
            bool success = solver->getValue(current, condition, value);
            assert(success && "FIXME: Unhandled solver failure");
            (void) success;
            addConstraint(current, EqExpr::create(value, condition));
            condition = value;
        }
    }

    time::Span timeout = coreSolverTimeout;
    if (isSeeding)
        timeout *= static_cast<unsigned>(it->second.size());
    solver->setTimeout(timeout);
    bool success = solver->evaluate(current, condition, res);
    solver->setTimeout(time::Span());
    if (!success) {
        current.pc = current.prevPC;
        terminateStateEarly(current, "Query timed out (fork).");
        return StatePair(0, 0);
    }

    if (!isSeeding) {
        if (replayPath && !isInternal) {
            assert(replayPosition<replayPath->size() &&
                    "ran out of branches in replay path mode");
            bool branch = (*replayPath)[replayPosition++];

            if (res==Solver::True) {
                assert(branch && "hit invalid branch in replay path mode");
            } else if (res==Solver::False) {
                assert(!branch && "hit invalid branch in replay path mode");
            } else {
                // add constraints
                if(branch) {
                    res = Solver::True;
                    addConstraint(current, condition);
                } else  {
                    res = Solver::False;
                    addConstraint(current, Expr::createIsZero(condition));
                }
            }
        } else if (res==Solver::Unknown) {
            assert(!replayKTest && "in replay mode, only one branch can be true.");

            if ((MaxMemoryInhibit && atMemoryLimit) ||
                    current.forkDisabled ||
                    inhibitForking ||
                    (MaxForks!=~0u && stats::forks >= MaxForks)) {

                if (MaxMemoryInhibit && atMemoryLimit)
                    klee_warning_once(0, "skipping fork (memory cap exceeded)");
                else if (current.forkDisabled)
                    klee_warning_once(0, "skipping fork (fork disabled on current path)");
                else if (inhibitForking)
                    klee_warning_once(0, "skipping fork (fork disabled globally)");
                else
                    klee_warning_once(0, "skipping fork (max-forks reached)");

                TimerStatIncrementer timer(stats::forkTime);
                if (theRNG.getBool()) {
                    addConstraint(current, condition);
                    res = Solver::True;
                } else {
                    addConstraint(current, Expr::createIsZero(condition));
                    res = Solver::False;
                }
            }
        }
    }

    // Fix branch in only-replay-seed mode, if we don't have both true
    // and false seeds.
    if (isSeeding &&
            (current.forkDisabled || OnlyReplaySeeds) &&
            res == Solver::Unknown) {
        bool trueSeed=false, falseSeed=false;
        // Is seed extension still ok here?
        for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                siie = it->second.end(); siit != siie; ++siit) {
            ref<ConstantExpr> res;
            bool success =
                solver->getValue(current, siit->assignment.evaluate(condition), res);
            assert(success && "FIXME: Unhandled solver failure");
            (void) success;
            if (res->isTrue()) {
                trueSeed = true;
            } else {
                falseSeed = true;
            }
            if (trueSeed && falseSeed)
                break;
        }
        if (!(trueSeed && falseSeed)) {
            assert(trueSeed || falseSeed);

            res = trueSeed ? Solver::True : Solver::False;
            addConstraint(current, trueSeed ? condition : Expr::createIsZero(condition));
        }
    }


    // XXX - even if the constraint is provable one way or the other we
    // can probably benefit by adding this constraint and allowing it to
    // reduce the other constraints. For example, if we do a binary
    // search on a particular value, and then see a comparison against
    // the value it has been fixed at, we should take this as a nice
    // hint to just use the single constraint instead of all the binary
    // search ones. If that makes sense.
    if (res==Solver::True) {
        if (!isInternal) {
            if (pathWriter) {
                current.pathOS << "1";
            }
        }

        return StatePair(&current, 0);
    } else if (res==Solver::False) {
        if (!isInternal) {
            if (pathWriter) {
                current.pathOS << "0";
            }
        }

        return StatePair(0, &current);
    } else {
        TimerStatIncrementer timer(stats::forkTime);
        ExecutionState *falseState, *trueState = &current;

        ++stats::forks;

        falseState = trueState->branch();
        addedStates.push_back(falseState);

        if (it != seedMap.end()) {
            std::vector<SeedInfo> seeds = it->second;
            it->second.clear();
            std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
            std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
            for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
                    siie = seeds.end(); siit != siie; ++siit) {
                ref<ConstantExpr> res;
                bool success =
                    solver->getValue(current, siit->assignment.evaluate(condition), res);
                assert(success && "FIXME: Unhandled solver failure");
                (void) success;
                if (res->isTrue()) {
                    trueSeeds.push_back(*siit);
                } else {
                    falseSeeds.push_back(*siit);
                }
            }

            bool swapInfo = false;
            if (trueSeeds.empty()) {
                if (&current == trueState) swapInfo = true;
                seedMap.erase(trueState);
            }
            if (falseSeeds.empty()) {
                if (&current == falseState) swapInfo = true;
                seedMap.erase(falseState);
            }
            if (swapInfo) {
                std::swap(trueState->coveredNew, falseState->coveredNew);
                std::swap(trueState->coveredLines, falseState->coveredLines);
            }
        }

        processTree->attach(current.ptreeNode, falseState, trueState);

        if (pathWriter) {
            // Need to update the pathOS.id field of falseState, otherwise the same id
            // is used for both falseState and trueState.
            falseState->pathOS = pathWriter->open(current.pathOS);
            if (!isInternal) {
                trueState->pathOS << "1";
                falseState->pathOS << "0";
            }
        }
        if (symPathWriter) {
            falseState->symPathOS = symPathWriter->open(current.symPathOS);
            if (!isInternal) {
                trueState->symPathOS << "1";
                falseState->symPathOS << "0";
            }
        }

        addConstraint(*trueState, condition);
        addConstraint(*falseState, Expr::createIsZero(condition));

        // Kinda gross, do we even really still want this option?
        if (MaxDepth && MaxDepth<=trueState->depth) {
            terminateStateEarly(*trueState, "max-depth exceeded.");
            terminateStateEarly(*falseState, "max-depth exceeded.");
            return StatePair(0, 0);
        }

        return StatePair(trueState, falseState);
    }
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    if (!CE->isTrue())
      llvm::report_fatal_error("attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success =
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint");
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition,
                                 ConstantExpr::alloc(1, Expr::Bool));
}

const Cell& Executor::eval(KInstruction *ki, unsigned index,
                           ExecutionState &state) const {
    assert(index < ki->inst->getNumOperands());
    int vnumber = ki->operands[index];

    assert(vnumber != -1 &&
            "Invalid operand to eval(), not a value or constant!");

    // Determine if this is a constant or not.
    if (vnumber < 0) {
        unsigned index = -vnumber - 2;
        return kmodule->constantTable[index];
    } else {
        unsigned index = vnumber;
        StackFrame &sf = state.stack.back();
        return sf.locals[index];
    }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state,
                         ref<Expr> value) {
    getDestCell(state, target).value = value;
}

void Executor::bindArgument(KFunction *kf, unsigned index,
                            ExecutionState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, ref<Expr> &e) {
    ref<Expr> result = e;

    if (!isa<ConstantExpr>(e)) {
        ref<ConstantExpr> value;
        bool isTrue = false;
        e = optimizer.optimizeExpr(e, true);
        solver->setTimeout(coreSolverTimeout);
        if (solver->getValue(state, e, value)) {
            ref<Expr> cond = EqExpr::create(e, value);
            cond = optimizer.optimizeExpr(cond, false);
            if (solver->mustBeTrue(state, cond, isTrue) && isTrue)
                result = value;
        }
        solver->setTimeout(time::Span());
    }

    return result;
}


/* Concretize the given expression, and return a possible constant value.
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr>
Executor::toConstant(ExecutionState &state,
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints.simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;

  std::string str;
  llvm::raw_string_ostream os(str);
  os << "silently concretizing (reason: " << reason << ") expression " << e
     << " to value " << value << " (" << (*(state.pc)).info->file << ":"
     << (*(state.pc)).info->line << ")";

  if (AllExternalWarnings)
    klee_warning("%s", os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));

  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               ref<Expr> e,
                               KInstruction *target) {
  e = state.constraints.simplifyExpr(e);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
    seedMap.find(&state);
  if (it==seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    e = optimizer.optimizeExpr(e, true);
    bool success = solver->getValue(state, e, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    bindLocal(target, state, value);
  } else {
    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
           siie = it->second.end(); siit != siie; ++siit) {
      ref<Expr> cond = siit->assignment.evaluate(e);
      cond = optimizer.optimizeExpr(cond, true);
      ref<ConstantExpr> value;
      bool success = solver->getValue(state, cond, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }

    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(),
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches);

    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(),
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es, *vit);
      ++bit;
    }
  }
}

void Executor::printDebugInstructions(ExecutionState &state) {
  // check do not print
  if (DebugPrintInstructions.getBits() == 0)
	  return;

  llvm::raw_ostream *stream = 0;
  if (DebugPrintInstructions.isSet(STDERR_ALL) ||
      DebugPrintInstructions.isSet(STDERR_SRC) ||
      DebugPrintInstructions.isSet(STDERR_COMPACT))
    stream = &llvm::errs();
  else
    stream = &debugLogBuffer;

  if (!DebugPrintInstructions.isSet(STDERR_COMPACT) &&
      !DebugPrintInstructions.isSet(FILE_COMPACT)) {
    (*stream) << "     " << state.pc->getSourceLocation() << ":";
  }

  (*stream) << state.pc->info->assemblyLine;

  if (DebugPrintInstructions.isSet(STDERR_ALL) ||
      DebugPrintInstructions.isSet(FILE_ALL))
    (*stream) << ":" << *(state.pc->inst);
  (*stream) << "\n";

  if (DebugPrintInstructions.isSet(FILE_ALL) ||
      DebugPrintInstructions.isSet(FILE_COMPACT) ||
      DebugPrintInstructions.isSet(FILE_SRC)) {
    debugLogBuffer.flush();
    (*debugInstFile) << debugLogBuffer.str();
    debugBufferString = "";
  }
}

void Executor::stepInstruction(ExecutionState &state) {
  printDebugInstructions(state);
  if (statsTracker)
    statsTracker->stepInstruction(state);

  ++stats::instructions;
  ++state.steppedInstructions;
  state.prevPC = state.pc;
  ++state.pc;

  if (stats::instructions == MaxInstructions)
    haltExecution = true;
}

static inline const llvm::fltSemantics *fpWidthToSemantics(unsigned width) {
  switch (width) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(4, 0)
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle();
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble();
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended();
#else
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
#endif
  default:
    return 0;
  }
}

void Executor::executeCall(ExecutionState &state,
                           KInstruction *ki,
                           Function *f,
                           std::vector< ref<Expr> > &arguments) {
    Instruction *i = ki->inst;
    if (i && isa<DbgInfoIntrinsic>(i))
        return;
    if (f && f->isDeclaration()) {
        switch(f->getIntrinsicID()) {
            case Intrinsic::not_intrinsic:
                // state may be destroyed by this call, cannot touch
                callExternalFunction(state, ki, f, arguments);
                break;
            case Intrinsic::fabs: {
                                      ref<ConstantExpr> arg =
                                          toConstant(state, eval(ki, 0, state).value, "floating point");
                                      if (!fpWidthToSemantics(arg->getWidth()))
                                          return terminateStateOnExecError(
                                                  state, "Unsupported intrinsic llvm.fabs call");

                                      llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()),
                                              arg->getAPValue());
                                      Res = llvm::abs(Res);

                                      bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
                                      break;
                                  }
                                  // va_arg is handled by caller and intrinsic lowering, see comment for
                                  // ExecutionState::varargs
            case Intrinsic::vastart:  {
                                          StackFrame &sf = state.stack.back();

                                          // varargs can be zero if no varargs were provided
                                          if (!sf.varargs)
                                              return;

                                          // FIXME: This is really specific to the architecture, not the pointer
                                          // size. This happens to work for x86-32 and x86-64, however.
                                          Expr::Width WordSize = Context::get().getPointerWidth();
                                          if (WordSize == Expr::Int32) {
                                              executeMemoryOperation(state, true, arguments[0],
                                                      sf.varargs->getBaseExpr(), 0);
                                          } else {
                                              assert(WordSize == Expr::Int64 && "Unknown word size!");

                                              // x86-64 has quite complicated calling convention. However,
                                              // instead of implementing it, we can do a simple hack: just
                                              // make a function believe that all varargs are on stack.
                                              executeMemoryOperation(state, true, arguments[0],
                                                      ConstantExpr::create(48, 32), 0); // gp_offset
                                              executeMemoryOperation(state, true,
                                                      AddExpr::create(arguments[0],
                                                          ConstantExpr::create(4, 64)),
                                                      ConstantExpr::create(304, 32), 0); // fp_offset
                                              executeMemoryOperation(state, true,
                                                      AddExpr::create(arguments[0],
                                                          ConstantExpr::create(8, 64)),
                                                      sf.varargs->getBaseExpr(), 0); // overflow_arg_area
                                              executeMemoryOperation(state, true,
                                                      AddExpr::create(arguments[0],
                                                          ConstantExpr::create(16, 64)),
                                                      ConstantExpr::create(0, 64), 0); // reg_save_area
                                          }
                                          break;
                                      }
            case Intrinsic::vaend:
                                      // va_end is a noop for the interpreter.
                                      //
                                      // FIXME: We should validate that the target didn't do something bad
                                      // with va_end, however (like call it twice).
                                      break;

            case Intrinsic::vacopy:
                                      // va_copy should have been lowered.
                                      //
                                      // FIXME: It would be nice to check for errors in the usage of this as
                                      // well.
            default:
                                      klee_error("unknown intrinsic: %s", f->getName().data());
        }

        if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
            transferToBasicBlock(ii->getNormalDest(), i->getParent(), state);
    } else {
        // Check if maximum stack size was reached.
        // We currently only count the number of stack frames
        if (RuntimeMaxStackFrames && state.stack.size() > RuntimeMaxStackFrames) {
            terminateStateEarly(state, "Maximum stack size reached.");
            klee_warning("Maximum stack size reached.");
            return;
        }

        // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
        // guess. This just done to avoid having to pass KInstIterator everywhere
        // instead of the actual instruction, since we can't make a KInstIterator
        // from just an instruction (unlike LLVM).
        KFunction *kf = kmodule->functionMap[f];

        state.pushFrame(state.prevPC, kf);
        state.pc = kf->instructions;

        if (statsTracker)
            statsTracker->framePushed(state, &state.stack[state.stack.size()-2]);

        // TODO: support "byval" parameter attribute
        // TODO: support zeroext, signext, sret attributes

        unsigned callingArgs = arguments.size();
        unsigned funcArgs = f->arg_size();
        if (!f->isVarArg()) {
            if (callingArgs > funcArgs) {
                klee_warning_once(f, "calling %s with extra arguments.",
                        f->getName().data());
            } else if (callingArgs < funcArgs) {
                terminateStateOnError(state, "calling function with too few arguments",
                        User);
                return;
            }
        } else {
            Expr::Width WordSize = Context::get().getPointerWidth();

            if (callingArgs < funcArgs) {
                terminateStateOnError(state, "calling function with too few arguments",
                        User);
                return;
            }

            StackFrame &sf = state.stack.back();
            unsigned size = 0;
            bool requires16ByteAlignment = false;
            for (unsigned i = funcArgs; i < callingArgs; i++) {
                // FIXME: This is really specific to the architecture, not the pointer
                // size. This happens to work for x86-32 and x86-64, however.
                if (WordSize == Expr::Int32) {
                    size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
                } else {
                    Expr::Width argWidth = arguments[i]->getWidth();
                    // AMD64-ABI 3.5.7p5: Step 7. Align l->overflow_arg_area upwards to a
                    // 16 byte boundary if alignment needed by type exceeds 8 byte
                    // boundary.
                    //
                    // Alignment requirements for scalar types is the same as their size
                    if (argWidth > Expr::Int64) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
                        size = llvm::alignTo(size, 16);
#else
                        size = llvm::RoundUpToAlignment(size, 16);
#endif
                        requires16ByteAlignment = true;
                    }
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
                    size += llvm::alignTo(argWidth, WordSize) / 8;
#else
                    size += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
#endif
                }
            }

            MemoryObject *mo = sf.varargs =
                memory->allocate(size, true, false, state.prevPC->inst,
                        (requires16ByteAlignment ? 16 : 8));
            if (!mo && size) {
                terminateStateOnExecError(state, "out of memory (varargs)");
                return;
            }

            if (mo) {
                if ((WordSize == Expr::Int64) && (mo->address & 15) &&
                        requires16ByteAlignment) {
                    // Both 64bit Linux/Glibc and 64bit MacOSX should align to 16 bytes.
                    klee_warning_once(
                            0, "While allocating varargs: malloc did not align to 16 bytes.");
                }

                ObjectState *os = bindObjectInState(state, mo, true);
                unsigned offset = 0;
                for (unsigned i = funcArgs; i < callingArgs; i++) {
                    // FIXME: This is really specific to the architecture, not the pointer
                    // size. This happens to work for x86-32 and x86-64, however.
                    if (WordSize == Expr::Int32) {
                        os->write(offset, arguments[i]);
                        offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
                    } else {
                        assert(WordSize == Expr::Int64 && "Unknown word size!");

                        Expr::Width argWidth = arguments[i]->getWidth();
                        if (argWidth > Expr::Int64) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
                            offset = llvm::alignTo(offset, 16);
#else
                            offset = llvm::RoundUpToAlignment(offset, 16);
#endif
                        }
                        os->write(offset, arguments[i]);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
                        offset += llvm::alignTo(argWidth, WordSize) / 8;
#else
                        offset += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
#endif
                    }
                }
            }
        }

        unsigned numFormals = f->arg_size();
        for (unsigned i=0; i<numFormals; ++i)
            bindArgument(kf, i, state, arguments[i]);
    }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src,
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.

  // XXX this lookup has to go ?
  KFunction *kf = state.stack.back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc = &kf->instructions[entry];
  if (state.pc->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc->inst);
    state.incomingBBIndex = first->getBasicBlockIndex(src);
  }
}

/// Compute the true target of a function call, resolving LLVM aliases
/// and bitcasts.
Function* Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
    SmallPtrSet<const GlobalValue*, 3> Visited;

    Constant *c = dyn_cast<Constant>(calledVal);
    if (!c)
        return 0;

    while (true) {
        if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
            if (!Visited.insert(gv).second)
                return 0;

            if (Function *f = dyn_cast<Function>(gv))
                return f;
            else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
                c = ga->getAliasee();
            else
                return 0;
        } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
            if (ce->getOpcode()==Instruction::BitCast)
                c = ce->getOperand(0);
            else
                return 0;
        } else
            return 0;
    }
}

//*Haoxin for AEG
//For find the special symbolic array
const Array* scan2(ref<Expr> e, std::set<std::string> &symNameList) {
    //std::set<std::string> symNameList;
    const Array *array;
    Expr *ep = e.get();
    //ep->dump();
        for (unsigned i=0; i<ep->getNumKids(); i++)
          scan2(ep->getKid(i), symNameList);
        if (const ReadExpr *re = dyn_cast<ReadExpr>(e)) {
          //printf("In execution array->name = %s\n", re->updates.root->name.c_str());
          symNameList.insert(re->updates.root->name);
          array = re->updates.root;
          //break;
          //re->dump();
        }
        return array;
}
// *Haoxin end


void print_symbols(std::vector<elf_parser::symbol_t> &symbols) {
    printf("Num:    Value  Size Type    Bind   Vis      Ndx Name\n");
    for (auto &symbol : symbols) {
        printf("%-3d: %08" PRIx64 "  %-4d %-8s %-7s %-9s %-3s %s(%s)\n",
                symbol.symbol_num,
                symbol.symbol_value,
                symbol.symbol_size,
                symbol.symbol_type.c_str(),
                symbol.symbol_bind.c_str(),
                symbol.symbol_visibility.c_str(),
                symbol.symbol_index.c_str(),
                symbol.symbol_name.c_str(),
                symbol.symbol_section.c_str());
    }
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
    Instruction *i = ki->inst;
    switch (i->getOpcode()) {
        // Control flow
        case Instruction::Ret: {
                                   ReturnInst *ri = cast<ReturnInst>(i);
                                   KInstIterator kcaller = state.stack.back().caller;
                                   Instruction *caller = kcaller ? kcaller->inst : 0;
                                   bool isVoidReturn = (ri->getNumOperands() == 0);
                                   ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);

                                   if (!isVoidReturn) {
                                       result = eval(ki, 0, state).value;
                                   }

                                   if (state.stack.size() <= 1) {
                                       assert(!caller && "caller set on initial stack frame");
                                       terminateStateOnExit(state);
                                   } else {
                                       state.popFrame();

                                       if (statsTracker)
                                           statsTracker->framePopped(state);

                                       if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
                                           transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
                                       } else {
                                           state.pc = kcaller;
                                           ++state.pc;
                                       }

                                       if (!isVoidReturn) {
                                           Type *t = caller->getType();
                                           if (t != Type::getVoidTy(i->getContext())) {
                                               // may need to do coercion due to bitcasts
                                               Expr::Width from = result->getWidth();
                                               Expr::Width to = getWidthForLLVMType(t);

                                               if (from != to) {
                                                   CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) :
                                                           CallSite(cast<CallInst>(caller)));

                                                   // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
                                                   bool isSExt = cs.hasRetAttr(llvm::Attribute::SExt);
#else
                                                   bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#endif
                                                   if (isSExt) {
                                                       result = SExtExpr::create(result, to);
                                                   } else {
                                                       result = ZExtExpr::create(result, to);
                                                   }
                                               }

                                               bindLocal(kcaller, state, result);
                                           }
                                       } else {
                                           // We check that the return value has no users instead of
                                           // checking the type, since C defaults to returning int for
                                           // undeclared functions.
                                           if (!caller->use_empty()) {
                                               terminateStateOnExecError(state, "return void when caller expected a result");
                                           }
                                       }
                                   }
                                   break;
                               }
        case Instruction::Br: {
                                  BranchInst *bi = cast<BranchInst>(i);
                                  if (bi->isUnconditional()) {
                                      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
                                  } else {
                                      // FIXME: Find a way that we don't have this hidden dependency.
                                      assert(bi->getCondition() == bi->getOperand(0) &&
                                              "Wrong operand index!");
                                      ref<Expr> cond = eval(ki, 0, state).value;

                                      cond = optimizer.optimizeExpr(cond, false);
                                      Executor::StatePair branches = fork(state, cond, false);

                                      // NOTE: There is a hidden dependency here, markBranchVisited
                                      // requires that we still be in the context of the branch
                                      // instruction (it reuses its statistic id). Should be cleaned
                                      // up with convenient instruction specific data.
                                      if (statsTracker && state.stack.back().kf->trackCoverage)
                                          statsTracker->markBranchVisited(branches.first, branches.second);

                                      if (branches.first)
                                          transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first);
                                      if (branches.second)
                                          transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second);
                                  }
                                  break;
                              }
        case Instruction::IndirectBr: {
                                          // implements indirect branch to a label within the current function
                                          const auto bi = cast<IndirectBrInst>(i);
                                          auto address = eval(ki, 0, state).value;
                                          address = toUnique(state, address);

                                          // concrete address
                                          if (const auto CE = dyn_cast<ConstantExpr>(address.get())) {
                                              const auto bb_address = (BasicBlock *) CE->getZExtValue(Context::get().getPointerWidth());
                                              transferToBasicBlock(bb_address, bi->getParent(), state);
                                              break;
                                          }

                                          // symbolic address
                                          const auto numDestinations = bi->getNumDestinations();
                                          std::vector<BasicBlock *> targets;
                                          targets.reserve(numDestinations);
                                          std::vector<ref<Expr>> expressions;
                                          expressions.reserve(numDestinations);

                                          ref<Expr> errorCase = ConstantExpr::alloc(1, Expr::Bool);
                                          SmallPtrSet<BasicBlock *, 5> destinations;
                                          // collect and check destinations from label list
                                          for (unsigned k = 0; k < numDestinations; ++k) {
                                              // filter duplicates
                                              const auto d = bi->getDestination(k);
                                              if (destinations.count(d)) continue;
                                              destinations.insert(d);

                                              // create address expression
                                              const auto PE = Expr::createPointer(reinterpret_cast<std::uint64_t>(d));
                                              ref<Expr> e = EqExpr::create(address, PE);

                                              // exclude address from errorCase
                                              errorCase = AndExpr::create(errorCase, Expr::createIsZero(e));

                                              // check feasibility
                                              bool result;
                                              bool success __attribute__ ((unused)) = solver->mayBeTrue(state, e, result);
                                              assert(success && "FIXME: Unhandled solver failure");
                                              if (result) {
                                                  targets.push_back(d);
                                                  expressions.push_back(e);
                                              }
                                          }
                                          // check errorCase feasibility
                                          bool result;
                                          bool success __attribute__ ((unused)) = solver->mayBeTrue(state, errorCase, result);
                                          assert(success && "FIXME: Unhandled solver failure");
                                          if (result) {
                                              expressions.push_back(errorCase);
                                          }

                                          // fork states
                                          std::vector<ExecutionState *> branches;
                                          branch(state, expressions, branches);

                                          // terminate error state
                                          if (result) {
                                              terminateStateOnExecError(*branches.back(), "indirectbr: illegal label address");
                                              branches.pop_back();
                                          }

                                          // branch states to resp. target blocks
                                          assert(targets.size() == branches.size());
                                          for (std::vector<ExecutionState *>::size_type k = 0; k < branches.size(); ++k) {
                                              if (branches[k]) {
                                                  transferToBasicBlock(targets[k], bi->getParent(), *branches[k]);
                                              }
                                          }

                                          break;
                                      }
        case Instruction::Switch: {
                                      SwitchInst *si = cast<SwitchInst>(i);
                                      ref<Expr> cond = eval(ki, 0, state).value;
                                      BasicBlock *bb = si->getParent();

                                      cond = toUnique(state, cond);
                                      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
                                          // Somewhat gross to create these all the time, but fine till we
                                          // switch to an internal rep.
                                          llvm::IntegerType *Ty = cast<IntegerType>(si->getCondition()->getType());
                                          ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
                                          unsigned index = si->findCaseValue(ci)->getSuccessorIndex();
#else
                                          unsigned index = si->findCaseValue(ci).getSuccessorIndex();
#endif
                                          transferToBasicBlock(si->getSuccessor(index), si->getParent(), state);
                                      } else {
                                          // Handle possible different branch targets

                                          // We have the following assumptions:
                                          // - each case value is mutual exclusive to all other values
                                          // - order of case branches is based on the order of the expressions of
                                          //   the case values, still default is handled last
                                          std::vector<BasicBlock *> bbOrder;
                                          std::map<BasicBlock *, ref<Expr> > branchTargets;

                                          std::map<ref<Expr>, BasicBlock *> expressionOrder;

                                          // Iterate through all non-default cases and order them by expressions
                                          for (auto i : si->cases()) {
                                              ref<Expr> value = evalConstant(i.getCaseValue());

                                              BasicBlock *caseSuccessor = i.getCaseSuccessor();
                                              expressionOrder.insert(std::make_pair(value, caseSuccessor));
                                          }

                                          // Track default branch values
                                          ref<Expr> defaultValue = ConstantExpr::alloc(1, Expr::Bool);

                                          // iterate through all non-default cases but in order of the expressions
                                          for (std::map<ref<Expr>, BasicBlock *>::iterator
                                                  it = expressionOrder.begin(),
                                                  itE = expressionOrder.end();
                                                  it != itE; ++it) {
                                              ref<Expr> match = EqExpr::create(cond, it->first);

                                              // skip if case has same successor basic block as default case
                                              // (should work even with phi nodes as a switch is a single terminating instruction)
                                              if (it->second == si->getDefaultDest()) continue;

                                              // Make sure that the default value does not contain this target's value
                                              defaultValue = AndExpr::create(defaultValue, Expr::createIsZero(match));

                                              // Check if control flow could take this case
                                              bool result;
                                              match = optimizer.optimizeExpr(match, false);
                                              bool success = solver->mayBeTrue(state, match, result);
                                              assert(success && "FIXME: Unhandled solver failure");
                                              (void) success;
                                              if (result) {
                                                  BasicBlock *caseSuccessor = it->second;

                                                  // Handle the case that a basic block might be the target of multiple
                                                  // switch cases.
                                                  // Currently we generate an expression containing all switch-case
                                                  // values for the same target basic block. We spare us forking too
                                                  // many times but we generate more complex condition expressions
                                                  // TODO Add option to allow to choose between those behaviors
                                                  std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> res =
                                                      branchTargets.insert(std::make_pair(
                                                                  caseSuccessor, ConstantExpr::alloc(0, Expr::Bool)));

                                                  res.first->second = OrExpr::create(match, res.first->second);

                                                  // Only add basic blocks which have not been target of a branch yet
                                                  if (res.second) {
                                                      bbOrder.push_back(caseSuccessor);
                                                  }
                                              }
                                          }

                                          // Check if control could take the default case
                                          defaultValue = optimizer.optimizeExpr(defaultValue, false);
                                          bool res;
                                          bool success = solver->mayBeTrue(state, defaultValue, res);
                                          assert(success && "FIXME: Unhandled solver failure");
                                          (void) success;
                                          if (res) {
                                              std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> ret =
                                                  branchTargets.insert(
                                                          std::make_pair(si->getDefaultDest(), defaultValue));
                                              if (ret.second) {
                                                  bbOrder.push_back(si->getDefaultDest());
                                              }
                                          }

                                          // Fork the current state with each state having one of the possible
                                          // successors of this switch
                                          std::vector< ref<Expr> > conditions;
                                          for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                                  ie = bbOrder.end();
                                                  it != ie; ++it) {
                                              conditions.push_back(branchTargets[*it]);
                                          }
                                          std::vector<ExecutionState*> branches;
                                          branch(state, conditions, branches);

                                          std::vector<ExecutionState*>::iterator bit = branches.begin();
                                          for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                                  ie = bbOrder.end();
                                                  it != ie; ++it) {
                                              ExecutionState *es = *bit;
                                              if (es)
                                                  transferToBasicBlock(*it, bb, *es);
                                              ++bit;
                                          }
                                      }
                                      break;
                                  }
        case Instruction::Unreachable:
                                  // Note that this is not necessarily an internal bug, llvm will
                                  // generate unreachable instructions in cases where it knows the
                                  // program will crash. So it is effectively a SEGV or internal
                                  // error.
                                  terminateStateOnExecError(state, "reached \"unreachable\" instruction");
                                  break;

        case Instruction::Invoke:
        case Instruction::Call: {
                                    // Ignore debug intrinsic calls
                                    if (isa<DbgInfoIntrinsic>(i))
                                        break;
                                    CallSite cs(i);

                                    unsigned numArgs = cs.arg_size();
                                    Value *fp = cs.getCalledValue();
                                    Function *f = getTargetFunction(fp, state);

                                    if (isa<InlineAsm>(fp)) {
                                        terminateStateOnExecError(state, "inline assembly is unsupported");
                                        break;
                                    }
                                    // evaluate arguments
                                    std::vector< ref<Expr> > arguments;
                                    arguments.reserve(numArgs);

                                    for (unsigned j=0; j<numArgs; ++j)
                                        arguments.push_back(eval(ki, j+1, state).value);

                                    if (f) {
                                        const FunctionType *fType =
                                            dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
                                        const FunctionType *fpType =
                                            dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

                                        // special case the call with a bitcast case
                                        if (fType != fpType) {
                                            assert(fType && fpType && "unable to get function type");

                                            // XXX check result coercion

                                            // XXX this really needs thought and validation
                                            unsigned i=0;
                                            for (std::vector< ref<Expr> >::iterator
                                                    ai = arguments.begin(), ie = arguments.end();
                                                    ai != ie; ++ai) {
                                                Expr::Width to, from = (*ai)->getWidth();

                                                if (i<fType->getNumParams()) {
                                                    to = getWidthForLLVMType(fType->getParamType(i));

                                                    if (from != to) {
                                                        // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
                                                        bool isSExt = cs.paramHasAttr(i, llvm::Attribute::SExt);
#else
                                                        bool isSExt = cs.paramHasAttr(i+1, llvm::Attribute::SExt);
#endif
                                                        if (isSExt) {
                                                            arguments[i] = SExtExpr::create(arguments[i], to);
                                                        } else {
                                                            arguments[i] = ZExtExpr::create(arguments[i], to);
                                                        }
                                                    }
                                                }

                                                i++;
                                            }
                                        }

                                        executeCall(state, ki, f, arguments);
                                    } else {
                                        ref<Expr> v = eval(ki, 0, state).value;

                                        /* Jiaqi */
                                        if(!isa<ConstantExpr>(v))
                                        {
                                            printf ("AEG: indirect call destination is a symbol. \n");
                                            //terminateStateOnExecError(state, "exploit succeed: symbolic function pointer");
                                            //terminateStateOnExit(state);
                                            //exit(1);
                                        }
                                        if(isa<ConstantExpr>(v))
                                        {
                                            printf ("indirect call destination is a constant. \n");
                                        }
                                        /* /Jiaqi */

					// *Haoxin for AEG
      printf("------ InDirect function call executed!\n");
      //ref<Expr> v = eval(ki, 0, state).value;
      v->dump();
      printf("Location of the instruction : %s\n", state.pc->getSourceLocation().c_str());
      std::string location = state.pc->getSourceLocation();
      if (location.find("test.cc") != std::string::npos)
          printf("This is what I want\n");
      printf("// This the current instruction : \n");
      printf("  dest = %d, operand = %d\n", ki->dest, *ki->operands);
      //ki->inst->dump();
      unsigned current_dest = ki->dest;
      int op = *ki->operands;

      // Solution 1 : Find the name of global function call
      std::string opnd_name;

      for (int i = 0; i < state.stack[state.stack.size()-1].kf->numInstructions; i++){
          if (state.stack[state.stack.size()-1].kf->instructions[i]->dest == op) {
              printf("//AEG: We found the operand instruction in current stack!\n");
              llvm::Instruction * inst = state.stack[state.stack.size()-1].kf->instructions[i]->inst;
              //printf("  dest = %d, operand = %d\n", state.stack[state.stack.size()-1].kf->instructions[i]->dest,
               //       *state.stack[state.stack.size()-1].kf->instructions[i]->operands);
              //inst->dump();
                ref<Expr> base_test = eval(state.stack[state.stack.size()-1].kf->instructions[i], 0, state).value;
                printf("address of function pointer start\n");
                base_test->dump();
                printf("address of function pointer done\n");
              //find the name
              if (inst->getNumOperands() != 1)
                terminateStateOnExecError(state, "Error in handle indirect function call!\n");
              llvm::Value *opnd = inst->getOperand(0);
              if (opnd->hasName()){
                opnd_name = opnd->getName();
                //ref<Expr> base_test = eval(state.stack[state.stack.size()-1].kf->instructions[i], 0, state).value;
                //printf("address of function pointer start\n");
                //base_test->dump();
                //printf("address of function pointer done\n");
              }
              else{
                 ;//terminateStateOnExecError(state, "Error in handle indirect function call (Operand don't have a name)!\n");
              }
              //printf("//We found the name of operand \n");
              printf("  opnd_name = %s\n", opnd_name.c_str());
          }
      }


                                        ExecutionState *free = &state;
                                        bool hasInvalid = false, first = true;

          static bool success_aaw = 0;
                                        /* XXX This is wasteful, no need to do a full evaluate since we
                                           have already got a value. But in the end the caches should
                                           handle it for us, albeit with some overhead. */
                                        do {
                                            v = optimizer.optimizeExpr(v, true);
                                            ref<ConstantExpr> value;

					            // Haoxin AEG
        // For debug purpose
        if (!isa<ConstantExpr>(v)){
            printf("Warning! Calling a symbolic function address\n");
            v->dump();
            printf("symExecuted = %d\n", state.symExecuted);
            //print fpUpdateList
            printf("Size of fpUpdateList = %d\n", state.addressSpace.fpUpdateList.size());
            //std::map<uint64_t, std::vector<long long>>::iterator it;
            for (auto it = state.addressSpace.fpUpdateList.begin(); it != state.addressSpace.fpUpdateList.end(); it++){
                printf("key = %lu\n", it->first);
                //for (auto i : it->second)
                std::vector<long long> temp = it->second;
                printf("  address: %llu  offset: %llu\n", temp[0], temp[1]);
            }
            //terminateStateOnExecError(state, "Debug: calling a symbolic function address!\n");
            //exit(1);
            //ref<Expr> t = ConstantExpr::create(0x555555554889, 64);
            //executeMemoryOperation(state, true, v, t, 0);
            //terminateStateOnExit(state);
            //terminateStateEarly(state, "Find a possible exploit, stop and generate exploit (ktest file)!");
        }

                                        StatePair res;
                                        //if(success_aaw == 0){
                                            //printf("v is still a symbol but no aaw was detected --------------------------\n");
                                            bool success = solver->getValue(*free, v, value);
                                            assert(success && "FIXME: Unhandled solver failure");
                                            (void) success;
                                            res = fork(*free, EqExpr::create(v, value), true);
                                        //}
                                            if (res.first) {

          // Haoxin for AEG
          if (state.addressSpace.WriteExploitCapability.size() == 0){
            printf("**************We have found an exploitable point************\n");
            printf("size of FunctionCalls = %d\n", FunctionCalls.size());
            //Ready to overwrite the function pointer address
            printf("handler's address = %d\n", FunctionCalls["handler"]);
            printf("goodFunc's address = %d\n", FunctionCalls["goodFunc"]);
            printf("badFunc's address = %d\n", FunctionCalls["badFunc"]);
          }

          //Iteratively check wether there is a successful hajacking
          ref<Expr> base = ConstantExpr::create(FunctionCalls["handler"], 64);
          //if (state.addressSpace.WriteExploitCapability.size() != 0){
            //add new constraints here
          std::map<ref<Expr>, ref<Expr>>::iterator it_wec;
          ref<Expr> pre_write;

            for (it_wec = state.addressSpace.WriteExploitCapability.begin(); it_wec != state.addressSpace.WriteExploitCapability.end(); it_wec++){
                printf("-------------------------AEG: Now handling AAW Exploit----------------------------------\n");
                ref<Expr> temp = it_wec->first;
                pre_write = it_wec->second;
                temp->dump();
                std::string name;
                std::map<const llvm::GlobalValue*, ref<ConstantExpr>>::iterator it_map;
                ref<ConstantExpr> fp_expr;
                printf("globalAddresses.size = %d\n", globalAddresses.size());
                //Step 1: add constraint of "symbolic expression == function pointer expression"
                if (opnd_name.size() != 0){ // situation 1: call a global function pointer
                for (it_map = globalAddresses.begin(); it_map != globalAddresses.end(); ++it_map){
                    const llvm::GlobalValue *v_temp = it_map->first;
                    name = v_temp->getGlobalIdentifier().c_str();
                    //find expr for opnd_name
                    if (name == opnd_name){
                        fp_expr = it_map->second;
                        printf("fp_expr's value: %d\n", fp_expr->getZExtValue());
                        fp_expr->dump();

                        //add constraint of "symbolic expression == function pointer expression"
                        Expr::Width type = fp_expr->getWidth();

                        if (type != Expr::Int64)
                            terminateStateOnExecError(state, "Type mismatch while adding additional constraints (handling indirect call)!");
                        //TODO Here we need use the address from elf file
                        std::string program("test");
                        elf_parser::Elf_parser elf_parser(program);

                        std::vector<elf_parser::symbol_t> syms = elf_parser.get_symbols();
                        unsigned long long fp_pie = 0;
                        for (auto s : syms){
                            //printf("symbol_name = %s ; symbol_value = %08x \n", s.symbol_name.c_str(), s.symbol_value);
                            if (s.symbol_name == opnd_name){
                                fp_pie = s.symbol_value;
                            }
                        }
                        if (fp_pie == 0)
                            terminateStateOnExecError(state, "Failed to find a name of global function pointer in binary (is this the name issue?)!");
                        unsigned long long heap_base = 0x555555554000;
                        printf("native funtion pointer address is %p \n", heap_base + fp_pie);
                        //print_symbols(syms); //printf all symbol details

                        // Accoding to the recordings in fpUpdateList, decide whether it's a directly write or an indirectly write
                        long long addr_in_list = state.addressSpace.fpUpdateList[FunctionCalls[name]][0];
                        printf("addr_in_list = %lld\n", addr_in_list);
                        long offset = 0;
                        //TODO Deal with directly write
                        if (addr_in_list == 0) {
		                    printf("Get constant from symbolic address!!!\n");
		                    ref<Expr> p_address = v;
	                        Expr *pp = p_address.get();
	                        for (int i = 0; i < pp->getNumKids(); i++){
		                        if (isa<ConstantExpr>(pp->getKid(i))){
			                        //update constant to fpUpdateList
			                        pp->getKid(i)->dump();
			                        ref<ConstantExpr> base_fp = toConstant(state, pp->getKid(i), "constant in symbolic fp");
        		                    uint64_t fp_address = base_fp->getZExtValue();
                                    offset += fp_address;
		                        }
	                        }
                	        printf("direct FP to be written = %p \n", fp_pie + heap_base);
                		    printf("aaw value to be written = %p \n", fp_pie + heap_base - offset);
                            klee_warning("AEG: offset to target object %lld\n", offset);
				            //ref<Expr> fp = ConstantExpr::create(fp_pie + heap_base, Expr::Int64); //test global_a
				ref<Expr> fp = ConstantExpr::create(0x5555557578e0, Expr::Int64); //test heap object
				//0x555555756cf0
                            if (name != "__exit_cleanup") { //Just omit this buildin function
                                printf("*************AEG: Write the first constraint***********\n");
                                addConstraint(state, EqExpr::create(temp, fp));
                            }
                            break; //alreadly successfully write a constraint
                        }else {
                            //TODO Deal with indirect write (data-dependency)
                            printf("size of fpUpdateList = %d\n", state.addressSpace.fpUpdateList.size());
			                for (auto it = state.addressSpace.fpUpdateList.begin(); it != state.addressSpace.fpUpdateList.end(); it++){
                		        printf("key = %lld\n", it->first);
                                //for (auto i : it->second)
                                std::vector<long long> temp = it->second;
                                printf("  address: %llu  offset: %lld\n", temp[0], temp[1]);
                                offset += temp[1];
                            }
		                    printf("Get constant from symbolic address!!!\n");
		                    ref<Expr> p_address = v;
	                        Expr *pp = p_address.get();
	                        for (int i = 0; i < pp->getNumKids(); i++){
		                        if (isa<ConstantExpr>(pp->getKid(i))){
			                        //update constant to fpUpdateList
			                        pp->getKid(i)->dump();
			                        ref<ConstantExpr> base_fp = toConstant(state, pp->getKid(i), "constant in symbolic fp");
        		                    uint64_t fp_address = base_fp->getZExtValue();
                                    offset += fp_address;
		                        }
	                        }
                            printf("offset overall = %lld\n", offset);
                            //TODO find out the variable name to be written
                            unsigned long indirect_address = 0;

                            if (indirect_name.size() != 0) { //TODO should be changed with address
                                printf("size of FunctionAddressMap = %d\n", FunctionCalls.size());
			                    for (auto it = FunctionCalls.begin(); it != FunctionCalls.end(); it++){
                		            //printf("key = %s \t", it->first.c_str());
                		            //printf("value = %lld \n", it->second);
                                    if (it->second == addr_in_list){
                                        for (auto s : syms){
                                            //printf("symbol_name = %s ; symbol_value = %08x \n", s.symbol_name.c_str(), s.symbol_value);
                                            if (s.symbol_name == it->first){
                                                indirect_address = s.symbol_value;
                                            }
                                            //else{
                                            //    terminateStateOnExecError(state, "AEG: Failed to find the global variable name in ELF file!");
                                            //}
                                        }
                                    }
                                }
                            }else {
                                klee_warning("AEG: Can not find the name of variable to be written!");
                                //break;
                            }
                	        printf("indirect_address to be written = %p \n", indirect_address + heap_base);
                		    printf("aaw value to be written = %p \n", indirect_address + heap_base - offset);
                            klee_warning("AEG: offset to target object %lld\n", offset);
				            ref<Expr> fp = ConstantExpr::create(indirect_address + heap_base, Expr::Int64); //test global_a
			                //ref<Expr> fp = ConstantExpr::create(toConstant(state, state.addressSpace.FunctionAddressMap["handler"], "function pointer write to constraints")->getZExtValue(), Expr::Int64);
                            //if (cast<ConstantExpr>(fp)->isTrue()) { //We can not handle it if fp is a symbolic variable?
                            if (name != "__exit_cleanup") { //Just omit this buildin function
                                printf("*************AEG: Write the first constraint***********\n");
                                addConstraint(state, EqExpr::create(temp, fp));
                            }
                            //ref<Expr> t = ConstantExpr::create(0x555555554889, 64);
                            //executeMemoryOperation(state, true, fp, t, 0);
                            //break; // stop if we already add this constraint
                            success_aaw = 1;
                            break; // alreadly successfully write the indirectly write constraint
                            }
                        }
                    }
                    }else { // situatio2: no name record: it's a local funtion pointer //TODO
                        printf("AEG: Calling a local function pointer!\n");
                        //TODO Here we need use the address from elf file
                        opnd_name = "handler"; //should be fetch from symbolic name;
                        std::string program("test");
                        elf_parser::Elf_parser elf_parser(program);

                        std::vector<elf_parser::symbol_t> syms = elf_parser.get_symbols();
                        unsigned long long fp_pie = 0;
                        for (auto s : syms){
                            //printf("symbol_name = %s ; symbol_value = %08x \n", s.symbol_name.c_str(), s.symbol_value);
                            if (s.symbol_name == opnd_name){
                                fp_pie = s.symbol_value;
                            }
                        }
                        if (fp_pie == 0)
                            terminateStateOnExecError(state, "Failed to find a name of global function pointer in binary (is this the name issue?)!");
                        unsigned long long heap_base = 0x555555554000;
                        printf("native funtion pointer address is %p \n", heap_base + fp_pie);

                        //TODO checking for indirect write (data-dependency)
                        long offset = 0;
                        printf("size of fpUpdateList = %d\n", state.addressSpace.fpUpdateList.size());
			            for (auto it = state.addressSpace.fpUpdateList.begin(); it != state.addressSpace.fpUpdateList.end(); it++){
                		    printf("key = %lld\n", it->first);
                            //for (auto i : it->second)
                            std::vector<long long> temp = it->second;
                            printf("  address: %llu  offset: %lld\n", temp[0], temp[1]);
                            offset += temp[1];
                        }
		                printf("Get constant from symbolic address!!!\n");
		                ref<Expr> p_address = v;
	                    Expr *pp = p_address.get();
	                    for (int i = 0; i < pp->getNumKids(); i++){
		                    if (isa<ConstantExpr>(pp->getKid(i))){
			                //update constant to fpUpdateList
			                pp->getKid(i)->dump();
			                ref<ConstantExpr> base_fp = toConstant(state, pp->getKid(i), "constant in symbolic fp");
        		            uint64_t fp_address = base_fp->getZExtValue();
                            offset += fp_address;
		                    }
	                    }
                        printf("offset overall = %lld\n", offset);
                        //TODO find out the variable name to be written
                        unsigned long indirect_address = 0;
                        if (indirect_name.size() != 0) { //TODO should be changed with address
                            printf("size of FunctionAddressMap = %d\n", FunctionCalls.size());
			                for (auto it = FunctionCalls.begin(); it != FunctionCalls.end(); it++){
                		        //printf("key = %s \t", it->first.c_str());
                		        //printf("value = %lld \n", it->second);
                                for (auto s : syms){
                                    //printf("symbol_name = %s ; symbol_value = %08x \n", s.symbol_name.c_str(), s.symbol_value);
                                    if (s.symbol_name == "global_a"){
                                        indirect_address = s.symbol_value;
                                    }
                                }
                            }
                        }else {
                            klee_warning("AEG: Can not find the name of variable to be written!");
                            //break;
                        }
                		printf("indirect_address to be written = %p \n", indirect_address + heap_base);
                		printf("aaw value to be written = %p \n", indirect_address + heap_base - offset);
                        klee_warning("AEG: offset to target object %lld\n", offset);
				        ref<Expr> fp = ConstantExpr::create(indirect_address + heap_base, Expr::Int64); //test global_a
			            //ref<Expr> fp = ConstantExpr::create(toConstant(state, state.addressSpace.FunctionAddressMap["handler"], "function pointer write to constraints")->getZExtValue(), Expr::Int64);
                        //if (cast<ConstantExpr>(fp)->isTrue()) { //We can not handle it if fp is a symbolic variable?
                        if (name != "__exit_cleanup") { //Just omit this buildin function
                            printf("*************AEG: Write the first constraint***********\n");
                            addConstraint(state, EqExpr::create(temp, fp));
                        }
                        //ref<Expr> t = ConstantExpr::create(0x555555554889, 64);
                        //executeMemoryOperation(state, true, fp, t, 0);
                        //break; // stop if we already add this constraint
                        success_aaw = 1;
                     } //end else of local function pointer
                    }

          //std::set<uint64_t>::iterator it;
          //for (it = legalFunctions.begin(); it != legalFunctions.end(); ++it){
            //overwrite
            //ref<Expr> temp = ConstantExpr::create(*it, 64);
            //executeMemoryOperation(state, true, base, temp, 0);
        uint64_t addr = 0;
        //if (!success_aaw)
          addr = value->getZExtValue();

        if (!isa<ConstantExpr>(v)){
          //addr = FunctionCalls["badFunc"];
          //addr = 0x555555554889;
            terminateStateOnExecError(state, "AEG: Find a possible exploit");
            break;
        }
        printf("Function call address is %lld\n", addr);
        /*
        for (auto s : state.stack){
                printf("//In indirect call : Instructions in stack: Num.%d\n", i);
            for (int i = 0; i < s.kf->numInstructions; i++){
                llvm::Instruction * inst = s.kf->instructions[i]->inst;
                inst->dump();
            }
        }
        */
        //else
          //addr = pre_write->getZExtValue();

          //if (location.find("test.cc") != std::string::npos)
          //  addr = FunctionCalls["_Z7badFuncPKi"];
                                                if (legalFunctions.count(addr)) {
                                                    f = (Function*) addr;

                                                    // Don't give warning on unique resolution
                                                    if (res.second || !first)
                                                        klee_warning_once(reinterpret_cast<void*>(addr),
                                                                "resolved symbolic function pointer to: %s",
                                                                f->getName().data());

                                                    //if (!isa<ConstantExpr>(v)){
                                                    //    printf("Function call address in KLEE = %lld\n", addr);
                                                    //}
                                                    executeCall(*res.first, ki, f, arguments);
                                                    //if (addr == FunctionCalls["badFunc"]){
                                                    //    terminateStateOnExecError(state, "Found a possible expolit!!!\n");
                                                    //}
                                                } else {
                                                    if (!hasInvalid) {
                                                        terminateStateOnExecError(state, "invalid function pointer");
                                                        // /* Jiaqi */
                                                        // printf ("state: %p. \n", state);
                                                        // /* /Jiaqi */
                                                        hasInvalid = true;
                                                    }
                                                }
                                            }

                                            first = false;
                                            free = res.second;
                                        } while (free);
                                    }
                                    break;
                                }
        case Instruction::PHI: {
                                   ref<Expr> result = eval(ki, state.incomingBBIndex, state).value;
                                   bindLocal(ki, state, result);
                                   break;
                               }

                               // Special instructions
        case Instruction::Select: {
                                      // NOTE: It is not required that operands 1 and 2 be of scalar type.
                                      ref<Expr> cond = eval(ki, 0, state).value;
                                      ref<Expr> tExpr = eval(ki, 1, state).value;
                                      ref<Expr> fExpr = eval(ki, 2, state).value;
                                      ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
                                      bindLocal(ki, state, result);
                                      break;
                                  }

        case Instruction::VAArg:
                                  terminateStateOnExecError(state, "unexpected VAArg instruction");
                                  break;

                                  // Arithmetic / logical

        case Instruction::Add: {
                                   ref<Expr> left = eval(ki, 0, state).value;
                                   ref<Expr> right = eval(ki, 1, state).value;
                                   bindLocal(ki, state, AddExpr::create(left, right));
                                   break;
                               }

        case Instruction::Sub: {
                                   ref<Expr> left = eval(ki, 0, state).value;
                                   ref<Expr> right = eval(ki, 1, state).value;
                                   bindLocal(ki, state, SubExpr::create(left, right));
                                   break;
                               }

        case Instruction::Mul: {
                                   ref<Expr> left = eval(ki, 0, state).value;
                                   ref<Expr> right = eval(ki, 1, state).value;
                                   bindLocal(ki, state, MulExpr::create(left, right));
                                   break;
                               }

        case Instruction::UDiv: {
                                    ref<Expr> left = eval(ki, 0, state).value;
                                    ref<Expr> right = eval(ki, 1, state).value;
                                    ref<Expr> result = UDivExpr::create(left, right);
                                    bindLocal(ki, state, result);
                                    break;
                                }

        case Instruction::SDiv: {
                                    ref<Expr> left = eval(ki, 0, state).value;
                                    ref<Expr> right = eval(ki, 1, state).value;
                                    ref<Expr> result = SDivExpr::create(left, right);
                                    bindLocal(ki, state, result);
                                    break;
                                }

        case Instruction::URem: {
                                    ref<Expr> left = eval(ki, 0, state).value;
                                    ref<Expr> right = eval(ki, 1, state).value;
                                    ref<Expr> result = URemExpr::create(left, right);
                                    bindLocal(ki, state, result);
                                    break;
                                }

        case Instruction::SRem: {
                                    ref<Expr> left = eval(ki, 0, state).value;
                                    ref<Expr> right = eval(ki, 1, state).value;
                                    ref<Expr> result = SRemExpr::create(left, right);
                                    bindLocal(ki, state, result);
                                    break;
                                }

        case Instruction::And: {
                                   ref<Expr> left = eval(ki, 0, state).value;
                                   ref<Expr> right = eval(ki, 1, state).value;
                                   ref<Expr> result = AndExpr::create(left, right);
                                   bindLocal(ki, state, result);
                                   break;
                               }

        case Instruction::Or: {
                                  ref<Expr> left = eval(ki, 0, state).value;
                                  ref<Expr> right = eval(ki, 1, state).value;
                                  ref<Expr> result = OrExpr::create(left, right);
                                  bindLocal(ki, state, result);
                                  break;
                              }

        case Instruction::Xor: {
                                   ref<Expr> left = eval(ki, 0, state).value;
                                   ref<Expr> right = eval(ki, 1, state).value;
                                   ref<Expr> result = XorExpr::create(left, right);
                                   bindLocal(ki, state, result);
                                   break;
                               }

        case Instruction::Shl: {
                                   ref<Expr> left = eval(ki, 0, state).value;
                                   ref<Expr> right = eval(ki, 1, state).value;
                                   ref<Expr> result = ShlExpr::create(left, right);
                                   bindLocal(ki, state, result);
                                   break;
                               }

        case Instruction::LShr: {
                                    ref<Expr> left = eval(ki, 0, state).value;
                                    ref<Expr> right = eval(ki, 1, state).value;
                                    ref<Expr> result = LShrExpr::create(left, right);
                                    bindLocal(ki, state, result);
                                    break;
                                }

        case Instruction::AShr: {
                                    ref<Expr> left = eval(ki, 0, state).value;
                                    ref<Expr> right = eval(ki, 1, state).value;
                                    ref<Expr> result = AShrExpr::create(left, right);
                                    bindLocal(ki, state, result);
                                    break;
                                }

                                // Compare

        case Instruction::ICmp: {
                                    CmpInst *ci = cast<CmpInst>(i);
                                    ICmpInst *ii = cast<ICmpInst>(ci);

                                    switch(ii->getPredicate()) {
                                        case ICmpInst::ICMP_EQ: {
                                                                    ref<Expr> left = eval(ki, 0, state).value;
                                                                    ref<Expr> right = eval(ki, 1, state).value;
                                                                    ref<Expr> result = EqExpr::create(left, right);
                                                                    bindLocal(ki, state, result);
                                                                    break;
                                                                }

                                        case ICmpInst::ICMP_NE: {
                                                                    ref<Expr> left = eval(ki, 0, state).value;
                                                                    ref<Expr> right = eval(ki, 1, state).value;
                                                                    ref<Expr> result = NeExpr::create(left, right);
                                                                    bindLocal(ki, state, result);
                                                                    break;
                                                                }

                                        case ICmpInst::ICMP_UGT: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = UgtExpr::create(left, right);
                                                                     bindLocal(ki, state,result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_UGE: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = UgeExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_ULT: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = UltExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_ULE: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = UleExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_SGT: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = SgtExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_SGE: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = SgeExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_SLT: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = SltExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        case ICmpInst::ICMP_SLE: {
                                                                     ref<Expr> left = eval(ki, 0, state).value;
                                                                     ref<Expr> right = eval(ki, 1, state).value;
                                                                     ref<Expr> result = SleExpr::create(left, right);
                                                                     bindLocal(ki, state, result);
                                                                     break;
                                                                 }

                                        default:
                                                                 terminateStateOnExecError(state, "invalid ICmp predicate");
                                    }
                                    break;
                                }

                                // Memory instructions...
        case Instruction::Alloca: {
                                      AllocaInst *ai = cast<AllocaInst>(i);
                                      unsigned elementSize =
                                          kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
                                      ref<Expr> size = Expr::createPointer(elementSize);
                                      if (ai->isArrayAllocation()) {
                                          ref<Expr> count = eval(ki, 0, state).value;
                                          count = Expr::createZExtToPointerWidth(count);
                                          size = MulExpr::create(size, count);
                                      }
                                      executeAlloc(state, size, true, ki);
                                      break;
                                  }
	/* orginal load and store
        case Instruction::Load: {
                                    ref<Expr> base = eval(ki, 0, state).value;
                                    executeMemoryOperation(state, false, base, 0, ki);
                                    break;
                                }
        case Instruction::Store: {
                                     ref<Expr> base = eval(ki, 1, state).value;
                                     ref<Expr> value = eval(ki, 0, state).value;
                                     executeMemoryOperation(state, true, base, value, 0);
                                     break;
                                 }
	*/
	// Haoxin for AEG
	//New load and store
	 case Instruction::Load: { //read operation
    //printf("yyyyyyyyyyyyyyyyyyyyyyyy Read yyyyyyyyyyyyyyyyyyyyyyyy\n");
    //ki->inst->dump();
    ref<Expr> base = eval(ki, 0, state).value;
    //base->dump();
    //printf("print base in load instruction after eval:\n");
    //base->dump();
    //std::string str_addressInfo = getAddressInfo(state, base);
    //printf("    %s\n", str_addressInfo.c_str());

        std::string location = ki->getSourceLocation();
        if (location.find("test.c:37") != std::string::npos || location.find("test.c:38") != std::string::npos){
            printf("111 dest = %d, operands = %d\n", ki->dest, *ki->operands);
        }
    //if (!isa<ConstantExpr>(base)){
        //state.addressSpace.ReadExploitCapability.insert(base);
        //printf("ReadExploitCapability.size() = %d\n", state.addressSpace.ReadExploitCapability.size());
    //    printf("+++This is a symbolic Load instruction!\n");
        //std::set<std::string> nameList;
        //const Array *array = scan2(base, nameList);
        //printf("array->name = %s \t size of nameList = %d\n", array->name.c_str(), nameList.size());
        //printf("array->size = %d\n", array->size);
        //std::string location = ki->getSourceLocation();
        //if (location.find("test.c:37") != std::string::npos){
        //    printf("111\n");
        //}
      //  base->dump();
      //  break;
    //}
    executeMemoryOperation(state, false, base, 0, ki);
    break;
  }
  case Instruction::Store: { //write operation
    //printf("xxxxxxxxxxxxxxxxxxxxxxxxx Write xxxxxxxxxxxxxxxxxxxxxxx\n");
    //ki->inst->dump();
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;
    //base->dump();
    //value->dump();
    //ref<ConstantExpr> value_temp = toConstant(state, value, "...");
    static std::vector<std::string> symFpName;
    static std::vector<uint64_t> fpAddress;
    if (!isa<ConstantExpr>(base)){
        state.addressSpace.WriteExploitCapability.insert(std::pair<ref<Expr>, ref<Expr>>(base, value));
        printf("WriteExploitCapability.size() = %d\n", state.addressSpace.WriteExploitCapability.size());
        printf("+++This is a symbolic Store instruction!\n");
        //iteratively visit globalAddresses
        std::map<const llvm::GlobalValue*, ref<ConstantExpr>>::iterator it_map;
        for (it_map = globalAddresses.begin(); it_map != globalAddresses.end(); ++it_map){
            if (it_map->first->getValueType()->isPointerTy()){
                const llvm::GlobalValue *v_temp = it_map->first;
                ref<ConstantExpr> e_temp = it_map->second;
                std::string name = v_temp->getGlobalIdentifier().c_str();
                //find os for mo
                if ((name.find("_") == std::string::npos) &&
                        (name.find("environ") == std::string::npos) &&
                        (name.find("stderr") == std::string::npos) &&
                        (name.find("stdin") == std::string::npos) &&
                        (name.find("stdout") == std::string::npos)){
                printf("name : %s\n", v_temp->getGlobalIdentifier().c_str());
                e_temp->dump();
                //if (name.find("__")){
                //Expr::Width type = getWidthForLLVMType(ki->inst->getType());
                Expr::Width type = e_temp->getWidth();
                unsigned bytes = Expr::getMinBytesForWidth(type);
                e_temp = optimizer.optimizeExpr(e_temp, true);
                ObjectPair op;
                bool success;
                solver->setTimeout(coreSolverTimeout);
                if (!state.addressSpace.resolveOne(state, solver,e_temp, op, success)){
                    e_temp = toConstant(state, e_temp, "111");
                    success = state.addressSpace.resolveOne(cast<ConstantExpr>(e_temp), op);
                }
                solver->setTimeout(time::Span());
                if (success){
                    const MemoryObject *mo = op.first;
                    const ObjectState *os = op.second;
                    ref<Expr> offset = mo->getOffsetExpr(e_temp);
                    ref<Expr> check = mo->getBoundsCheckOffset(offset, bytes);
                    check = optimizer.optimizeExpr(check, true);

                    bool inBounds;
                    bool success = solver->mustBeTrue(state, check, inBounds);

                    if (inBounds){
                        ref<Expr> result = os->read(offset, type);
                        //result->dump();
                        printf("ObjectState readOnly = %d\n", os->readOnly);
                        state.addressSpace.FunctionAddressMap[name] = result;
                    }
                    //make variable symbolic
                    std::string sym_name = "sym_" + name + "_" + std::to_string(mo->address);
                    /*
                    ObjectState *os_temp = new ObjectState(mo);
                    const Array *array = arrayCache.CreateArray(sym_name, 8);
                    ref<Expr> symExpr = ReadExpr::createTempRead(array, Expr::Int64);
                    symExpr->dump();
                    */
                    printf("Execute executeMakeSymbolic!!!\n");
                    executeMakeSymbolic(state, mo, sym_name);
                    symFpName.push_back(sym_name);
                    fpAddress.push_back(mo->address);
                    // initialize symUpdateList
                    std::vector<long long> temp_variable;
                    state.addressSpace.fpUpdateList.insert(std::pair<uint64_t, std::vector<long long>>(mo->address, temp_variable));
                    state.addressSpace.fpUpdateList[mo->address] = {0, 0};
                    printf("size of symFpName : %d\n", symFpName.size());
                    //use the symbolic one replace the orginal one
                    //istate.addressSpace.FPAddressSymExprMap.insert(std::pair<uint64_t, ref<Expr>>(mo->address, symExpr));
                    printf("size of FPAddressSymExprMap = %d\n", state.addressSpace.FPAddressSymExprMap.size());
                    }
                } //end find string
            }
        }
        //std::set<std::string> nameList;
        //const Array *array = scan2(base, nameList);
        //printf("array->name = %s \t size of nameList = %d\n", array->name.c_str(), nameList.size());
        //printf("array->size = %d\n", array->size);
        //ref<Expr> fp = ConstantExpr::create(9999, 64);
        //ref<Expr> last = base->getKid(1)->getKid(1)->getKid(1)->getKid(1);
        //last->dump();
        //addConstraint(state, EqExpr::create(fp, base));
        //addConstraint(state, base); //crash
        //base = fp;
        //printf("***LOOK*** %d\n", base->getKind());
        base->dump();
        break;
    }
    //printf("size of symFpName : %d\n", symFpName.size());
    //replace the original to symbolic
    uint64_t address = toConstant(state, base, "address")->getZExtValue();
    std::map<uint64_t, ref<Expr>>::iterator iter;
    if (state.addressSpace.WriteExploitCapability.size() != 0){
        printf("Before replacing: address = %d\n", address);
        iter = state.addressSpace.FPAddressSymExprMap.find(address);
        if (iter != state.addressSpace.FPAddressSymExprMap.end()){
            printf("We found it !!!!!!!!!!!!!\n");
            //now replace
            base = state.addressSpace.FPAddressSymExprMap[address];
            printf("After replacing base !!!!!!!!!!!!!\n");
            base->dump();
        }
    }

      /*
      std::string location = state.pc->getSourceLocation();
      if (location.find("memcpy.c") != std::string::npos){
          printf("----This is a store instruction in memcpy!----\n");
          //ki->inst->dump();
          base->dump();
          value->dump();
          printf("----End----\n");
      }
	*/
    if (value.get() == NULL){
        printf("+++This is a symbolic Store after Load instruction!\n");
        base->dump();
        break;
    }

    std::set<std::string> nameList;
    const Array *array;
    static int isSymFp;
    static int isFpBase;

    // is a fp base?
    ref<ConstantExpr> base_temp = toConstant(state, base, "...");
    uint64_t addr = base_temp->getZExtValue();
    //printf("size of fpAddress = %d\n", fpAddress.size());
    for (int i = 0; i < fpAddress.size(); i++){
        printf("in fpAddress : %d\n", fpAddress[i]);
        if (addr == fpAddress[i]){
            isFpBase = 1;
            break;
        }
    }

    // Situation 2
    //printf("isFpBase = %d, isSymFp = %d\n", isFpBase, isSymFp);
    //deal with symboli/concrete value
    if (!isa<ConstantExpr>(value) && isFpBase == 1){ // deal with symbolic value
        //printf("array->name = %s \t size of nameList = %d\n", array->name.c_str(), nameList.size());
        //printf("array->size = %d\n", array->size);
        std::string location = state.pc->getSourceLocation();
        if (location.find("test.c") != std::string::npos){ //TODO for debug purpose
            //traverse allocaMap
            for (auto iter = allocaMap.begin(); iter != allocaMap.end(); iter++){
                printf("dest : %d --- address : %d\n", iter->first, iter->second);
            }
            printf("****////*This is a symbolic value\n");
            value->dump();
            printf("****////*Corresponding base\n");
            base->dump();
        }
	printf("****////situation 2 : *Corresponding base\n");
	value->dump();
	/*
	ref<Expr> p_address = value;
	Expr *pp = p_address.get();
	for (int i = 0; i < pp->getNumKids(); i++){
		if (isa<ConstantExpr>(pp->getKid(i))){
			//update constant to fpUpdateList
			pp->getKid(i)->dump();
			ref<ConstantExpr> base_fp = toConstant(state, base, "base_fp");
        		uint64_t fp_address = base_fp->getZExtValue();
		}
	}
	*/
        executeMemoryOperation(state, true, base, value, 0); // Save it
        state.symExecuted = 1;
    }

    // Situation 1 & 4
    if (isFpBase == 1 && isa<ConstantExpr>(value)){ // deal with concrete value
        printf("****////*This is a concrete value to Fp\n");
        base->dump();
        ref<ConstantExpr> base_fp = toConstant(state, base, "base_fp");
        std::vector<long long> variable_temp;
        uint64_t fp_address = base_fp->getZExtValue();
        printf("////***** Do backtracing : Begin *****/////\n");
        //traverse allocaMap
        for (auto iter = allocaMap.begin(); iter != allocaMap.end(); iter++){
            printf("dest : %d --- address : %p\n", iter->first, iter->second);
        }
        //ki->inst->dump();
        if (*ki->operands < 0){
            printf("    This is a direct constant to be stored\n");
        }
        if (*ki->operands > 0){
            printf("    This is a indirect constant(loaded from other variable) to be stored\n");
            //doing trace here
            for (int i = 0; i < state.stack[state.stack.size()-1].kf->numInstructions; i++){
                if (state.stack[state.stack.size()-1].kf->instructions[i]->dest == *ki->operands) {
                    printf("//We found the operand instruction in current stack before tracing!\n");
                    llvm::Instruction * inst = state.stack[state.stack.size()-1].kf->instructions[i]->inst;
                    printf("  dest = %d, operand = %d\n", state.stack[state.stack.size()-1].kf->instructions[i]->dest,
                           *state.stack[state.stack.size()-1].kf->instructions[i]->operands);
                    printf("  inst->getOpcode() = %d\n", inst->getOpcode());
                    //inst->dump();
                    //continue to trace back
                    printf("  Keep tracing until we found the Load instruction\n");
                    llvm::Instruction *inst_temp;
                    KInstruction *ki_temp;
                    static long long offset = 0;
                    while (inst_temp->getOpcode() != Instruction::Load){
                        //checking and recording for add/sub instructions first
                        //checking for load instruction
                        if (inst_temp->getOpcode() == Instruction::Add){
                            ref<Expr> right = eval(ki_temp, 1, state).value;
                            ref<ConstantExpr> add_value = toConstant(state, right, "add in backtracing");
                            printf("    add value : %d\n", add_value->getZExtValue());
                            offset += add_value->getZExtValue();
                        }
                        if (inst_temp->getOpcode() == Instruction::Sub){
                            ref<Expr> right = eval(ki_temp, 1, state).value;
                            ref<ConstantExpr> sub_value = toConstant(state, right, "sub in backtracing");
                            printf("    sub value : %d\n", sub_value->getZExtValue());
                            offset -= sub_value->getZExtValue();
                        }
                        //TODO find out the dest of ki_temp ? or directly interpreter all Arithmetic operation?
                        if (inst_temp->getOpcode() == Instruction::SExt){
                            CastInst *ci = cast<CastInst>(inst_temp);
                            ref<Expr> sext = SExtExpr::create(eval(ki_temp, 0, state).value, getWidthForLLVMType(ci->getType()));
                            ref<ConstantExpr> sext_value = toConstant(state, sext, "sext in backtracing");
                            printf("    sext value : %d\n", sext_value->getZExtValue());
                        }
                        inst_temp = state.stack[state.stack.size()-1].kf->instructions[i]->inst;
                        ki_temp = state.stack[state.stack.size()-1].kf->instructions[i];
                        //inst_temp->dump();
                        if (inst_temp->getOpcode() == Instruction::Load){
                            //TODO ad-hoc solution to solve the * issue
                            if (inst_temp->getType()->getTypeID() != 15) //TODO for pointer?
                                ki_temp = state.stack[state.stack.size()-1].kf->instructions[i-1];
                            printf("    dest of loaded from  : %d\n", *ki_temp->operands);
                            printf("    dest   : %d\n", ki_temp->dest);
                            printf("    address of the loaded variable :%lu \n", allocaMap[*ki_temp->operands]);
                            //printf("    address of the loaded variable ++++ :%p \n", ki_temp->operands);
                            // find the load address so that we don't need to care about the different types of objects
                            ref<Expr> base_target = eval(ki_temp, 0, state).value;
                            printf("directly load address start\n");
                            base_target->dump();
                            printf("directly load address done\n");
                            //start to find the address of different objects;
                            //current support global variables and heap objects
                            ref<ConstantExpr> target_value = toConstant(state, base_target, "target_address");
                            uint64_t target_address = target_value->getZExtValue();

                            //find it in ELF file;
                            std::string target_name = inst_temp->getOperand(0)->getName().str();
                            //opnd_name = "handler"; //should be fetch from symbolic name;
                            printf(" +++ target_name = %s, size of target_name = %d\n", target_name.c_str(), target_name.size());
                            unsigned long long fp_pie = 0;
                            if (target_name.size() == 0 && target_address > 0x600000000000){ //a heap object
                                printf("Great! This is a target object in heap!\n");
                            }
                            else if (target_name.size() != 0){ // a possible global object
                                std::string program("test");
                                elf_parser::Elf_parser elf_parser(program);

                                std::vector<elf_parser::symbol_t> syms = elf_parser.get_symbols();
                                for (auto s : syms){
                                    //printf("symbol_name = %s ; symbol_value = %08x \n", s.symbol_name.c_str(), s.symbol_value);
                                    if (s.symbol_name == target_name){
                                        fp_pie = s.symbol_value;
                                    }
                                }
                                if (fp_pie != 0){
                                    printf("Great! This is a target object in global!\n");
                                    //terminateStateOnExecError(state, "Failed to find a name of global function pointer in binary (is this the name issue?)!");
                                    unsigned long long heap_base = 0x555555554000;
                                    printf("native global variable address is %p \n", heap_base + fp_pie);
                                }else {
                                    terminateStateOnExecError(state, "Something error in finding address in ELF file when performing backwardTracing");
                                }
                            }
                            else{ // not avaiable object
                            //if (target_address < 0x600000000000 && target_name.size() == 0) {
                                printf("Opps, This is a not a global variable or heap object\n");
                            }
                            //TODO find the name of load
                            //llvm::AllocaInst *allocaTest = dyn_cast<llvm::AllocaInst>(&*inst_temp);
                            printf(" +++ name = %s\n", inst_temp->getOperand(0)->getName().str().c_str());
                            indirect_name = inst_temp->getOperand(0)->getName().str();
                            //state.addressSpace.fpUpdateList[fp_address] = {(long long) FunctionCalls[indirect_name], offset};
                //Expr::Width type = e_temp->getWidth();
                unsigned bytes = Expr::getMinBytesForWidth(Expr::Int64);
                //e_temp = optimizer.optimizeExpr(e_temp, true);
                ref<Expr> e_temp = ConstantExpr::create(allocaMap[*ki_temp->operands], Expr::Int64);
		        //ref<Expr> e_temp = ConstantExpr::create(0x655555756cf0, Expr::Int64);

                e_temp = optimizer.optimizeExpr(e_temp, true);
                ObjectPair op;
                bool success;
                solver->setTimeout(coreSolverTimeout);
                if (!state.addressSpace.resolveOne(state, solver,e_temp, op, success)){
                    e_temp = toConstant(state, e_temp, "111");
                    success = state.addressSpace.resolveOne(cast<ConstantExpr>(e_temp), op);
                }
                solver->setTimeout(time::Span());
                printf("success = %d\n", success);
                if (success){
                    const MemoryObject *mo = op.first;
                    const ObjectState *os = op.second;
                    ref<Expr> offset = mo->getOffsetExpr(e_temp);
                    ref<Expr> check = mo->getBoundsCheckOffset(offset, bytes);
                    check = optimizer.optimizeExpr(check, true);

                    bool inBounds;
                    bool success = solver->mustBeTrue(state, check, inBounds);

                    printf("inBounds = %d\n", inBounds);
                    if (inBounds){
                        printf("We found its OS?\n");
                        ref<Expr> result = os->read(offset, Expr::Int32);
                        result->dump();
                        printf("ObjectState readOnly in tracing back  = %d\n", os->readOnly);
                    }
                }
			                long long temp = state.addressSpace.fpUpdateList[fp_address][1];
                            if (allocaMap[*ki_temp->operands] > 0)
                                state.addressSpace.fpUpdateList[fp_address] = {(long long)allocaMap[*ki_temp->operands], offset +temp};
                            else
                                state.addressSpace.fpUpdateList[fp_address] = {(long long) FunctionCalls[indirect_name], offset};
                        }
                        i--;
                    }
                    offset = 0;
                    /*
                    //find the name
                    if (inst->getNumOperands() != 1)
                        terminateStateOnExecError(state, "Error in handle indirect function call!\n");
                    llvm::Value *opnd = inst->getOperand(0);
                    if (opnd->hasName()){
                    opnd_name = opnd->getName();
                    }
                    else{
                        terminateStateOnExecError(state, "Error in handle indirect function call (Operand don't have a name)!\n");
                    }
                    //printf("//We found the name of operand \n");
                    printf("  opnd_name = %s\n", opnd_name.c_str());
                    */
                    break;
                }
            }
        }

        printf("////***** Do backtracing : Finish *****/////\n");
        //executeMemoryOperation(state, true, base, value, 0); // we skip it and record the information
        state.symExecuted = 0;
    }else{
        executeMemoryOperation(state, true, base, value, 0);
    }

    // Situation 3
    //Find the special symlic name first
    static bool isSymFpValue = 0;
    if (!isa<ConstantExpr>(value)){
        printf("This is a symbolic value to be read to FP!\n");
        base->dump();
        value->dump();
        std::set<std::string> nameList;
        array = scan2(value, nameList);
        //printf("array->name = %s \t size of nameList = %d\n", array->name.c_str(), nameList.size());
        //printf("array->size = %d\n", array->size);
        for (auto it = nameList.begin(); it != nameList.end(); it++){
            printf("in nameList : %s\n", (*it).c_str());
            for (int i = 0; i < symFpName.size(); i++){
                printf("in symFpName : %s\n", symFpName[i].c_str());
                if (*it == symFpName[i])
                    isSymFpValue = 1;
                break;
            }
        }
    }

    if (isFpBase == 0 && !isa<ConstantExpr>(value) && isSymFpValue == 1){
        printf("--- New function pointer : isSymFpValue = %d --- \n", isSymFpValue);
        base->dump();
        ref<ConstantExpr> new_fp = toConstant(state, base, "temp_base");
        printf("    ---fpAddress before adding size = %d ---\n", fpAddress.size());
        fpAddress.push_back(new_fp->getZExtValue());
        printf("    ---fpAddress after adding size = %d ---\n", fpAddress.size());
        value->dump();
        state.symExecuted = 1;
        //add to fpUpdateList
        std::vector<long long> temp_variable;
        state.addressSpace.fpUpdateList.insert(std::pair<uint64_t, std::vector<long long>>(new_fp->getZExtValue(), temp_variable));
        state.addressSpace.fpUpdateList[new_fp->getZExtValue()] = {0, 0};
    }
    //ref<ConstantExpr> temp_base = toConstant(state, base, "temp_base");
    //printf("temp_base = %d\n", temp_base->getZExtValue());

    //ref<ConstantExpr> temp_value = toConstant(state, value, "temp_value");
    //printf("temp_value = %d\n", temp_value->getZExtValue());

    //printf("callMallocFunction = %d\n", callMallocFunction);
    //printf("PC-----:\n");
    //state.pc->inst->dump();
    //executeMemoryOperation(state, true, base, value, 0); //original executeMemoryOperation()
    //executeMemoryOperationForMalloc(state, true, base, value, 0);
    isFpBase = 0;
    break;
  }

        case Instruction::GetElementPtr: {
                                             KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
                                             ref<Expr> base = eval(ki, 0, state).value;
                                             //printf("================== GetElementPtr Start =================\n");
                                             //base->dump();
                                             //printf("================== GetElementPtr End =================\n");

                                             for (std::vector< std::pair<unsigned, uint64_t> >::iterator
                                                     it = kgepi->indices.begin(), ie = kgepi->indices.end();
                                                     it != ie; ++it) {
                                                 uint64_t elementSize = it->second;
                                                 ref<Expr> index = eval(ki, it->first, state).value;
                                                 base = AddExpr::create(base,
                                                         MulExpr::create(Expr::createSExtToPointerWidth(index),
                                                             Expr::createPointer(elementSize)));
                                             }
                                             if (kgepi->offset)
                                                 base = AddExpr::create(base,
                                                         Expr::createPointer(kgepi->offset));
                                             bindLocal(ki, state, base);
                                             break;
                                         }

                                         // Conversion
        case Instruction::Trunc: {
                                     CastInst *ci = cast<CastInst>(i);
                                     ref<Expr> result = ExtractExpr::create(eval(ki, 0, state).value,
                                             0,
                                             getWidthForLLVMType(ci->getType()));
                                     bindLocal(ki, state, result);
                                     break;
                                 }
        case Instruction::ZExt: {
                                    CastInst *ci = cast<CastInst>(i);
                                    ref<Expr> result = ZExtExpr::create(eval(ki, 0, state).value,
                                            getWidthForLLVMType(ci->getType()));
                                    bindLocal(ki, state, result);
                                    break;
                                }
        case Instruction::SExt: {
                                    CastInst *ci = cast<CastInst>(i);
                                    ref<Expr> result = SExtExpr::create(eval(ki, 0, state).value,
                                            getWidthForLLVMType(ci->getType()));
                                    bindLocal(ki, state, result);
                                    break;
                                }

        case Instruction::IntToPtr: {
                                        CastInst *ci = cast<CastInst>(i);
                                        Expr::Width pType = getWidthForLLVMType(ci->getType());
                                        ref<Expr> arg = eval(ki, 0, state).value;
                                        bindLocal(ki, state, ZExtExpr::create(arg, pType));
                                        break;
                                    }
        case Instruction::PtrToInt: {
                                        CastInst *ci = cast<CastInst>(i);
                                        Expr::Width iType = getWidthForLLVMType(ci->getType());
                                        ref<Expr> arg = eval(ki, 0, state).value;
                                        bindLocal(ki, state, ZExtExpr::create(arg, iType));
                                        break;
                                    }

        case Instruction::BitCast: {
                                       ref<Expr> result = eval(ki, 0, state).value;
                                       bindLocal(ki, state, result);
                                       break;
                                   }

                                   // Floating point instructions

        case Instruction::FAdd: {
                                    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                            "floating point");
                                    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                            "floating point");
                                    if (!fpWidthToSemantics(left->getWidth()) ||
                                            !fpWidthToSemantics(right->getWidth()))
                                        return terminateStateOnExecError(state, "Unsupported FAdd operation");

                                    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
                                    Res.add(APFloat(*fpWidthToSemantics(right->getWidth()),right->getAPValue()), APFloat::rmNearestTiesToEven);
                                    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
                                    break;
                                }

        case Instruction::FSub: {
                                    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                            "floating point");
                                    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                            "floating point");
                                    if (!fpWidthToSemantics(left->getWidth()) ||
                                            !fpWidthToSemantics(right->getWidth()))
                                        return terminateStateOnExecError(state, "Unsupported FSub operation");
                                    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
                                    Res.subtract(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
                                    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
                                    break;
                                }

        case Instruction::FMul: {
                                    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                            "floating point");
                                    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                            "floating point");
                                    if (!fpWidthToSemantics(left->getWidth()) ||
                                            !fpWidthToSemantics(right->getWidth()))
                                        return terminateStateOnExecError(state, "Unsupported FMul operation");

                                    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
                                    Res.multiply(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
                                    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
                                    break;
                                }

        case Instruction::FDiv: {
                                    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                            "floating point");
                                    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                            "floating point");
                                    if (!fpWidthToSemantics(left->getWidth()) ||
                                            !fpWidthToSemantics(right->getWidth()))
                                        return terminateStateOnExecError(state, "Unsupported FDiv operation");

                                    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
                                    Res.divide(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
                                    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
                                    break;
                                }

        case Instruction::FRem: {
                                    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                            "floating point");
                                    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                            "floating point");
                                    if (!fpWidthToSemantics(left->getWidth()) ||
                                            !fpWidthToSemantics(right->getWidth()))
                                        return terminateStateOnExecError(state, "Unsupported FRem operation");
                                    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
                                    Res.mod(
                                            APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()));
                                    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
                                    break;
                                }

        case Instruction::FPTrunc: {
                                       FPTruncInst *fi = cast<FPTruncInst>(i);
                                       Expr::Width resultType = getWidthForLLVMType(fi->getType());
                                       ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                               "floating point");
                                       if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
                                           return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

                                       llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
                                       bool losesInfo = false;
                                       Res.convert(*fpWidthToSemantics(resultType),
                                               llvm::APFloat::rmNearestTiesToEven,
                                               &losesInfo);
                                       bindLocal(ki, state, ConstantExpr::alloc(Res));
                                       break;
                                   }

        case Instruction::FPExt: {
                                     FPExtInst *fi = cast<FPExtInst>(i);
                                     Expr::Width resultType = getWidthForLLVMType(fi->getType());
                                     ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                             "floating point");
                                     if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
                                         return terminateStateOnExecError(state, "Unsupported FPExt operation");
                                     llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
                                     bool losesInfo = false;
                                     Res.convert(*fpWidthToSemantics(resultType),
                                             llvm::APFloat::rmNearestTiesToEven,
                                             &losesInfo);
                                     bindLocal(ki, state, ConstantExpr::alloc(Res));
                                     break;
                                 }

        case Instruction::FPToUI: {
                                      FPToUIInst *fi = cast<FPToUIInst>(i);
                                      Expr::Width resultType = getWidthForLLVMType(fi->getType());
                                      ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                              "floating point");
                                      if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
                                          return terminateStateOnExecError(state, "Unsupported FPToUI operation");

                                      llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
                                      uint64_t value = 0;
                                      bool isExact = true;
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
                                      auto valueRef = makeMutableArrayRef(value);
#else
                                      uint64_t *valueRef = &value;
#endif
                                      Arg.convertToInteger(valueRef, resultType, false,
                                              llvm::APFloat::rmTowardZero, &isExact);
                                      bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
                                      break;
                                  }

        case Instruction::FPToSI: {
                                      FPToSIInst *fi = cast<FPToSIInst>(i);
                                      Expr::Width resultType = getWidthForLLVMType(fi->getType());
                                      ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                              "floating point");
                                      if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
                                          return terminateStateOnExecError(state, "Unsupported FPToSI operation");
                                      llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());

                                      uint64_t value = 0;
                                      bool isExact = true;
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
                                      auto valueRef = makeMutableArrayRef(value);
#else
                                      uint64_t *valueRef = &value;
#endif
                                      Arg.convertToInteger(valueRef, resultType, true,
                                              llvm::APFloat::rmTowardZero, &isExact);
                                      bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
                                      break;
                                  }

        case Instruction::UIToFP: {
                                      UIToFPInst *fi = cast<UIToFPInst>(i);
                                      Expr::Width resultType = getWidthForLLVMType(fi->getType());
                                      ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                              "floating point");
                                      const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
                                      if (!semantics)
                                          return terminateStateOnExecError(state, "Unsupported UIToFP operation");
                                      llvm::APFloat f(*semantics, 0);
                                      f.convertFromAPInt(arg->getAPValue(), false,
                                              llvm::APFloat::rmNearestTiesToEven);

                                      bindLocal(ki, state, ConstantExpr::alloc(f));
                                      break;
                                  }

        case Instruction::SIToFP: {
                                      SIToFPInst *fi = cast<SIToFPInst>(i);
                                      Expr::Width resultType = getWidthForLLVMType(fi->getType());
                                      ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                              "floating point");
                                      const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
                                      if (!semantics)
                                          return terminateStateOnExecError(state, "Unsupported SIToFP operation");
                                      llvm::APFloat f(*semantics, 0);
                                      f.convertFromAPInt(arg->getAPValue(), true,
                                              llvm::APFloat::rmNearestTiesToEven);

                                      bindLocal(ki, state, ConstantExpr::alloc(f));
                                      break;
                                  }

        case Instruction::FCmp: {
                                    FCmpInst *fi = cast<FCmpInst>(i);
                                    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                            "floating point");
                                    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                            "floating point");
                                    if (!fpWidthToSemantics(left->getWidth()) ||
                                            !fpWidthToSemantics(right->getWidth()))
                                        return terminateStateOnExecError(state, "Unsupported FCmp operation");

                                    APFloat LHS(*fpWidthToSemantics(left->getWidth()),left->getAPValue());
                                    APFloat RHS(*fpWidthToSemantics(right->getWidth()),right->getAPValue());
                                    APFloat::cmpResult CmpRes = LHS.compare(RHS);

                                    bool Result = false;
                                    switch( fi->getPredicate() ) {
                                        // Predicates which only care about whether or not the operands are NaNs.
                                        case FCmpInst::FCMP_ORD:
                                            Result = (CmpRes != APFloat::cmpUnordered);
                                            break;

                                        case FCmpInst::FCMP_UNO:
                                            Result = (CmpRes == APFloat::cmpUnordered);
                                            break;

                                            // Ordered comparisons return false if either operand is NaN.  Unordered
                                            // comparisons return true if either operand is NaN.
                                        case FCmpInst::FCMP_UEQ:
                                            Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpEqual);
                                            break;
                                        case FCmpInst::FCMP_OEQ:
                                            Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpEqual);
                                            break;

                                        case FCmpInst::FCMP_UGT:
                                            Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpGreaterThan);
                                            break;
                                        case FCmpInst::FCMP_OGT:
                                            Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpGreaterThan);
                                            break;

                                        case FCmpInst::FCMP_UGE:
                                            Result = (CmpRes == APFloat::cmpUnordered || (CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual));
                                            break;
                                        case FCmpInst::FCMP_OGE:
                                            Result = (CmpRes != APFloat::cmpUnordered && (CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual));
                                            break;

                                        case FCmpInst::FCMP_ULT:
                                            Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpLessThan);
                                            break;
                                        case FCmpInst::FCMP_OLT:
                                            Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpLessThan);
                                            break;

                                        case FCmpInst::FCMP_ULE:
                                            Result = (CmpRes == APFloat::cmpUnordered || (CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual));
                                            break;
                                        case FCmpInst::FCMP_OLE:
                                            Result = (CmpRes != APFloat::cmpUnordered && (CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual));
                                            break;

                                        case FCmpInst::FCMP_UNE:
                                            Result = (CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual);
                                            break;
                                        case FCmpInst::FCMP_ONE:
                                            Result = (CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual);
                                            break;

                                        default:
                                            assert(0 && "Invalid FCMP predicate!");
                                            break;
                                        case FCmpInst::FCMP_FALSE:
                                            Result = false;
                                            break;
                                        case FCmpInst::FCMP_TRUE:
                                            Result = true;
                                            break;
                                    }

                                    bindLocal(ki, state, ConstantExpr::alloc(Result, Expr::Bool));
                                    break;
                                }
        case Instruction::InsertValue: {
                                           KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

                                           ref<Expr> agg = eval(ki, 0, state).value;
                                           ref<Expr> val = eval(ki, 1, state).value;

                                           ref<Expr> l = NULL, r = NULL;
                                           unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val->getWidth();

                                           if (lOffset > 0)
                                               l = ExtractExpr::create(agg, 0, lOffset);
                                           if (rOffset < agg->getWidth())
                                               r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

                                           ref<Expr> result;
                                           if (!l.isNull() && !r.isNull())
                                               result = ConcatExpr::create(r, ConcatExpr::create(val, l));
                                           else if (!l.isNull())
                                               result = ConcatExpr::create(val, l);
                                           else if (!r.isNull())
                                               result = ConcatExpr::create(r, val);
                                           else
                                               result = val;

                                           bindLocal(ki, state, result);
                                           break;
                                       }
        case Instruction::ExtractValue: {
                                            KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

                                            ref<Expr> agg = eval(ki, 0, state).value;

                                            ref<Expr> result = ExtractExpr::create(agg, kgepi->offset*8, getWidthForLLVMType(i->getType()));

                                            bindLocal(ki, state, result);
                                            break;
                                        }
        case Instruction::Fence: {
                                     // Ignore for now
                                     break;
                                 }
        case Instruction::InsertElement: {
                                             InsertElementInst *iei = cast<InsertElementInst>(i);
                                             ref<Expr> vec = eval(ki, 0, state).value;
                                             ref<Expr> newElt = eval(ki, 1, state).value;
                                             ref<Expr> idx = eval(ki, 2, state).value;

                                             ConstantExpr *cIdx = dyn_cast<ConstantExpr>(idx);
                                             if (cIdx == NULL) {
                                                 terminateStateOnError(
                                                         state, "InsertElement, support for symbolic index not implemented",
                                                         Unhandled);
                                                 return;
                                             }
                                             uint64_t iIdx = cIdx->getZExtValue();
                                             const llvm::VectorType *vt = iei->getType();
                                             unsigned EltBits = getWidthForLLVMType(vt->getElementType());

                                             if (iIdx >= vt->getNumElements()) {
                                                 // Out of bounds write
                                                 terminateStateOnError(state, "Out of bounds write when inserting element",
                                                         BadVectorAccess);
                                                 return;
                                             }

                                             const unsigned elementCount = vt->getNumElements();
                                             llvm::SmallVector<ref<Expr>, 8> elems;
                                             elems.reserve(elementCount);
                                             for (unsigned i = elementCount; i != 0; --i) {
                                                 auto of = i - 1;
                                                 unsigned bitOffset = EltBits * of;
                                                 elems.push_back(
                                                         of == iIdx ? newElt : ExtractExpr::create(vec, bitOffset, EltBits));
                                             }

                                             assert(Context::get().isLittleEndian() && "FIXME:Broken for big endian");
                                             ref<Expr> Result = ConcatExpr::createN(elementCount, elems.data());
                                             bindLocal(ki, state, Result);
                                             break;
                                         }
        case Instruction::ExtractElement: {
                                              ExtractElementInst *eei = cast<ExtractElementInst>(i);
                                              ref<Expr> vec = eval(ki, 0, state).value;
                                              ref<Expr> idx = eval(ki, 1, state).value;

                                              ConstantExpr *cIdx = dyn_cast<ConstantExpr>(idx);
                                              if (cIdx == NULL) {
                                                  terminateStateOnError(
                                                          state, "ExtractElement, support for symbolic index not implemented",
                                                          Unhandled);
                                                  return;
                                              }
                                              uint64_t iIdx = cIdx->getZExtValue();
                                              const llvm::VectorType *vt = eei->getVectorOperandType();
                                              unsigned EltBits = getWidthForLLVMType(vt->getElementType());

                                              if (iIdx >= vt->getNumElements()) {
                                                  // Out of bounds read
                                                  terminateStateOnError(state, "Out of bounds read when extracting element",
                                                          BadVectorAccess);
                                                  return;
                                              }

                                              unsigned bitOffset = EltBits * iIdx;
                                              ref<Expr> Result = ExtractExpr::create(vec, bitOffset, EltBits);
                                              bindLocal(ki, state, Result);
                                              break;
                                          }
        case Instruction::ShuffleVector:
                                          // Should never happen due to Scalarizer pass removing ShuffleVector
                                          // instructions.
                                          terminateStateOnExecError(state, "Unexpected ShuffleVector instruction");
                                          break;
        case Instruction::AtomicRMW:
                                          terminateStateOnExecError(state, "Unexpected Atomic instruction, should be "
                                                  "lowered by LowerAtomicInstructionPass");
                                          break;
        case Instruction::AtomicCmpXchg:
                                          terminateStateOnExecError(state,
                                                  "Unexpected AtomicCmpXchg instruction, should be "
                                                  "lowered by LowerAtomicInstructionPass");
                                          break;
                                          // Other instructions...
                                          // Unhandled
        default:
                                          terminateStateOnExecError(state, "illegal instruction");
                                          break;
    }
}

//*Haoxin for AEG (modular) TODO
void maintainUpdateList(std::map<std::string, ref<Expr>> FunctionAddressMap, KInstruction *ki) {
    ;
}

std::pair<std::string, uint64_t> backwardTracing(ExecutionState &state, KInstruction *ki){
    std::pair<std::string, uint64_t> ret;
    return ret;
}

std::pair<uint64_t, u_int64_t> setAAWTarget(std::map<std::string, ref<Expr>> FunctionAddressMap,
                                            std::map<uint64_t, std::pair<std::string, uint64_t>> fpUpdateList,
                                            KInstruction *ki,
                                            uint64_t dest){
    std::pair<uint64_t, uint64_t> ret;
    return ret;
}
// *Haoxin end


void Executor::updateStates(ExecutionState *current) {
  if (searcher) {
    searcher->update(current, addedStates, removedStates);
  }

  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();

  for (std::vector<ExecutionState *>::iterator it = removedStates.begin(),
                                               ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    assert(it2!=states.end());
    states.erase(it2);
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 =
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    delete es;
  }
  removedStates.clear();
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else if (const auto set = dyn_cast<SequentialType>(*ii)) {
      uint64_t elementSize =
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index =
          evalConstant(c)->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend =
          index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
#if LLVM_VERSION_CODE >= LLVM_VERSION(4, 0)
    } else if (const auto ptr = dyn_cast<PointerType>(*ii)) {
      auto elementSize =
        kmodule->targetData->getTypeStoreSize(ptr->getElementType());
      auto operand = ii.getOperand();
      if (auto c = dyn_cast<Constant>(operand)) {
        auto index = evalConstant(c)->SExt(Context::get().getPointerWidth());
        auto addend = index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
#endif
    } else
      assert("invalid type" && 0);
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (auto &kfp : kmodule->functions) {
    KFunction *kf = kfp.get();
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  kmodule->constantTable =
      std::unique_ptr<Cell[]>(new Cell[kmodule->constants.size()]);
  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c.value = evalConstant(kmodule->constants[i]);
  }
}

void Executor::checkMemoryUsage() {
  if (!MaxMemory)
    return;
  if ((stats::instructions & 0xFFFF) == 0) {
    // We need to avoid calling GetTotalMallocUsage() often because it
    // is O(elts on freelist). This is really bad since we start
    // to pummel the freelist once we hit the memory cap.
    unsigned mbs = (util::GetTotalMallocUsage() >> 20) +
                   (memory->getUsedDeterministicSize() >> 20);

    if (mbs > MaxMemory) {
      if (mbs > MaxMemory + 100) {
        // just guess at how many to kill
        unsigned numStates = states.size();
        unsigned toKill = std::max(1U, numStates - numStates * MaxMemory / mbs);
        klee_warning("killing %d states (over memory cap)", toKill);
        std::vector<ExecutionState *> arr(states.begin(), states.end());
        for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
          unsigned idx = rand() % N;
          // Make two pulls to try and not hit a state that
          // covered new code.
          if (arr[idx]->coveredNew)
            idx = rand() % N;

          std::swap(arr[idx], arr[N - 1]);
          terminateStateEarly(*arr[N - 1], "Memory limit exceeded.");
        }
      }
      atMemoryLimit = true;
    } else {
      atMemoryLimit = false;
    }
  }
}

void Executor::doDumpStates() {
    if (!DumpStatesOnHalt || states.empty())
        return;

    klee_message("halting execution, dumping remaining states");
    for (const auto &state : states)
        terminateStateEarly(*state, "Execution halting.");
    updateStates(nullptr);
}

void Executor::run(ExecutionState &initialState) {
    bindModuleConstants();

    // Delay init till now so that ticks don't accrue during optimization and such.
    timers.reset();

    states.insert(&initialState);

    if (usingSeeds) {
        std::vector<SeedInfo> &v = seedMap[&initialState];

        for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(),
                ie = usingSeeds->end(); it != ie; ++it)
            v.push_back(SeedInfo(*it));

        int lastNumSeeds = usingSeeds->size()+10;
        time::Point lastTime, startTime = lastTime = time::getWallTime();
        ExecutionState *lastState = 0;
        while (!seedMap.empty()) {
            if (haltExecution) {
                doDumpStates();
                return;
            }

            std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it =
                seedMap.upper_bound(lastState);
            if (it == seedMap.end())
                it = seedMap.begin();
            lastState = it->first;
            ExecutionState &state = *lastState;
            KInstruction *ki = state.pc;
            stepInstruction(state);

            executeInstruction(state, ki);
            timers.invoke();
            if (::dumpStates) dumpStates();
            if (::dumpPTree) dumpPTree();
            updateStates(&state);

            if ((stats::instructions % 1000) == 0) {
                int numSeeds = 0, numStates = 0;
                for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
                        it = seedMap.begin(), ie = seedMap.end();
                        it != ie; ++it) {
                    numSeeds += it->second.size();
                    numStates++;
                }
                const auto time = time::getWallTime();
                const time::Span seedTime(SeedTime);
                if (seedTime && time > startTime + seedTime) {
                    klee_warning("seed time expired, %d seeds remain over %d states",
                            numSeeds, numStates);
                    break;
                } else if (numSeeds<=lastNumSeeds-10 ||
                        time - lastTime >= time::seconds(10)) {
                    lastTime = time;
                    lastNumSeeds = numSeeds;
                    klee_message("%d seeds remaining over: %d states",
                            numSeeds, numStates);
                }
            }
        }

        klee_message("seeding done (%d states remain)", (int) states.size());

        if (OnlySeed) {
            doDumpStates();
            return;
        }
    }

    searcher = constructUserSearcher(*this);

    std::vector<ExecutionState *> newStates(states.begin(), states.end());
    searcher->update(0, newStates, std::vector<ExecutionState *>());

    while (!states.empty() && !haltExecution) {
        ExecutionState &state = searcher->selectState();
        KInstruction *ki = state.pc;
        stepInstruction(state);

        executeInstruction(state, ki);
        timers.invoke();
        if (::dumpStates) dumpStates();
        if (::dumpPTree) dumpPTree();

        checkMemoryUsage();

        updateStates(&state);
    }

    delete searcher;
    searcher = 0;

    doDumpStates();
}

std::string Executor::getAddressInfo(ExecutionState &state,
                                     ref<Expr> address) const{
    std::string Str;
    llvm::raw_string_ostream info(Str);
    info << "\taddress: " << address << "\n";
    uint64_t example;
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
        example = CE->getZExtValue();
    } else {
        ref<ConstantExpr> value;
        bool success = solver->getValue(state, address, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        example = value->getZExtValue();
        info << "\texample: " << example << "\n";
        std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
        info << "\trange: [" << res.first << ", " << res.second <<"]\n";
    }

    MemoryObject hack((unsigned) example);
    MemoryMap::iterator lower = state.addressSpace.objects.upper_bound(&hack);
    info << "\tnext: ";
    if (lower==state.addressSpace.objects.end()) {
        info << "none\n";
    } else {
        const MemoryObject *mo = lower->first;
        std::string alloc_info;
        mo->getAllocInfo(alloc_info);
        info << "object at " << mo->address
            << " of size " << mo->size << "\n"
            << "\t\t" << alloc_info << "\n";
    }
    if (lower!=state.addressSpace.objects.begin()) {
        --lower;
        info << "\tprev: ";
        if (lower==state.addressSpace.objects.end()) {
            info << "none\n";
        } else {
            const MemoryObject *mo = lower->first;
            std::string alloc_info;
            mo->getAllocInfo(alloc_info);
            info << "object at " << mo->address
                << " of size " << mo->size << "\n"
                << "\t\t" << alloc_info << "\n";
        }
    }

    return info.str();
}


void Executor::terminateState(ExecutionState &state) {
    if (replayKTest && replayPosition!=replayKTest->numObjects) {
        klee_warning_once(replayKTest,
                "replay did not consume all objects in test input.");
    }

    interpreterHandler->incPathsExplored();

    std::vector<ExecutionState *>::iterator it =
        std::find(addedStates.begin(), addedStates.end(), &state);
    if (it==addedStates.end()) {
        state.pc = state.prevPC;

        removedStates.push_back(&state);
    } else {
        // never reached searcher, just delete immediately
        std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3 =
            seedMap.find(&state);
        if (it3 != seedMap.end())
            seedMap.erase(it3);
        addedStates.erase(it);
        processTree->remove(state.ptreeNode);
        delete &state;
    }
}

void Executor::terminateStateEarly(ExecutionState &state,
                                   const Twine &message) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  terminateState(state);
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, 0, 0);
  terminateState(state);
}

const InstructionInfo & Executor::getLastNonKleeInternalInstruction(const ExecutionState &state,
    Instruction ** lastInstruction) {
  // unroll the stack of the applications state and find
  // the last instruction which is not inside a KLEE internal function
  ExecutionState::stack_ty::const_reverse_iterator it = state.stack.rbegin(),
      itE = state.stack.rend();

  // don't check beyond the outermost function (i.e. main())
  itE--;

  const InstructionInfo * ii = 0;
  if (kmodule->internalFunctions.count(it->kf->function) == 0){
    ii =  state.prevPC->info;
    *lastInstruction = state.prevPC->inst;
    //  Cannot return yet because even though
    //  it->function is not an internal function it might of
    //  been called from an internal function.
  }

  // Wind up the stack and check if we are in a KLEE internal function.
  // We visit the entire stack because we want to return a CallInstruction
  // that was not reached via any KLEE internal functions.
  for (;it != itE; ++it) {
    // check calling instruction and if it is contained in a KLEE internal function
    const Function * f = (*it->caller).inst->getParent()->getParent();
    if (kmodule->internalFunctions.count(f)){
      ii = 0;
      continue;
    }
    if (!ii){
      ii = (*it->caller).info;
      *lastInstruction = (*it->caller).inst;
    }
  }

  if (!ii) {
    // something went wrong, play safe and return the current instruction info
    *lastInstruction = state.prevPC->inst;
    return *state.prevPC->info;
  }
  return *ii;
}

bool Executor::shouldExitOn(enum TerminateReason termReason) {
    std::vector<TerminateReason>::iterator s = ExitOnErrorType.begin();
    std::vector<TerminateReason>::iterator e = ExitOnErrorType.end();

    for (; s != e; ++s)
        if (termReason == *s)
            return true;

    return false;
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     enum TerminateReason termReason,
                                     const char *suffix,
                                     const llvm::Twine &info) {
    std::string message = messaget.str();
    static std::set< std::pair<Instruction*, std::string> > emittedErrors;
    Instruction * lastInst;
    const InstructionInfo &ii = getLastNonKleeInternalInstruction(state, &lastInst);

    if (EmitAllErrors ||
            emittedErrors.insert(std::make_pair(lastInst, message)).second) {
        if (ii.file != "") {
            klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
        } else {
            klee_message("ERROR: (location information missing) %s", message.c_str());
        }
        if (!EmitAllErrors)
            klee_message("NOTE: now ignoring this error at this location");

        std::string MsgString;
        llvm::raw_string_ostream msg(MsgString);
        msg << "Error: " << message << "\n";
        if (ii.file != "") {
            msg << "File: " << ii.file << "\n";
            msg << "Line: " << ii.line << "\n";
            msg << "assembly.ll line: " << ii.assemblyLine << "\n";
        }
        msg << "Stack: \n";
        state.dumpStack(msg);

        std::string info_str = info.str();
        if (info_str != "")
            msg << "Info: \n" << info_str;

        std::string suffix_buf;
        if (!suffix) {
            suffix_buf = TerminateReasonNames[termReason];
            suffix_buf += ".err";
            suffix = suffix_buf.c_str();
        }

        interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
    }

    terminateState(state);

    if (shouldExitOn(termReason))
        haltExecution = true;
}

// XXX shoot me
static const char *okExternalsList[] = { "printf",
                                         "fprintf",
                                         "puts",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList +
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

void Executor::callExternalFunction(ExecutionState &state,
                                    KInstruction *target,
                                    Function *function,
                                    std::vector< ref<Expr> > &arguments) {
    // check if specialFunctionHandler wants it
    if (specialFunctionHandler->handle(state, function, target, arguments))
        return;

    if (ExternalCalls == ExternalCallPolicy::None
            && !okExternals.count(function->getName())) {
        klee_warning("Disallowed call to external function: %s\n",
                function->getName().str().c_str());
        terminateStateOnError(state, "external calls disallowed", User);
        return;
    }

    // normal external function handling path
    // allocate 128 bits for each argument (+return value) to support fp80's;
    // we could iterate through all the arguments first and determine the exact
    // size we need, but this is faster, and the memory usage isn't significant.
    uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
    memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
    unsigned wordIndex = 2;
    for (std::vector<ref<Expr> >::iterator ai = arguments.begin(),
            ae = arguments.end(); ai!=ae; ++ai) {
        /* Jiaqi */
        // printf ("wordIndex: %d. \n", wordIndex);
        /* /Jiaqi */
        if (ExternalCalls == ExternalCallPolicy::All) { // don't bother checking uniqueness
            *ai = optimizer.optimizeExpr(*ai, true);
            ref<ConstantExpr> ce;
            bool success = solver->getValue(state, *ai, ce);
            assert(success && "FIXME: Unhandled solver failure");
            (void) success;
            ce->toMemory(&args[wordIndex]);
            /* Jiaqi */
            uint64_t t_addr = ce->getZExtValue();
            if (t_addr >= n_heap_l && t_addr <= n_heap_h)
            {
                ObjectPair op;
                bool ret = state.addressSpace.resolveOne(ce, op);
                if (ret)
                {
                    args[wordIndex] = op.first->kleeAddress;
                    printf ("symbolic args to external call. prepare args for external call. wordIndex: %d, klee address: %lx. native address: %lx. \n", wordIndex, op.first->kleeAddress, op.first->nativeAddress);
                }
                else
                {
                    printf ("recover kleeAddress for args in external call failed. arg: %lx.\n", t_addr);
                    terminateStateOnError(state, "failed external call: " + function->getName(), External);
                    return;
                }
            }
            /* /Jiaqi */
            ObjectPair op;
            // Checking to see if the argument is a pointer to something
            if (ce->getWidth() == Context::get().getPointerWidth() &&
                    state.addressSpace.resolveOne(ce, op)) {
                op.second->flushToConcreteStore(solver, state);
            }
            wordIndex += (ce->getWidth()+63)/64;
        } else {
            ref<Expr> arg = toUnique(state, *ai);
            if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
                // XXX kick toMemory functions from here
                ce->toMemory(&args[wordIndex]);
                /* Jiaqi */
                uint64_t t_addr = ce->getZExtValue();
                if (t_addr >= n_heap_l && t_addr <= n_heap_h)
                {
                    ObjectPair op;
                    bool ret = state.addressSpace.resolveOne(ce, op);
                    if (ret)
                    {
                        args[wordIndex] = op.first->kleeAddress;
                        // printf ("prepare args for external call. wordIndex: %d, klee address: %lx. native address: %lx. \n", wordIndex, op.first->kleeAddress, op.first->nativeAddress);
                    }
                    else
                    {
                        printf ("recover kleeAddress for args in concrete args external call failed. addr: %lx. \n", t_addr);
                        // /* Jiaqi, dump all mo in heap */
                        // MemoryMap::iterator begin = state.addressSpace.objects.begin();
                        // MemoryMap::iterator end = state.addressSpace.objects.end();
                        // // MemoryMap::iterator oi; // = begin;
                        // printf ("++++++++++++++++++++++++++ \n");
                        // for (auto oi = begin; oi != end; ++oi) {
                        //     const MemoryObject *mo = oi->first;
                        //     // if (mo->isGlobal || mo->isLocal)
                        //     //     continue;
                        //     // else
                        //     if (mo->isHeap)
                        //     {
                        //         printf ("state: %p. mo at: %p, id: %d, size: %lx, address: %lx, nativeAddress: %lx, kleeAddress: %lx. \n", &state, mo, mo->id, mo->size, mo->address, mo->nativeAddress, mo->kleeAddress);
                        //     }
                        // }

                        // printf ("---------------------------- \n");
                        // printf (" \n");
                        // printf (" \n");
                        // /* /Jiaqi */
                        terminateStateOnError(state, "failed external call: " + function->getName(), External);
                        return;
                    }
                }
                /* /Jiaqi */
                wordIndex += (ce->getWidth()+63)/64;
            } else {
                terminateStateOnExecError(state,
                        "external call with symbolic argument: " +
                        function->getName());
                return;
            }
        }
    }

    /* Jiaqi */
    // printf ("args[0]: %lx. args[1]: %lx. args[2]: %lx. args[3]: %lx. args[4]: %lx. args[5]: %lx. \n", args[0], args[1], args[2], args[3], args[4], args[5]);
    /* /Jiaqi */

    // Prepare external memory for invoking the function
    state.addressSpace.copyOutConcretes();
#ifndef WINDOWS
    // Update external errno state with local state value
    int *errno_addr = getErrnoLocation(state);
    ObjectPair result;
    bool resolved = state.addressSpace.resolveOne(
            ConstantExpr::create((uint64_t)errno_addr, Expr::Int64), result);
    if (!resolved)
        klee_error("Could not resolve memory object for errno");
    ref<Expr> errValueExpr = result.second->read(0, sizeof(*errno_addr) * 8);
    ConstantExpr *errnoValue = dyn_cast<ConstantExpr>(errValueExpr);
    if (!errnoValue) {
        terminateStateOnExecError(state,
                "external call with errno value symbolic: " +
                function->getName());
        return;
    }

    externalDispatcher->setLastErrno(
            errnoValue->getZExtValue(sizeof(*errno_addr) * 8));
#endif

    if (!SuppressExternalWarnings) {

        std::string TmpStr;
        llvm::raw_string_ostream os(TmpStr);
        os << "calling external: " << function->getName().str() << "(";
        for (unsigned i=0; i<arguments.size(); i++) {
            os << arguments[i];
            if (i != arguments.size()-1)
                os << ", ";
        }
        os << ") at " << state.pc->getSourceLocation();

        if (AllExternalWarnings)
            klee_warning("%s", os.str().c_str());
        else
            klee_warning_once(function, "%s", os.str().c_str());
    }

    bool success = externalDispatcher->executeCall(function, target->inst, args);
    if (!success) {
        terminateStateOnError(state, "failed external call: " + function->getName(),
                External);
        return;
    }

    if (!state.addressSpace.copyInConcretes()) {
        terminateStateOnError(state, "external modified read-only object",
                External);
        return;
    }

#ifndef WINDOWS
    // Update errno memory object with the errno value from the call
    int error = externalDispatcher->getLastErrno();
    state.addressSpace.copyInConcrete(result.first, result.second,
            (uint64_t)&error);
#endif

    Type *resultType = target->inst->getType();
    if (resultType != Type::getVoidTy(function->getContext())) {
        ref<Expr> e = ConstantExpr::fromMemory((void*) args,
                getWidthForLLVMType(resultType));
        bindLocal(target, state, e);
    }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state,
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayKTest || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason to?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() % n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.

  static unsigned id;
  const Array *array =
      arrayCache.CreateArray("rrws_arr" + llvm::utostr(++id),
                             Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  llvm::errs() << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state,
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace.bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack.back().allocas.push_back(mo);

  return os;
}

/* Jiaqi */
// void Executor::executeAlloc(ExecutionState &state,
//                             ref<Expr> size,
//                             bool isLocal,
//                             KInstruction *target,
//                             bool zeroMemory,
//                             const ObjectState *reallocFrom,
//                             size_t allocationAlignment) {
void Executor::executeAlloc(ExecutionState &state,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            bool zeroMemory,
                            const ObjectState *reallocFrom,
                            size_t allocationAlignment, bool isheap) {
/* /Jiaqi */
    size = toUnique(state, size);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
        const llvm::Value *allocSite = state.prevPC->inst;
        if (allocationAlignment == 0) {
            allocationAlignment = getAllocationAlignment(allocSite);
        }
        MemoryObject *mo =
            memory->allocate(CE->getZExtValue(), isLocal, /*isGlobal=*/false,
                    allocSite, allocationAlignment);
        if (!mo) {
            bindLocal(target, state,
                    ConstantExpr::alloc(0, Context::get().getPointerWidth()));
        } else {
            /* Jiaqi */
            if (isheap)
            {
                mo->isHeap = isheap;
                mo->kleeAddress = mo->address;
                mo->address = 0;
                HeapAlloc* heap_alloc = new HeapAlloc(mo, 1, CE->getZExtValue(), allocationAlignment, NULL);
                state.heap_allocs.push_back(*heap_alloc);
                printf ("issue nme_req for malloc. \n");
                nme_req(&state, 1);
                // emulate_nme_req(&state, 1);
                mo->nativeAddress = state.heap_allocs.back().nativeAddress;
                mo->address = mo->nativeAddress;
                /*
		        //Haoxin for AEG start
                std::string location = target->getSourceLocation();
		        printf("location: %s\n", location.c_str());
                printf("in executeAlloc target->dest = %d", target->dest);
                printf("in executeAlloc mo->getBaseExpr() in executeAlloc = %p\n", mo->address);
                allocaMap.insert(std::pair<unsigned, uint64_t>(target->dest, mo->address));
                KInstruction *ki_temp;
                for (int i = 0; i < state.stack[state.stack.size() - 1].kf->numInstructions; i++){
                    ki_temp = state.stack[state.stack.size()-1].kf->instructions[i];
                    printf("In is heap: dest = %d --- operands = %p\n", ki_temp->dest, *ki_temp->operands);
                }
                //Haoxin for AEG end
                */
                printf ("in malloc, state: %p. mo->size: 0x%lx. mo->address: %lx. mo->kleeAddress: %lx, mo->nativeAddress: %lx. \n", &state, mo->size, mo->address, mo->kleeAddress, mo->nativeAddress);
            }
            /* /Jiaqi */
            ObjectState *os = bindObjectInState(state, mo, isLocal);
            if (zeroMemory) {
                os->initializeToZero();
            } else {
                os->initializeToRandom();
            }

            /*
	    // Haoxin for AEG
      ref<ConstantExpr> temp = mo->getBaseExpr();
      //printf("mo->getBaseExpr() in executeAlloc = %d\n", temp->getZExtValue());
      //
      std::string location = target->getSourceLocation();
      if (location.find("libc/") == std::string::npos){ // TODO should we parse arguments from command line?
        printf("in executeAlloc target->dest = %d; address = %d\n", target->dest, temp->getZExtValue());
        printf("out of isHeap in executeAlloc mo->getBaseExpr() in executeAlloc = %p\n", mo->kleeAddress);
        allocaMap.insert(std::pair<unsigned, uint64_t>(target->dest, mo->address));
      }
      */
            bindLocal(target, state, mo->getBaseExpr());

            if (reallocFrom) {
                unsigned count = std::min(reallocFrom->size, os->size);
                for (unsigned i=0; i<count; i++)
                    os->write(i, reallocFrom->read8(i));
                state.addressSpace.unbindObject(reallocFrom->getObject());
            }
        }
    } else {
        // XXX For now we just pick a size. Ideally we would support
        // symbolic sizes fully but even if we don't it would be better to
        // "smartly" pick a value, for example we could fork and pick the
        // min and max values and perhaps some intermediate (reasonable
        // value).
        //
        // It would also be nice to recognize the case when size has
        // exactly two values and just fork (but we need to get rid of
        // return argument first). This shows up in pcre when llvm
        // collapses the size expression with a select.

        size = optimizer.optimizeExpr(size, true);

        ref<ConstantExpr> example;
        bool success = solver->getValue(state, size, example);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;

        // Try and start with a small example.
        Expr::Width W = example->getWidth();
        while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
            ref<ConstantExpr> tmp = example->LShr(ConstantExpr::alloc(1, W));
            bool res;
            bool success = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
            assert(success && "FIXME: Unhandled solver failure");
            (void) success;
            if (!res)
                break;
            example = tmp;
        }

        StatePair fixedSize = fork(state, EqExpr::create(example, size), true);

        if (fixedSize.second) {
            // Check for exactly two values
            ref<ConstantExpr> tmp;
            bool success = solver->getValue(*fixedSize.second, size, tmp);
            assert(success && "FIXME: Unhandled solver failure");
            (void) success;
            bool res;
            success = solver->mustBeTrue(*fixedSize.second,
                    EqExpr::create(tmp, size),
                    res);
            assert(success && "FIXME: Unhandled solver failure");
            (void) success;
            if (res) {
                /* Jiaqi */
                // executeAlloc(*fixedSize.second, tmp, isLocal,
                //         target, zeroMemory, reallocFrom);
                executeAlloc(*fixedSize.second, tmp, isLocal,
                        target, zeroMemory, reallocFrom, isheap);
                /* /Jiaqi */
            } else {
                // See if a *really* big value is possible. If so assume
                // malloc will fail for it, so lets fork and return 0.
                StatePair hugeSize =
                    fork(*fixedSize.second,
                            UltExpr::create(ConstantExpr::alloc(1U<<31, W), size),
                            true);
                if (hugeSize.first) {
                    klee_message("NOTE: found huge malloc, returning 0");
                    bindLocal(target, *hugeSize.first,
                            ConstantExpr::alloc(0, Context::get().getPointerWidth()));
                }

                if (hugeSize.second) {

                    std::string Str;
                    llvm::raw_string_ostream info(Str);
                    ExprPPrinter::printOne(info, "  size expr", size);
                    info << "  concretization : " << example << "\n";
                    info << "  unbound example: " << tmp << "\n";
                    terminateStateOnError(*hugeSize.second, "concretized symbolic size",
                            Model, NULL, info.str());
                }
            }
        }

        if (fixedSize.first) // can be zero when fork fails
            /* Jiaqi */
            // executeAlloc(*fixedSize.first, example, isLocal,
            //         target, zeroMemory, reallocFrom);
            executeAlloc(*fixedSize.first, example, isLocal,
                    target, zeroMemory, reallocFrom, isheap);
            /* /Jiaqi */
    }
}

void Executor::executeFree(ExecutionState &state,
                           ref<Expr> address,
                           KInstruction *target) {
    address = optimizer.optimizeExpr(address, true);
    StatePair zeroPointer = fork(state, Expr::createIsZero(address), true);
    if (zeroPointer.first) {
        if (target)
            bindLocal(target, *zeroPointer.first, Expr::createPointer(0));
    }
    if (zeroPointer.second) { // address != 0
        ExactResolutionList rl;
        resolveExact(*zeroPointer.second, address, rl, "free");

        for (Executor::ExactResolutionList::iterator it = rl.begin(),
                ie = rl.end(); it != ie; ++it) {
            const MemoryObject *mo = it->first.first;
            if (mo->isLocal) {
                terminateStateOnError(*it->second, "free of alloca", Free, NULL,
                        getAddressInfo(*it->second, address));
            } else if (mo->isGlobal) {
                terminateStateOnError(*it->second, "free of global", Free, NULL,
                        getAddressInfo(*it->second, address));
            } else {
                /* Jiaqi */
                // if (mo->nativeAddress)
                if (mo->isHeap)
                {
                    MemoryObject* Mo = const_cast<MemoryObject*>(mo);
                    HeapAlloc* heap_alloc = new HeapAlloc(Mo, 2, Mo->size, 0, Mo->address);
                    it->second->heap_allocs.push_back(*heap_alloc);
                    printf ("issue nme_req for free. \n");
                    nme_req(&state, 1);
                    // emulate_nme_req(&state, 1);
                    printf ("in free, mo->name: %s. mo->kleeAddress: %lx, mo->nativeAddress: %lx. \n", mo->name, mo->kleeAddress, mo->nativeAddress);
                }
                /* /Jiaqi */
                it->second->addressSpace.unbindObject(mo);
                if (target)
                    bindLocal(target, *it->second, Expr::createPointer(0));
            }
        }
    }
}

void Executor::resolveExact(ExecutionState &state,
                            ref<Expr> p,
                            ExactResolutionList &results,
                            const std::string &name) {
    p = optimizer.optimizeExpr(p, true);
    // XXX we may want to be capping this?
    ResolutionList rl;
    state.addressSpace.resolve(state, solver, p, rl);

    ExecutionState *unbound = &state;
    for (ResolutionList::iterator it = rl.begin(), ie = rl.end();
            it != ie; ++it) {
        ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());

        StatePair branches = fork(*unbound, inBounds, true);

        if (branches.first)
            results.push_back(std::make_pair(*it, branches.first));

        unbound = branches.second;
        if (!unbound) // Fork failure
            break;
    }

    if (unbound) {
        terminateStateOnError(*unbound, "memory error: invalid pointer: " + name,
                Ptr, NULL, getAddressInfo(*unbound, p));
    }
}

void Executor::executeMemoryOperation(ExecutionState &state,
                                      bool isWrite,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target /* undef if write */) {
    Expr::Width type = (isWrite ? value->getWidth() :
            getWidthForLLVMType(target->inst->getType()));
    unsigned bytes = Expr::getMinBytesForWidth(type);

    /* Jiaqi */
    // if (!isa<ConstantExpr>(address) && !isa<ConstantExpr>(value))
    if (!isa<ConstantExpr>(address))
    {
        // printf ("~~~~~~~~~~~~~~~~~~~~Instruction in line: %d, assemblyline: %d. \n", target->info->line, target->info->assemblyLine);
        printf ("symbolic addr for memory operation, isWrite: %d. state: %p. \n", isWrite, &state);
        if (!isWrite)
        {
            printf ("~~~~~~~~~~~~~~~~~~~~Instruction in file: %s, in line: %d, assemblyline: %d. \n", target->info->file.c_str(), target->info->line, target->info->assemblyLine);
        }
        terminateStateOnError(state, "exploit succeed: memory operation with symbolic addr", Ptr, NULL, getAddressInfo(state, address));
        // terminateStateOnError(state, "exploit succeed: memory operation with symbolic addr", ReadOnly);
        // terminateStateOnExecError(state, "exploit succeed: arbitrary write with arbitrary value");
        // terminateStateOnExit(state);
        // terminateStateOnError(state, "memory error: object read only", ReadOnly);
        // terminateStateEarly(state, "Query timed out (bounds check).");
        // terminateState(state);
        return;//if without return, the following code would still be executed which crashes KLEE.
    }
    /* /Jiaqi */

    if (SimplifySymIndices) {
        if (!isa<ConstantExpr>(address))
            address = state.constraints.simplifyExpr(address);
        if (isWrite && !isa<ConstantExpr>(value))
            value = state.constraints.simplifyExpr(value);
    }

    // /* Jiaqi */
    // if (isWrite)
    // {
    //     unsigned offset_ = 1;
    //     unsigned width_ = 2;
    //     ref<Expr> value_ = ExtractExpr::create(value, offset_*8, width_*8);
    //     printf ("value width: %d. \n", value->getWidth());
    //     printf ("value_ width: %d. \n", value_->getWidth());
    //     value->dump();
    //     value_->dump();
    //     printf ("numkids of value: %d. \n", value->getNumKids());
    //     if (value->getNumKids() == 2)
    //     {
    //         value->getKid(0)->dump();
    //         value->getKid(1)->dump();
    //         value->getKid(1)->getKid(0)->dump();
    //         value->getKid(1)->getKid(1)->dump();
    //         value->getKid(1)->getKid(1)->getKid(0)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->getKid(0)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->getKid(1)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->getKid(1)->getKid(0)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->getKid(1)->getKid(1)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->getKid(1)->getKid(1)->getKid(0)->dump();
    //         value->getKid(1)->getKid(1)->getKid(1)->getKid(1)->getKid(1)->dump();
    //         // printf ("numkids of value's second kid: %d. \n", value->getKid(1)->getKid(1)->getKid(1)->getKid(1)->getNumKids());
    //     }
    //     // if (!isa<ConstantExpr>(value))
    //     // {
    //     //     cast<ConstantExpr>(value);
    //     // }
    //     if (isa<ConstantExpr>(value_))
    //     {
    //         printf ("is value a constant: %d. is valu_ a constant: %d. \n", isa<ConstantExpr>(value), isa<ConstantExpr>(value_));
    //         printf ("value:%lx, value_: %lx. \n", (cast<ConstantExpr>(value))->getZExtValue(), (cast<ConstantExpr>(value_))->getZExtValue());
    //     }
    // }
    // /* /Jiaqi */

    address = optimizer.optimizeExpr(address, true);

    // fast path: single in-bounds resolution
    ObjectPair op;
    bool success;
    solver->setTimeout(coreSolverTimeout);
    if (!state.addressSpace.resolveOne(state, solver, address, op, success)) {
        address = toConstant(state, address, "resolveOne failure");
        success = state.addressSpace.resolveOne(cast<ConstantExpr>(address), op);
    }
    solver->setTimeout(time::Span());

    if (success) {
        const MemoryObject *mo = op.first;

        if (MaxSymArraySize && mo->size >= MaxSymArraySize) {
            address = toConstant(state, address, "max-sym-array-size");
        }

        ref<Expr> offset = mo->getOffsetExpr(address);
        ref<Expr> check = mo->getBoundsCheckOffset(offset, bytes);
        check = optimizer.optimizeExpr(check, true);

        bool inBounds;
        solver->setTimeout(coreSolverTimeout);
        bool success = solver->mustBeTrue(state, check, inBounds);
        solver->setTimeout(time::Span());
        if (!success) {
            state.pc = state.prevPC;
            terminateStateEarly(state, "Query timed out (bounds check).");
            return;
        }

        if (inBounds) {
            const ObjectState *os = op.second;
            if (isWrite) {
                if (os->readOnly) {
                    /* Jiaqi */
                    // printf ("in concrete and in Bound try. state: %p. \n", state);
                    /* /Jiaqi */
                    terminateStateOnError(state, "memory error: object read only",
                            ReadOnly);
                } else {
                    ObjectState *wos = state.addressSpace.getWriteable(mo, os);
                    wos->write(offset, value);
                }
            } else {
                ref<Expr> result = os->read(offset, type);

                if (interpreterOpts.MakeConcreteSymbolic)
                    result = replaceReadWithSymbolic(state, result);

                bindLocal(target, state, result);
            }

            return;
        }
    }

    // we are on an error path (no resolution, multiple resolution, one
    // resolution with out of bounds)

    address = optimizer.optimizeExpr(address, true);
    ResolutionList rl;
    solver->setTimeout(coreSolverTimeout);
    bool incomplete = state.addressSpace.resolve(state, solver, address, rl,
            0, coreSolverTimeout);
    solver->setTimeout(time::Span());

    // XXX there is some query wasteage here. who cares?
    ExecutionState *unbound = &state;

    for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
        const MemoryObject *mo = i->first;
        const ObjectState *os = i->second;
        ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);

        StatePair branches = fork(*unbound, inBounds, true);
        ExecutionState *bound = branches.first;

        // bound can be 0 on failure or overlapped
        if (bound) {
            if (isWrite) {
                if (os->readOnly) {
                    /* Jiaqi */
                    // printf ("in symbolic try. bound: %p. \n", bound);
                    /* /Jiaqi */
                    terminateStateOnError(*bound, "memory error: object read only",
                            ReadOnly);
                } else {
                    ObjectState *wos = bound->addressSpace.getWriteable(mo, os);
                    wos->write(mo->getOffsetExpr(address), value);
                }
            } else {
                ref<Expr> result = os->read(mo->getOffsetExpr(address), type);
                bindLocal(target, *bound, result);
            }
        }

        unbound = branches.second;
        if (!unbound)
            break;
    }

    /* Jiaqi */
    // MemoryMap::iterator begin = state.addressSpace.objects.begin();
    // MemoryMap::iterator end = state.addressSpace.objects.end();
    // // MemoryMap::iterator oi; // = begin;
    // printf ("++++++++++++++++++++++++++ \n");
    // for (auto oi = begin; oi != end; ++oi) {
    //     const MemoryObject *mo = oi->first;
    //     if (mo->isGlobal || mo->isLocal)
    //         continue;
    //     else
    //     {
    //         printf ("state: %p. mo at: %p, id: %d, size: %lx, address: %lx. \n", &state, mo, mo->id, mo->size, mo->address);
    //     }
    // }

    // printf ("---------------------------- \n");
    // printf (" \n");
    // printf (" \n");
    // /* /Jiaqi */

    // XXX should we distinguish out of bounds and overlapped cases?
    if (unbound) {
        if (incomplete) {
            terminateStateEarly(*unbound, "Query timed out (resolve).");
        } else {
            /* Jiaqi */
            // printf ("error reported here. continue to explore. state: %p. iunbound: %p, isWrite:%d. \n", state, unbound, isWrite);
            // if (isWrite)
            // {
            //     printf ("state: %p. target: %p. \n", state, target);
            //     // std::cout << target->info->file << ":" << target->info->line << "\n";
            //     // printf("file: %s, line: %d. \n", target->info->file, target->info->line);
            //     // printf("line: %d. \n", target->info->line);
            //     // printf("file: %s. \n", target->info->file);
            //     return;
            // }
            // else
            // {
            //     // bindLocal(target, state, )
            //     printf ("state: %p. target: %p, unbound:%p. \n", state, target, unbound);
            //     printf("file: %s, line: %d. \n", target->info->file, target->info->line);
            //     terminateStateOnError(*unbound, "memory error: out of bound pointer", Ptr, NULL, getAddressInfo(*unbound, address));
            //     return;
            // }
            /* /Jiaqi */

            /* Jiaqi, overflow error detected. states should contains all state
             * in seedMap (keep them) and the others (terminate them). */
            if (OnlySeed)
            {
                printf ("first overflow error detected, try to exit seed mode. \n");
                for (ExecutionState *es : states)
                {
                    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = seedMap.find(&state);
                    if (it!=seedMap.end()) {
                        printf("for state %p in seedMap, keep it in states, but remove its entry in seedMap. \n", it->first);
                        seedMap.erase(it);
                    }
                    else
                    {
                        printf("for state: %p not in seedMap, terminate it. \n", es);
                        terminateState(*es);
                    }
                }
                /* disable OnlySeed */
                OnlySeed.setValue(false);
                if(seedMap.size())
                    printf("!!!!!!!!!!!!!!! error, seedMap is not empty after adjusting state in states. \n");
            }
            /* /Jiaqi */

            /* Jiaqi */
            ConstantExpr *CE = dyn_cast<ConstantExpr>(address);// address must be a constant Expr, otherwise it is caught as a symbolic read/write operation
            uint64_t addr = CE->getZExtValue();

            /* only handle overflow within native heap addr range */
            if (addr <= n_heap_l || addr > n_heap_h)
            {
                printf ("overflow outside native heap range. addr: %lx, bytes: %lx. state:%p, unbound:%p. \n", addr, bytes, &state, unbound);
                terminateStateOnError(*unbound, "++++++++++++++++++memory error: out of bound pointer", Ptr, NULL, getAddressInfo(*unbound, address));
            }

            /* if it is a write and the value is a symbol, return directly ? */
            if (isWrite && !isa<ConstantExpr>(value))
            {
                printf ("overflow with symbolic value not handled. addr: %lx, bytes: %x. state: %p, unbound: %p. \n", addr, bytes, &state, unbound);
                return;
            }
            else if (isWrite && isa<ConstantExpr>(value))
            {
                printf ("write %d bytes concrete into addr %lx. state: %p, unbound: %p. \n", bytes, addr, &state, unbound);
                return;
            }
            else if (!isWrite)
            {
                printf ("overread %d bytes from addr: %lx. state: %p. unbound: %p. \n", bytes, addr, state, unbound);
                ref<Expr> Kids[bytes];
                for (unsigned i = 0; i < bytes; i ++)
                {
                    ref<Expr> result = ConstantExpr::create(0x88, Expr::Int8);
                    Kids[i] = result;
                }
                ref<Expr> Result = ConcatExpr::createN(bytes, &Kids[0]);
                bindLocal(target, *unbound, Result);
                return;
            }
            else
            {
                printf ("--------------unrecogonized scenario. \n");
                terminateStateOnError(*unbound, "-----------------memory error: out of bound pointer", Ptr, NULL, getAddressInfo(*unbound, address));
            }
            /* / */

            // // In this path, there is no resolution for address (unbound == state) or bytes is out of bound of resolution (unbound == newly forked state)
            // if (&state != last_state)//roll back native heap if there is a state switch
            // {
            //     nme_req(&state, 0);
            // }

            // ConstantExpr *CE = dyn_cast<ConstantExpr>(address);// address must be a constant Expr, otherwise it is caught as a symbolic read/write operation
            // uint64_t addr = CE->getZExtValue();
            // if (isWrite)
            //     oflow_k->rw = 1;
            // else
            //     oflow_k->rw = 0;
            // oflow_k->addr = addr;
            // oflow_k->bytes = bytes;

            // kn_indicator->flag = 2;
            //
            // do {
            // } while (kn_indicator->flag != 0);

            // /* no matter r/w, nme returns the native value in nme_store[] */

            // int req_num= kn_indicator->num;
            // ref<Expr> Kids[req_num];
            //
            // for (unsigned i = 0; i < req_num; i ++)
            // {
            //     ref<Expr> bytes_ = ConstantExpr::create(oflow_n[i].bytes, Expr::Int64);
            //     ref<Expr> addr_ = ConstantExpr::create(oflow_n[i].addr_s, Expr::Int64);
            //     if (oflow_n[i].mo)//with valid object in KLEE
            //     {
            //         const MemoryObject* mo = oflow_n[i].mo;
            //         const ObjectState* os = state.addressSpace.findObject(mo);
            //
            //         if (isWrite)
            //         {
            //             if (os->readOnly) {
            //                 terminateStateOnError(*unbound, "memory error: object read only", ReadOnly);
            //             } else {
            //                 ref<Expr> offset_ = mo->getOffsetExpr(addr_);
            //                 ObjectState *wos = unbound->addressSpace.getWriteable(mo, os);
            //                 uint64_t value_offset = oflow_n[i].addr_s - addr;
            //                 ref<Expr> value_ = ExtractExpr::create(value, 8*value_offset, oflow_n[i].bytes*8);
            //                 wos->write(offset_, value_);

            //                 // if (ptaMode == DynamicSymbolicMode && mo->isGlobal && mo->allocSite != nullptr) {
            //                 //     state.modifiedGlobals.insert(mo->allocSite);
            //                 // }
            //                 //
            //                 // if (shouldUpdatePoinstTo(state)) {
            //                 //     updatePointsToOnStore(state, state.prevPC, mo, offset, value, UseStrongUpdates);
            //                 // }
            //                 // if (ptaMode == AIMode && state.isDummy) {
            //                 //     updateAIPhase(state, state.prevPC, mo, offset, value);
            //                 // }
            //             }
            //         } else {
            //             ref<Expr> result = os->read(mo->getOffsetExpr(addr_), oflow_n[i].bytes*8);
            //             Kids[i] = result;
            //         }
            //     } else { //without valid object in KLEE, i.e. meta data or freed objects
            //         if (isWrite) // add constraints to make metadata bytes equal to its native value for write operation
            //         {
            //             uint64_t value_offset = oflow_n[i].addr_s - addr;
            //             ref<Expr> value_ = ExtractExpr::create(value, 8*value_offset, oflow_n[i].bytes*8);
            //             ref<Expr> n_value = ConstantExpr::create(nme_store[value_offset], oflow_n[i].bytes*8);
            //             ref<Expr> e = EqExpr::create(value_, n_value);
            //             unbound->addConstraint(e);
            //         }
            //         else // for read operation
            //         {
            //             ref<Expr> Res(0);
            //             uint64_t value_offset = oflow_n[i].addr_s - addr;
            //             Res = ConstantExpr::create(nme_store[value_offset], oflow_n[i].bytes*8);
            //             // for (unsigned j = 0; j != oflow_n[i].bytes; ++i) {
            //             //     // unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
            //             //     uint64_t value_offset = oflow_n[i].addr_s - addr;
            //             //     ref<Expr> Byte = ConstantExpr::create(nme_store[value_offset + j], Expr::Int8);
            //             //     Res = j ? ConcatExpr::create(Byte, Res) : Byte;
            //             // }
            //             Kids[i] = Res;
            //         }
            //     }
            // }

            // /* synthesize the value array returned by NME with KLEE for read
            //  * operation */
            // if (!isWrite)
            // {
            //     ref<Expr> Result = ConcatExpr::createN(req_num, &Kids[0]);
            //     bindLocal(target, *unbound, Result);
            // }
            // return;
            // /* /Jiaqi */

            terminateStateOnError(*unbound, "memory error: out of bound pointer", Ptr,
                    NULL, getAddressInfo(*unbound, address));
        }
    }
}

void Executor::executeMakeSymbolic(ExecutionState &state,
                                   const MemoryObject *mo,
                                   const std::string &name) {
    // Create a new object state for the memory object (instead of a copy).
    if (!replayKTest) {
        // Find a unique name for this array.  First try the original name,
        // or if that fails try adding a unique identifier.
        unsigned id = 0;
        std::string uniqueName = name;
        while (!state.arrayNames.insert(uniqueName).second) {
            uniqueName = name + "_" + llvm::utostr(++id);
        }
        const Array *array = arrayCache.CreateArray(uniqueName, mo->size);
        bindObjectInState(state, mo, false, array);
        state.addSymbolic(mo, array);

        std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
            seedMap.find(&state);
        if (it!=seedMap.end()) { // In seed mode we need to add this as a
            // binding.
            for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                    siie = it->second.end(); siit != siie; ++siit) {
                SeedInfo &si = *siit;
                KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

                if (!obj) {
                    if (ZeroSeedExtension) {
                        std::vector<unsigned char> &values = si.assignment.bindings[array];
                        values = std::vector<unsigned char>(mo->size, '\0');
                    } else if (!AllowSeedExtension) {
                        terminateStateOnError(state, "ran out of inputs during seeding",
                                User);
                        break;
                    }
                } else {
                    if (obj->numBytes != mo->size &&
                            ((!(AllowSeedExtension || ZeroSeedExtension)
                              && obj->numBytes < mo->size) ||
                             (!AllowSeedTruncation && obj->numBytes > mo->size))) {
                        std::stringstream msg;
                        msg << "replace size mismatch: "
                            << mo->name << "[" << mo->size << "]"
                            << " vs " << obj->name << "[" << obj->numBytes << "]"
                            << " in test\n";

                        terminateStateOnError(state, msg.str(), User);
                        break;
                    } else {
                        std::vector<unsigned char> &values = si.assignment.bindings[array];
                        values.insert(values.begin(), obj->bytes,
                                obj->bytes + std::min(obj->numBytes, mo->size));
                        if (ZeroSeedExtension) {
                            for (unsigned i=obj->numBytes; i<mo->size; ++i)
                                values.push_back('\0');
                        }
                    }
                }
            }
        }
    } else {
        ObjectState *os = bindObjectInState(state, mo, false);
        if (replayPosition >= replayKTest->numObjects) {
            terminateStateOnError(state, "replay count mismatch", User);
        } else {
            KTestObject *obj = &replayKTest->objects[replayPosition++];
            if (obj->numBytes != mo->size) {
                terminateStateOnError(state, "replay size mismatch", User);
            } else {
                for (unsigned i=0; i<mo->size; i++)
                    os->write8(i, obj->bytes[i]);
            }
        }
    }
}

/***/

void Executor::runFunctionAsMain(Function *f,
				 int argc,
				 char **argv,
				 char **envp) {
    std::vector<ref<Expr> > arguments;

    // force deterministic initialization of memory objects
    srand(1);
    srandom(1);

    MemoryObject *argvMO = 0;

    // In order to make uclibc happy and be closer to what the system is
    // doing we lay out the environments at the end of the argv array
    // (both are terminated by a null). There is also a final terminating
    // null that uclibc seems to expect, possibly the ELF header?

    int envc;
    for (envc=0; envp[envc]; ++envc) ;

    unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
    KFunction *kf = kmodule->functionMap[f];
    assert(kf);
    Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
    if (ai!=ae) {
        arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));
        if (++ai!=ae) {
            Instruction *first = &*(f->begin()->begin());
            argvMO =
                memory->allocate((argc + 1 + envc + 1 + 1) * NumPtrBytes,
                        /*isLocal=*/false, /*isGlobal=*/true,
                        /*allocSite=*/first, /*alignment=*/8);

            if (!argvMO)
                klee_error("Could not allocate memory for function arguments");

            arguments.push_back(argvMO->getBaseExpr());

            if (++ai!=ae) {
                uint64_t envp_start = argvMO->address + (argc+1)*NumPtrBytes;
                arguments.push_back(Expr::createPointer(envp_start));

                if (++ai!=ae)
                    klee_error("invalid main function (expect 0-3 arguments)");
            }
        }
    }

    ExecutionState *state = new ExecutionState(kmodule->functionMap[f]);

    if (pathWriter)
        state->pathOS = pathWriter->open();
    if (symPathWriter)
        state->symPathOS = symPathWriter->open();


    if (statsTracker)
        statsTracker->framePushed(*state, 0);

    assert(arguments.size() == f->arg_size() && "wrong number of arguments");
    for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
        bindArgument(kf, i, *state, arguments[i]);

    if (argvMO) {
        ObjectState *argvOS = bindObjectInState(*state, argvMO, false);

        for (int i=0; i<argc+1+envc+1+1; i++) {
            if (i==argc || i>=argc+1+envc) {
                // Write NULL pointer
                argvOS->write(i * NumPtrBytes, Expr::createPointer(0));
            } else {
                char *s = i<argc ? argv[i] : envp[i-(argc+1)];
                int j, len = strlen(s);

                MemoryObject *arg =
                    memory->allocate(len + 1, /*isLocal=*/false, /*isGlobal=*/true,
                            /*allocSite=*/state->pc->inst, /*alignment=*/8);
                if (!arg)
                    klee_error("Could not allocate memory for function arguments");
                ObjectState *os = bindObjectInState(*state, arg, false);
                for (j=0; j<len+1; j++)
                    os->write8(j, s[j]);

                // Write pointer to newly allocated and initialised argv/envp c-string
                argvOS->write(i * NumPtrBytes, arg->getBaseExpr());
            }
        }
    }

    initializeGlobals(*state);

    processTree = std::make_unique<PTree>(state);
    run(*state);
    processTree = nullptr;

    // hack to clear memory objects
    delete memory;
    memory = new MemoryManager(NULL);

    globalObjects.clear();
    globalAddresses.clear();

    if (statsTracker)
        statsTracker->done();
}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state, std::string &res,
                                Interpreter::LogType logFormat) {

  switch (logFormat) {
  case STP: {
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } break;

  case KQUERY: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprPPrinter::printConstraints(info, state.constraints);
    res = info.str();
  } break;

  case SMTLIB2: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprSMTLIBPrinter printer;
    printer.setOutput(info);
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    printer.setQuery(query);
    printer.generateOutput();
    res = info.str();
  } break;

  default:
    klee_warning("Executor::getConstraintLog() : Log format not supported!");
  }
}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector<
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {
    solver->setTimeout(coreSolverTimeout);

    ExecutionState tmp(state);

    // Go through each byte in every test case and attempt to restrict
    // it to the constraints contained in cexPreferences.  (Note:
    // usually this means trying to make it an ASCII character (0-127)
    // and therefore human readable. It is also possible to customize
    // the preferred constraints.  See test/Features/PreferCex.c for
    // an example) While this process can be very expensive, it can
    // also make understanding individual test cases much easier.
    for (unsigned i = 0; i != state.symbolics.size(); ++i) {
        const auto &mo = state.symbolics[i].first;
        std::vector< ref<Expr> >::const_iterator pi =
            mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
        for (; pi != pie; ++pi) {
            bool mustBeTrue;
            // Attempt to bound byte to constraints held in cexPreferences
            bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi),
                    mustBeTrue);
            // If it isn't possible to constrain this particular byte in the desired
            // way (normally this would mean that the byte can't be constrained to
            // be between 0 and 127 without making the entire constraint list UNSAT)
            // then just continue on to the next byte.
            if (!success) break;
            // If the particular constraint operated on in this iteration through
            // the loop isn't implied then add it to the list of constraints.
            if (!mustBeTrue) tmp.addConstraint(*pi);
        }
        if (pi!=pie) break;
    }

    std::vector< std::vector<unsigned char> > values;
    std::vector<const Array*> objects;
    for (unsigned i = 0; i != state.symbolics.size(); ++i)
        objects.push_back(state.symbolics[i].second);
    bool success = solver->getInitialValues(tmp, objects, values);
    solver->setTimeout(time::Span());
    if (!success) {
        klee_warning("unable to compute initial values (invalid constraints?)!");
        ExprPPrinter::printQuery(llvm::errs(), state.constraints,
                ConstantExpr::alloc(0, Expr::Bool));
        return false;
    }

    for (unsigned i = 0; i != state.symbolics.size(); ++i)
        res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
    return true;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace.findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly &&
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace.getWriteable(mo, os);
        wos->write(CE, it->second);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

size_t Executor::getAllocationAlignment(const llvm::Value *allocSite) const {
  // FIXME: 8 was the previous default. We shouldn't hard code this
  // and should fetch the default from elsewhere.
  const size_t forcedAlignment = 8;
  size_t alignment = 0;
  llvm::Type *type = NULL;
  std::string allocationSiteName(allocSite->getName().str());
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(allocSite)) {
    alignment = GV->getAlignment();
    if (const GlobalVariable *globalVar = dyn_cast<GlobalVariable>(GV)) {
      // All GlobalVariables's have pointer type
      llvm::PointerType *ptrType =
          dyn_cast<llvm::PointerType>(globalVar->getType());
      assert(ptrType && "globalVar's type is not a pointer");
      type = ptrType->getElementType();
    } else {
      type = GV->getType();
    }
  } else if (const AllocaInst *AI = dyn_cast<AllocaInst>(allocSite)) {
    alignment = AI->getAlignment();
    type = AI->getAllocatedType();
  } else if (isa<InvokeInst>(allocSite) || isa<CallInst>(allocSite)) {
    // FIXME: Model the semantics of the call to use the right alignment
    llvm::Value *allocSiteNonConst = const_cast<llvm::Value *>(allocSite);
    const CallSite cs = (isa<InvokeInst>(allocSiteNonConst)
                             ? CallSite(cast<InvokeInst>(allocSiteNonConst))
                             : CallSite(cast<CallInst>(allocSiteNonConst)));
    llvm::Function *fn =
        klee::getDirectCallTarget(cs, /*moduleIsFullyLinked=*/true);
    if (fn)
      allocationSiteName = fn->getName().str();

    klee_warning_once(fn != NULL ? fn : allocSite,
                      "Alignment of memory from call \"%s\" is not "
                      "modelled. Using alignment of %zu.",
                      allocationSiteName.c_str(), forcedAlignment);
    alignment = forcedAlignment;
  } else {
    llvm_unreachable("Unhandled allocation site");
  }

  if (alignment == 0) {
    assert(type != NULL);
    // No specified alignment. Get the alignment for the type.
    if (type->isSized()) {
      alignment = kmodule->targetData->getPrefTypeAlignment(type);
    } else {
      klee_warning_once(allocSite, "Cannot determine memory alignment for "
                                   "\"%s\". Using alignment of %zu.",
                        allocationSiteName.c_str(), forcedAlignment);
      alignment = forcedAlignment;
    }
  }

  // Currently we require alignment be a power of 2
  if (!bits64::isPowerOfTwo(alignment)) {
    klee_warning_once(allocSite, "Alignment of %zu requested for %s but this "
                                 "not supported. Using alignment of %zu",
                      alignment, allocSite->getName().str().c_str(),
                      forcedAlignment);
    alignment = forcedAlignment;
  }
  assert(bits64::isPowerOfTwo(alignment) &&
         "Returned alignment must be a power of two");
  return alignment;
}

void Executor::prepareForEarlyExit() {
  if (statsTracker) {
    // Make sure stats get flushed out
    statsTracker->done();
  }
}

/// Returns the errno location in memory
int *Executor::getErrnoLocation(const ExecutionState &state) const {
#if !defined(__APPLE__) && !defined(__FreeBSD__)
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  return __errno_location();
#else
  return __error();
#endif
}


void Executor::dumpPTree() {
  if (!::dumpPTree) return;

  char name[32];
  snprintf(name, sizeof(name),"ptree%08d.dot", (int) stats::instructions);
  auto os = interpreterHandler->openOutputFile(name);
  if (os) {
    processTree->dump(*os);
  }

  ::dumpPTree = 0;
}

void Executor::dumpStates() {
  if (!::dumpStates) return;

  auto os = interpreterHandler->openOutputFile("states.txt");

  if (os) {
    for (ExecutionState *es : states) {
      *os << "(" << es << ",";
      *os << "[";
      auto next = es->stack.begin();
      ++next;
      for (auto sfIt = es->stack.begin(), sf_ie = es->stack.end();
           sfIt != sf_ie; ++sfIt) {
        *os << "('" << sfIt->kf->function->getName().str() << "',";
        if (next == es->stack.end()) {
          *os << es->prevPC->info->line << "), ";
        } else {
          *os << next->caller->info->line << "), ";
          ++next;
        }
      }
      *os << "], ";

      StackFrame &sf = es->stack.back();
      uint64_t md2u = computeMinDistToUncovered(es->pc,
                                                sf.minDistToUncoveredOnReturn);
      uint64_t icnt = theStatisticManager->getIndexedValue(stats::instructions,
                                                           es->pc->info->id);
      uint64_t cpicnt = sf.callPathNode->statistics.getValue(stats::instructions);

      *os << "{";
      *os << "'depth' : " << es->depth << ", ";
      *os << "'queryCost' : " << es->queryCost << ", ";
      *os << "'coveredNew' : " << es->coveredNew << ", ";
      *os << "'instsSinceCovNew' : " << es->instsSinceCovNew << ", ";
      *os << "'md2u' : " << md2u << ", ";
      *os << "'icnt' : " << icnt << ", ";
      *os << "'CPicnt' : " << cpicnt << ", ";
      *os << "}";
      *os << ")\n";
    }
  }

  ::dumpStates = 0;
}

///

Interpreter *Interpreter::create(LLVMContext &ctx, const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(ctx, opts, ih);
}
