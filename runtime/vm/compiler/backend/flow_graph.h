// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_FLOW_GRAPH_H_
#define RUNTIME_VM_COMPILER_BACKEND_FLOW_GRAPH_H_

#if defined(DART_PRECOMPILED_RUNTIME)
#error "AOT runtime should not use compiler sources (including header files)"
#endif  // defined(DART_PRECOMPILED_RUNTIME)

#include <utility>

#include "vm/bit_vector.h"
#include "vm/compiler/backend/dart_calling_conventions.h"
#include "vm/compiler/backend/il.h"
#include "vm/growable_array.h"
#include "vm/hash_map.h"
#include "vm/parser.h"
#include "vm/thread.h"

namespace dart {

class LoopHierarchy;
class VariableLivenessAnalysis;

namespace compiler {
class GraphIntrinsifier;
}

class BlockIterator : public ValueObject {
 public:
  explicit BlockIterator(const GrowableArray<BlockEntryInstr*>& block_order)
      : block_order_(block_order), current_(0) {}

  BlockIterator(const BlockIterator& other)
      : ValueObject(),
        block_order_(other.block_order_),
        current_(other.current_) {}

  void Advance() {
    ASSERT(!Done());
    current_++;
  }

  bool Done() const { return current_ >= block_order_.length(); }

  BlockEntryInstr* Current() const { return block_order_[current_]; }

 private:
  const GrowableArray<BlockEntryInstr*>& block_order_;
  intptr_t current_;
};

class ConstantAndRepresentation {
 public:
  ConstantAndRepresentation(const Object& constant, Representation rep)
      : constant_(constant),
        representation_(rep),
        hash_(ComputeHash(constant)) {}

  const Object& constant() const { return constant_; }
  Representation representation() const { return representation_; }
  uword hash() const { return hash_; }

 private:
  static inline uword ComputeHash(const Object& constant) {
    // Caveat: a null might be hiding inside a handle which overrides
    // CanonicalizeHash() and does not check for |null| before computing the
    // hash. Thus doing Instance::Cast(constant).CanonicalizeHash() and
    // Instance::Handle(constant.ptr()).CanonicalizeHash() will produce
    // different results. To work-around this problem check for null first.
    if (constant.IsNull()) {
      return kNullIdentityHash;
    }
    return constant.IsInstance() ? Instance::Cast(constant).CanonicalizeHash()
                                 : Utils::WordHash(constant.GetClassId());
  }

  const Object& constant_;
  Representation representation_;
  uword hash_;
};

struct ConstantPoolTrait {
  typedef ConstantInstr* Value;
  typedef ConstantAndRepresentation Key;
  typedef ConstantInstr* Pair;

  static Key KeyOf(Pair kv) { return {kv->value(), kv->representation()}; }

  static Value ValueOf(Pair kv) { return kv; }

  static inline uword Hash(Key key) { return key.hash(); }

  static inline bool IsKeyEqual(Pair kv, Key key) {
    return (kv->value().ptr() == key.constant().ptr()) &&
           (kv->representation() == key.representation());
  }
};

struct PrologueInfo {
  // The first blockid used for prologue building.  This information can be used
  // by the inliner for budget calculations: The prologue code falls away when
  // inlining, so we should not include it in the budget.
  intptr_t min_block_id;

  // The last blockid used for prologue building.  This information can be used
  // by the inliner for budget calculations: The prologue code falls away when
  // inlining, so we should not include it in the budget.
  intptr_t max_block_id;

  PrologueInfo(intptr_t min, intptr_t max)
      : min_block_id(min), max_block_id(max) {}

  bool Contains(intptr_t block_id) const {
    return min_block_id <= block_id && block_id <= max_block_id;
  }
};

struct InliningInfo {
  // Maps inline_id_to_function[inline_id] -> function. Top scope
  // function has inline_id 0. The map is populated by the inliner.
  GrowableArray<const Function*> inline_id_to_function;
  // Token position where inlining occurred.
  GrowableArray<TokenPosition> inline_id_to_token_pos;
  // For a given inlining-id(index) specifies the caller's inlining-id.
  GrowableArray<intptr_t> caller_inline_id;

