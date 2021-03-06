/*
 * Copyright 1997-2007 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *  
 */
// This file is a derivative work resulting from (and including) modifications
// made by Azul Systems, Inc.  The date of such changes is 2010.
// Copyright 2010 Azul Systems, Inc.  All Rights Reserved.
//
// Please contact Azul Systems, Inc., 1600 Plymouth Street, Mountain View, 
// CA 94043 USA, or visit www.azulsystems.com if you need additional information 
// or have any questions.


#include "assembler_pd.hpp"
#include "ciInstance.hpp"
#include "copy.hpp"
#include "jvmtiExport.hpp"
#include "matcher.hpp"
#include "mutexLocker.hpp"
#include "parse.hpp"
#include "rootnode.hpp"
#include "runtime.hpp"
#include "sharedRuntime.hpp"
#include "stubRoutines.hpp"

#include "atomic_os_pd.inline.hpp"
#include "bitMap.inline.hpp"
#include "frame.inline.hpp"
#include "handles.inline.hpp"
#include "heapRef_pd.inline.hpp"
#include "markSweep.inline.hpp"
#include "markWord.inline.hpp"
#include "mutex.inline.hpp"
#include "objectRef_pd.inline.hpp"
#include "oop.inline.hpp"
#include "orderAccess_os_pd.inline.hpp"
#include "space.inline.hpp"
#include "stackRef_pd.inline.hpp"

// Static array so we can figure out which bytecodes stop us from compiling
// the most. Some of the non-static variables are needed in bytecodeInfo.cpp
// and eventually should be encapsulated in a proper class (gri 8/18/98).

int nodes_created              = 0; int nodes_created_old              = 0;
int methods_parsed             = 0; int methods_parsed_old             = 0;
int methods_seen               = 0; int methods_seen_old               = 0;

int explicit_null_checks_inserted = 0, explicit_null_checks_inserted_old = 0;
int explicit_null_checks_elided   = 0, explicit_null_checks_elided_old   = 0;
int all_null_checks_found         = 0, implicit_null_checks              = 0;
int implicit_null_throws          = 0;

int parse_idx = 0;
size_t parse_arena = 0;
int reclaim_idx  = 0;
int reclaim_in   = 0;
int reclaim_node = 0;

#ifndef PRODUCT
bool Parse::BytecodeParseHistogram::_initialized = false;
uint Parse::BytecodeParseHistogram::_bytecodes_parsed [Bytecodes::number_of_codes];
uint Parse::BytecodeParseHistogram::_nodes_constructed[Bytecodes::number_of_codes];
uint Parse::BytecodeParseHistogram::_nodes_transformed[Bytecodes::number_of_codes];
uint Parse::BytecodeParseHistogram::_new_values       [Bytecodes::number_of_codes];
#endif

//------------------------------print_statistics-------------------------------
#ifndef PRODUCT
void Parse::print_statistics() {
C2OUT->print_cr("--- Compiler Statistics ---");
C2OUT->print("Methods seen: %d  Methods parsed: %d",methods_seen,methods_parsed);
C2OUT->print("  Nodes created: %d",nodes_created);
C2OUT->cr();
  if (methods_seen != methods_parsed)
C2OUT->print_cr("Reasons for parse failures (NOT cumulative):");

  if( explicit_null_checks_inserted )
C2OUT->print_cr("%d original NULL checks - %d elided (%2d%%); optimizer leaves %d,",explicit_null_checks_inserted,explicit_null_checks_elided,(100*explicit_null_checks_elided)/explicit_null_checks_inserted,all_null_checks_found);
  if( all_null_checks_found )
C2OUT->print_cr("%d made implicit (%2d%%)",implicit_null_checks,
                  (100*implicit_null_checks)/all_null_checks_found);
  if( implicit_null_throws )
    tty->print_cr("%d implicit null exceptions at runtime",
                  implicit_null_throws);

  if( PrintParseStatistics && BytecodeParseHistogram::initialized() ) {
    BytecodeParseHistogram::print();
  }
}
#endif

//------------------------------ON STACK REPLACEMENT---------------------------

// Construct a node which can be used to get incoming state for
// on stack replacement.
Node *Parse::fetch_interpreter_state(int index,
                                     BasicType bt,
                                     Node *local_addrs,
                                     Node *local_addrs_base) {
  Node *mem = memory(Compile::AliasIdxRaw);
const int endian=0;
Node*adr=basic_plus_adr(local_addrs_base,local_addrs,-index*wordSize+endian);

  // Very similar to LoadNode::make, except we handle un-aligned longs and
  // doubles on Sparc.  Intel can handle them just fine directly.
LoadNode*l=NULL;
  switch( bt ) {                // Signature is flattened
  case T_INT:     l = new (C, 3) LoadINode( 0, mem, adr, TypeRawPtr::BOTTOM ); break;
  case T_FLOAT:   l = new (C, 3) LoadFNode( 0, mem, adr, TypeRawPtr::BOTTOM ); break;
  case T_ADDRESS: 
case T_OBJECT:l=new(C,3)LoadPNode(0,mem,adr,TypeRawPtr::BOTTOM,TypeInstPtr::OBJECT);break;
  case T_LONG:
  case T_DOUBLE: {
    // Since arguments are in reverse order, the argument address 'adr'
    // refers to the back half of the long/double.  Recompute adr.
    adr = basic_plus_adr( local_addrs_base, local_addrs, -(index+1)*wordSize );
    l = (bt == T_DOUBLE) 
?(LoadNode*)new(C,3)LoadDNode(0,mem,adr,TypeRawPtr::BOTTOM)
:(LoadNode*)new(C,3)LoadLNode(0,mem,adr,TypeRawPtr::BOTTOM);
    break;
  }
  default: ShouldNotReachHere();
  }
  l->guarding_test_no_longer_precise(); // Do not allow hoisting
  return _gvn.transform(l);
}

// Helper routine to prevent the interpreter from handing
// unexpected typestate to an OSR method.
// The Node l is a value newly dug out of the interpreter frame.
// The type is the type predicted by ciTypeFlow.  Note that it is
// not a general type, but can only come from Type::get_typeflow_type.
// The safepoint is a map which will feed an uncommon trap.
Node* Parse::check_interpreter_type(Node* l, const Type* type,
                                    SafePointNode* &bad_type_exit) {

  const TypeOopPtr* tp = type->isa_oopptr();

  // TypeFlow may assert null-ness if a type appears unloaded.
  if (type == TypePtr::NULL_PTR ||
      (tp != NULL && !tp->klass()->is_loaded())) {
    // Value must be null, not a real oop.
    Node* chk = _gvn.transform( new (C, 3) CmpPNode(l, null()) );
    Node* tst = _gvn.transform( new (C, 2) BoolNode(chk, BoolTest::eq) );
    IfNode* iff = create_and_map_if(control(), tst, PROB_MAX, COUNT_UNKNOWN);
    set_control(_gvn.transform( new (C, 1) IfTrueNode(iff) ));
    Node* bad_type = _gvn.transform( new (C, 1) IfFalseNode(iff) );
    bad_type_exit->control()->add_req(bad_type);
    l = null();
  }

  // Typeflow can also cut off paths from the CFG, based on
  // types which appear unloaded, or call sites which appear unlinked.
  // When paths are cut off, values at later merge points can rise
  // toward more specific classes.  Make sure these specific classes
  // are still in effect.
  if( !tp ) return l;
ciKlass*klass=tp->klass();
  CPData_Null cpdn;
  cpdn._null = true;  // Be conservative about the null-check - no need for speed on OSR entry
  cpdn._fail = false; // But don't generate code to throw if we fail, assume TypeFlow is OK.
  if( klass != C->env()->Object_klass() ){// If need an interesting cast
    // TypeFlow asserted a specific object type.  Value must have that type.
    Node* bad_type_ctrl = NULL;
l=gen_checkcast(l,makecon(TypeKlassPtr::make(klass)),&bad_type_ctrl,"OSR initial state null chk",&cpdn);
    bad_type_exit->control()->add_req(bad_type_ctrl);
  } 
  // Could need interface checks as well
  for( ciInstanceKlass*const* iptr=tp->ifaces(); *iptr; iptr++ ) {
    Node* bad_type_ctrl = NULL;
    l = gen_checkcast(l, makecon(TypeKlassPtr::make(*iptr)), &bad_type_ctrl, "OSR initial state null chk", &cpdn);
    bad_type_exit->control()->add_req(bad_type_ctrl);
  }

  BasicType bt_l = _gvn.type(l)->basic_type();
  BasicType bt_t = type->basic_type();
  assert(_gvn.type(l)->higher_equal(type), "must constrain OSR typestate");
  return l;
}

