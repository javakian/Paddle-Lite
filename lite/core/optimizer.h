// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "lite/core/mir/generate_program_pass.h"
#include "lite/core/mir/pass_manager.h"
#include "lite/core/mir/pass_utils.h"
#include "lite/core/mir/ssa_graph.h"
#include "lite/core/mir/static_kernel_pick_pass.h"
#include "lite/core/mir/type_target_cast_pass.h"
#include "lite/core/program.h"
#include "lite/core/types.h"
#include "lite/model_parser/model_parser.h"
#ifdef LITE_WITH_NPU
#include "lite/core/mir/subgraph/generate_npu_program_pass.h"
#endif
#ifdef LITE_WITH_XPU
#include "lite/core/mir/subgraph/generate_xpu_program_pass.h"
#endif

namespace paddle {
namespace lite {

/*
 * lite::Optimizer optimize a program. It utilize the mir passes to analysis the
 * program and export an optimized program.
 */
class Optimizer {
 public:
  void Run(Program&& program,
           const std::vector<Place>& valid_places,
           core::KernelPickFactor kernel_pick_factor,
           const std::vector<std::string>& passes = {}) {
    program_ = &program;
    valid_places_ = valid_places;
    CHECK(!valid_places.empty()) << "At least one valid_place should be set";
    CHECK(!graph_) << "duplicate optimize found";

    graph_.reset(new mir::SSAGraph);
    graph_->Build(program, valid_places);
    graph_->SetValidPlaces(valid_places);

    SpecifyKernelPickTactic(kernel_pick_factor);
    InitTargetTypeTransformPass();

    if (passes.empty()) {
      std::vector<std::string> passes_local{
          {"lite_quant_dequant_fuse_pass",     //
           "lite_conv_elementwise_fuse_pass",  // conv-elemwise-bn
           "lite_conv_bn_fuse_pass",           //
           "lite_conv_elementwise_fuse_pass",  // conv-bn-elemwise
           // TODO(Superjomn) Refine the fusion related design to select fusion
           // kernels for devices automatically.
           "lite_conv_activation_fuse_pass",              //
           "lite_fc_fuse_pass",                           //
           "lite_shuffle_channel_fuse_pass",              //
           "lite_transpose_softmax_transpose_fuse_pass",  //
           "lite_interpolate_fuse_pass",                  //
           "identity_scale_eliminate_pass",               //
#if (defined LITE_WITH_LIGHT_WEIGHT_FRAMEWORK) || (defined LITE_WITH_CUDA)
           "lite_elementwise_add_activation_fuse_pass",  //
#endif
           "static_kernel_pick_pass",        // pick original kernel from graph
           "variable_place_inference_pass",  // inference arg/var's
           // info(target/precision/layout/device)
           // using kernel info
           "argument_type_display_pass",  // debug pass: show arg-type-node's
                                          // info
                                          // (target/precision/layout/device)

           "type_target_cast_pass",  // add io_copy/io_copy_once if meet
                                     // different targets when last and next
                                     // node
           "variable_place_inference_pass",  //
           "argument_type_display_pass",     //

           "io_copy_kernel_pick_pass",    //
           "argument_type_display_pass",  //

           "variable_place_inference_pass",  //
           "argument_type_display_pass",     //

           "type_precision_cast_pass",       //
           "variable_place_inference_pass",  //
           "argument_type_display_pass",     //

           "type_layout_cast_pass",  // add layout/layout_once op if meet
                                     // different layout when last and next node
           "argument_type_display_pass",  //

           "variable_place_inference_pass",  //
           "argument_type_display_pass",

           "runtime_context_assign_pass",
           "argument_type_display_pass",
           "memory_optimize_pass"}};
      RunPasses(passes_local);
    } else {
      RunPasses(passes);
    }
    exec_scope_ = program.exec_scope();
  }

  const lite::Scope* exec_scope() const { return exec_scope_; }

  // Generate a new program based on the mir graph.
  std::unique_ptr<RuntimeProgram> GenRuntimeProgram() {
    // Extra passes are applied for NPU and XPU, they depends on the shapes
    // of input tensors. so GenRuntimeProgram() must be called after the shapes
    // of input tensors are determined.
    std::vector<std::string> subgraph_passes{"generate_npu_program_pass",
                                             "generate_xpu_program_pass"};
    RunPasses(subgraph_passes);

    auto pass = mir::PassManager::Global().LookUp<mir::GenerateProgramPass>(
        "generate_program_pass");
    pass->Apply(graph_);
    auto program = pass->GenProgram();
    CHECK(exec_scope_);
    program->set_exec_scope(exec_scope_);
    return program;
  }

  void InitTargetTypeTransformPass() {
    auto* pass =
        mir::PassManager::Global().LookUp<mir::TypeTargetTransformPass>(
            "type_target_cast_pass");
    CHECK(pass);
    CHECK(!valid_places_.empty());
    pass->SetValidPlaces(valid_places_);
  }

  // Generate C++ code which combines the inference program, model and weights.
  void GenCode(const std::string& code_dir);

  const mir::SSAGraph& ssa_graph() const {
    CHECK(graph_);
    return *graph_;
  }

  mir::SSAGraph* mutable_ssa_graph() {
    CHECK(graph_);
    return graph_.get();
  }

  lite::Scope* exec_scope() { return exec_scope_; }

 protected:
  void SpecifyKernelPickTactic(core::KernelPickFactor factor);

  // Specify the passes and run them.
  void RunPasses(const std::vector<std::string>& passes) {
    for (auto& x : passes) {
      LOG(INFO) << "== Running pass: " << x;
      mir::Pass* pass = mir::PassManager::Global().LookUp(x);
      if (!pass) {
        LOG(INFO) << "   - Skip " << x << " because the pass isn't found.";
        continue;
      }
      std::set<TargetType> targets;
      for (const auto& place : valid_places_) {
        targets.insert(place.target);
      }
      bool matched =
          PassMatchesTarget(*pass, targets) && PassMatchesKernels(*pass);
      if (!matched) {
        LOG(INFO) << "   - Skip " << x
                  << " because the target or kernel does not match.";
      } else {
        pass->Apply(graph_);
        LOG(INFO) << "== Finished running: " << x;
      }
    }
  }

 private:
  std::unique_ptr<mir::SSAGraph> graph_;
  std::vector<Place> valid_places_;
  lite::Scope* exec_scope_{};
  Program* program_{};
};

}  // namespace lite
}  // namespace paddle