  explicit InliningInfo(const Function* function) {
    // Top scope function is at inlining id 0.
    inline_id_to_function.Add(function);
    // Top scope function has no caller (-1).
    caller_inline_id.Add(-1);
    // We do not add a token position for the top scope function to
    // |inline_id_to_token_pos| because it is not (currently) inlined into
    // another graph at a given token position. A side effect of this is that
    // the length of |inline_id_to_function| and |caller_inline_id| is always
    // larger than the length of |inline_id_to_token_pos| by one.
  }
};

// Class to encapsulate the construction and manipulation of the flow graph.
class FlowGraph : public ZoneAllocated {
 public:
  enum class CompilationMode {
    kUnoptimized,
    kOptimized,
    kIntrinsic,
  };

  FlowGraph(const ParsedFunction& parsed_function,
            GraphEntryInstr* graph_entry,
            intptr_t max_block_id,
            PrologueInfo prologue_info,
            CompilationMode compilation_mode);

  // Function properties.
  const ParsedFunction& parsed_function() const { return parsed_function_; }
  const Function& function() const { return parsed_function_.function(); }

  void Print(const char* phase = "unknown");

  // The number of directly accessable parameters (above the frame pointer).
  // All other parameters can only be indirectly loaded via metadata found in
  // the arguments descriptor.
  intptr_t num_direct_parameters() const { return num_direct_parameters_; }

  // The number of variables (or boxes) which code can load from / store to.
  // The SSA renaming will insert phi's for them (and only them - i.e. there
  // will be no phi insertion for [LocalVariable]s pointing to the expression
  // stack!).
  intptr_t variable_count() const {
    return num_direct_parameters_ + parsed_function_.num_stack_locals();
  }

  // The number of variables during OSR, which may include stack slots
  // that pass in initial contents for the expression stack.
  intptr_t osr_variable_count() const {
    ASSERT(IsCompiledForOsr());
    return variable_count() + graph_entry()->osr_entry()->stack_depth();
  }

  static Representation ParameterRepresentationAt(const Function& function,
                                                  intptr_t index);

  static Representation ReturnRepresentationOf(const Function& function);

  // The number of variables (or boxes) inside the functions frame - meaning
  // below the frame pointer.  This does not include the expression stack.
  intptr_t num_stack_locals() const {
    return parsed_function_.num_stack_locals();
  }

  bool IsIrregexpFunction() const { return function().IsIrregexpFunction(); }

  LocalVariable* SuspendStateVar() const {
    return parsed_function().suspend_state_var();
  }

  intptr_t SuspendStateEnvIndex() const { return EnvIndex(SuspendStateVar()); }

  LocalVariable* CurrentContextVar() const {
    return parsed_function().current_context_var();
  }

  intptr_t CurrentContextEnvIndex() const {
    return EnvIndex(parsed_function().current_context_var());
  }

  intptr_t RawTypeArgumentEnvIndex() const {
    return EnvIndex(parsed_function().RawTypeArgumentsVariable());
  }

  intptr_t ArgumentDescriptorEnvIndex() const {
    return EnvIndex(parsed_function().arg_desc_var());
  }

  intptr_t EnvIndex(const LocalVariable* variable) const {
    ASSERT(!variable->is_captured());
    return num_direct_parameters_ - variable->index().value();
  }

  // Context and :suspend_state variables are never pruned and
  // artificially kept alive.
  bool IsImmortalVariable(intptr_t env_index) const {
    return (env_index == CurrentContextEnvIndex()) ||
           (SuspendStateVar() != nullptr &&
            env_index == SuspendStateEnvIndex());
  }

  // Flow graph orders.
  const GrowableArray<BlockEntryInstr*>& preorder() const { return preorder_; }
  const GrowableArray<BlockEntryInstr*>& postorder() const {
    return postorder_;
  }
  const GrowableArray<BlockEntryInstr*>& reverse_postorder() const {
    return reverse_postorder_;
  }
  const GrowableArray<BlockEntryInstr*>& optimized_block_order() const {
    return optimized_block_order_;
  }
  const GrowableArray<TryEntryInstr*>& try_entries() const {
    return try_entries_;
  }
  TryEntryInstr* GetTryEntryByTryIndex(intptr_t try_index) const {
    ASSERT(try_index < try_entries_.length());
    return try_entries_[try_index];
  }
  CatchBlockEntryInstr* GetCatchBlockByTryIndex(intptr_t try_index) const {
    return GetTryEntryByTryIndex(try_index)->catch_target();
  }
  intptr_t max_try_index() const { return max_try_index_; }