// Helper routine which sets up elements of the initial parser map when
// performing a parse for on stack replacement.  Add values into map.
// The only parameter contains the address of a interpreter arguments.
void Parse::load_interpreter_state(Node* osr_buf) {
  int index;
  int max_locals = jvms()->loc_size();
  int max_stack  = jvms()->stk_size();


  // Mismatch between method and jvms can occur since map briefly held
  // an OSR entry state (which takes up one RawPtr word).
  assert(max_locals == method()->max_locals(), "sanity");
  assert(max_stack  >= method()->max_stack(),  "sanity");
  assert((int)jvms()->endoff() == TypeFunc::Parms + max_locals + max_stack, "sanity");
  assert((int)jvms()->endoff() == (int)map()->req(), "sanity");

  // Find the start block.
  Block* osr_block = start_block();
  assert(osr_block->start() == osr_bci(), "sanity");

  // Set initial BCI.
  set_parse_bci(osr_block->start());

  // Set initial stack depth.
  set_sp(osr_block->start_sp());

  // Check bailouts.  We currently do not perform on stack replacement
  // of loops in catch blocks or loops which branch with a non-empty stack.
  if (sp() != 0) {
    C->record_method_not_compilable("OSR starts with non-empty stack");
    return;
  }
  // Do not OSR inside finally clauses:
  if (osr_block->has_trap_at(osr_block->start())) {
    C->record_method_not_compilable("OSR starts with an immediate trap");
    return;
  }

  MethodLivenessResult live_locals = method()->liveness_at_bci(osr_bci());
  if (!live_locals.is_valid()) {
    // Degenerate or breakpointed method.
    C->record_method_not_compilable("OSR in empty or breakpointed method");
    return;
  }

  // Extract the needed locals from the interpreter frame.
  Node *locals_addr = basic_plus_adr(osr_buf, osr_buf, (max_locals-1)*wordSize);

  // find all the locals that the interpreter thinks contain live oops
  //const BitMap live_oops = method()->live_local_oops_at_bci(osr_bci());
  for (index = 0; index < max_locals; index++) {

    if (!live_locals.at(index)) {
      continue;
    }

    const Type *type = osr_block->local_type_at(index);

    if (type->isa_oopptr() != NULL) {

      // 6403625: Verify that the interpreter oopMap thinks that the oop is live
      // else we might load a stale oop if the MethodLiveness disagrees with the
      // result of the interpreter. If the interpreter says it is dead we agree
      // by making the value go to top.
      //
      // CNC - With tagged interpreter stack, oops are kept live in the
      // interpreter until stomped over or the frame exits.  Hence the value
      // will exist to be checked, even if the (now deleted) interpreter oop
      // map generator would have flagged it dead.  We do not need this
      // check here.
      //if (!live_oops.at(index)) {
      //  if (C->log() != NULL) {
      //    C->log()->elem("OSR_mismatch local_index='%d'",index);
      //  }
      //  set_local(index, null());
      //  // and ignore it for the loads
      //  continue;
      //}
    }

    // Filter out TOP, HALF, and BOTTOM.  (Cf. ensure_phi.)
    if (type == Type::TOP || type == Type::HALF) {
      continue;
    }
    // If the type falls to bottom, then this must be a local that
    // is mixing ints and oops or some such.  Forcing it to top
    // makes it go dead.
    if (type == Type::BOTTOM) {
      continue;
    }
    // Construct code to access the appropriate local.
    Node *value = fetch_interpreter_state(index, type->basic_type(), locals_addr, osr_buf);
    set_local(index, value);
  }

  // Extract the needed stack entries from the interpreter frame.
for(int index=0;index<sp();index++){
    const Type *type = osr_block->stack_type_at(index);
    if (type != Type::TOP) {
      // Currently the compiler bails out when attempting to on stack replace
      // at a bci with a non-empty stack.  We should not reach here.
      ShouldNotReachHere();
    }
  }

  // End the OSR migration
make_runtime_call(RC_LEAF,false/*!must_callruntimenode*/,
OptoRuntime::osr_end_Type(),
                    CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_end),
                    "OSR_migration_end", TypeRawPtr::BOTTOM,
                    osr_buf);

  // Now that the interpreter state is loaded, make sure it will match
  // at execution time what the compiler is expecting now:
  SafePointNode* bad_type_exit = clone_map();
  bad_type_exit->set_control(new (C, 1) RegionNode(1));

for(int index=0;index<max_locals;index++){
    if (stopped())  break;
    Node* l = local(index);
    if (l->is_top())  continue;  // nothing here
    const Type *type = osr_block->local_type_at(index);
    if (type->isa_oopptr() != NULL) {
      // CNC - Not needed on Azul with our tagged interpreter stack
      //if (!live_oops.at(index)) {
      //  // skip type check for dead oops
      //  continue;
      //}
    }
    set_local(index, check_interpreter_type(l, type, bad_type_exit));
  }

for(int index=0;index<sp();index++){
    if (stopped())  break;
    Node* l = stack(index);
    if (l->is_top())  continue;  // nothing here
    const Type *type = osr_block->stack_type_at(index);
    set_stack(index, check_interpreter_type(l, type, bad_type_exit));
  }

  if (bad_type_exit->control()->req() > 1) {
    // Build an uncommon trap here, if any inputs can be unexpected.
    bad_type_exit->set_control(_gvn.transform( bad_type_exit->control() ));
    record_for_igvn(bad_type_exit->control());
    SafePointNode* types_are_good = map();
    set_map(bad_type_exit);
    // The unexpected type happens because a new edge is active
    // in the CFG, which typeflow had previously ignored.
    // E.g., Object x = coldAtFirst() && notReached()? "str": new Integer(123).
    // This x will be typed as Integer if notReached is not yet linked.
    uncommon_trap(Deoptimization::Reason_unreached, NULL, "osr finds unexpected type", false);
    set_map(types_are_good);
  }
}

//------------------------------cpdata-----------------------------------------
CPData *Parse::cpdata( ) const {
  // Get the offset into the CodeProfile array for this method+bci.
  // Need to add the inline-offset - the offset into the whole CodeProfile
  // that represents this inlined method's data.
  assert0( bci() >= InvocationEntryBci && bci() < method()->code_size() );
  int cpdoff = method()->bci2cpd_map()->bci_to_cpdoff(bci());
  return (cpdoff != BCI2CPD_mapping::unprofiled_bytecode_offset)
    ? c1_cp()->cpdoff_to_cpd(_c1_cp_inloff, cpdoff)
    : NULL;                     // This bytecode has records no info
}

CPData *Parse::c2cpdata( ) const {
  assert0( bci() >= -1 && bci() < method()->code_size() );
  int cpdoff = method()->bci2cpd_map()->bci_to_cpdoff(bci());
  // Return NULL if the inloff is non-zero since c2 doesn't keep inlined
  // sections of the cpdata structure
  if (cpdoff != BCI2CPD_mapping::unprofiled_bytecode_offset && depth() == 1) {
    return C->c2_cp()->cpdoff_to_cpd(_c1_cp_inloff, cpdoff);
  } else {
    return NULL;
  }
}


//------------------------------Parse------------------------------------------
// Main parser constructor.
Parse::Parse(JVMState* caller, ciMethod* parse_method, CodeProfile *c1_cp, int c1_cp_inloff, bool matches_c1_inlining, float expected_uses, CPData_Invoke *cpdi)
  : _exits(caller), _c1_cp(c1_cp), _c1_cp_inloff(c1_cp_inloff), _matches_c1_inlining(matches_c1_inlining)
{
  // Init some variables
  _caller = caller;
  _method = parse_method;
  _expected_uses = expected_uses;
  _depth = 1 + (caller->has_method() ? caller->depth() : 0);
  _wrote_final = false;
  _entry_bci = InvocationEntryBci;
  _tf = NULL;
  _block = NULL;
  debug_only(_block_count = -1);
  debug_only(_blocks = (Block*)-1);
  if (PrintCompilation || PrintOpto) {
    // Make sure I have an inline tree, so I can print messages about it.
    JVMState* ilt_caller = is_osr_parse() ? caller->caller() : caller;
    InlineTree::find_subtree_from_root(C->ilt(), ilt_caller, parse_method, true);
  }

  _tf = TypeFunc::make(method());
  _iter.reset_to_method(method());
  _flow = method()->get_flow_analysis();
  if (_flow->failing()) {
    C->record_method_not_compilable_all_tiers(_flow->failure_reason());
  }

  // If we are an inlined function that was also inlined in C1, then we use
  // the C1-inlined counts directly.  Otherwise we're looking at a new
  // CodeProfile and we grab the top-level invoke counts.
  if (_expected_uses <= 0) {
    _prof_factor = 1;
  } else {
    int invoke_count = UseC1 
      ? (c1_cp_inloff ? cpdi->site_count() : parse_method->get_codeprofile_count(CodeProfile::_invoke))
      : parse_method->invocation_count();
    if( invoke_count == 0 ) invoke_count = 1;
    float prof_total = invoke_count;
    if (prof_total <= _expected_uses) {
      _prof_factor = 1;
    } else {
      _prof_factor = _expected_uses / prof_total;
    }
  }

  assert(method()->has_balanced_monitors(), "Can not parse unbalanced monitors, cutout earlier");

  // Always register dependence if JVMTI is enabled, because
  // either breakpoint setting or hotswapping of methods may
  // cause deoptimization.

  // However, since methods containing breakpoints never get inlined, we
  // should not add the dependency here otherwise it will cause spurious
  // dependency violation in ciEnv::check_for_system_dictionary_modification().
  if (JvmtiExport::can_hotswap_or_post_breakpoint()) {
    // We are here to add a note that if the bytecodes for this method were
    // changed under our feet the method being compiled now should be recompiled.
    // For now we don't support hot swapping of bytecode.
    //Unimplemented();
    //C->dependencies()->assert_evol_method(method());
  }

  methods_seen++;

  // Do some special top-level things.
  if (depth() == 1 && C->is_osr_compilation()) {
    _entry_bci = C->entry_bci();
    _flow = method()->get_osr_flow_analysis(osr_bci());
    if (_flow->failing()) {
      C->record_method_not_compilable(_flow->failure_reason());
    }
    _tf = C->tf();     // the OSR entry type is different
  }

#ifdef ASSERT
  if (depth() == 1) {
    assert(C->is_osr_compilation() == this->is_osr_parse(), "OSR in sync");
    if (C->tf() != tf()) {
MutexLocker ml(Compile_lock);
      assert(C->env()->system_dictionary_modification_counter_changed(), 
             "Must invalidate if TypeFuncs differ");
    }
  } else {
    assert(!this->is_osr_parse(), "no recursive OSR");
  }
#endif

  methods_parsed++;
#ifndef PRODUCT
  // add method size here to guarantee that inlined methods are added too
  if (TimeCompiler) 
    _total_bytes_compiled += method()->code_size();

  show_parse_info();
#endif

  if (failing()) return;

  gvn().set_type(root(), root()->bottom_type());
  gvn().transform(top());

  // Import the results of the ciTypeFlow.
  init_blocks();

  // Merge point for all normal exits
  build_exits();

  // Setup the initial JVM state map.
SafePointNode*entry_map=create_entry_map(cpdi);

  // Check for bailouts during map initialization
if(failing()||entry_map==NULL)return;

  Node_Notes* caller_nn = C->default_node_notes();
  // Collect debug info for inlined calls unless -XX:-DebugInlinedCalls.
if(false||depth()==1){
    C->set_default_node_notes(make_node_notes(caller_nn));
  }

  if (is_osr_parse()) {
    Node* osr_buf = entry_map->in(TypeFunc::Parms+0);
    entry_map->set_req(TypeFunc::Parms+0, top());
    set_map(entry_map);
    load_interpreter_state(osr_buf);
  } else {
    set_map(entry_map);
do_method_entry(cpdi);
    // Top-level entry points need a Safepoint to break tight recursive cycles.
    if( depth() == 1 && !stopped() ) {
      set_bci(InvocationEntryBci);
      add_safepoint(); // Initial safepoint on method entry; it's cheap and breaks recursive loops
    }
  }

  // Check for bailouts during method entry.
  if (failing()) {
    C->set_default_node_notes(caller_nn);
    return;
  }

  entry_map = map();  // capture any changes performed by method setup code
  assert(jvms()->endoff() == map()->req(), "map matches JVMS layout");

  // We begin parsing as if we have just encountered a jump to the
  // method entry.
  Block* entry_block = start_block();
  assert(entry_block->start() == (is_osr_parse() ? osr_bci() : 0), "");
  set_map_clone(entry_map);
  merge_common(entry_block, entry_block->next_path_num());

#ifndef PRODUCT
  BytecodeParseHistogram *parse_histogram_obj = new (C->env()->arena()) BytecodeParseHistogram(this, C);
  set_parse_histogram( parse_histogram_obj );
#endif

  // Parse all the basic blocks.
  do_all_blocks();

  C->set_default_node_notes(caller_nn);

  // Check for bailouts during conversion to graph
  if (failing()) return;

  // Fix up all exiting control flow.
  set_map(entry_map);
  do_exits();

  // Collect a few more statistics.
  parse_idx += C->unique(); 
  parse_arena += C->node_arena()->used(); 
}

