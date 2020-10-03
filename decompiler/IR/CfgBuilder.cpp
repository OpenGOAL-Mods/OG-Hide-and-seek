#include <unordered_set>
#include "CfgBuilder.h"
#include "decompiler/Function/CfgVtx.h"
#include "decompiler/Function/Function.h"

std::vector<std::shared_ptr<IR>> IR::get_all_ir(LinkedObjectFile& file) const {
  std::vector<std::shared_ptr<IR>> result;
  get_children(&result);
  size_t last_checked = 0;
  size_t last_last_checked = -1;

  while (last_checked != last_last_checked) {
    last_last_checked = last_checked;
    auto end_of_check = result.size();
    for (size_t i = last_checked; i < end_of_check; i++) {
      auto it = result.at(i).get();
      assert(it);
      it->get_children(&result);
    }
    last_checked = end_of_check;
  }

  // Todo, remove this check which is just for debugging.
  std::unordered_set<std::shared_ptr<IR>> unique_ir;
  for (auto& x : result) {
    unique_ir.insert(x);
  }
  assert(unique_ir.size() == result.size());
  return result;
}

namespace {

std::shared_ptr<IR> cfg_to_ir(Function& f, LinkedObjectFile& file, CfgVtx* vtx);

/*!
 * This adds a single CfgVtx* to a list of IR's by converting it with cfg to IR.
 * The trick here is that it will recursively inline anything which would generate an IR begin.
 * This avoids the case where Begin's are nested excessively.
 */
void insert_cfg_into_list(Function& f,
                          LinkedObjectFile& file,
                          std::vector<std::shared_ptr<IR>>* output,
                          CfgVtx* vtx) {
  auto as_sequence = dynamic_cast<SequenceVtx*>(vtx);
  auto as_block = dynamic_cast<BlockVtx*>(vtx);
  if (as_sequence) {
    for (auto& x : as_sequence->seq) {
      insert_cfg_into_list(f, file, output, x);
    }
  } else if (as_block) {
    auto& block = f.basic_blocks.at(as_block->block_id);
    IR* last = nullptr;
    for (int instr = block.start_word; instr < block.end_word; instr++) {
      auto got = f.get_basic_op_at_instr(instr);
      if (got.get() == last) {
        continue;
      }
      last = got.get();
      output->push_back(got);
    }
  } else {
    // output->push_back(cfg_to_ir(f, file, vtx));
    auto ir = cfg_to_ir(f, file, vtx);
    auto ir_as_begin = dynamic_cast<IR_Begin*>(ir.get());
    if (ir_as_begin) {
      // we unexpectedly got a begin, even though we didn't think we would.  This is okay, but we
      // should inline this begin to avoid nested begins.
      for (auto& x : ir_as_begin->forms) {
        output->push_back(x);
      }
    } else {
      output->push_back(ir);
    }
  }
}

std::pair<IR_Branch*, std::vector<std::shared_ptr<IR>>*> get_condition_branch_as_vector(IR* in) {
  auto as_seq = dynamic_cast<IR_Begin*>(in);
  if (as_seq) {
    auto irb = dynamic_cast<IR_Branch*>(as_seq->forms.back().get());
    auto loc = &as_seq->forms;
    assert(irb);
    return std::make_pair(irb, loc);
  }
  return std::make_pair(nullptr, nullptr);
}

std::pair<IR_Branch*, std::shared_ptr<IR>*> get_condition_branch(std::shared_ptr<IR>* in) {
  IR_Branch* condition_branch = dynamic_cast<IR_Branch*>(in->get());
  std::shared_ptr<IR>* condition_branch_location = in;
  if (!condition_branch) {
    // not 100% sure this will always work
    auto as_seq = dynamic_cast<IR_Begin*>(in->get());
    if (as_seq) {
      condition_branch = dynamic_cast<IR_Branch*>(as_seq->forms.back().get());
      condition_branch_location = &as_seq->forms.back();
    }
  }
  return std::make_pair(condition_branch, condition_branch_location);
}

void clean_up_cond_with_else(IR_CondWithElse* cwe, LinkedObjectFile& file) {
  for (auto& e : cwe->entries) {
    auto jump_to_next = get_condition_branch(&e.condition);
    assert(jump_to_next.first);
    assert(jump_to_next.first->branch_delay.kind == BranchDelay::NOP);
    // printf("got cond condition %s\n", jump_to_next.first->print(file).c_str());
    auto replacement = std::make_shared<IR_Compare>(jump_to_next.first->condition);
    *(jump_to_next.second) = replacement;

    auto jump_to_end = get_condition_branch(&e.body);
    assert(jump_to_end.first);
    assert(jump_to_end.first->branch_delay.kind == BranchDelay::NOP);
    assert(jump_to_end.first->condition.kind == Condition::ALWAYS);
    auto as_end_of_sequence = get_condition_branch_as_vector(e.body.get());
    if (as_end_of_sequence.first) {
      assert(as_end_of_sequence.second->size() > 1);
      as_end_of_sequence.second->pop_back();
    } else {
      // this means the case is empty, which is a little bit weird but does actually appear to
      // happen in a few places. so we just replace the jump with a nop.  In the future we could
      // consider having a more explicit "this case is empty" operator so this doesn't get confused
      // with an actual MIPS nop.
      *(jump_to_end.second) = std::make_shared<IR_Nop>();
    }
  }
}

std::shared_ptr<IR> try_sc_as_type_of(Function& f, LinkedObjectFile& file, ShortCircuit* vtx) {
  // the assembly looks like this:
  /*
         dsll32 v1, a0, 29                   ;; (set! v1 (shl a0 61))
         beql v1, r0, L60                    ;; (bl! (= v1 r0) L60 (unknown-branch-delay))
         lw v1, binteger(s7)

         bgtzl v1, L60                       ;; (bl! (>0.s v1) L60 (unknown-branch-delay))
         lw v1, pair(s7)

         lwu v1, -4(a0)                      ;; (set! v1 (l.wu (+.i a0 -4)))
     L60:
   */

  // some of these checks may be a little bit overkill but it's a nice way to sanity check that
  // we have actually decoded everything correctly.
  if (vtx->entries.size() != 3) {
    return nullptr;
  }

  auto b0 = dynamic_cast<BlockVtx*>(vtx->entries.at(0));
  auto b1 = dynamic_cast<BlockVtx*>(vtx->entries.at(1));
  auto b2 = dynamic_cast<BlockVtx*>(vtx->entries.at(2));

  if (!b0 || !b1 || !b2) {
    return nullptr;
  }

  auto b0_ptr = cfg_to_ir(f, file, b0);
  auto b0_ir = dynamic_cast<IR_Begin*>(b0_ptr.get());

  auto b1_ptr = cfg_to_ir(f, file, b1);
  auto b1_ir = dynamic_cast<IR_Branch*>(b1_ptr.get());

  auto b2_ptr = cfg_to_ir(f, file, b2);
  auto b2_ir = dynamic_cast<IR_Set*>(b2_ptr.get());
  if (!b0_ir || !b1_ir || !b2_ir) {
    return nullptr;
  }

  // todo determine temp and source reg from dsll32 instruction.

  auto set_shift = dynamic_cast<IR_Set*>(b0_ir->forms.at(b0_ir->forms.size() - 2).get());
  if (!set_shift) {
    return nullptr;
  }

  auto temp_reg0 = dynamic_cast<IR_Register*>(set_shift->dst.get());
  if (!temp_reg0) {
    return nullptr;
  }

  auto shift = dynamic_cast<IR_IntMath2*>(set_shift->src.get());
  if (!shift || shift->kind != IR_IntMath2::LEFT_SHIFT) {
    return nullptr;
  }
  auto src_reg = dynamic_cast<IR_Register*>(shift->arg0.get());
  auto sa = dynamic_cast<IR_IntegerConstant*>(shift->arg1.get());
  if (!src_reg || !sa || sa->value != 61) {
    return nullptr;
  }

  auto first_branch = dynamic_cast<IR_Branch*>(b0_ir->forms.back().get());
  auto second_branch = b1_ir;
  auto else_case = b2_ir;

  if (!first_branch || first_branch->branch_delay.kind != BranchDelay::SET_BINTEGER ||
      first_branch->condition.kind != Condition::ZERO || !first_branch->likely) {
    return nullptr;
  }
  auto temp_reg = dynamic_cast<IR_Register*>(first_branch->condition.src0.get());
  assert(temp_reg);
  assert(temp_reg->reg == temp_reg0->reg);
  auto dst_reg = dynamic_cast<IR_Register*>(first_branch->branch_delay.destination.get());
  assert(dst_reg);

  if (!second_branch || second_branch->branch_delay.kind != BranchDelay::SET_PAIR ||
      second_branch->condition.kind != Condition::GREATER_THAN_ZERO_SIGNED ||
      !second_branch->likely) {
    return nullptr;
  }

  // check we agree on destination register.
  auto dst_reg2 = dynamic_cast<IR_Register*>(second_branch->branch_delay.destination.get());
  assert(dst_reg2->reg == dst_reg->reg);

  // else case is a lwu to grab the type from a basic
  assert(else_case);
  auto dst_reg3 = dynamic_cast<IR_Register*>(else_case->dst.get());
  assert(dst_reg3);
  assert(dst_reg3->reg == dst_reg->reg);
  auto load_op = dynamic_cast<IR_Load*>(else_case->src.get());
  if (!load_op || load_op->kind != IR_Load::UNSIGNED || load_op->size != 4) {
    return nullptr;
  }
  auto load_loc = dynamic_cast<IR_IntMath2*>(load_op->location.get());
  if (!load_loc || load_loc->kind != IR_IntMath2::ADD) {
    return nullptr;
  }
  auto src_reg3 = dynamic_cast<IR_Register*>(load_loc->arg0.get());
  auto offset = dynamic_cast<IR_IntegerConstant*>(load_loc->arg1.get());
  if (!src_reg3 || !offset) {
    return nullptr;
  }

  assert(src_reg3->reg == src_reg->reg);
  assert(offset->value == -4);

  printf("Candidates for SC type-of:\n%s\n%s\n%s\n", b0_ir->print(file).c_str(),
         b1_ir->print(file).c_str(), b2_ir->print(file).c_str());

  std::shared_ptr<IR> clobber = nullptr;
  if (temp_reg->reg != src_reg->reg && temp_reg->reg != dst_reg->reg) {
    clobber = first_branch->condition.src0;
  }
  if (b0_ir->forms.size() == 2) {
    return std::make_shared<IR_Set>(IR_Set::REG_64, else_case->dst,
                                    std::make_shared<IR_GetRuntimeType>(shift->arg0, clobber));
  } else {
    // i'm not brave enough to enable this until I have found a better test case
    // remove the branch
    b0_ir->forms.pop_back();
    // remove the shift
    b0_ir->forms.pop_back();
    // add the type-of
    b0_ir->forms.push_back(std::make_shared<IR_Set>(
        IR_Set::REG_64, else_case->dst, std::make_shared<IR_GetRuntimeType>(shift->arg0, clobber)));
    return b0_ptr;
  }
  return nullptr;  // todo
}

std::shared_ptr<IR> cfg_to_ir(Function& f, LinkedObjectFile& file, CfgVtx* vtx) {
  if (dynamic_cast<BlockVtx*>(vtx)) {
    auto* bv = dynamic_cast<BlockVtx*>(vtx);
    auto& block = f.basic_blocks.at(bv->block_id);
    std::vector<std::shared_ptr<IR>> irs;
    IR* last = nullptr;
    for (int instr = block.start_word; instr < block.end_word; instr++) {
      auto got = f.get_basic_op_at_instr(instr);
      if (got.get() == last) {
        continue;
      }
      last = got.get();
      irs.push_back(got);
    }

    if (irs.size() == 1) {
      return irs.front();
    } else {
      return std::make_shared<IR_Begin>(irs);
    }

  } else if (dynamic_cast<SequenceVtx*>(vtx)) {
    auto* sv = dynamic_cast<SequenceVtx*>(vtx);

    std::vector<std::shared_ptr<IR>> irs;
    insert_cfg_into_list(f, file, &irs, sv);

    return std::make_shared<IR_Begin>(irs);
  } else if (dynamic_cast<WhileLoop*>(vtx)) {
    auto wvtx = dynamic_cast<WhileLoop*>(vtx);
    auto result = std::make_shared<IR_WhileLoop>(cfg_to_ir(f, file, wvtx->condition),
                                                 cfg_to_ir(f, file, wvtx->body));
    return result;
  } else if (dynamic_cast<CondWithElse*>(vtx)) {
    auto* cvtx = dynamic_cast<CondWithElse*>(vtx);
    std::vector<IR_CondWithElse::Entry> entries;
    for (auto& x : cvtx->entries) {
      IR_CondWithElse::Entry e;
      e.condition = cfg_to_ir(f, file, x.condition);
      e.body = cfg_to_ir(f, file, x.body);
      entries.push_back(std::move(e));
    }
    auto else_ir = cfg_to_ir(f, file, cvtx->else_vtx);
    auto result = std::make_shared<IR_CondWithElse>(entries, else_ir);
    clean_up_cond_with_else(result.get(), file);
    return result;
  } else if (dynamic_cast<ShortCircuit*>(vtx)) {
    auto* svtx = dynamic_cast<ShortCircuit*>(vtx);
    auto as_type_of = try_sc_as_type_of(f, file, svtx);
    if (as_type_of) {
      return as_type_of;
    }
  }

  throw std::runtime_error("not yet implemented IR conversion.");
  return nullptr;
}

void clean_up_while_loops(IR_Begin* sequence, LinkedObjectFile& file) {
  std::vector<size_t> to_remove;  // the list of branches to remove by index in this sequence
  for (size_t i = 0; i < sequence->forms.size(); i++) {
    auto* form_as_while = dynamic_cast<IR_WhileLoop*>(sequence->forms.at(i).get());
    if (form_as_while) {
      assert(i != 0);
      auto prev_as_branch = dynamic_cast<IR_Branch*>(sequence->forms.at(i - 1).get());
      assert(prev_as_branch);
      // printf("got while intro branch %s\n", prev_as_branch->print(file).c_str());
      // this should be an always jump. We'll assume that the CFG builder successfully checked
      // the brach destination, but we will check the condition.
      assert(prev_as_branch->condition.kind == Condition::ALWAYS);
      assert(prev_as_branch->branch_delay.kind == BranchDelay::NOP);
      to_remove.push_back(i - 1);

      // now we should try to find the condition branch:

      auto condition_branch = get_condition_branch(&form_as_while->condition);

      assert(condition_branch.first);
      assert(condition_branch.first->branch_delay.kind == BranchDelay::NOP);
      // printf("got while condition branch %s\n", condition_branch.first->print(file).c_str());
      auto replacement = std::make_shared<IR_Compare>(condition_branch.first->condition);
      *(condition_branch.second) = replacement;
    }
  }

  // remove the implied forward always branches.
  for (int i = int(to_remove.size()); i-- > 0;) {
    auto idx = to_remove.at(i);
    assert(dynamic_cast<IR_Branch*>(sequence->forms.at(idx).get()));
    sequence->forms.erase(sequence->forms.begin() + idx);
  }
}
}  // namespace

std::shared_ptr<IR> build_cfg_ir(Function& function,
                                 ControlFlowGraph& cfg,
                                 LinkedObjectFile& file) {
  //  printf("build cfg ir\n");
  if (!cfg.is_fully_resolved()) {
    return nullptr;
  }

  try {
    auto top_level = cfg.get_single_top_level();
    // todo, we should apply transformations for fixing up branch instructions for each IR.
    // and possibly annotate the IR control flow structure so that we can determine if its and/or
    // or whatever. This may require rejecting a huge number of inline assembly functions, and
    // possibly resolving the min/max/ash issue.
    // auto ir = cfg_to_ir(function, file, top_level);
    auto ir = std::make_shared<IR_Begin>();
    insert_cfg_into_list(function, file, &ir->forms, top_level);
    auto all_children = ir->get_all_ir(file);
    all_children.push_back(ir);
    for (auto& child : all_children) {
      //      printf("child is %s\n", child->print(file).c_str());
      auto as_begin = dynamic_cast<IR_Begin*>(child.get());
      if (as_begin) {
        clean_up_while_loops(as_begin, file);
      }
    }
    return ir;
  } catch (std::runtime_error& e) {
    return nullptr;
  }
}