  // In AOT these are guaranteed to be topologically sorted, but not in JIT.
  GrowableArray<BlockEntryInstr*>* CodegenBlockOrder();
  const GrowableArray<BlockEntryInstr*>* CodegenBlockOrder() const;

  // Iterators.
  BlockIterator reverse_postorder_iterator() const {
    return BlockIterator(reverse_postorder());
  }
  BlockIterator postorder_iterator() const {
    return BlockIterator(postorder());
  }

  void EnsureSSATempIndex(Definition* defn, Definition* replacement);

  void ReplaceCurrentInstruction(ForwardInstructionIterator* iterator,
                                 Instruction* current,
                                 Instruction* replacement);

  Instruction* CreateCheckClass(Definition* to_check,
                                const Cids& cids,
                                intptr_t deopt_id,
                                const InstructionSource& source);

  bool ShouldOmitCheckBoundsIn(const Function& caller);

  Instruction* AppendCheckBound(Instruction* cursor,
                                Definition* length,
                                Definition** index,
                                intptr_t deopt_id,
                                Environment* env);

  void AddExactnessGuard(InstanceCallInstr* call, intptr_t receiver_cid);

  intptr_t current_ssa_temp_index() const { return current_ssa_temp_index_; }
  void set_current_ssa_temp_index(intptr_t index) {
    current_ssa_temp_index_ = index;
  }

  intptr_t max_vreg() const {
    return current_ssa_temp_index() * kMaxLocationCount;
  }

  enum class ToCheck { kNoCheck, kCheckNull, kCheckCid };

  // Uses CHA to determine if the called method can be overridden.
  // Return value indicates that the call needs no check at all,
  // just a null check, or a full class check.
  ToCheck CheckForInstanceCall(InstanceCallInstr* call,
                               UntaggedFunction::Kind kind) const;

  Thread* thread() const { return thread_; }
  Zone* zone() const { return thread()->zone(); }
  IsolateGroup* isolate_group() const { return thread()->isolate_group(); }

  intptr_t max_block_id() const { return max_block_id_; }
  void set_max_block_id(intptr_t id) { max_block_id_ = id; }
  intptr_t allocate_block_id() { return ++max_block_id_; }

  GraphEntryInstr* graph_entry() const { return graph_entry_; }

  ConstantInstr* constant_null() const { return constant_null_; }

  ConstantInstr* constant_dead() const { return constant_dead_; }

  void AllocateSSAIndex(Definition* def) {
    def->set_ssa_temp_index(current_ssa_temp_index_);
    current_ssa_temp_index_++;
  }

  intptr_t InstructionCount() const;

  // Returns the definition for the object from the constant pool if
  // one exists, otherwise returns nullptr.
  ConstantInstr* GetExistingConstant(
      const Object& object,
      Representation representation = kTagged) const;

  // Always returns a definition for the object from the constant pool,
  // allocating one if it doesn't already exist.
  ConstantInstr* GetConstant(const Object& object,
                             Representation representation = kTagged);

  void AddToGraphInitialDefinitions(Definition* defn);
  void AddToInitialDefinitions(BlockEntryWithInitialDefs* entry,
                               Definition* defn);

  // Tries to create a constant definition with the given value which can be
  // used to replace the given operation. Ensures that the representation of
  // the replacement matches the representation of the original definition.
  // If the given value can't be represented using matching representation
  // then returns op itself.
  Definition* TryCreateConstantReplacementFor(Definition* op,
                                              const Object& value);

  // Returns true if the given constant value can be represented in the given
  // representation.
  static bool IsConstantRepresentable(const Object& value,
                                      Representation target_rep,
                                      bool tagged_value_must_be_smi);

  enum UseKind { kEffect, kValue };