//---------------------------do_all_blocks-------------------------------------
void Parse::do_all_blocks() {
  _blocks_merged = 0;
  _blocks_parsed = 0;

  int old_blocks_merged = -1;
  int old_blocks_parsed = -1;

  for (int tries = 0; ; tries++) {
    visit_blocks();
    if (failing())  return; // Check for bailout

    // No need for a work list.  The outer loop is hardly ever repeated.
    // The following loop traverses the blocks in a reasonable pre-order,
    // as produced by the ciTypeFlow pass.

    // This loop can be taken more than once if there are two entries to
    // a loop (irreduceable CFG), and the edge which ciTypeFlow chose
    // as the first predecessor to the loop goes dead in the parser,
    // due to parse-time optimization.  (Could happen with obfuscated code.)

    // Look for progress, or the lack of it:
    if (_blocks_parsed == block_count()) {
      // That's all, folks.
      if (TraceOptoParse) {
C2OUT->print_cr("All blocks parsed.");
      }
      break;
    }

    // How much work was done this time around?
    int new_blocks_merged = _blocks_merged - old_blocks_merged;
    int new_blocks_parsed = _blocks_parsed - old_blocks_parsed;
    if (new_blocks_merged == 0) {
      if (TraceOptoParse) {
C2OUT->print_cr("All live blocks parsed; %d dead blocks.",block_count()-_blocks_parsed);
      }
      // No new blocks have become parseable.  Some blocks are just dead.
      break;
    }
    assert(new_blocks_parsed > 0, "must make progress");
    assert(tries < block_count(), "the pre-order cannot be this bad!");

    old_blocks_merged = _blocks_merged;
    old_blocks_parsed = _blocks_parsed;
  }

#ifndef PRODUCT
  // Make sure there are no half-processed blocks remaining.
  // Every remaining unprocessed block is dead and may be ignored now.
  for (int po = 0; po < block_count(); po++) {
    Block* block = pre_order_at(po);
    if (!block->is_parsed()) {
      if (TraceOptoParse) {
C2OUT->print("Skipped dead block %d at bci:%d",po,block->start());
        assert(!block->is_merged(), "no half-processed blocks");
      }
    }
  }
#endif
}

//---------------------------visit_blocks--------------------------------------
void Parse::visit_blocks() {
  // Walk over all blocks, parsing every one that has been reached (merged).
  for (int po = 0; po < block_count(); po++) {
    Block* block = pre_order_at(po);

    if (block->is_parsed()) {
      // Do not parse twice.
      continue;
    }

    if (!block->is_merged()) {
      // No state on this block.  It had not yet been reached.
      // Delay reaching it until later.
      continue;
    }

    // Prepare to parse this block.
    load_state_from(block);

    if (stopped()) {
      // Block is dead.
      continue;
    }

    if (!block->is_ready() || block->is_handler()) {
      // Not all preds have been parsed.  We must build phis everywhere.
      // (Note that dead locals do not get phis built, ever.)
      ensure_phis_everywhere();

      // Leave behind an undisturbed copy of the map, for future merges.
      set_map(clone_map());
    }

    // Ready or not, parse the block.
    do_one_block();

    // Check for bailouts.
    if (failing())  return;
  }
}

//-------------------------------build_exits----------------------------------
// Build normal and exceptional exit merge points.
void Parse::build_exits() {
  // make a clone of caller to prevent sharing of side-effects
  _exits.set_map(_exits.clone_map());
  _exits.clean_stack(_exits.sp());
  _exits.sync_jvms();

  RegionNode* region = new (C, 1) RegionNode(1);
  record_for_igvn(region);
  gvn().set_type_bottom(region);
  _exits.set_control(region);

  // Note:  iophi and memphi are not transformed until do_exits.
  Node* iophi  = new (C, region->req()) PhiNode(region, Type::ABIO);
  Node* memphi = new (C, region->req()) PhiNode(region, Type::MEMORY, TypePtr::BOTTOM);
  _exits.set_i_o(iophi);
  _exits.set_all_memory(memphi);

  // Add a return value to the exit state.  (Do not push it yet.)
  if (tf()->range()->cnt() > TypeFunc::Parms) {
    const Type* ret_type = tf()->range()->field_at(TypeFunc::Parms);
    // Don't "bind" an unloaded return klass to the ret_phi. If the klass
    // becomes loaded during the subsequent parsing, the loaded and unloaded 
    // types will not join when we transform and push in do_exits().
    const TypeOopPtr* ret_oop_type = ret_type->isa_oopptr();
    if (ret_oop_type && !ret_oop_type->klass()->is_loaded()) {
      ret_type = TypeInstPtr::OBJECT;
    }
    int         ret_size = type2size[ret_type->basic_type()];
    Node*       ret_phi  = new (C, region->req()) PhiNode(region, ret_type);
    _exits.ensure_stack(ret_size);
    assert((int)(tf()->range()->cnt() - TypeFunc::Parms) == ret_size, "good tf range");
    assert(method()->return_type()->size() == ret_size, "tf agrees w/ method");
    _exits.set_argument(0, ret_phi);  // here is where the parser finds it
    // Note:  ret_phi is not yet pushed, until do_exits.
  }
}


//----------------------------build_start_state-------------------------------
// Construct a state which contains only the incoming arguments from an
// unknown caller.  The method & bci will be NULL & InvocationEntryBci.
JVMState* Compile::build_start_state(StartNode* start, const TypeFunc* tf) {
  int        arg_size = tf->domain()->cnt();
  int        max_size = MAX2(arg_size, (int)tf->range()->cnt());
  JVMState*  jvms     = new (this) JVMState(max_size - TypeFunc::Parms);
SafePointNode*map=new(this,max_size)SafePointNode(max_size,NULL,NULL,NULL);
  record_for_igvn(map);
  assert(arg_size == TypeFunc::Parms + (is_osr_compilation() ? 1 : method()->arg_size()), "correct arg_size");
  Node_Notes* old_nn = default_node_notes();
  if (old_nn != NULL && has_method()) {
    Node_Notes* entry_nn = old_nn->clone(this);
    JVMState* entry_jvms = new(this) JVMState(method(), old_nn->jvms());
    entry_jvms->set_offsets(0);
    entry_jvms->set_bci(entry_bci());
    entry_nn->set_jvms(entry_jvms);
    set_default_node_notes(entry_nn);
  }
  uint i;
  for (i = 0; i < (uint)arg_size; i++) {
    Node* parm = initial_gvn()->transform(new (this, 1) ParmNode(start, i));
    map->init_req(i, parm);
    // Record all these guys for later GVN.
    record_for_igvn(parm);
  }
  for (; i < map->req(); i++) {
    map->init_req(i, top());
  }
  assert(jvms->argoff() == TypeFunc::Parms, "parser gets arguments here");
  set_default_node_notes(old_nn);
map->set_jvms(jvms,NULL);
  jvms->set_map(map);
  return jvms;
}

//-----------------------------make_node_notes---------------------------------
Node_Notes* Parse::make_node_notes(Node_Notes* caller_nn) {
  if (caller_nn == NULL)  return NULL;
  Node_Notes* nn = caller_nn->clone(C);
  JVMState* caller_jvms = nn->jvms();
  JVMState* jvms = new (C) JVMState(method(), caller_jvms);
  jvms->set_offsets(0);
  jvms->set_bci(_entry_bci);
  nn->set_jvms(jvms);
  return nn;
}


//--------------------------return_values--------------------------------------
void Compile::return_values(JVMState* jvms) {
  GraphKit kit(jvms);
  Node* ret = new (this, TypeFunc::Parms) ReturnNode(TypeFunc::Parms, 
                             kit.control(),
                             kit.i_o(),
                             kit.reset_memory(),
                             kit.frameptr(),
                             kit.returnadr());
  // Add zero or 1 return values
  int ret_size = tf()->range()->cnt() - TypeFunc::Parms;
  if (ret_size > 0) {
    kit.inc_sp(-ret_size);  // pop the return value(s)
    kit.sync_jvms();
    ret->add_req(kit.argument(0));
    // Note:  The second dummy edge is not needed by a ReturnNode.
  }
  // bind it to root
  root()->add_req(ret);
  record_for_igvn(ret);
  initial_gvn()->transform_no_reclaim(ret);
}

//------------------------rethrow_exceptions-----------------------------------
// Bind all exception states in the list into a single RethrowNode.
void Compile::rethrow_exceptions(JVMState* jvms) {
  GraphKit kit(jvms);
  if (!kit.has_exceptions())  return;  // nothing to generate
  // Load my combined exception state into the kit, with all phis transformed:
  SafePointNode* ex_map = kit.combine_and_pop_all_exception_states();
  Node* ex_oop = kit.use_exception_state(ex_map);

  // Store the exception oop back into pending_exception
  Node *thread = kit.gvn().transform( new (this, 1) ThreadLocalNode() );
  Node *ex_adr = kit.basic_plus_adr(top(), thread, in_bytes(JavaThread::pending_exception_offset()));
const TypePtr*adr_type=ex_adr->bottom_type()->is_ptr();
int pending_ex_alias_idx=get_alias_index(adr_type);
Node*mem=kit.memory(pending_ex_alias_idx);
  Node *st = kit.gvn().transform( StoreNode::make( this, kit.control(), mem, ex_adr, adr_type, ex_oop, T_OBJECT ) );
record_for_igvn(st);
kit.set_memory(st,pending_ex_alias_idx);

RethrowNode*exit=new(this,TypeFunc::Parms+0)RethrowNode(kit.control(),
                                      kit.i_o(), kit.reset_memory(),
kit.frameptr(),kit.returnadr());
  // bind to root
  root()->add_req(exit);
  record_for_igvn(exit);
  initial_gvn()->transform_no_reclaim(exit);
}

