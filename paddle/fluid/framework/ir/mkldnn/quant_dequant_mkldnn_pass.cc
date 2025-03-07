// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/framework/ir/mkldnn/quant_dequant_mkldnn_pass.h"

#include <string>

#include "paddle/fluid/framework/ir/graph_helper.h"
#include "paddle/fluid/framework/ir/mkldnn/mkldnn_pass_util.h"
#include "paddle/fluid/framework/op_version_registry.h"

namespace paddle {
namespace framework {
namespace ir {

void QuantDequantMkldnnPass::MarkSkipQuantizedOps(
    ir::Graph* graph, const std::unordered_set<std::string>& skip_ops) const {
  VLOG(3) << "mark skip quantized ops";
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;

    if (skip_ops.count(op_node->Name())) {
      auto* op_desc = op_node->Op();
      if (!op_desc->HasAttr("quantization_type")) {
        bool is_quantized_op = true;
        for (auto* node_input : op_node->inputs) {
          for (auto* node_input_input : node_input->inputs) {
            if (!node_input_input->IsOp()) continue;
            if (node_input_input->Name().find("quantize") ==
                std::string::npos) {
              is_quantized_op = false;
              break;
            }
          }
          if (!is_quantized_op) break;
        }

        if (!is_quantized_op) {
          op_node->Op()->SetAttr("skip_quant", 1);
        }
      }
    }
  }
}

void QuantDequantMkldnnPass::CollectInfoFromFake(
    ir::Graph* graph,
    Scope* scope,
    const std::unordered_set<std::string>& fake_dequantize_types,
    std::unordered_map<std::string, std::vector<float>>* weight_thresholds)
    const {
  VLOG(3) << "gather weight_thresholds from fake dequantized ops";
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;

    if (fake_dequantize_types.count(op_node->Name())) {
      auto* op_desc = op_node->Op();
      auto x_var_name = op_desc->Input("X")[0];

      if (op_desc->HasAttr("max_range")) {
        const float max_range =
            PADDLE_GET_CONST(float, op_desc->GetAttr("max_range"));
        std::vector<float> thresholds = {127 * 127 / max_range};
        weight_thresholds->insert(std::make_pair(x_var_name, thresholds));
      } else {
        auto scale_name = op_desc->Input("Scales")[0];
        auto* var = scope->FindVar(scale_name);
        PADDLE_ENFORCE_NOT_NULL(
            var,
            platform::errors::NotFound(
                "The Scales variable [%s] of dequantize op is not found.",
                var));

        auto* scale_tensor = var->GetMutable<LoDTensor>();
        auto* scale_data = scale_tensor->data<float>();
        std::vector<float> thresholds{};
        for (int i = 0; i < scale_tensor->numel(); i++) {
          thresholds.push_back(scale_data[i]);
        }
        weight_thresholds->insert(std::make_pair(x_var_name, thresholds));
      }
    }
  }
}

void QuantDequantMkldnnPass::CollectInputScalesFromFake(
    ir::Graph* graph,
    Scope* scope,
    const std::unordered_set<std::string>& fake_quantize_types,
    std::unordered_map<std::string, std::vector<float>>* var_quant_scales)
    const {
  VLOG(3) << "gather input scales from fake quantized ops";
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;

    if (op_node->Name() == "fake_quantize_dequantize_moving_average_abs_max" ||
        fake_quantize_types.count(op_node->Name())) {
      auto* op_desc = op_node->Op();
      const int bit_length =
          PADDLE_GET_CONST(int, op_desc->GetAttr("bit_length"));
      PADDLE_ENFORCE_EQ(bit_length,
                        8,
                        platform::errors::InvalidArgument(
                            "Unsupported number quantization "
                            "bits: %d, only 8 is supported now.",
                            bit_length));

      auto x_var_name = op_desc->Input("X")[0];
      auto scale_name = op_desc->Input("InScale")[0];
      auto out_var_name = op_desc->Output("Out")[0];
      auto* var = scope->FindVar(scale_name);
      PADDLE_ENFORCE_NOT_NULL(
          var,
          platform::errors::NotFound(
              "The InScale variable [%s] of quantize op is not found.", var));

      auto* scale_tensor = var->GetMutable<LoDTensor>();
      auto* scale_data = scale_tensor->data<float>();
      float scale = 1.0 / scale_data[0];
      if (std::isinf(scale) || std::isnan(scale)) {
        scale = 0.0;
      }

      if (!var_quant_scales->count(x_var_name)) {
        std::vector<float> scale_v = {scale};
        var_quant_scales->insert(std::make_pair(x_var_name, scale_v));
      }

      if (!var_quant_scales->count(out_var_name)) {
        std::vector<float> scale_v = {scale};
        var_quant_scales->insert(std::make_pair(out_var_name, scale_v));
      }
    }
  }
}