  void InsertBefore(Instruction* next,
                    Instruction* instr,
                    Environment* env,
                    UseKind use_kind) {
    InsertAfter(next->previous(), instr, env, use_kind);
  }
  void InsertSpeculativeBefore(Instruction* next,
                               Instruction* instr,
                               Environment* env,
                               UseKind use_kind) {
    InsertSpeculativeAfter(next->previous(), instr, env, use_kind);
  }
  void InsertAfter(Instruction* prev,
                   Instruction* instr,
                   Environment* env,
                   UseKind use_kind);

  // Inserts a speculative [instr] after existing [prev] instruction.
  //
  // If the inserted [instr] deopts eagerly or lazily we will always continue in
  // unoptimized code at before-call using the given [env].
  //
  // This is mainly used during inlining / call specializing when replacing
  // calls with N specialized instructions where the inserted [1..N[
  // instructions cannot continue in unoptimized code after-call since they
  // would miss instructions following the one that lazy-deopted.
  //
  // For example specializing an instance call to an implicit field setter
  //
  //     InstanceCall:<id>(v0, set:<name>, args = [v1])
  //
  // to
  //
  //     v2 <- AssertAssignable:<id>(v1, ...)
  //     StoreField(v0, v2)
  //
  // If the [AssertAssignable] causes a lazy-deopt on return, we'll have to
  // *re-try* the implicit setter call in unoptimized mode, i.e. lazy deopt to
  // before-call (otherwise - if we continued after-call - the
  // StoreField would not be performed).
  void InsertSpeculativeAfter(Instruction* prev,
                              Instruction* instr,
                              Environment* env,
                              UseKind use_kind);
  Instruction* AppendTo(Instruction* prev,
                        Instruction* instr,
                        Environment* env,
                        UseKind use_kind);
  Instruction* AppendSpeculativeTo(Instruction* prev,
                                   Instruction* instr,
                                   Environment* env,
                                   UseKind use_kind);

  // Operations on the flow graph.
  void ComputeSSA(ZoneGrowableArray<Definition*>* inlining_parameters);

  // Verification method for debugging.
  bool VerifyRedefinitions();

  void DiscoverBlocks();

  void MergeBlocks();

  // Insert a redefinition of an original definition after prev and rename all
  // dominated uses of the original.  If an equivalent redefinition is already
  // present, nothing is inserted.
  // Returns the redefinition, if a redefinition was inserted, nullptr
  // otherwise.
  RedefinitionInstr* EnsureRedefinition(Instruction* prev,
                                        Definition* original,
                                        CompileType compile_type);

  // Remove the redefinition instructions inserted to inhibit code motion.
  void RemoveRedefinitions(bool keep_checks = false);

  // Insert MoveArgument instructions and remove explicit def-use
  // relations between calls and their arguments.
  //
  // Compute the maximum number of arguments.
  void InsertMoveArguments();

  // Copy deoptimization target from one instruction to another if we still
  // have to keep deoptimization environment at gotos for LICM purposes.
  void CopyDeoptTarget(Instruction* to, Instruction* from) {
    if (is_licm_allowed()) {
      to->InheritDeoptTarget(zone(), from);
    }
  }

  // Returns true if every Goto in the graph is expected to have a
  // deoptimization environment and can be used as deoptimization target
  // for hoisted instructions.
  bool is_licm_allowed() const { return licm_allowed_; }

  // Stop preserving environments on Goto instructions. LICM is not allowed
  // after this point.
  void disallow_licm() { licm_allowed_ = false; }

  // Returns true if mismatch in input/output representations is allowed.
  bool unmatched_representations_allowed() const {
    return unmatched_representations_allowed_;
  }

  // After the last SelectRepresentations pass all further transformations
  // should maintain matching input/output representations.
  void disallow_unmatched_representations() {
    unmatched_representations_allowed_ = false;
  }

  // Returns true if this flow graph was built for a huge method
  // and certain optimizations should be disabled.
  bool is_huge_method() const { return huge_method_; }
  // Mark this flow graph as huge and disable certain optimizations.
  void mark_huge_method() { huge_method_ = true; }