bool Parse::can_rerun_bytecode() {
  switch (bc()) {
  case Bytecodes::_ldc:
  case Bytecodes::_ldc_w:
  case Bytecodes::_ldc2_w:
  case Bytecodes::_getfield:
  case Bytecodes::_putfield:
  case Bytecodes::_getstatic:
  case Bytecodes::_putstatic:
  case Bytecodes::_arraylength:
  case Bytecodes::_baload:
  case Bytecodes::_caload:
  case Bytecodes::_iaload:
  case Bytecodes::_saload:
  case Bytecodes::_faload:
  case Bytecodes::_aaload:
  case Bytecodes::_laload:
  case Bytecodes::_daload:
  case Bytecodes::_bastore:
  case Bytecodes::_castore:
  case Bytecodes::_iastore:
  case Bytecodes::_sastore:
  case Bytecodes::_fastore:
  case Bytecodes::_aastore:
  case Bytecodes::_lastore:
  case Bytecodes::_dastore:
  case Bytecodes::_irem:
  case Bytecodes::_idiv:
  case Bytecodes::_lrem:
  case Bytecodes::_ldiv:
  case Bytecodes::_frem:
  case Bytecodes::_fdiv:
  case Bytecodes::_drem:
  case Bytecodes::_ddiv:
  case Bytecodes::_checkcast:
  case Bytecodes::_instanceof:
  case Bytecodes::_athrow:
  case Bytecodes::_anewarray: 
  case Bytecodes::_newarray:
  case Bytecodes::_multianewarray:
  case Bytecodes::_new:
  case Bytecodes::_monitorenter:  // can re-run initial null check, only
  case Bytecodes::_return:
    return true;
    break;

  case Bytecodes::_invokestatic:
  case Bytecodes::_invokespecial:
  case Bytecodes::_invokevirtual: 
  case Bytecodes::_invokeinterface:
    return false;
    break;

  default:
    assert(false, "unexpected bytecode produced an exception");
    return true;
  }
}

//---------------------------do_exceptions-------------------------------------
// Process exceptions arising from the current bytecode.
// Send caught exceptions to the proper handler within this method.
// Unhandled exceptions feed into _exit.
void Parse::do_exceptions() {
  if (!has_exceptions())  return;

  if (failing()) {
    // Pop them all off and throw them away.
    while (pop_exception_state() != NULL) ;
    return;
  }

  // Make sure we can classify this bytecode if we need to.
  debug_only(can_rerun_bytecode());

  PreserveJVMState pjvms(this, false);

  SafePointNode* ex_map;
  while ((ex_map = pop_exception_state()) != NULL) {
    if (!method()->has_exception_handlers()) {
      // Common case:  Transfer control outward.
      // Doing it this early allows the exceptions to common up
      // even between adjacent method calls.
      throw_to_exit(ex_map);
    } else {
      // Have to look at the exception first.
      assert(stopped(), "catch_inline_exceptions trashes the map");
      catch_inline_exceptions(ex_map);
      stop_and_kill_map();      // we used up this exception state; kill it
    }
  }

  // We now return to our regularly scheduled program:
}

//---------------------------throw_to_exit-------------------------------------
// Merge the given map into an exception exit from this method.
// The exception exit will handle any unlocking of receiver.
// The ex_oop must be saved within the ex_map, unlike merge_exception.
void Parse::throw_to_exit(SafePointNode* ex_map) {
  // Pop the JVMS to (a copy of) the caller.
  GraphKit caller;
  caller.set_map_clone(_caller->map());
  caller.set_bci(_caller->bci());
  caller.set_sp(_caller->sp());
  // Copy out the standard machine state:
  for (uint i = 0; i < TypeFunc::Parms; i++) {
    caller.map()->set_req(i, ex_map->in(i));
  }
  // ...and the exception:
  Node*          ex_oop        = saved_ex_oop(ex_map);
  SafePointNode* caller_ex_map = caller.make_exception_state(ex_oop);
  // Finally, collect the new exception state in my exits:
  _exits.add_exception_state(caller_ex_map);
}

//------------------------------do_exits---------------------------------------
void Parse::do_exits() {
  set_parse_bci(InvocationEntryBci);

  // Now peephole on the return bits
  Node* region = _exits.control();
  _exits.set_control(gvn().transform(region));

  Node* iophi = _exits.i_o();
  _exits.set_i_o(gvn().transform(iophi));

  for (MergeMemStream mms(_exits.merged_memory()); mms.next_non_empty(); ) {
    // transform each slice of the original memphi:
    mms.set_memory(_gvn.transform(mms.memory()));
  }

  if (wrote_final()) {
    // This method (which must be a constructor by the rules of Java)
    // wrote a final.  The effects of all initializations must be
    // committed to memory before any code after the constructor
    // publishes the reference to the newly constructor object.
    // Rather than wait for the publication, we simply block the
    // writes here.  Rather than put a barrier on only those writes
    // which are required to complete, we force all writes to complete.
    //
    // "All bets are off" unless the first publication occurs after a
    // normal return from the constructor.  We do not attempt to detect
    // such unusual early publications.  But no barrier is needed on
    // exceptional returns, since they cannot publish normally.
    //
    _exits.insert_mem_bar(Op_MemBarRelease);
  }

  if (tf()->range()->cnt() > TypeFunc::Parms) {
    const Type* ret_type = tf()->range()->field_at(TypeFunc::Parms);
    Node*       ret_phi  = _gvn.transform( _exits.argument(0) );
    assert(_exits.control()->is_top() || !_gvn.type(ret_phi)->empty(), "return value must be well defined");
    _exits.push_node(ret_type->basic_type(), ret_phi);
  }

  // Note:  Logic for creating and optimizing the ReturnNode is in Compile.

  // Unlock along the exceptional paths.
  // This is done late so that we can common up equivalent exceptions
  // (e.g., null checks) arising from multiple points within this method.
  // See GraphKit::add_exception_state, which performs the commoning.
bool do_synch=method()->is_synchronized();

if(do_synch){
    // First move the exception list out of _exits:
    GraphKit kit(_exits.transfer_exceptions_into_jvms());
    SafePointNode* normal_map = kit.map();  // keep this guy safe 
    // Now re-collect the exceptions into _exits:
    SafePointNode* ex_map;
    while ((ex_map = kit.pop_exception_state()) != NULL) {
      Node* ex_oop = kit.use_exception_state(ex_map);
      // Force the exiting JVM state to have this method at InvocationEntryBci.
      // The exiting JVM state is otherwise a copy of the calling JVMS.
      JVMState* caller = kit.jvms();
      JVMState* ex_jvms = caller->clone_shallow(C);
      ex_jvms->set_map(kit.clone_map());
ex_jvms->map()->set_jvms(ex_jvms,NULL);
      ex_jvms->set_bci(   InvocationEntryBci);
      kit.set_jvms(ex_jvms);
      if (do_synch) {
        // Add on the synchronized-method box/object combo
kit.map()->push_monitor(_synch_obj);
        // Unlock!
kit.shared_unlock(_synch_obj);
      }
      // Done with exception-path processing.
      ex_map = kit.make_exception_state(ex_oop);
      assert(ex_jvms->same_calls_as(ex_map->jvms()), "sanity");
      // Pop the last vestige of this method:
ex_map->set_jvms(caller->clone_shallow(C),NULL);
      ex_map->jvms()->set_map(ex_map);
      _exits.push_exception_state(ex_map);
    }
    assert(_exits.map() == normal_map, "keep the same return state");
  }

  {
    // Capture very early exceptions (receiver null checks) from caller JVMS
    GraphKit caller(_caller);
    SafePointNode* ex_map;
    while ((ex_map = caller.pop_exception_state()) != NULL) {
      _exits.add_exception_state(ex_map);
    }
  }
}

//-----------------------------create_entry_map-------------------------------
// Initialize our parser map to contain the types at method entry.
// For OSR, the map contains a single RawPtr parameter.
// Initial monitor locking for sync. methods is performed by do_method_entry.
SafePointNode* Parse::create_entry_map(CPData_Invoke *cpdi) {
  // Check for really stupid bail-out cases.  
  uint len = TypeFunc::Parms + method()->max_locals() + method()->max_stack();
  if (len >= 32760) { 
    C->record_method_not_compilable_all_tiers("too many local variables");
    return NULL;
  }

  // If this is an inlined method, we may have to do a receiver null check.
  if (_caller->has_method() && is_normal_parse() && !method()->is_static()) {
    GraphKit kit(_caller);
kit.null_check_receiver(method(),cpdi);
    _caller = kit.transfer_exceptions_into_jvms();
    if (kit.stopped()) {
      _exits.add_exception_states_from(_caller);
      _exits.set_jvms(_caller);
      return NULL;
    }
  }

  assert(method() != NULL, "parser must have a method");

  // Create an initial safepoint to hold JVM state during parsing
  JVMState* jvms = new (C) JVMState(method(), _caller->has_method() ? _caller : NULL);
set_map(new(C,len)SafePointNode(len,jvms,NULL,cpdi));
  jvms->set_map(map());
  record_for_igvn(map());
  assert(jvms->endoff() == len, "correct jvms sizing");

  SafePointNode* inmap = _caller->map();
  assert(inmap != NULL, "must have inmap");

  uint i;

  // Pass thru the predefined input parameters.
  for (i = 0; i < TypeFunc::Parms; i++) {
    map()->init_req(i, inmap->in(i));
  }

  if (depth() == 1) {
    assert(map()->memory()->Opcode() == Op_Parm, "");
    // Insert the memory aliasing node
    set_all_memory(reset_memory());
  }
  assert(merged_memory(), "");

  // Now add the locals which are initially bound to arguments:
  uint arg_size = tf()->domain()->cnt();
  ensure_stack(arg_size - TypeFunc::Parms);  // OSR methods have funny args
  for (i = TypeFunc::Parms; i < arg_size; i++) {
    map()->init_req(i, inmap->argument(_caller, i - TypeFunc::Parms));
  }

  // Clear out the rest of the map (locals and stack)
  for (i = arg_size; i < len; i++) {
    map()->init_req(i, top());
  }

  SafePointNode* entry_map = stop();
  return entry_map;
}