void QuantDequantMkldnnPass::CollectOutputScalesFromAttr(
    ir::Graph* graph,
    std::unordered_map<std::string, std::vector<float>>* var_quant_scales)
    const {
  VLOG(3) << "gather output scales from op's attr";
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;

    auto* op_desc = op_node->Op();
    if (op_desc->HasAttr("out_threshold")) {
      const float attr_scale =
          PADDLE_GET_CONST(float, op_desc->GetAttr("out_threshold"));
      if (attr_scale == 0.0) continue;
      float scale = 1.0 / attr_scale;
      std::vector<float> scale_v = {scale};

      auto var_name_map = op_desc->Outputs();
      for (auto iter = var_name_map.begin(); iter != var_name_map.end();
           ++iter) {
        for (auto var_name : iter->second) {
          var_quant_scales->insert(std::make_pair(var_name, scale_v));
        }
      }
    }
  }
}

void QuantDequantMkldnnPass::CollectFakeQuantizeOps(
    ir::Graph* graph,
    Node* op_node,
    std::unordered_set<const Node*>* nodes2rm) const {
  auto* op_desc = op_node->Op();
  auto x_var_name = op_desc->Input("X")[0];
  auto in_scale_name = op_desc->Input("InScale")[0];
  auto out_var_name = op_desc->Output("Out")[0];
  auto out_scale_name = op_desc->Output("OutScale")[0];

  Node* fake_quant_in = nullptr;
  Node* fake_quant_in_scale = nullptr;
  for (auto* node_input : op_node->inputs) {
    if (node_input->Name() == x_var_name) {
      fake_quant_in = node_input;
    } else if (node_input->Name() == in_scale_name) {
      fake_quant_in_scale = node_input;
    }
  }

  Node* fake_quant_out = nullptr;
  Node* fake_quant_out_scale = nullptr;
  for (auto* node_output : op_node->outputs) {
    if (node_output->Name() == out_var_name) {
      fake_quant_out = node_output;
    } else if (node_output->Name() == out_scale_name) {
      fake_quant_out_scale = node_output;
    }
  }

  PADDLE_ENFORCE_NOT_NULL(
      fake_quant_in,
      platform::errors::NotFound(
          "The input var [%s] of quantize op is not found.", x_var_name));
  PADDLE_ENFORCE_NOT_NULL(
      fake_quant_out,
      platform::errors::NotFound(
          "The output var [%s] of quantize op is not found.", out_var_name));

  std::string input_act_name = fake_quant_in->Var()->Name();
  std::string output_act_name = fake_quant_out->Var()->Name();
  auto outlinks = fake_quant_out->outputs;
  for (auto* next_node : outlinks) {
    if (!next_node->IsOp()) continue;
    next_node->Op()->RenameInput(output_act_name, input_act_name);
    IR_NODE_LINK_TO(fake_quant_in, next_node);
  }

  nodes2rm->insert(op_node);
  nodes2rm->insert(fake_quant_in_scale);
  nodes2rm->insert(fake_quant_out);
  nodes2rm->insert(fake_quant_out_scale);
}