  PrologueInfo prologue_info() const { return prologue_info_; }

  // Computes the loop hierarchy of the flow graph on demand.
  const LoopHierarchy& GetLoopHierarchy() {
    if (loop_hierarchy_ == nullptr) {
      loop_hierarchy_ = ComputeLoops();
    }
    return loop_hierarchy();
  }

  const LoopHierarchy& loop_hierarchy() const { return *loop_hierarchy_; }

  // Resets the loop hierarchy of the flow graph. Use this to
  // force a recomputation of loop detection by the next call
  // to GetLoopHierarchy() (note that this does not immediately
  // reset the loop_info fields of block entries, although
  // these will be overwritten by that next call).
  void ResetLoopHierarchy() {
    loop_hierarchy_ = nullptr;
    loop_invariant_loads_ = nullptr;
  }

  // Per loop header invariant loads sets. Each set contains load id for
  // those loads that are not affected by anything in the loop and can be
  // hoisted out. Sets are computed by LoadOptimizer.
  ZoneGrowableArray<BitVector*>* loop_invariant_loads() const {
    return loop_invariant_loads_;
  }
  void set_loop_invariant_loads(
      ZoneGrowableArray<BitVector*>* loop_invariant_loads) {
    loop_invariant_loads_ = loop_invariant_loads;
  }

  bool IsCompiledForOsr() const { return graph_entry()->IsCompiledForOsr(); }

  BitVector* captured_parameters() const { return captured_parameters_; }

  intptr_t inlining_id() const { return inlining_id_; }
  void set_inlining_id(intptr_t value) { inlining_id_ = value; }

  InliningInfo& inlining_info() { return inlining_info_; }
  const InliningInfo& inlining_info() const { return inlining_info_; }

  // Returns true if any instructions were canonicalized away.
  bool Canonicalize();

  // Attaches new ICData's to static/instance calls which don't already have
  // them.
  void PopulateWithICData(const Function& function);

  void SelectRepresentations();

  // Remove environments from the instructions which do not deoptimize.
  void EliminateEnvironments();

  // Extract typed data payloads prior to any LoadIndexed, StoreIndexed, or
  // MemoryCopy instruction where the incoming typed data array(s) are not
  // proven to be internal typed data objects at compile time.
  //
  // Once this is done, no intra-block code motion should be performed.
  void ExtractNonInternalTypedDataPayloads();

  bool IsReceiver(Definition* def) const;

  // Optimize (a << b) & c pattern: if c is a positive Smi or zero, then the
  // shift can be a truncating Smi shift-left and result is always Smi.
  // Merge instructions (only per basic-block).
  void TryOptimizePatterns();

  // Replaces uses that are dominated by dom of 'def' with 'other'.
  // Note: uses that occur at instruction dom itself are not dominated by it.
  static void RenameDominatedUses(Definition* def,
                                  Instruction* dom,
                                  Definition* other);

  // Renames uses of redefined values to make sure that uses of redefined
  // values that are dominated by a redefinition are renamed.
  void RenameUsesDominatedByRedefinitions();

  bool should_print() const { return should_print_; }
  const uint8_t* compiler_pass_filters() const {
    return compiler_pass_filters_;
  }

  bool should_reorder_blocks() const { return should_reorder_blocks_; }

  bool should_omit_check_bounds() const { return should_omit_check_bounds_; }

  //
  // High-level utilities.
  //

  // Logical-AND (for use in short-circuit diamond).
  struct LogicalAnd {
    LogicalAnd(ConditionInstr* x, ConditionInstr* y) : oper1(x), oper2(y) {}
    ConditionInstr* oper1;
    ConditionInstr* oper2;
  };

  // Constructs a diamond control flow at the instruction, inheriting
  // properties from inherit and using the given compare. Returns the
  // join (and true/false blocks in out parameters). Updates dominance
  // relation, but not the succ/pred ordering on block.
  JoinEntryInstr* NewDiamond(Instruction* instruction,
                             Instruction* inherit,
                             ConditionInstr* condition,
                             TargetEntryInstr** block_true,
                             TargetEntryInstr** block_false);