//-----------------------------do_method_entry--------------------------------
// Emit any code needed in the pseudo-block before BCI zero.
// The main thing to do is lock the receiver of a synchronized method.
void Parse::do_method_entry( CPData_Invoke *caller_cpdi ) {
  set_parse_bci(InvocationEntryBci); // Pseudo-BCP
  set_sp(0);                      // Java Stack Pointer

  // If the method is synchronized, we need to construct a lock node, attach
  // it to the Start node, and pin it there.  
  if (method()->is_synchronized()) {
    // Insert a LockNode right after the Start which takes as arguments
    // the current thread pointer, the "this" pointer & the address of the
    // stack slot pair used for the lock.  The "this" pointer is a projection
    // off the start node, but the locking spot has to be constructed by
    // creating a ConLNode of 0, and boxing it with a BoxLockNode.  The BoxLockNode
    // becomes the second argument to the LockNode call.  The
    // LockNode becomes the new control parent to pin it to the start.

    // Setup Object Pointer
    Node *lock_obj = NULL;
    if(method()->is_static()) {
      ciInstance* mirror = _method->holder()->java_mirror();
      const TypeInstPtr *t_lock = TypeInstPtr::make(mirror);
      lock_obj = makecon(t_lock);
    } else {                  // Else pass the "this" pointer, 
      lock_obj = local(0);    // which is Parm0 from StartNode
    }
    // Clear out dead values from the debug info.
    kill_dead_locals();
    // Build the LockNode
    _synch_obj = lock_obj;
    shared_lock(lock_obj, caller_cpdi);
  }
}

//------------------------------init_blocks------------------------------------
// Initialize our parser map to contain the types/monitors at method entry.
void Parse::init_blocks() {
  // Create the blocks.
  _block_count = flow()->block_count();
  _blocks = NEW_RESOURCE_ARRAY(Block, _block_count);
  Copy::zero_to_bytes(_blocks, sizeof(Block)*_block_count);

  int po;

  // Initialize the structs.
  for (po = 0; po < block_count(); po++) {
    Block* block = pre_order_at(po);
    block->init_node(this, po);
  }

  // Collect predecessor and successor information.
  for (po = 0; po < block_count(); po++) {
    Block* block = pre_order_at(po);
    block->init_graph(this);
  }
}

//-------------------------------init_node-------------------------------------
void Parse::Block::init_node(Parse* outer, int po) {
  _flow = outer->flow()->pre_order_at(po);
  _pred_count = 0;
  _preds_parsed = 0;
  assert(pred_count() == 0 && preds_parsed() == 0, "sanity");
  assert(!(is_merged() || is_parsed() || is_handler()), "sanity");
  assert(_live_locals.size() == 0, "sanity");

  // entry point has additional predecessor
  if (flow()->is_start())  _pred_count++;
  assert(flow()->is_start() == (this == outer->start_block()), "");
}

//-------------------------------init_graph------------------------------------
void Parse::Block::init_graph(Parse* outer) {
  // Create the successor list for this parser block.
  GrowableArray<ciTypeFlow::Block*>* tfs = flow()->successors();
  GrowableArray<ciTypeFlow::Block*>* tfe = flow()->exceptions();
  int ns = tfs->length();
  int ne = tfe->length();
  _num_successors = ns;
  _all_successors = ns+ne;
  _successors = (ns+ne == 0) ? NULL : NEW_RESOURCE_ARRAY(Block*, ns+ne);
  int p = 0;
  for (int i = 0; i < ns+ne; i++) {
    ciTypeFlow::Block* tf2 = (i < ns) ? tfs->at(i) : tfe->at(i-ns);
    Block* block2 = outer->pre_order_at(tf2->pre_order());
    _successors[i] = block2;

    // Accumulate pred info for the other block, too.
    if (i < ns) {
      block2->_pred_count++;
    } else {
      block2->_is_handler = true;
    }

#ifdef ASSERT
    // A block's successors must be distinguishable by BCI.
    // That is, no bytecode is allowed to branch to two different
    // clones of the same code location.
    for (int j = 0; j < i; j++) {
      Block* block1 = _successors[j];
      if (block1 == block2)  continue;  // duplicates are OK
      assert(block1->start() != block2->start(), "successors have unique bcis");
    }
#endif
  }

  // Note: We never call next_path_num along exception paths, so they
  // never get processed as "ready".  Also, the input phis of exception
  // handlers get specially processed, so that 
}

//---------------------------successor_for_bci---------------------------------
Parse::Block* Parse::Block::successor_for_bci(int bci) {
  for (int i = 0; i < all_successors(); i++) {
    Block* block2 = successor_at(i);
    if (block2->start() == bci)  return block2;
  }
  // We can actually reach here if ciTypeFlow traps out a block
  // due to an unloaded class, and concurrently with compilation the
  // class is then loaded, so that a later phase of the parser is
  // able to see more of the bytecode CFG.  Or, the flow pass and
  // the parser can have a minor difference of opinion about executability
  // of bytecodes.  For example, "obj.field = null" is executable even
  // if the field's type is an unloaded class; the flow pass used to
  // make a trap for such code.
  return NULL;
}


//-----------------------------stack_type_at-----------------------------------
const Type* Parse::Block::stack_type_at(int i) const {
  return get_type(flow()->stack_type_at(i));
}


//-----------------------------local_type_at-----------------------------------
const Type* Parse::Block::local_type_at(int i) const {
  // Make dead locals fall to bottom.
  if (_live_locals.size() == 0) {
    MethodLivenessResult live_locals = flow()->outer()->method()->liveness_at_bci(start());
    // This bitmap can be zero length if we saw a breakpoint.
    // In such cases, pretend they are all live.
    ((Block*)this)->_live_locals = live_locals;
  }
  if (_live_locals.size() > 0 && !_live_locals.at(i))
    return Type::BOTTOM;

  return get_type(flow()->local_type_at(i));
}


#ifndef PRODUCT

//----------------------------name_for_bc--------------------------------------
// helper method for BytecodeParseHistogram
static const char* name_for_bc(int i) {
  return Bytecodes::is_defined(i) ? Bytecodes::name(Bytecodes::cast(i)) : "xxxunusedxxx";
}

//----------------------------BytecodeParseHistogram------------------------------------
Parse::BytecodeParseHistogram::BytecodeParseHistogram(Parse *p, Compile *c) {
  _parser   = p;
  _compiler = c;
  if( ! _initialized ) { _initialized = true; reset(); }
}

//----------------------------current_count------------------------------------
int Parse::BytecodeParseHistogram::current_count(BPHType bph_type) {
  switch( bph_type ) {
  case BPH_transforms: { return _parser->gvn().made_progress(); }
  case BPH_values:     { return _parser->gvn().made_new_values(); }
  default: { ShouldNotReachHere(); return 0; }
  }
}

//----------------------------initialized--------------------------------------
bool Parse::BytecodeParseHistogram::initialized() { return _initialized; }

//----------------------------reset--------------------------------------------
void Parse::BytecodeParseHistogram::reset() {
  int i = Bytecodes::number_of_codes;
  while (i-- > 0) { _bytecodes_parsed[i] = 0; _nodes_constructed[i] = 0; _nodes_transformed[i] = 0; _new_values[i] = 0; }
}

//----------------------------set_initial_state--------------------------------
// Record info when starting to parse one bytecode
void Parse::BytecodeParseHistogram::set_initial_state( Bytecodes::Code bc ) {
  if( PrintParseStatistics && !_parser->is_osr_parse() ) {
    _initial_bytecode    = bc;
    _initial_node_count  = _compiler->unique();
    _initial_transforms  = current_count(BPH_transforms);
    _initial_values      = current_count(BPH_values);
  }
}

//----------------------------record_change--------------------------------
// Record results of parsing one bytecode
void Parse::BytecodeParseHistogram::record_change() { 
  if( PrintParseStatistics && !_parser->is_osr_parse() ) {
    ++_bytecodes_parsed[_initial_bytecode];
    _nodes_constructed [_initial_bytecode] += (_compiler->unique() - _initial_node_count);
    _nodes_transformed [_initial_bytecode] += (current_count(BPH_transforms) - _initial_transforms);
    _new_values        [_initial_bytecode] += (current_count(BPH_values)     - _initial_values);
  }
}


//----------------------------print--------------------------------------------
void Parse::BytecodeParseHistogram::print(float cutoff) {
  ResourceMark rm;
  // print profile
  int total  = 0;
  int i      = 0;
  for( i = 0; i < Bytecodes::number_of_codes; ++i ) { total += _bytecodes_parsed[i]; }
  int abs_sum = 0;
C2OUT->cr();//0123456789012345678901234567890123456789012345678901234567890123456789
C2OUT->print_cr("Histogram of %d parsed bytecodes:",total);
  if( total == 0 ) { return; }
C2OUT->cr();
C2OUT->print_cr("absolute:  count of compiled bytecodes of this type");
C2OUT->print_cr("relative:  percentage contribution to compiled nodes");
C2OUT->print_cr("nodes   :  Average number of nodes constructed per bytecode");
C2OUT->print_cr("rnodes  :  Significance towards total nodes constructed, (nodes*relative)");
C2OUT->print_cr("transforms: Average amount of tranform progress per bytecode compiled");
C2OUT->print_cr("values  :  Average number of node values improved per bytecode");
C2OUT->print_cr("name    :  Bytecode name");
C2OUT->cr();
C2OUT->print_cr("  absolute  relative   nodes  rnodes  transforms  values   name");
C2OUT->print_cr("----------------------------------------------------------------------");
  while (--i > 0) {
    int       abs = _bytecodes_parsed[i];
    float     rel = abs * 100.0F / total;
    float   nodes = _bytecodes_parsed[i] == 0 ? 0 : (1.0F * _nodes_constructed[i])/_bytecodes_parsed[i];
    float  rnodes = _bytecodes_parsed[i] == 0 ? 0 :  rel * nodes;
    float  xforms = _bytecodes_parsed[i] == 0 ? 0 : (1.0F * _nodes_transformed[i])/_bytecodes_parsed[i];
    float  values = _bytecodes_parsed[i] == 0 ? 0 : (1.0F * _new_values       [i])/_bytecodes_parsed[i];
    if (cutoff <= rel) {
C2OUT->print_cr("%10d  %7.2f%%  %6.1f  %6.2f   %6.1f   %6.1f     %s",abs,rel,nodes,rnodes,xforms,values,name_for_bc(i));
      abs_sum += abs;
    }
  }
C2OUT->print_cr("----------------------------------------------------------------------");
  float rel_sum = abs_sum * 100.0F / total;
C2OUT->print_cr("%10d  %7.2f%%    (cutoff = %.2f%%)",abs_sum,rel_sum,cutoff);
C2OUT->print_cr("----------------------------------------------------------------------");
C2OUT->cr();
}
#endif