void QuantDequantMkldnnPass::CollectFakeDequantizeOps(
    ir::Graph* graph,
    Node* op_node,
    std::unordered_set<const Node*>* nodes2rm) const {
  auto* op_desc = op_node->Op();
  auto x_var_name = op_desc->Input("X")[0];
  auto out_var_name = op_desc->Output("Out")[0];

  Node* fake_dequant_in = nullptr;
  for (auto* node_input : op_node->inputs) {
    if (node_input->Name() == x_var_name) {
      fake_dequant_in = node_input;
      break;
    }
  }

  Node* fake_dequant_out = nullptr;
  for (auto* node_output : op_node->outputs) {
    if (node_output->Name() == out_var_name) {
      fake_dequant_out = node_output;
      break;
    }
  }

  PADDLE_ENFORCE_NOT_NULL(
      fake_dequant_in,
      platform::errors::NotFound(
          "The input var [%s] of dequantize op is not found.", x_var_name));
  PADDLE_ENFORCE_NOT_NULL(
      fake_dequant_out,
      platform::errors::NotFound(
          "The output var [%s] of dequantize op is not found.", out_var_name));

  std::string input_act_name = fake_dequant_in->Var()->Name();
  std::string output_act_name = fake_dequant_out->Var()->Name();
  auto outlinks = fake_dequant_out->outputs;
  for (auto* next_node : outlinks) {
    next_node->Op()->RenameInput(output_act_name, input_act_name);
    IR_NODE_LINK_TO(fake_dequant_in, next_node);
  }

  nodes2rm->insert(op_node);
  nodes2rm->insert(fake_dequant_out);
}

void QuantDequantMkldnnPass::RemoveFakeOps(
    ir::Graph* graph,
    const std::unordered_set<std::string>& fake_quantize_types,
    const std::unordered_set<std::string>& fake_dequantize_types,
    const std::unordered_set<std::string>& fake_quantize_dequantize_types)
    const {
  VLOG(3) << "remove fake quantize and dequantize ops";

  std::unordered_set<const Node*> nodes2rm = {};
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;

    if (fake_quantize_types.count(op_node->Name())) {
      CollectFakeQuantizeOps(graph, op_node, &nodes2rm);
    } else if (fake_dequantize_types.count(op_node->Name())) {
      CollectFakeDequantizeOps(graph, op_node, &nodes2rm);
    } else if (fake_quantize_dequantize_types.count(op_node->Name())) {
      CollectFakeDequantizeOps(graph, op_node, &nodes2rm);
    }
  }

  GraphSafeRemoveNodes(graph, nodes2rm);
}

void QuantDequantMkldnnPass::TransposeWeight(Tensor* input) const {
  const auto in_dims = input->dims();
  std::vector<int> out_dim_v;
  std::vector<int> axis;
  for (int i = in_dims.size() - 1; i >= 0; i--) {
    axis.push_back(i);
    out_dim_v.push_back(in_dims[i]);
  }

  const auto out_dims = phi::make_ddim(out_dim_v);
  const int rank = axis.size();
  auto in_stride = phi::stride(in_dims);
  auto out_stride = phi::stride(out_dims);
  const int count = input->numel();

  Tensor trans_tensor;
  trans_tensor.Resize(out_dims);
  float* trans_data = trans_tensor.mutable_data<float>(platform::CPUPlace());
  float* in_data = input->mutable_data<float>(platform::CPUPlace());

  for (int64_t out_idx = 0; out_idx < count; ++out_idx) {
    int64_t in_idx = 0;
    int64_t tmp_idx = out_idx;
    for (int i = 0; i < rank; ++i) {
      const int64_t coordinate = tmp_idx / out_stride[i];
      tmp_idx -= coordinate * out_stride[i];
      in_idx += coordinate * in_stride[axis[i]];
    }
    trans_data[out_idx] = in_data[in_idx];
  }

  input->Resize(out_dims);
  for (int i = 0; i < input->numel(); i++) {
    in_data[i] = trans_data[i];
  }
}

bool QuantDequantMkldnnPass::IsInt8Weight(
    Node* op_node, Scope* scope, const std::string& weight_name) const {
  auto* op_desc = op_node->Op();
  auto var_name = op_desc->Input(weight_name)[0];
  auto* var = scope->FindVar(var_name);
  if (var == nullptr) {
    return false;
  }
  auto* weight_tensor = var->GetMutable<LoDTensor>();
  auto* weight_data = weight_tensor->data<float>();
  bool is_int8 = true;
  for (int i = 0; i < weight_tensor->numel(); i++) {
    if (weight_data[i] - static_cast<int>(weight_data[i]) != 0) {
      is_int8 = false;
      break;
    }
  }
  return is_int8;
}