  // As above, but with a short-circuit on two comparisons.
  JoinEntryInstr* NewDiamond(Instruction* instruction,
                             Instruction* inherit,
                             const LogicalAnd& condition,
                             TargetEntryInstr** block_true,
                             TargetEntryInstr** block_false);

  // Adds a 2-way phi.
  PhiInstr* AddPhi(JoinEntryInstr* join, Definition* d1, Definition* d2);

  // SSA transformation methods and fields.
  void ComputeDominators(GrowableArray<BitVector*>* dominance_frontier);

  void CreateCommonConstants();

  const Array& coverage_array() const { return *coverage_array_; }
  void set_coverage_array(const Array& array) { coverage_array_ = &array; }

  // Renumbers SSA values and basic blocks to make numbering dense.
  // Preserves order among block ids.
  //
  // Also collects definitions which are detached from the flow graph
  // but still referenced (currently only MaterializeObject instructions
  // can be detached).
  void CompactSSA(ZoneGrowableArray<Definition*>* detached_defs = nullptr);

  // Maximum number of word-sized slots needed for outgoing arguments.
  intptr_t max_argument_slot_count() const {
    RELEASE_ASSERT(max_argument_slot_count_ >= 0);
    return max_argument_slot_count_;
  }
  void set_max_argument_slot_count(intptr_t count) {
    RELEASE_ASSERT(max_argument_slot_count_ == -1);
    max_argument_slot_count_ = count;
  }

  const std::pair<Location, Representation>& GetDirectParameterInfoAt(
      intptr_t i) {
    return direct_parameter_locations_[i];
  }

  static intptr_t ComputeLocationsOfFixedParameters(
      Zone* zone,
      const Function& function,
      bool should_assign_stack_locations = false,
      compiler::ParameterInfoArray* parameter_info = nullptr);

  static intptr_t ComputeArgumentsSizeInWords(const Function& function,
                                              intptr_t arguments_count);

  static constexpr CompilationMode CompilationModeFrom(bool is_optimizing) {
    return is_optimizing ? CompilationMode::kOptimized
                         : CompilationMode::kUnoptimized;
  }

  // If either IsExternalPayloadClassId([cid]) or
  // IsExternalPayloadClassId(array()->Type()->ToCid()) is true and
  // [array] (an input of [instr]) is tagged, inserts a load of the array
  // payload as an untagged pointer and rebinds [array] to the new load.
  //
  // Otherwise does not change the flow graph.
  //
  // Returns whether any changes were made to the flow graph.
  bool ExtractExternalUntaggedPayload(Instruction* instr,
                                      Value* array,
                                      classid_t cid);

 private:
  friend class FlowGraphCompiler;  // TODO(ajcbik): restructure
  friend class FlowGraphChecker;
  friend class IfConverter;
  friend class BranchSimplifier;
  friend class ConstantPropagator;
  friend class DeadCodeElimination;
  friend class compiler::GraphIntrinsifier;

  void CompressPath(intptr_t start_index,
                    intptr_t current_index,
                    GrowableArray<intptr_t>* parent,
                    GrowableArray<intptr_t>* label);

  void AddSyntheticPhis(BlockEntryInstr* block);

  void Rename(GrowableArray<PhiInstr*>* live_phis,
              VariableLivenessAnalysis* variable_liveness,
              ZoneGrowableArray<Definition*>* inlining_parameters);
  void RenameRecursive(BlockEntryInstr* block_entry,
                       GrowableArray<Definition*>* env,
                       GrowableArray<PhiInstr*>* live_phis,
                       VariableLivenessAnalysis* variable_liveness,
                       ZoneGrowableArray<Definition*>* inlining_parameters);
#if defined(DEBUG)
  // Validates no phis are missing on join entry instructions.
  void ValidatePhis();
#endif  // defined(DEBUG)

  void PopulateEnvironmentFromFunctionEntry(
      FunctionEntryInstr* function_entry,
      GrowableArray<Definition*>* env,
      GrowableArray<PhiInstr*>* live_phis,
      VariableLivenessAnalysis* variable_liveness,
      ZoneGrowableArray<Definition*>* inlining_parameters);