//----------------------------load_state_from----------------------------------
// Load block/map/sp.  But not do not touch iter/bci.
void Parse::load_state_from(Block* block) {
  set_block(block);
  // load the block's JVM state:
  set_map(block->start_map());
  set_sp( block->start_sp());
}


//-----------------------------record_state------------------------------------
void Parse::Block::record_state(Parse* p) {
  assert(!is_merged(), "can only record state once, on 1st inflow");
  assert(start_sp() == p->sp(), "stack pointer must agree with ciTypeFlow");
  set_start_map(p->stop());
}


//------------------------------do_one_block-----------------------------------
void Parse::do_one_block() {
  if (TraceOptoParse) {
    Block *b = block();
    int ns = b->num_successors();
    int nt = b->all_successors();

C2OUT->print("Parsing block #%d at bci [%d,%d), successors: ",
                  block()->pre_order(), block()->start(), block()->limit());
    for (int i = 0; i < nt; i++) {
C2OUT->print(((i<ns)?" %d":" %d(e)"),b->successor_at(i)->pre_order());
    }
C2OUT->cr();
  }

  assert(block()->is_merged(), "must be merged before being parsed");
  block()->mark_parsed();
  ++_blocks_parsed;

  // Set iterator to start of block.
  iter().reset_to_bci(block()->start());

  // Parse bytecodes
  while (!stopped() && !failing()) {
    iter().next();

    // Learn the current bci from the iterator:
    set_parse_bci(iter().cur_bci());

    if (bci() == block()->limit()) {
      // Do not walk into the next block until directed by do_all_blocks.
      merge(bci());
      break;
    }
    assert(bci() < block()->limit(), "bci still in block");

    if (block()->has_trap_at(bci())) {
      // We must respect the flow pass's traps, because it will refuse
      // to produce successors for trapping blocks.
      int trap_index = block()->flow()->trap_index();
      assert(trap_index != 0, "trap index must be valid");
      uncommon_trap((Deoptimization::DeoptReason)-trap_index, NULL, "ciTypeFlow traps and ends compilation", false);
      break;
    }

    NOT_PRODUCT( parse_histogram()->set_initial_state(bc()); );

#ifdef ASSERT
    int pre_bc_sp = sp();
    int inputs, depth;
    bool have_se = !stopped() && compute_stack_effects(inputs, depth);
    assert(!have_se || pre_bc_sp >= inputs, "have enough stack to execute this BC");
#endif //ASSERT

    do_one_bytecode();

    assert(!have_se || stopped() || failing() || (sp() - pre_bc_sp) == depth, "correct depth prediction");

    do_exceptions();

    NOT_PRODUCT( parse_histogram()->record_change(); );

    // Fall into next bytecode.  Each bytecode normally has 1 sequential
    // successor which is typically made ready by visiting this bytecode.
    // If the successor has several predecessors, then it is a merge
    // point, starts a new basic block, and is handled like other basic blocks.
  }
}


//------------------------------merge------------------------------------------
void Parse::set_parse_bci(int bci) {
  set_bci(bci);
  Node_Notes* nn = C->default_node_notes();
  if (nn == NULL)  return;

  // Collect debug info for inlined calls unless -XX:-DebugInlinedCalls.
if(!false&&depth()>1){
    return;
  }

  // Update the JVMS annotation, if present.
  JVMState* jvms = nn->jvms();
  if (jvms != NULL && jvms->bci() != bci) {
    // Update the JVMS.
    jvms = jvms->clone_shallow(C);
    jvms->set_bci(bci);
    nn->set_jvms(jvms);
  }
}

//------------------------------merge------------------------------------------
// Merge the current mapping into the basic block starting at bci
void Parse::merge(int target_bci) {
  Block* target = successor_for_bci(target_bci);
  if (target == NULL) { handle_missing_successor(target_bci); return; }
  assert(!target->is_ready(), "our arrival must be expected");
  int pnum = target->next_path_num();
  merge_common(target, pnum);
}

//-------------------------merge_new_path--------------------------------------
// Merge the current mapping into the basic block, using a new path
void Parse::merge_new_path(int target_bci) {
  Block* target = successor_for_bci(target_bci);
  if (target == NULL) { handle_missing_successor(target_bci); return; }
  assert(!target->is_ready(), "new path into frozen graph");
  int pnum = target->add_new_path();
  merge_common(target, pnum);
}

//-------------------------merge_exception-------------------------------------
// Merge the current mapping into the basic block starting at bci
// The ex_oop must be pushed on the stack, unlike throw_to_exit.
void Parse::merge_exception(int target_bci) {
  assert(sp() == 1, "must have only the throw exception on the stack");
  Block* target = successor_for_bci(target_bci);
  if (target == NULL) { handle_missing_successor(target_bci); return; }
  assert(target->is_handler(), "exceptions are handled by special blocks");
  int pnum = target->add_new_path();
  merge_common(target, pnum);
}

//--------------------handle_missing_successor---------------------------------
void Parse::handle_missing_successor(int target_bci) {
#ifndef PRODUCT
  Block* b = block();
  int trap_bci = b->flow()->has_trap()? b->flow()->trap_bci(): -1; 
C2OUT->print_cr("### Missing successor at bci:%d for block #%d (trap_bci:%d)",target_bci,b->pre_order(),trap_bci);
#endif
  ShouldNotReachHere();
}

//--------------------------merge_common---------------------------------------
void Parse::merge_common(Parse::Block* target, int pnum) {
  if (TraceOptoParse) {
C2OUT->print("Merging state at block #%d bci:%d",target->pre_order(),target->start());
  }

  // Zap extra stack slots to top
  assert(sp() == target->start_sp(), "");
  clean_stack(sp());

  if (!target->is_merged()) {   // No prior mapping at this bci
if(TraceOptoParse){C2OUT->print(" with empty state");}

    // If this path is dead, do not bother capturing it as a merge.
    // It is "as if" we had 1 fewer predecessors from the beginning.
    if (stopped()) {
if(TraceOptoParse)C2OUT->print_cr(", but path is dead and doesn't count");
      return;
    }

    // Record that a new block has been merged.
    ++_blocks_merged;

    // Make a region if we know there are multiple or unpredictable inputs.
    // (Also, if this is a plain fall-through, we might see another region,
    // which must not be allowed into this block's map.)
    if (pnum > PhiNode::Input         // Known multiple inputs.
        || target->is_handler()       // These have unpredictable inputs.
        || control()->is_Region()) {  // We must hide this guy.
      // Add a Region to start the new basic block.  Phis will be added
      // later lazily.
      int edges = target->pred_count();
      if (edges < pnum)  edges = pnum;  // might be a new path!
      Node *r = new (C, edges+1) RegionNode(edges+1);
      gvn().set_type(r, Type::CONTROL);
      record_for_igvn(r);
      // zap all inputs to NULL for debugging (done in Node(uint) constructor)
      // for (int j = 1; j < edges+1; j++) { r->init_req(j, NULL); }
      r->init_req(pnum, control());
      set_control(r);
    }

    // Convert the existing Parser mapping into a mapping at this bci.
    store_state_to(target);
    assert(target->is_merged(), "do not come here twice");

  } else {                      // Prior mapping at this bci
if(TraceOptoParse){C2OUT->print(" with previous state");}

    // We must not manufacture more phis if the target is already parsed.
    bool nophi = target->is_parsed();

    SafePointNode* newin = map();// Hang on to incoming mapping
    Block* save_block = block(); // Hang on to incoming block;
    load_state_from(target);    // Get prior mapping

    assert(newin->jvms()->locoff() == jvms()->locoff(), "JVMS layouts agree");
    assert(newin->jvms()->stkoff() == jvms()->stkoff(), "JVMS layouts agree");
    assert(newin->jvms()->monoff() == jvms()->monoff(), "JVMS layouts agree");
    assert(newin->jvms()->endoff() == jvms()->endoff(), "JVMS layouts agree");

    // Iterate over my current mapping and the old mapping.
    // Where different, insert Phi functions.
    // Use any existing Phi functions.
    assert(control()->is_Region(), "must be merging to a region");
    RegionNode* r = control()->as_Region();

    // Compute where to merge into
    // Merge incoming control path
    r->set_req(pnum, newin->control());

    if (pnum == 1) {            // Last merge for this Region?
      _gvn.transform_no_reclaim(r);
      record_for_igvn(r);
    }

    // Update all the non-control inputs to map:
    assert(TypeFunc::Parms == newin->jvms()->locoff(), "parser map should contain only youngest jvms");
    for (uint j = 1; j < newin->req(); j++) {
      Node* m = map()->in(j);   // Current state of target.
      Node* n = newin->in(j);   // Incoming change to target state.
      PhiNode* phi;
      if (m->is_Phi() && m->as_Phi()->region() == r)
        phi = m->as_Phi();
      else
        phi = NULL;
      if (m != n) {             // Different; must merge
        switch (j) {
        // Frame pointer and Return Address never changes
        case TypeFunc::FramePtr:// Drop m, use the original value
        case TypeFunc::ReturnAdr:
          break;
        case TypeFunc::Memory:  // Merge inputs to the MergeMem node
          assert(phi == NULL, "the merge contains phis, not vice versa");
          merge_memory_edges(n->as_MergeMem(), pnum, nophi);
          continue;
        default:                // All normal stuff
          if (phi == NULL)  phi = ensure_phi(j, nophi);
          break;
        }
      }
      // At this point, n might be top if:
      //  - there is no phi (because TypeFlow detected a conflict), or
      //  - the corresponding control edges is top (a dead incoming path)
      // It is a bug if we create a phi which sees a garbage value on a live path.

      if (phi != NULL) {
        assert(n != top() || r->in(pnum) == top(), "live value must not be garbage");
        assert(phi->region() == r, "");
        phi->set_req(pnum, n);  // Then add 'n' to the merge
        if (pnum == PhiNode::Input) {
          // Last merge for this Phi.
          // So far, Phis have had a reasonable type from ciTypeFlow.
          // Now _gvn will join that with the meet of current inputs.
          // BOTTOM is never permissible here, 'cause pessimistically
          // Phis of pointers cannot lose the basic pointer type.
          debug_only(const Type* bt1 = phi->bottom_type());
          assert(bt1 != Type::BOTTOM, "should not be building conflict phis");
          map()->set_req(j, _gvn.transform_no_reclaim(phi));
          debug_only(const Type* bt2 = phi->bottom_type());
          assert(bt2->higher_equal(bt1), "must be consistent with type-flow");
          record_for_igvn(phi);
        }
      }
    } // End of for all values to be merged

    if (pnum == PhiNode::Input &&
        !r->in(0)) {         // The occasional useless Region
      assert(control() == r, "");
      set_control(r->nonnull_req());
    }

    // newin has been subsumed into the lazy merge, and is now dead.
    set_block(save_block);

    stop();                     // done with this guy, for now
  }

  if (TraceOptoParse) {
C2OUT->print_cr(" on path %d",pnum);
  }

  // Done with this parser state.
  assert(stopped(), "");
}


