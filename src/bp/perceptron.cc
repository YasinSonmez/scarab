/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Acknowledgement:
 * This file use gshare.cc as a template.
 */

#include "perceptron.h"

#include <vector>
#include <map>

extern "C" {
#include "bp/bp.param.h"
#include "core.param.h"
#include "globals/assert.h"
#include "statistics.h"
}

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP_DIR, ##args)
// (experimental) optimal threshold: a function of the history length
#define BP_PERCEPTRON_THRESHOLD (int32)(1.93 * BP_PERCEPTRON_HIST_LENGTH + 14)

namespace {

struct Entry {
  // when aliasing
  std::vector<int> perceptron;
  Addr tag;

  // when no aliasing
  std::map<Addr, std::vector<int> > perceptrons;

  Addr get_tag(const Addr addr) {
    if (BP_PERCEPTRON_NO_ALIASING == FALSE) {
      return tag;
    } else {
      // https://stackoverflow.com/questions/1409454/c-map-find-to-possibly-insert-how-to-optimize-operations
      std::map<Addr, std::vector<int> >::iterator it;
      it = perceptrons.find(addr);
      if (it == perceptrons.end()){
        return 0;
      } else {
        return addr;
      }
    }
  }

  std::vector<int>& get_perceptron(const Addr addr) {
    if (BP_PERCEPTRON_NO_ALIASING == FALSE) {
      return perceptron;
    } else {
      // https://stackoverflow.com/questions/1409454/c-map-find-to-possibly-insert-how-to-optimize-operations
      std::map<Addr, std::vector<int> >::iterator it;
      it = perceptrons.find(addr);
      if (it == perceptrons.end()){
        // when do not exist, set all weights to zere
        // default ctor, and resize
        perceptrons[addr].resize(BP_PERCEPTRON_HIST_LENGTH + 1, 0);
      }
      return perceptrons[addr];
    }
  }

  // void reset_perceptron() {
  //   std::fill(perceptron.begin(), perceptron.end(), 0);
  // }

  void set_tag(Addr addr) {
    if (BP_PERCEPTRON_NO_ALIASING == FALSE) {
      tag = addr;
    }
  }
};

struct Perceptron_State {
  // 1-dimension size is the amount of branch in flight
  // 2-dimension size is the number of previous branches that is considered
  // "per-address perceptron table" yo what's up!
  // std::vector<std::vector<int> > ppt;
  std::vector<Entry> ppt;
  // TODO: if no branch aliasing
  // std::map<Addr, std::vector<int> > full_ppt;
};

std::vector<Perceptron_State> perceptron_state_all_cores;

uns32 get_ppt_index(const Addr addr) {
  // const uns32 cooked_addr = (addr >> 2) & N_BIT_MASK(HIST_LENGTH);
  const uns32 addr_modulo = addr % BP_PERCEPTRON_TABLE_SIZE;
  return addr_modulo;
}
}  // namespace

// The only speculative state of gshare is the global history which is managed
// by bp.c. Thus, no internal timestamping and recovery mechanism is needed.
void bp_perceptron_timestamp(Op* op) {}
void bp_perceptron_recover(Recovery_Info* info) {}
void bp_perceptron_spec_update(Op* op) {}
void bp_perceptron_retire(Op* op) {}

std::vector<bool> get_hist_bits(const uns32 hist) {
  // TODO: x0 = 1 for the branch itself, shift to right or left?
  // high bits new, low bits old
  // in case all 32 bits are used
  uns64 hist_shift_extend = hist >> (32 - BP_PERCEPTRON_HIST_LENGTH) | 1 << (BP_PERCEPTRON_HIST_LENGTH + 1);
  // ref: https://stackoverflow.com/questions/2249731/how-do-i-get-bit-by-bit-data-from-an-integer-value-in-c
  // (n >> k) & 1
  std::vector<bool> hist_bits;
  for (size_t i = 0; i < BP_PERCEPTRON_HIST_LENGTH + 1; i++) {
    // low index old, high index new
    hist_bits.push_back((hist_shift_extend >> i) & 1);
  }
  return hist_bits;
}

int dot_product(const uns32 hist, const std::vector<int>& perceptron) {
  std::vector<bool> hist_bits = get_hist_bits(hist);
  int sum = 0;

  for (size_t i = 0; i <perceptron.size(); i++) {
    sum += (hist_bits[i] == true ? 1 : -1) * perceptron[i];
  }

  return sum;
}