void QuantDequantMkldnnPass::DequantizeOpWeights(
    Node* op_node,
    Scope* scope,
    const std::string& weight_name,
    const std::string& output_name,
    const std::unordered_map<std::string, std::vector<float>>&
        weight_thresholds) const {
  auto* op_desc = op_node->Op();
  std::string weight_var_name = op_desc->Input(weight_name)[0];
  std::string output_var_name = op_desc->Output(output_name)[0];

  std::vector<float> scales;
  auto iter = weight_thresholds.find(output_var_name);
  if (iter != weight_thresholds.end()) {
    scales = iter->second;
  } else {
    PADDLE_THROW(paddle::platform::errors::Fatal(
        "Could not find threshold information for [%s] var, please check if "
        "the model is correct.",
        output_var_name));
  }

  auto* var = scope->FindVar(weight_var_name);
  PADDLE_ENFORCE_NOT_NULL(
      var,
      platform::errors::NotFound(
          "The input persistable [%s] var of [%s] op is not found.",
          weight_var_name,
          op_desc->Type()));
  auto* weight_tensor = var->GetMutable<LoDTensor>();
  const auto weight_dims = weight_tensor->dims();

  const int size = scales.size();
  if (size == 1 || size == weight_dims[0]) {
    auto* weight_data =
        weight_tensor->mutable_data<float>(platform::CPUPlace());
    for (int i = 0; i < weight_tensor->numel(); i++) {
      weight_data[i] /= 127;
    }

    TransposeWeight(weight_tensor);

    if (size == 1) {
      for (int i = 0; i < weight_tensor->numel(); i++) {
        weight_data[i] *= scales[0];
      }
    } else {
      for (int i = 0; i < weight_tensor->numel(); i++) {
        weight_data[i] *= scales[i % size];
      }
    }

    TransposeWeight(weight_tensor);
  } else if (weight_dims.size() > 1 && size == weight_dims[1]) {
    auto* weight_data =
        weight_tensor->mutable_data<float>(platform::CPUPlace());
    for (int i = 0; i < weight_tensor->numel(); i++) {
      weight_data[i] /= 127;
    }

    int step_n = 1;
    for (int i = 1; i < weight_dims.size(); i++) {
      step_n *= weight_dims[i];
    }
    int step_c = step_n / size;
    for (int i = 0; i < weight_dims[0]; i++) {
      int begin_n = i * step_n;
      for (int j = begin_n; j < begin_n + step_n; j++) {
        for (int k = 0; k < size; k++) {
          int begin_c = k * step_c;
          for (int m = begin_c; m < begin_c + step_c; m++) {
            weight_data[m] *= scales[k];
          }
        }
      }
    }
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "The size of weight scales vector (%d) does not "
        "match the dimensions (%d) of the weights tensor %s.",
        size,
        weight_tensor->dims().size(),
        weight_var_name));
  }

  weight_tensor->Resize(weight_dims);
}

void QuantDequantMkldnnPass::DequantizeWeights(
    ir::Graph* graph,
    Scope* scope,
    const std::unordered_map<std::string, std::vector<float>>&
        weight_thresholds) const {
  VLOG(3) << "dequantize weight for ops which has weight";

  if (weight_thresholds.empty()) {
    VLOG(3)
        << "No need to dequantize weights because weight_thresholds is empty.";
    return;
  }

  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;
    if (op_node->Name() == "conv2d" || op_node->Name() == "depthwise_conv2d") {
      if (IsInt8Weight(op_node, scope, "Filter")) {
        DequantizeOpWeights(
            op_node, scope, "Filter", "Output", weight_thresholds);
      }
    } else if (op_node->Name() == "mul" || op_node->Name() == "matmul" ||
               op_node->Name() == "matmul_v2") {
      if (IsInt8Weight(op_node, scope, "Y")) {
        DequantizeOpWeights(op_node, scope, "Y", "Out", weight_thresholds);
      }
    }
  }
}