//--------------------------merge_memory_edges---------------------------------
void Parse::merge_memory_edges(MergeMemNode* n, int pnum, bool nophi) {
  // (nophi means we must not create phis, because we already parsed here)
  assert(n != NULL, "");
  // Merge the inputs to the MergeMems
  MergeMemNode* m = merged_memory();

  assert(control()->is_Region(), "must be merging to a region");
  RegionNode* r = control()->as_Region();

  PhiNode* base = NULL;
  MergeMemNode* remerge = NULL;
  for (MergeMemStream mms(m, n); mms.next_non_empty2(); ) {
    Node *p = mms.force_memory();
    Node *q = mms.memory2();
    if (mms.is_empty() && nophi) {
      // Trouble:  No new splits allowed after a loop body is parsed.
      // Instead, wire the new split into a MergeMem on the backedge.
      // The optimizer will sort it out, slicing the phi.
      if (remerge == NULL) {
        assert(base != NULL, "");
        assert(base->in(0) != NULL, "should not be xformed away");
        remerge = MergeMemNode::make(C, base->in(pnum));
        gvn().set_type(remerge, Type::MEMORY);
        base->set_req(pnum, remerge);
      }
      remerge->set_memory_at(mms.alias_idx(), q);
      continue;
    }
    assert(!q->is_MergeMem(), "");
    PhiNode* phi;
    if (p != q) {
      phi = ensure_memory_phi(mms.alias_idx(), nophi);
    } else {
      if (p->is_Phi() && p->as_Phi()->region() == r)
        phi = p->as_Phi();
      else
        phi = NULL;
    }
    // Insert q into local phi
    if (phi != NULL) {
      assert(phi->region() == r, "");
      p = phi;
      phi->set_req(pnum, q);
      if (mms.at_base_memory()) {
        base = phi;  // delay transforming it
      } else if (pnum == 1) {
        record_for_igvn(phi);
        p = _gvn.transform_no_reclaim(phi);
      }
      mms.set_memory(p);// store back through the iterator
    }
  }
  // Transform base last, in case we must fiddle with remerging.
  if (base != NULL && pnum == 1) {
    record_for_igvn(base);
    m->set_base_memory( _gvn.transform_no_reclaim(base) );
  }
}


//------------------------ensure_phis_everywhere-------------------------------
void Parse::ensure_phis_everywhere() {
  ensure_phi(TypeFunc::I_O);

  // Ensure a phi on all currently known memories.
  for (MergeMemStream mms(merged_memory()); mms.next_non_empty(); ) {
    ensure_memory_phi(mms.alias_idx());
    debug_only(mms.set_memory());  // keep the iterator happy
  }

  // Note:  This is our only chance to create phis for memory slices.
  // If we miss a slice that crops up later, it will have to be
  // merged into the base-memory phi that we are building here.
  // Later, the optimizer will comb out the knot, and build separate
  // phi-loops for each memory slice that matters.

  // Monitors must nest nicely and not get confused amongst themselves.
  // Phi-ify everything up to the monitors, though.
  uint monoff = map()->jvms()->monoff();
  uint nof_monitors = map()->jvms()->nof_monitors();

  assert(TypeFunc::Parms == map()->jvms()->locoff(), "parser map should contain only youngest jvms");
  for (uint i = TypeFunc::Parms; i < monoff; i++) {
    ensure_phi(i);
  }
  // Even monitors need Phis, though they are well-structured.
  // This is true for OSR methods, and also for the rare cases where
  // a monitor object is the subject of a replace_in_map operation.
  // See bugs 4426707 and 5043395.
  for (uint m = 0; m < nof_monitors; m++) {
    ensure_phi(map()->jvms()->monitor_obj_offset(m));
  }
}


//-----------------------------add_new_path------------------------------------
// Add a previously unaccounted predecessor to this block.
int Parse::Block::add_new_path() {
  // If there is no map, return the lowest unused path number.
  if (!is_merged())  return pred_count()+1;  // there will be a map shortly

  SafePointNode* map = start_map();
  if (!map->control()->is_Region())
    return pred_count()+1;  // there may be a region some day
  RegionNode* r = map->control()->as_Region();

  // Add new path to the region.
  uint pnum = r->req();
  r->add_req(NULL);

  for (uint i = 1; i < map->req(); i++) {
    Node* n = map->in(i);
    if (i == TypeFunc::Memory) {
      // Ensure a phi on all currently known memories.
      for (MergeMemStream mms(n->as_MergeMem()); mms.next_non_empty(); ) {
        Node* phi = mms.memory();
        if (phi->is_Phi() && phi->as_Phi()->region() == r) {
          assert(phi->req() == pnum, "must be same size as region");
          phi->add_req(NULL);
        }
      }
    } else {
      if (n->is_Phi() && n->as_Phi()->region() == r) {
        assert(n->req() == pnum, "must be same size as region");
        n->add_req(NULL);
      }
    }
  }

  return pnum;
}

//------------------------------ensure_phi-------------------------------------
// Turn the idx'th entry of the current map into a Phi
PhiNode *Parse::ensure_phi(int idx, bool nocreate) {
  SafePointNode* map = this->map();
  Node* region = map->control();
  assert(region->is_Region(), "");

  Node* o = map->in(idx);
  assert(o != NULL, "");

  if (o == top())  return NULL; // TOP always merges into TOP

  if (o->is_Phi() && o->as_Phi()->region() == region) {
    return o->as_Phi();
  }

  // Now use a Phi here for merging
  assert(!nocreate, "Cannot build a phi for a block already parsed.");
  const JVMState* jvms = map->jvms();
const Type*t=NULL;
  if (jvms->is_loc(idx)) {
    t = block()->local_type_at(idx - jvms->locoff());
  } else if (jvms->is_stk(idx)) {
    t = block()->stack_type_at(idx - jvms->stkoff());
  } else if (jvms->is_mon(idx)) {
t=TypeInstPtr::OBJECT;//this is sufficient for a lock object
  } else if ((uint)idx < TypeFunc::Parms) {
    t = o->bottom_type();  // Type::RETURN_ADDRESS or such-like.
  } else {
    assert(false, "no type information for this phi");
  }

  // If the type falls to bottom, then this must be a local that
  // is mixing ints and oops or some such.  Forcing it to top
  // makes it go dead.
  if (t == Type::BOTTOM) {
    map->set_req(idx, top());
    return NULL;
  }
 
  // Do not create phis for top either.
  // A top on a non-null control flow must be an unused even after the.phi.
  if (t == Type::TOP || t == Type::HALF) {
    map->set_req(idx, top());
    return NULL;
  }

  PhiNode* phi = PhiNode::make(region, o, t);
  gvn().set_type(phi, t);
  if (DoEscapeAnalysis) record_for_igvn(phi);
  map->set_req(idx, phi);
  return phi;
}

//--------------------------ensure_memory_phi----------------------------------
// Turn the idx'th slice of the current memory into a Phi
PhiNode *Parse::ensure_memory_phi(int idx, bool nocreate) {
  MergeMemNode* mem = merged_memory();
  Node* region = control();
  assert(region->is_Region(), "");

  Node *o = (idx == Compile::AliasIdxBot)? mem->base_memory(): mem->memory_at(idx);
  assert(o != NULL && o != top(), "");

  PhiNode* phi;
  if (o->is_Phi() && o->as_Phi()->region() == region) {
    phi = o->as_Phi();
    if (phi == mem->base_memory() && idx >= Compile::AliasIdxRaw) {
      // clone the shared base memory phi to make a new memory split
      assert(!nocreate, "Cannot build a phi for a block already parsed.");
      const Type* t = phi->bottom_type();
      const TypePtr* adr_type = C->get_adr_type(idx);
      phi = phi->slice_memory(adr_type);
      gvn().set_type(phi, t);
    }
    return phi;
  }

  // Now use a Phi here for merging
  assert(!nocreate, "Cannot build a phi for a block already parsed.");
  const Type* t = o->bottom_type();
  const TypePtr* adr_type = C->get_adr_type(idx);
  phi = PhiNode::make(region, o, t, adr_type);
  gvn().set_type(phi, t);
  if (idx == Compile::AliasIdxBot)
    mem->set_base_memory(phi);
  else
    mem->set_memory_at(idx, phi);
  return phi;
}