  void PopulateEnvironmentFromOsrEntry(OsrEntryInstr* osr_entry,
                                       GrowableArray<Definition*>* env);

  void PopulateEnvironmentFromCatchEntry(CatchBlockEntryInstr* catch_entry,
                                         GrowableArray<Definition*>* env);

  void AttachEnvironment(Instruction* instr, GrowableArray<Definition*>* env);

  void InsertPhis(const GrowableArray<BlockEntryInstr*>& preorder,
                  const GrowableArray<BitVector*>& assigned_vars,
                  const GrowableArray<BitVector*>& dom_frontier,
                  GrowableArray<PhiInstr*>* live_phis);
  void AddCatchEntryParameter(intptr_t var_index,
                              CatchBlockEntryInstr* catch_entry);
  void InsertCatchBlockParams(const GrowableArray<BlockEntryInstr*>& preorder,
                              const GrowableArray<BitVector*>& assigned_vars);

  void RemoveDeadPhis(GrowableArray<PhiInstr*>* live_phis);

  void ReplacePredecessor(BlockEntryInstr* old_block,
                          BlockEntryInstr* new_block);

  // Finds the blocks in the natural loop for the back edge m->n. The
  // algorithm is described in "Advanced Compiler Design & Implementation"
  // (Muchnick) p192. Returns a BitVector indexed by block pre-order
  // number where each bit indicates membership in the loop.
  BitVector* FindLoopBlocks(BlockEntryInstr* m, BlockEntryInstr* n) const;

  // Finds the natural loops in the flow graph and attaches the loop
  // information to each entry block. Returns the loop hierarchy.
  LoopHierarchy* ComputeLoops() const;

  void InsertConversionsFor(Definition* def);
  void ConvertUse(Value* use, Representation from);
  void InsertConversion(Representation from,
                        Representation to,
                        Value* use,
                        bool is_environment_use);

  // Insert allocation of a record instance for [def]
  // which returns an unboxed record.
  void InsertRecordBoxing(Definition* def);

  void ComputeIsReceiver(PhiInstr* phi) const;
  void ComputeIsReceiverRecursive(PhiInstr* phi,
                                  GrowableArray<PhiInstr*>* unmark) const;

  void OptimizeLeftShiftBitAndSmiOp(
      ForwardInstructionIterator* current_iterator,
      Definition* bit_and_instr,
      Definition* left_instr,
      Definition* right_instr);

  void TryMergeTruncDivMod(GrowableArray<BinarySmiOpInstr*>* merge_candidates);

  void AppendExtractNthOutputForMerged(Definition* instr,
                                       intptr_t ix,
                                       Representation rep,
                                       intptr_t cid);

  void ExtractUntaggedPayload(Instruction* instr,
                              Value* array,
                              const Slot& slot,
                              InnerPointerAccess access);

  void ExtractNonInternalTypedDataPayload(Instruction* instr,
                                          Value* array,
                                          classid_t cid);

  Thread* thread_;

  // DiscoverBlocks computes parent_ and assigned_vars_ which are then used
  // if/when computing SSA.
  GrowableArray<intptr_t> parent_;

  intptr_t current_ssa_temp_index_;
  intptr_t max_block_id_;

  // Flow graph fields.
  const ParsedFunction& parsed_function_;
  intptr_t num_direct_parameters_;
  compiler::ParameterInfoArray direct_parameter_locations_;
  GraphEntryInstr* graph_entry_;
  GrowableArray<BlockEntryInstr*> preorder_;
  GrowableArray<BlockEntryInstr*> postorder_;
  GrowableArray<BlockEntryInstr*> reverse_postorder_;
  GrowableArray<BlockEntryInstr*> optimized_block_order_;
  // Try entries indexed by try-index
  GrowableArray<TryEntryInstr*> try_entries_;
  intptr_t max_try_index_ = -1;
  ConstantInstr* constant_null_;
  ConstantInstr* constant_dead_;

  bool licm_allowed_;
  bool unmatched_representations_allowed_ = true;
  bool huge_method_ = false;
  const bool should_reorder_blocks_;

  const PrologueInfo prologue_info_;