void QuantDequantMkldnnPass::UpdateActivations(ir::Graph* graph) const {
  VLOG(3) << "update conv2d or depthwise_conv2d fused activation";
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (!op_node->IsOp()) continue;

    if (op_node->Name() == "conv2d" || op_node->Name() == "depthwise_conv2d") {
      auto* op_desc = op_node->Op();
      if (!op_desc->HasAttr("fuse_activation")) {
        std::string activation;
        if (op_desc->GetAttrIfExists<bool>("fuse_relu")) {
          activation = "relu";
        }
        op_node->Op()->SetAttr("fuse_activation", activation);
      }
    }
  }
}

void QuantDequantMkldnnPass::RemoveCtrlVars(ir::Graph* graph) const {
  VLOG(3) << "remove control flow variable";
  std::unordered_set<const Node*> nodes2rm = {};
  for (auto* op_node :
       ir::TopologyVarientSort(*graph, static_cast<ir::SortKind>(0))) {
    if (op_node->IsCtrlVar()) {
      nodes2rm.insert(op_node);
    }
  }

  GraphSafeRemoveNodes(graph, nodes2rm);
}

void QuantDequantMkldnnPass::ApplyImpl(ir::Graph* graph) const {
  VLOG(3) << "Convert paddle slim quantized model to mkldnn quantized model.";
  const std::string pattern_name = "quant_dequant_mkldnn_pass";
  FusePassBase::Init(pattern_name, graph);

  const std::unordered_set<std::string> skip_ops = {
      "conv2d", "depthwise_conv2d", "mul", "matmul", "matmul_v2"};

  const std::unordered_set<std::string> fake_quantize_types = {
      "fake_quantize_moving_average_abs_max", "fake_quantize_range_abs_max"};

  const std::unordered_set<std::string> fake_dequantize_types = {
      "fake_dequantize_max_abs", "fake_channel_wise_dequantize_max_abs"};

  const std::unordered_set<std::string> fake_quantize_dequantize_types = {
      "fake_quantize_dequantize_abs_max",
      "fake_quantize_dequantize_moving_average_abs_max",
      "fake_channel_wise_quantize_dequantize_abs_max"};

  std::unordered_map<std::string, std::vector<float>> weight_thresholds{};
  std::unordered_map<std::string, std::vector<float>> var_quant_scales{};

  auto* scope = param_scope();
  MarkSkipQuantizedOps(graph, skip_ops);
  CollectInfoFromFake(graph, scope, fake_dequantize_types, &weight_thresholds);
  CollectInputScalesFromFake(
      graph, scope, fake_quantize_types, &var_quant_scales);
  CollectOutputScalesFromAttr(graph, &var_quant_scales);
  RemoveFakeOps(graph,
                fake_quantize_types,
                fake_dequantize_types,
                fake_quantize_dequantize_types);
  DequantizeWeights(graph, scope, weight_thresholds);
  UpdateActivations(graph);
  RemoveCtrlVars(graph);

  // save var_quant_scales in the first op's attr
  // for compute_propagate_scales_mkldnn_pass
  SaveInfoInTheFirstOp(
      graph, "has_quant_info", "var_quant_scales", var_quant_scales);
}

}  // namespace ir
}  // namespace framework
}  // namespace paddle

REGISTER_PASS(quant_dequant_mkldnn_pass,
              paddle::framework::ir::QuantDequantMkldnnPass);

REGISTER_PASS_CAPABILITY(quant_dequant_mkldnn_pass)
    .AddCombination(
        paddle::framework::compatible::OpVersionComparatorCombination()
            .LE("conv2d", 1)
            .EQ("fc", 0)
            .LE("conv2d_transpose", 2)
            .EQ("fake_quantize_abs_max", 0)
            .EQ("fake_quantize_range_abs_max", 0)
            .EQ("fake_quantize_moving_average_abs_max", 0)
            .LE("fake_channel_wise_quantize_abs_max", 1)
            .EQ("fake_dequantize_max_abs", 0));