//------------------------------call_register_finalizer-----------------------
// Check the klass of the receiver and call register_finalizer if the
// class need finalization.
void Parse::call_register_finalizer() {
  Node* receiver = local(0);
  assert(receiver != NULL && receiver->bottom_type()->isa_instptr() != NULL,
         "must have non-null instance type");

  const TypeInstPtr *tinst = receiver->bottom_type()->isa_instptr();
if(tinst->klass()->is_loaded()&&!tinst->klass_is_exact()){
    // The type isn't known exactly so see if CHA tells us anything.
    ciInstanceKlass* ik = tinst->klass()->as_instance_klass();
if(!ik->has_finalizable_subclass(&C->_masm)){
      // No finalizable subclasses so skip the dynamic check.
      return;
    }
  }

  // Insert a dynamic test for whether the instance needs
  // finalization.  In general this will fold up since the concrete
  // class is often visible so the access flags are constant.
  Node* kid   = _gvn.transform(new (C, 2) GetKIDNode(0,receiver,TypeKlassPtr::make_kid(tinst->klass(),tinst->klass_is_exact())));
Node*klass=_gvn.transform(new(C,2)KID2KlassNode(kid));

  Node* access_flags_addr = basic_plus_adr(klass, klass, Klass::access_flags_offset_in_bytes() + sizeof(oopDesc));
  Node* access_flags = make_load(NULL, access_flags_addr, TypeInt::INT, T_INT);

  Node* mask  = _gvn.transform(new (C, 3) AndINode(access_flags, intcon(JVM_ACC_HAS_FINALIZER)));
  Node* check = _gvn.transform(new (C, 3) CmpINode(mask, intcon(0)));
  Node* test  = _gvn.transform(new (C, 2) BoolNode(check, BoolTest::ne));

  IfNode* iff = create_and_map_if(control(), test, PROB_MAX, COUNT_UNKNOWN);

  RegionNode* result_rgn = new (C, 3) RegionNode(3);
  record_for_igvn(result_rgn);

  Node *skip_register = _gvn.transform(new (C, 1) IfFalseNode(iff));
  result_rgn->init_req(1, skip_register);

  Node *needs_register = _gvn.transform(new (C, 1) IfTrueNode(iff));
  set_control(needs_register);
  if (stopped()) {
    // There is no slow path.
    result_rgn->init_req(2, top());
  } else {
    Node *call = make_runtime_call(RC_NO_LEAF,
                                   false /* !must_callruntimenode */,
                                   OptoRuntime::register_finalizer_Type(),
StubRoutines::register_finalizer_entry(),
"register_finalizer",TypePtr::BOTTOM,
                                   receiver);
    make_slow_call_ex(call, env()->Throwable_klass(), true);

    Node* fast_io  = call->in(TypeFunc::I_O);
    Node* fast_mem = call->in(TypeFunc::Memory);
    // These two phis are pre-filled with copies of of the fast IO and Memory
    Node* io_phi   = PhiNode::make(result_rgn, fast_io,  Type::ABIO);
    Node* mem_phi  = PhiNode::make(result_rgn, fast_mem, Type::MEMORY, TypePtr::BOTTOM);

    result_rgn->init_req(2, control());
    io_phi    ->init_req(2, i_o());
    mem_phi   ->init_req(2, reset_memory());

    set_all_memory( _gvn.transform(mem_phi) );
    set_i_o(        _gvn.transform(io_phi) );
  }
  
  set_control( _gvn.transform(result_rgn) );
}

//------------------------------return_current---------------------------------
// Append current _map to _exit_return
void Parse::return_current(Node* value) {
  if (RegisterFinalizersAtInit &&
      method()->intrinsic_id() == vmIntrinsics::_Object_init) {
    call_register_finalizer();
  }

  // Do not set_parse_bci, so that return goo is credited to the return insn.
  set_bci(InvocationEntryBci);
  if (method()->is_synchronized()) {
    shared_unlock(_synch_obj);
  }
  SafePointNode* exit_return = _exits.map();
  exit_return->in( TypeFunc::Control  )->add_req( control() );
  exit_return->in( TypeFunc::I_O      )->add_req( i_o    () );
  Node *mem = exit_return->in( TypeFunc::Memory   );
  for (MergeMemStream mms(mem->as_MergeMem(), merged_memory()); mms.next_non_empty2(); ) {
    if (mms.is_empty()) {
      // get a copy of the base memory, and patch just this one input
      const TypePtr* adr_type = mms.adr_type(C);
      Node* phi = mms.force_memory()->as_Phi()->slice_memory(adr_type);
      assert(phi->as_Phi()->region() == mms.base_memory()->in(0), "");
      gvn().set_type_bottom(phi);
      phi->del_req(phi->req()-1);  // prepare to re-patch
      mms.set_memory(phi);
    }
    mms.memory()->add_req(mms.memory2());
  }
  
  // frame pointer is always same, already captured
  if (value != NULL) {
    // If returning oops to an interface-return, there is a silent free
    // cast from oop to interface allowed by the Verifier.  Make it explicit
    // here.
    Node* phi = _exits.argument(0);
    ciType *cit = method()->signature()->return_type();
    if( cit->is_instance_klass() && cit->is_loaded() && ((ciInstanceKlass*)cit)->is_interface() ) {
      const TypeInstPtr *tp = value->bottom_type()->isa_instptr();
      if (tp && tp->klass()->is_loaded() &&
          !tp->klass()->is_interface()) {
        // sharpen the type eagerly; this eases certain assert checking
        const TypePtr *tr = TypeOopPtr::make_from_klass_unique((ciInstanceKlass*)cit);
        if (tp->higher_equal(TypeInstPtr::NOTNULL))
          tr = tr->join(TypeInstPtr::NOTNULL)->is_instptr();
        value = _gvn.transform(new (C, 2) CheckCastPPNode(0,value,tr));
      }
    }
    phi->add_req(value);
  }

  stop_and_kill_map();          // This CFG path dies here
}


//------------------------------add_safepoint----------------------------------
void Parse::add_safepoint() {
  // See if we can avoid this safepoint.  No need for a SafePoint immediately
  // after a Call (except Leaf Call) or another SafePoint.
  Node *proj = control();
uint parms=TypeFunc::Parms;
  if( proj->is_Proj() ) {
    Node *n0 = proj->in(0);
    if( n0->is_Catch() ) {
      n0 = n0->in(0)->in(0);
      assert( n0->is_Call(), "expect a call here" );
    }
    if( n0->is_Call() ) {
      if( n0->as_Call()->guaranteed_safepoint() )
        return;
    } else if( n0->is_SafePoint() && n0->req() >= parms ) {
      return;
    }
  }

  // Clear out dead values from the debug info.
  kill_dead_locals();

  // Clone the JVM State
SafePointNode*sfpnt=new(C,parms)SafePointNode(parms,NULL,NULL,cpdata());

  // Capture memory state BEFORE a SafePoint.  Since we can block at a
  // SafePoint we need our GC state to be safe; i.e. we need all our current
  // write barriers (card marks) to not float down after the SafePoint so we
  // must read raw memory.  Likewise we need all oop stores to match the card
  // marks.  If deopt can happen, we need ALL stores (we need the correct JVM
  // state on a deopt).

  // We do not need to WRITE the memory state after a SafePoint.  The control
  // edge will keep card-marks and oop-stores from floating up from below a
  // SafePoint and our true dependency added here will keep them from floating
  // down below a SafePoint.

  // Clone the current memory state
  Node* mem = MergeMemNode::make(C, map()->memory());

  mem = _gvn.transform(mem);
  
  // Pass control through the safepoint
  sfpnt->init_req(TypeFunc::Control  , control());
  // Fix edges normally used by a call
  sfpnt->init_req(TypeFunc::I_O      , top() );
  sfpnt->init_req(TypeFunc::Memory   , mem   );
  sfpnt->init_req(TypeFunc::ReturnAdr, top() );
  sfpnt->init_req(TypeFunc::FramePtr , top() );

  // Fix up the JVM State edges
  add_safepoint_edges(sfpnt, cpdata(),/*must_throw=*/false);
  Node *transformed_sfpnt = _gvn.transform(sfpnt);
  set_control(transformed_sfpnt);

  // Provide an edge from root to safepoint.  This makes the safepoint
  // appear useful until the parse has completed.
if(transformed_sfpnt->is_SafePoint()){
    assert(C->root() != NULL, "Expect parse is still valid");
    C->root()->add_prec(transformed_sfpnt);
  }
}

#ifndef PRODUCT
//------------------------show_parse_info--------------------------------------
void Parse::show_parse_info() {
  InlineTree* ilt = NULL;
  if (C->ilt() != NULL) {
    JVMState* caller_jvms = is_osr_parse() ? caller()->caller() : caller();
    ilt = InlineTree::find_subtree_from_root(C->ilt(), caller_jvms, method());
  }
  if (PrintOpto && (depth() == 1 || PrintOptoInlining)) {
    // Print that we succeeded; suppress this message on the first osr parse.

if(method()->is_synchronized())C2OUT->print("s");
if(method()->has_exception_handlers())C2OUT->print("!");
    // Check this is not the final compiled version
if(false&&depth()==1){
      tty->print("-");
    } else {
      tty->print(" ");
    }
if(depth()!=1){C2OUT->print("   ");}//missing compile count
for(int i=1;i<depth();++i){C2OUT->print("  ");}
method()->print_short_name(C2OUT);
    if (is_osr_parse()) {
C2OUT->print(" @ %d",osr_bci());
    }
    if (ilt->caller_bci() != -1) {
C2OUT->print(" @ %d",ilt->caller_bci());
    }
C2OUT->print(" (%d bytes)",method()->code_size());
    if (ilt->count_inlines()) {
C2OUT->print(" __inlined %d (%d bytes)",ilt->count_inlines(),
                 ilt->count_inline_bcs());
    }
C2OUT->cr();
  }
}


//------------------------------dump-------------------------------------------
// Dump information associated with the bytecodes of current _method
void Parse::dump() {
  if( method() != NULL ) {
    // Iterate over bytecodes
    ciBytecodeStream iter(method());
    for( Bytecodes::Code bc = iter.next(); bc != ciBytecodeStream::EOBC() ; bc = iter.next() ) {
      dump_bci( iter.cur_bci() );
C2OUT->cr();
    }
  }
}

// Dump information associated with a byte code index, 'bci'
void Parse::dump_bci(int bci) {
  // Output info on merge-points, cloning, and within _jsr..._ret
  // NYI
C2OUT->print(" bci:%d",bci);
}

#endif