void bp_perceptron_init() {
  perceptron_state_all_cores.resize(NUM_CORES);
  for(auto& perceptron_state : perceptron_state_all_cores) {
    // TODO: the init value
    // TODO: the macro table size
    perceptron_state.ppt.resize(BP_PERCEPTRON_TABLE_SIZE);
    // for(auto& perceptron : perceptron_state.ppt) {
    if (BP_PERCEPTRON_NO_ALIASING == FALSE) {
      for(auto& entry : perceptron_state.ppt) {
        // TODO: parameterize
        // add 1 for the branch itself
        entry.perceptron.resize(BP_PERCEPTRON_HIST_LENGTH + 1, 0);
        entry.tag = 0;
      }
    }
  }
}

uns8 bp_perceptron_pred(Op* op) {
  const uns   proc_id      = op->proc_id;
  // TODO: this const sus
  // const auto& perceptron_state = perceptron_state_all_cores.at(proc_id);
  auto&       perceptron_state = perceptron_state_all_cores.at(proc_id);
  const Addr  addr      = op->oracle_info.pred_addr;
  const uns32 hist      = op->oracle_info.pred_global_hist;
  const uns32 ppt_index = get_ppt_index(addr);
  // const std::vector<int>& perceptron = perceptron_state.ppt[ppt_index];
  // non-const
  Entry& entry = perceptron_state.ppt[ppt_index];
  // get tag before get perceptron
  Addr tag = entry.get_tag(addr);
  const std::vector<int>& perceptron = entry.get_perceptron(addr);

  // assume 0 is the initial value
  if (tag == 0) {
    STAT_EVENT(proc_id, BP_PERCEPTRON_TABLE_COMPULSARY);
    entry.set_tag(addr);
  } else if (addr != tag) {
    STAT_EVENT(proc_id, BP_PERCEPTRON_TABLE_CONFLICT);
    entry.set_tag(addr);
  } else {
    STAT_EVENT(proc_id, BP_PERCEPTRON_TABLE_HIT);
  }

  // y
  const int weighted_sum = dot_product(hist, perceptron);
  const uns8  pred      = weighted_sum >= 0 ? 1 : 0;

  DEBUG(proc_id, "Predicting with perceptron for  op_num:%s  index:%d\n",
        unsstr64(op->op_num), ppt_index);
  // TODO: print weight
  DEBUG(proc_id, "Predicting  addr:%s  pht:%u  pred:%d  dir:%d\n",
        hexstr64s(addr), ppt_index, pred, op->oracle_info.dir);

  return pred;
}

void bp_perceptron_update(Op* op) {
  if(op->table_info->cf_type != CF_CBR) {
    // If op is not a conditional branch, we do not interact with gshare.
    return;
  }

  const uns   proc_id      = op->proc_id;
  auto&       perceptron_state = perceptron_state_all_cores.at(proc_id);
  const Addr  addr         = op->oracle_info.pred_addr;
  const uns32 hist         = op->oracle_info.pred_global_hist;
  const uns32 ppt_index    = get_ppt_index(addr);
  // std::vector<int>& perceptron = perceptron_state.ppt[ppt_index];
  Entry& entry = perceptron_state.ppt[ppt_index];
  std::vector<int>& perceptron = entry.get_perceptron(addr);

  // TODO: has the history already shifted in?
  const int weighted_sum   = dot_product(hist, perceptron);
  // y_out
  int training_switch = 0;
  if(weighted_sum > BP_PERCEPTRON_THRESHOLD) {
    training_switch = 1;
  }else if(weighted_sum < -1 * BP_PERCEPTRON_THRESHOLD) {
    training_switch = -1;
  }

  DEBUG(proc_id, "Writing perceptron PPT for  op_num:%s  index:%d  dir:%d\n",
        unsstr64(op->op_num), ppt_index, op->oracle_info.dir);

  int bipolar_t;
  if(op->oracle_info.dir) {
    bipolar_t = 1;
  } else {
    bipolar_t = -1;
  }

  if(training_switch != bipolar_t) {
    std::vector<bool> hist_bits = get_hist_bits(hist);

    for (size_t i = 0; i < perceptron.size(); i++) {
      perceptron[i] += bipolar_t * (hist_bits[i] == true ? 1 : -1); 
    }
  }

  // TODO: print
  // DEBUG(proc_id, "Updating addr:%s  pht:%u  ent:%u  dir:%d\n", hexstr64s(addr),
  //       pht_index, gshare_state.pht[pht_index], op->oracle_info.dir);
}