  // Loop related fields.
  LoopHierarchy* loop_hierarchy_;
  ZoneGrowableArray<BitVector*>* loop_invariant_loads_;

  DirectChainedHashMap<ConstantPoolTrait> constant_instr_pool_;
  BitVector* captured_parameters_;

  // Inlining related fields.
  intptr_t inlining_id_;
  InliningInfo inlining_info_;

  bool should_print_;
  const bool should_omit_check_bounds_;
  uint8_t* compiler_pass_filters_ = nullptr;

  intptr_t max_argument_slot_count_ = -1;

  const Array* coverage_array_ = &Array::empty_array();
};

class LivenessAnalysis : public ValueObject {
 public:
  LivenessAnalysis(intptr_t variable_count,
                   const GrowableArray<BlockEntryInstr*>& postorder);

  void Analyze();

  virtual ~LivenessAnalysis() {}

  BitVector* GetLiveInSetAt(intptr_t postorder_number) const {
    return live_in_[postorder_number];
  }

  BitVector* GetLiveOutSetAt(intptr_t postorder_number) const {
    return live_out_[postorder_number];
  }

  BitVector* GetLiveInSet(BlockEntryInstr* block) const {
    return GetLiveInSetAt(block->postorder_number());
  }

  BitVector* GetKillSet(BlockEntryInstr* block) const {
    return kill_[block->postorder_number()];
  }

  BitVector* GetLiveOutSet(BlockEntryInstr* block) const {
    return GetLiveOutSetAt(block->postorder_number());
  }

  // Print results of liveness analysis.
  void Dump();

 protected:
  // Compute initial values for live-out, kill and live-in sets.
  virtual void ComputeInitialSets() = 0;

  // Update live-out set for the given block: live-out should contain
  // all values that are live-in for block's successors.
  // Returns true if live-out set was changed.
  virtual bool UpdateLiveOut(const BlockEntryInstr& instr);

  // Update live-in set for the given block: live-in should contain
  // all values that are live-out from the block and are not defined
  // by this block.
  // Returns true if live-in set was changed.
  virtual bool UpdateLiveIn(const BlockEntryInstr& instr);

  // Perform fix-point iteration updating live-out and live-in sets
  // for blocks until they stop changing.
  void ComputeLiveInAndLiveOutSets();

  Zone* zone() const { return zone_; }

  Zone* zone_;

  const intptr_t variable_count_;

  const GrowableArray<BlockEntryInstr*>& postorder_;

  // Live-out sets for each block.  They contain indices of variables
  // that are live out from this block. That is values that were (1) either
  // defined in this block or live into it, and (2) that are used in some
  // successor block.
  GrowableArray<BitVector*> live_out_;

  // Kill sets for each block.  They contain indices of variables that
  // are defined by this block.
  GrowableArray<BitVector*> kill_;

  // Live-in sets for each block.  They contain indices of variables
  // that are used by this block or its successors.
  GrowableArray<BitVector*> live_in_;
};

class DefinitionWorklist : public ValueObject {
 public:
  DefinitionWorklist(FlowGraph* flow_graph, intptr_t initial_capacity)
      : defs_(initial_capacity),
        contains_vector_(new BitVector(flow_graph->zone(),
                                       flow_graph->current_ssa_temp_index())) {}

  void Add(Definition* defn) {
    if (!Contains(defn)) {
      defs_.Add(defn);
      contains_vector_->Add(defn->ssa_temp_index());
    }
  }

  bool Contains(Definition* defn) const {
    return (defn->ssa_temp_index() >= 0) &&
           contains_vector_->Contains(defn->ssa_temp_index());
  }

  bool IsEmpty() const { return defs_.is_empty(); }

  Definition* RemoveLast() {
    Definition* defn = defs_.RemoveLast();
    contains_vector_->Remove(defn->ssa_temp_index());
    return defn;
  }

  const GrowableArray<Definition*>& definitions() const { return defs_; }
  BitVector* contains_vector() const { return contains_vector_; }

  void Clear() {
    defs_.TruncateTo(0);
    contains_vector_->Clear();
  }

 private:
  GrowableArray<Definition*> defs_;
  BitVector* contains_vector_;
};

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_FLOW_GRAPH_H_
