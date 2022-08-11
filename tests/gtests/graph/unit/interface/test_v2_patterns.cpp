/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <memory>

#include "gtest/gtest.h"

#include "interface/graph.hpp"
#include "utils/pm/nested_matcher.hpp"

#include "graph/unit/utils.hpp"

using namespace dnnl::impl::graph;
using namespace dnnl::impl::graph::op_kind;
using namespace dnnl::impl::graph::utils::pm;
using namespace dnnl::graph::tests::unit::utils;

const iport_t IN0 = 0;
const iport_t IN1 = 1;
const oport_t OUT0 = 0;

//
// All pattern starts with a "pb_graph"
//
TEST(PatternMatcherV2, Graph) {
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");

    ASSERT_NE(pgraph, nullptr);
}

//
// Pattern is grown by appending pattern ops ("pb_op", "alternation" and
// "repetition") to a "pb_graph" with pb_graph.append_op(),
// append_alternation(), append_optional() and append_repeition().
// Pattern can be a nested graph since "alteration" and "repetition"
// embeds "pb_graph".
// Pattern graph has the following properties.
// - During matching, aggegrate pattern nodes (pb_graph, alternation,
// repetition) will be unpacked recursively until all nodes are expanded
// to just "pb_op"s
// - Any inner "pb_graph" embedded inside "alternation" or "repetition" needs
// to provide a mapping from the "pb_graph"'s in/out port to it's inner node's
// in/out port to enable unpacking. This is done by calling create_input_port()
// and create_output_port().
// - "alternation" and "repetition"'s in/out ports are mapped to the same
// numberred in/out ports of embedded "pb_graph"(s)
// - One graph op is matched with one "pb_op". And expanded pattern graph's
// "pb_op" are not aliased. So graph ops matched with different "pb_op"s cannot
// be aliased.
// - Graph op attribute checking for is done by "decision_function"s of a
// "pb_op". Every "pb_op" needs to provide at least one "decision_function".
// One "decision_function" needs to be passed as an arugument to append_op()
// Some variants of append_op() provides a quick way to setup common
// "decision_function"s.
// Use pb_op.append_decision_function() to add additional attribute checkers.
// - Pattern matcher matches graph op edges with pb_op edges. Graph ops can
// have more edges than constrained by the pattern graph. Those are marked as
// unhandled edges during matching. Unhandled edges are two types. One is
// connected to a graph op matched by this pattern and called an internal edge.
// The other is called an external edge.
// - Matcher has two different modes of handling unhandled edges. First mode
// assumes all unhandled inputs as external input and assumes unhandled outputs
// from ops matched with non root pb_op (side outputs) are not allowed.
// This mode is useful for backends backed by fixed kernels such as oneDNN
// primitives. To allow side outputs, pb_op.allow_external_output() is provided
// to override this behavior. The second mode auto exports unhandled external
// inputs and outputs.
// Pattern matcher has two different mode/way of handling unmatched graph op
// edges.
// - Order of external inputs and outputs returned by matcher is implementation
// dependent. (Port numbers provided by create_input_port() and
// create_output_port() may be used to enforce ordering for fixed patterns from
// a flat pattern graph. But the idea is not practical in general. For example,
// nested patterns may have variable number of side inputs so fixed ordering
// cannot be enforced.)
// - In case a match has multiple aliased external inputs, they are not merged
// and matcher reports them as separate inputs.
//

//
// Leaf pattern ops can be created by passing dnnl_graph op_kind.
// External inputs and outputs of a match will be ordered and
// exposed as part of the match. The order depends on matcher
// implementation.
//
TEST(PatternMatcherV2, GraphAppendLeafOp) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    // Grow internal graph
    // Leaf pattern op "Add"
    auto op0 = graphp->append_op(Add, "padd");
    ASSERT_NE(graphp, nullptr);
    ASSERT_NE(op0, nullptr);
}

//
// Convolution + BiasAdd
// A vector of all in coming edges to the new op can passed to
// append_op for non leaf pattern ops
//
TEST(PatternMatcherV2, GraphAppendNonLeafOp) {
    auto graphp = std::make_shared<pb_graph_t>("conv_bias");
    // Grow internal graph
    // Convolution -> BiasAdd
    // Leaf pattern op
    auto op0 = graphp->append_op(Convolution, "pconv");
    // Non leaf pattern op "BiasAdd" with only one of the inputs constrained
    // input 0 is constrained to output 0 of "Abs" op
    // unconstrained input is like matching "Any"
    // input 1 is free to match any op
    auto op1 = graphp->append_op(BiasAdd, {in_edge(IN0, op0, OUT0)}, "pbias");
    // Make sure that input1 to "BiasAdd" node does not come from within
    // the matched pattern
    ASSERT_NE(op1->get_producer(IN0), nullptr);
    ASSERT_EQ(op1->get_producer(IN0)->first, op0);
    ASSERT_EQ(op1->get_producer(IN0)->second, OUT0);
    ASSERT_EQ(op0->get_consumers(OUT0)->at(0)->first, op1);
    ASSERT_EQ(op0->get_consumers(OUT0)->at(0)->second, IN0);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t bias {1, BiasAdd, "bias"};
    op_t relu {2, ReLU, "relu"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    bias.add_input(lt_vec[2]);
    bias.add_input(lt_vec[3]);
    bias.add_output(lt_vec[4]);
    relu.add_input(lt_vec[4]);
    relu.add_output(lt_vec[5]);
    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&bias), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);

    // matched dnnl_graph_op will be marked
    for (auto &p : fusion_ops) {
        ASSERT_TRUE(p->get_attr<bool>(op_attr::matched));
    }
}

TEST(PatternMatcherV2, GraphNoAllowSideOutput) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto op0 = graphp->append_op(Convolution, "pconv");
    auto op1 = graphp->append_op(BiasAdd, {in_edge(IN0, op0, OUT0)}, "pbias");
    UNUSED(op1);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t bias {1, BiasAdd, "bias"};
    op_t relu {2, ReLU, "relu"};
    op_t add {3, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(8);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    bias.add_input(lt_vec[2]);
    bias.add_input(lt_vec[3]);
    bias.add_output(lt_vec[4]);
    relu.add_input(lt_vec[4]);
    relu.add_output(lt_vec[5]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[6]);
    add.add_output(lt_vec[7]);
    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&bias), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    op_t *internal_op = agraph.get_ops()[0].get();
    EXPECT_FALSE(match_pattern(internal_op, graphp, fusion_ops));
}

TEST(PatternMatcherV2, ConvAddFusion) {
    // conv + add fusion
    std::shared_ptr<pb_graph_t> pattern_graph
            = std::make_shared<pb_graph_t>("pgraph");

    auto pconv = pattern_graph->append_op(Convolution, "pconv");
    auto padd = pattern_graph->append_op(Add,
            {in_edge(IN0, pconv, OUT0), in_edge(IN1, pconv, OUT0)}, "padd");
    UNUSED(padd);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t add {1, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(4);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_output(lt_vec[3]);
    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(
            agraph.get_ops()[0].get(), pattern_graph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, FailToFuseConvAdd) {
    // conv = add fusion
    std::shared_ptr<pb_graph_t> pattern_graph
            = std::make_shared<pb_graph_t>("pgraph");

    auto pconv = pattern_graph->append_op(Convolution, "pconv");
    auto padd = pattern_graph->append_op(
            Add, {in_edge(IN0, pconv, OUT0)}, "padd");
    UNUSED(padd);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t add {1, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(4);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_output(lt_vec[3]);
    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(
            agraph.get_ops()[0].get(), pattern_graph, fusion_ops));
}

TEST(PatternMatcherV2, ConvAddFusionCase2) {
    std::shared_ptr<pb_graph_t> pattern_graph = std::make_shared<pb_graph_t>();

    auto pconv = pattern_graph->append_op(Convolution, "pconv");
    auto padd = pattern_graph->append_op(
            Add, {in_edge(IN0, pconv, OUT0)}, "padd");
    UNUSED(padd);

    graph_t agraph1;
    op_t conv0 {0, Convolution, "conv0"};
    set_conv_common_attr(conv0);
    op_t conv1 {1, Convolution, "conv1"};
    set_conv_common_attr(conv1);
    op_t add1 {2, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    conv0.add_input(lt_vec[0]);
    conv0.add_input(lt_vec[1]);
    conv0.add_output(lt_vec[2]);
    conv1.add_input(lt_vec[2]);
    conv1.add_input(lt_vec[3]);
    conv1.add_output(lt_vec[4]);
    add1.add_input(lt_vec[2]);
    add1.add_input(lt_vec[4]);
    add1.add_output(lt_vec[5]);
    ASSERT_EQ(agraph1.add_op(&conv0), status::success);
    ASSERT_EQ(agraph1.add_op(&conv1), status::success);
    ASSERT_EQ(agraph1.add_op(&add1), status::success);
    agraph1.build_graph();
    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(
            agraph1.get_ops()[0].get(), pattern_graph, fusion_ops));
    fusion_ops.clear();

    EXPECT_TRUE(match_pattern(
            agraph1.get_ops()[1].get(), pattern_graph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, CommutativeInputBothConstrained) {
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    std::shared_ptr<pb_graph_t> pattern_graph
            = std::make_shared<pb_graph_t>("pgraph");

    auto pconv = pattern_graph->append_op(Convolution, "pconv");
    auto pelu = pattern_graph->append_op(
            Elu, {in_edge(IN0, pconv, OUT0)}, "pelu");
    auto pabsnode = pattern_graph->append_op(
            Abs, {in_edge(IN0, pconv, OUT0)}, "pabs");
    auto padd = pattern_graph->append_op(Add,
            {in_edge(IN0, pelu, OUT0), in_edge(IN1, pabsnode, OUT0)}, "padd");
    UNUSED(padd);

    for (size_t elu_offset : {0, 1}) {
        graph_t agraph;
        op_t conv {0, Convolution, "conv"};
        set_conv_common_attr(conv);
        op_t elu {1, Elu, "elu"};
        elu.set_attr<float>(op_attr::alpha, 0.1);
        op_t abs {2, Abs, "abs"};
        op_t add {3, Add, "add"};
        std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
        conv.add_input(lt_vec[0]);
        conv.add_input(lt_vec[1]);
        conv.add_output(lt_vec[2]);
        elu.add_input(lt_vec[2]);
        elu.add_output(lt_vec[3]);
        abs.add_input(lt_vec[2]);
        abs.add_output(lt_vec[4]);
        if (elu_offset == 0) {
            add.add_input(lt_vec[3]);
            add.add_input(lt_vec[4]);
        } else {
            add.add_input(lt_vec[4]);
            add.add_input(lt_vec[3]);
        }
        add.add_output(lt_vec[5]);
        ASSERT_EQ(agraph.add_op(&conv), status::success);
        ASSERT_EQ(agraph.add_op(&elu), status::success);
        ASSERT_EQ(agraph.add_op(&abs), status::success);
        ASSERT_EQ(agraph.add_op(&add), status::success);
        agraph.build_graph();

        std::vector<op_t *> fusion_ops;
        EXPECT_TRUE(match_pattern(
                agraph.get_ops()[0].get(), pattern_graph, fusion_ops));
        ASSERT_EQ(fusion_ops.size(), 4U);
    }
}

TEST(PatternMatcherV2, CommutativeInput) {
    std::shared_ptr<pb_graph_t> pattern_graph
            = std::make_shared<pb_graph_t>("pgraph");
    auto pconv0 = pattern_graph->append_op(Convolution, "pconv0");
    pconv0->append_decision_function(
            [](op_t *o) -> bool { return o->num_inputs() == 3; });
    auto pconv1 = pattern_graph->append_op(Convolution, "pconv1");
    auto prelu0 = pattern_graph->append_op(
            ReLU, {in_edge(IN0, pconv0, OUT0)}, "prelu0");
    auto prelu1 = pattern_graph->append_op(
            ReLU, {in_edge(IN0, pconv1, OUT0)}, "prelu1");
    auto padd = pattern_graph->append_op(Add,
            {in_edge(IN0, prelu0, OUT0), in_edge(IN1, prelu1, OUT0)}, "padd");
    UNUSED(padd);

    graph_t agraph;
    op_t conv0 {0, Convolution, "conv0"};
    set_conv_common_attr(conv0);
    op_t conv1 {1, Convolution, "conv1"};
    set_conv_common_attr(conv1);
    op_t relu0 {2, ReLU, "relu0"};
    op_t relu1 {3, ReLU, "relu1"};
    op_t add {4, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(10);
    conv0.add_input(lt_vec[0]);
    conv0.add_input(lt_vec[1]);
    conv0.add_output(lt_vec[2]);
    relu0.add_input(lt_vec[2]);
    relu0.add_output(lt_vec[3]);
    conv1.add_input(lt_vec[4]);
    conv1.add_input(lt_vec[5]);
    conv1.add_input(lt_vec[6]);
    conv1.add_output(lt_vec[7]);
    relu1.add_input(lt_vec[7]);
    relu1.add_output(lt_vec[8]);
    add.add_input(lt_vec[3]);
    add.add_input(lt_vec[8]);
    add.add_output(lt_vec[9]);
    ASSERT_EQ(agraph.add_op(&conv0), status::success);
    ASSERT_EQ(agraph.add_op(&relu0), status::success);
    ASSERT_EQ(agraph.add_op(&conv1), status::success);
    ASSERT_EQ(agraph.add_op(&relu1), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(
            agraph.get_ops()[0].get(), pattern_graph, fusion_ops));
    fusion_ops.clear();
    EXPECT_TRUE(match_pattern(
            agraph.get_ops()[2].get(), pattern_graph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 5U);
}

//
// Convolution + BiasAdd + Elu
// Convolution + BiasAdd + Sigmoid
// Convolution + BiasAdd + ReLU
// Convolution + BiasAdd + Clamp
// Convolution + BiasAdd + Square
// Convolution + BiasAdd + Tanh
// Convolution + BiasAdd + Sqrt
//
TEST(PatternMatcherV2, ConvBiasActivationFusion) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pconv = graphp->append_op(Convolution, "pconv");
    auto pbias
            = graphp->append_op(BiasAdd, {in_edge(IN0, pconv, OUT0)}, "pbias");
    auto pact = graphp->append_alternation(
            {Elu, Sigmoid, ReLU, Clamp, Square, Tanh, Sqrt},
            {in_edge(IN0, pbias, OUT0)}, "pactivation");
    UNUSED(pact);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t bias {1, BiasAdd, "bias"};
    op_t relu {2, ReLU, "relu"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    bias.add_input(lt_vec[2]);
    bias.add_input(lt_vec[3]);
    bias.add_output(lt_vec[4]);
    relu.add_input(lt_vec[4]);
    relu.add_output(lt_vec[5]);

    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&bias), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

//
// Convolution + BiasAdd + Add + ReLU
// Convolution + BiasAdd + Add + ELU
//
TEST(PatternMatcherV2, ConvBiasSumActivationFusion) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pconv = graphp->append_op(Convolution, "pconv");
    auto pbias
            = graphp->append_op(BiasAdd, {in_edge(IN0, pconv, OUT0)}, "pbias");
    auto padd = graphp->append_op(Add, {in_edge(IN0, pbias, OUT0)}, "padd");
    auto pact = graphp->append_alternation(
            {Elu, ReLU}, {in_edge(IN0, padd, OUT0)}, "pactivation");
    UNUSED(pact);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t bias {1, BiasAdd, "bias"};
    op_t add {2, Add, "add"};
    op_t elu {3, Elu, "elu"};
    elu.set_attr<float>(op_attr::alpha, 0.1);

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(8);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    bias.add_input(lt_vec[2]);
    bias.add_input(lt_vec[3]);
    bias.add_output(lt_vec[4]);
    // Force check commutative input
    add.add_input(lt_vec[5]);
    add.add_input(lt_vec[4]);
    add.add_output(lt_vec[6]);
    elu.add_input(lt_vec[6]);
    elu.add_output(lt_vec[7]);

    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&bias), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    ASSERT_EQ(agraph.add_op(&elu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 4U);
}

//
// MatMul + BiasAdd + Add
//
TEST(PatternMatcherV2, MatmulBiasSumFusion) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pmatmul = graphp->append_op(MatMul, "pmatmul");
    auto pbias = graphp->append_op(
            BiasAdd, {in_edge(IN0, pmatmul, OUT0)}, "pbias");
    auto padd = graphp->append_op(Add, {in_edge(IN0, pbias, OUT0)}, "padd");
    UNUSED(padd);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t bias {1, BiasAdd, "bias"};
    op_t add {2, Add, "add"};
    op_t relu {3, ReLU, "relu"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(8);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    bias.add_input(lt_vec[2]);
    bias.add_input(lt_vec[3]);
    bias.add_output(lt_vec[4]);
    add.add_input(lt_vec[5]);
    add.add_input(lt_vec[4]);
    add.add_output(lt_vec[6]);
    relu.add_input(lt_vec[6]);
    relu.add_output(lt_vec[7]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&bias), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

//
// MatMul + ReLU
// MatMul + Elu
// MatMul + GELU
// MatMul + Sigmoid
// MatMul + Clamp
//
TEST(PatternMatcherV2, MatmulActivationFusion) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pmat = graphp->append_op(MatMul, "pmatmul");
    auto pact = graphp->append_alternation(
            {ReLU, Elu, GELU, Sigmoid, Clamp}, {in_edge(IN0, pmat, OUT0)});
    UNUSED(pact);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    op_t add {2, Add, "add"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);
    add.add_input(lt_vec[3]);
    add.add_input(lt_vec[4]);
    add.add_output(lt_vec[5]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(agraph.get_ops()[1].get(), graphp, fusion_ops));
    fusion_ops.clear();
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, ConvSwishFusion) {
    // conv_swish pass
    //   conv
    //   |   |
    //   | sigmoid
    //   |   |
    // multiply

    std::shared_ptr<pb_graph_t> pattern_graph
            = std::make_shared<pb_graph_t>("pgraph");
    auto pconv = pattern_graph->append_op(Convolution, "pconv");
    auto psigmoid = pattern_graph->append_op(
            Sigmoid, {in_edge(IN0, pconv, OUT0)}, "psigmoid");
    in_edges_t mul_edges
            = {in_edge(IN0, pconv, OUT0), in_edge(IN1, psigmoid, OUT0)};
    auto pmul = pattern_graph->append_op(Multiply, mul_edges, "pmul");
    UNUSED(pmul);

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t sigmoid {1, Sigmoid, "sigmoid"};
    op_t multiply {2, Multiply, "multiply"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(5);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    sigmoid.add_input(lt_vec[2]);
    sigmoid.add_output(lt_vec[3]);
    // Force check commutative input
    multiply.add_input(lt_vec[3]);
    multiply.add_input(lt_vec[2]);
    multiply.add_output(lt_vec[4]);

    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&sigmoid), status::success);
    ASSERT_EQ(agraph.add_op(&multiply), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(
            agraph.get_ops()[0].get(), pattern_graph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, ConvSumEltwiseFusion) {
    // conv + sum + (Relu / Elu / Clamp / Square / Tanh / Abs / Sqrt)
    std::shared_ptr<pb_graph_t> pattern_graph
            = std::make_shared<pb_graph_t>("pgraph");
    auto pconv = pattern_graph->append_op(Convolution, "pconv");
    auto padd = pattern_graph->append_op(
            Add, {in_edge(IN0, pconv, OUT0)}, "padd");

    std::shared_ptr<pb_graph_t> optional_act
            = std::make_shared<pb_graph_t>("poptionalbody");
    auto pact = optional_act->append_alternation(
            {Elu, ReLU, Square, Tanh, Abs, Sqrt, Clamp}, "pactivation");
    optional_act->create_input_port(IN0, pact, IN0);
    optional_act->create_output_port(OUT0, pact, OUT0);
    pattern_graph->append_optional(
            optional_act, {in_edge(IN0, padd, OUT0)}, "poptional");

    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t add {1, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(5);
    conv.add_input(lt_vec[0]);
    conv.add_input(lt_vec[1]);
    conv.add_output(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[3]);
    add.add_output(lt_vec[4]);
    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(
            agraph.get_ops()[0].get(), pattern_graph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

//
// Alternation, Repetition, Optional are nested pattern nodes
// that has a body(s) of graph.
// Input and Output ports of those nested patterns get mapped to
// the corresponding port (same index) of the body.
// If you need to change that mapping, wrap the body in a graph
// and use create_input_port/create_output_port to change the
// mapping.
//

//
// Alternation node wraps two or more alternatives and
// constructed with append_alternation.
// Input or Output "n" of the alternation node connects to
// Input of Output "n" of the alternative.
//
TEST(PatternMatcherV2, Alternation) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    // MatMul -> (Add | Multiply)
    auto pmatmul = graphp->append_op(MatMul, "pmatmul");

    // Prepare the alternative graphs
    auto addgraph = std::make_shared<pb_graph_t>("paddgraph");
    auto padd = addgraph->append_op(Add, "padd");
    addgraph->create_input_port(IN0, padd, IN0);
    addgraph->create_input_port(IN1, padd, IN1);
    addgraph->create_output_port(OUT0, padd, OUT0);
    auto mulgraph = std::make_shared<pb_graph_t>("pmulgraph");
    auto pmul = mulgraph->append_op(Multiply, "pmul");
    mulgraph->create_input_port(IN0, pmul, IN0);
    mulgraph->create_input_port(IN1, pmul, IN1);
    mulgraph->create_output_port(OUT0, pmul, OUT0);
    // We can add a helper function like
    // single_op_graph(op_kind);
    // that create a new graph add a single node and sets
    // inner consumer and producers.

    auto palt = graphp->append_alternation({addgraph, mulgraph},
            {in_edge(IN0, pmatmul, OUT0)}, "palternation");
    UNUSED(palt);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t add {1, Add, "add"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(5);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[3]);
    add.add_output(lt_vec[4]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(agraph.get_ops()[1].get(), graphp, fusion_ops));
    fusion_ops.clear();
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, AlternationWithConsumer) {
    /*
    pattern:
          matmul
            |
(softmax + relu) | (relu + softmax)
            |
          matmul
    graph:
         matmul
           |
         softmax
           |
          relu
           |
         matmul
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pmatmul = graphp->append_op(impl::op_kind::MatMul, "pmatmul");
    auto alter1 = std::make_shared<pb_graph_t>("alter1");
    auto psoftmax1 = alter1->append_op(impl::op_kind::SoftMax, "psoftmax1");
    auto prelu1 = alter1->append_op(
            impl::op_kind::ReLU, {in_edge(0, psoftmax1, 0)}, "prelu1");
    alter1->create_input_port(0, psoftmax1, 0);
    alter1->create_output_port(0, prelu1, 0);
    auto alter2 = std::make_shared<pb_graph_t>("alter2");
    auto prelu2 = alter2->append_op(impl::op_kind::ReLU, "prelu2");
    auto psoftmax2 = alter2->append_op(
            impl::op_kind::SoftMax, {in_edge(0, prelu2, 0)}, "psoftmax2");
    alter2->create_input_port(0, prelu2, 0);
    alter2->create_output_port(0, psoftmax2, 0);
    auto palter = graphp->append_alternation(
            {alter1, alter2}, {in_edge(0, pmatmul, 0)}, "palter");
    auto pmatmul2 = graphp->append_op(
            impl::op_kind::MatMul, {in_edge(0, palter, 0)}, "pmatmul2");
    UNUSED(pmatmul2);

    graph_t agraph;
    op_t matmul0 {0, MatMul, "matmul0"};
    op_t softmax {1, SoftMax, "softmax"};
    op_t relu {2, ReLU, "relu"};
    op_t matmul1 {3, MatMul, "matmul1"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(7);
    matmul0.add_input(lt_vec[0]);
    matmul0.add_input(lt_vec[1]);
    matmul0.add_output(lt_vec[2]);
    softmax.add_input(lt_vec[2]);
    softmax.add_output(lt_vec[3]);
    relu.add_input(lt_vec[3]);
    relu.add_output(lt_vec[4]);
    matmul1.add_input(lt_vec[4]);
    matmul1.add_input(lt_vec[5]);
    matmul1.add_output(lt_vec[6]);

    ASSERT_EQ(agraph.add_op(&matmul0), status::success);
    ASSERT_EQ(agraph.add_op(&softmax), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    // should match the 1st rep_unit
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    EXPECT_EQ(fusion_ops.size(), 4U);
}

//
// Repetition node wraps body that gets repeated a
// number of times specified by a range and constructed with
// append_repetition.
// The body repeats inself by connecting edges through an
// output port to input port mapping.
// The mapping has to be given as an argument to append_repetition.
//
TEST(PatternMatcherV2, Repetition) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    // Pattern that captures
    // MatMul -> (Add | Multiply) -> ReLU
    // MatMul -> (Add | Multiply) -> (Add | Multiply) -> ReLU
    auto pmatmul = graphp->append_op(MatMul, "pmatmul");
    auto repbody = std::make_shared<pb_graph_t>("prepetitionbody");
    auto paddormul = repbody->append_alternation({Add, Multiply}, "paddormul");
    repbody->create_input_port(IN0, paddormul, IN0);
    // No need to create IN1 for the body since it is not connected to
    // an outer pattern.
    // repbody->create_input_port(IN1, addormul, IN1);
    repbody->create_output_port(OUT0, paddormul, OUT0);

    // Repeat 1 or 2 times [1, 3) by mapping OUT0 back to IN0
    auto rep = graphp->append_repetition(repbody, {OUT0, IN0}, 1, 3,
            {in_edge(IN0, pmatmul, OUT0)}, "prepetition");
    auto prelu = graphp->append_op(ReLU, {in_edge(IN0, rep, OUT0)}, "prelu");
    UNUSED(prelu);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t add {1, Add, "add"};
    op_t mul {2, Multiply, "mul"};
    op_t relu {3, ReLU, "relu"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(8);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[3]);
    add.add_output(lt_vec[4]);
    mul.add_input(lt_vec[4]);
    mul.add_input(lt_vec[5]);
    mul.add_output(lt_vec[6]);
    relu.add_input(lt_vec[6]);
    relu.add_output(lt_vec[7]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    ASSERT_EQ(agraph.add_op(&mul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 4U);
}

TEST(PatternMatcherV2, RepetitionFail) {
    /* 
    Pattern:
     MatMul
       \    /
      [Add/Div]*[1,3]

     Graph:
          MatMul
            \   /
             Add
          \  /
          Div
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pmatmul = graphp->append_op(MatMul, "pmatmul");
    auto repbody = std::make_shared<pb_graph_t>("prepetitionbody");
    auto paddordiv = repbody->append_alternation({Add, Divide}, "paddormul");
    repbody->create_input_port(IN0, paddordiv, IN0);
    repbody->create_output_port(OUT0, paddordiv, OUT0);

    graphp->append_repetition(repbody, {OUT0, IN0}, 2, 3,
            {in_edge(IN0, pmatmul, OUT0)}, "prepetition");

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t add {1, Add, "add"};
    op_t div {2, Divide, "div"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(7);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    add.add_input(lt_vec[2]);
    add.add_input(lt_vec[3]);
    add.add_output(lt_vec[4]);
    // incorrect order for div
    div.add_input(lt_vec[5]);
    div.add_input(lt_vec[4]);
    div.add_output(lt_vec[6]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    ASSERT_EQ(agraph.add_op(&div), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
}

//
// "Optional" is a special case of repetition that repeats one or zero times
// and constructed with append_optional.
// output to input port mapping isn't needed since the body does not repeat
// more than once.
//
TEST(PatternMatcherV2, Optional) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    // Pattern that captures
    // MatMul -> ReLU
    // MatMul -> (Add | Multiply) -> ReLU
    auto pmatmul = graphp->append_op(MatMul, "pmatmul");
    auto repbody = std::make_shared<pb_graph_t>("poptionalbody");
    auto paddormul = repbody->append_alternation({Add, Multiply}, "paddormul");
    repbody->create_input_port(IN0, paddormul, IN0);
    repbody->create_output_port(OUT0, paddormul, OUT0);
    auto rep = graphp->append_optional(
            repbody, {in_edge(IN0, pmatmul, OUT0)}, "poptional");
    auto prelu = graphp->append_op(ReLU, {in_edge(IN0, rep, OUT0)}, "prelu");
    UNUSED(prelu);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(4);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);

    graph_t agraph2;
    op_t matmul2 {0, MatMul, "matmul"};
    op_t add2 {1, Add, "add"};
    op_t relu2 {2, ReLU, "relu"};

    std::vector<logical_tensor_t> lt_vec2 = create_logical_tensors(6);
    matmul2.add_input(lt_vec2[0]);
    matmul2.add_input(lt_vec2[1]);
    matmul2.add_output(lt_vec2[2]);
    add2.add_input(lt_vec2[2]);
    add2.add_input(lt_vec2[3]);
    add2.add_output(lt_vec2[4]);
    relu2.add_input(lt_vec2[4]);
    relu2.add_output(lt_vec2[5]);

    ASSERT_EQ(agraph2.add_op(&matmul2), status::success);
    ASSERT_EQ(agraph2.add_op(&add2), status::success);
    ASSERT_EQ(agraph2.add_op(&relu2), status::success);
    agraph2.build_graph();

    fusion_ops.clear();
    EXPECT_TRUE(match_pattern(agraph2.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

//
// ?: means optional
// ^: means repetition
// Conv+(BN)?+ReLU
// Conv+(BN)?+ReLU+Add
// Conv+(BN)?+ReLU+Conv+(BN)?+ReLU+Conv+(BN)?+ReLU+Add
//
// Conv+(BN)?+ReLU+(((Conv+(BN)?+ReLU)^2)?+Add)?
//
// Note that each "()" requires an addition pb_graph.
// So for this example, we need 1 + 5 = 6 pb_graphs.
//
// Since this example is not a fixed pattern and has
// variable number of side inputs, we cannot use
// create_input_port to setup globbal ordering for inputs.
//
// create_input_port/create_output_port is still needed for
// setting up the contact interface for nested patterns.
//
TEST(PatternMatcherV2, ComplexRepetition) {
    auto graphp = std::make_shared<pb_graph_t>("pmaingraph");
    // Basic building block
    // Convolution + (BatchNormInference)? + ReLU

    // Conv
    auto pconv = graphp->append_op(Convolution, "pconv1");
    // Optional BN
    auto body = std::make_shared<pb_graph_t>("poptional1body");
    auto pbn = body->append_op(BatchNormInference, "pbn1");
    // Interface for body
    body->create_input_port(IN0, pbn, IN0);
    body->create_output_port(OUT0, pbn, OUT0);
    auto popt = graphp->append_optional(
            body, {in_edge(IN0, pconv, OUT0)}, "poptional1");
    // ReLU
    auto prelu = graphp->append_op(ReLU, {in_edge(IN0, popt, OUT0)}, "prelu1");
    // Create same block to use as repetition body
    auto graphp2 = std::make_shared<pb_graph_t>("prepetitionbody");
    auto pconv2 = graphp2->append_op(Convolution, "pconv2");
    auto body2 = std::make_shared<pb_graph_t>("poptional2body");
    auto pbn2 = body2->append_op(BatchNormInference, "pbn2");
    // Interface for body2
    body2->create_input_port(IN0, pbn2, IN0);
    body2->create_output_port(OUT0, pbn2, OUT0);
    auto popt2 = graphp2->append_optional(
            body2, {in_edge(IN0, pconv2, OUT0)}, "poptional2");
    auto prelu2
            = graphp2->append_op(ReLU, {in_edge(IN0, popt2, OUT0)}, "prelu2");
    // Interface for graphp2
    graphp2->create_input_port(IN0, pconv2, IN0);
    graphp2->create_output_port(OUT0, prelu2, OUT0);

    // repeat body exactly two times
    auto graphp3 = std::make_shared<pb_graph_t>("poptional3");
    auto prep = graphp3->append_repetition(
            graphp2, {OUT0, IN0}, 2, 3, "prepetition");
    // Interface for graphp3
    graphp3->create_input_port(IN0, prep, IN0);
    graphp3->create_output_port(OUT0, prep, OUT0);

    // optional repeated body followed by an "Add"
    auto graphp4 = std::make_shared<pb_graph_t>("poptional4body");
    auto popt3 = graphp4->append_optional(graphp3, "poptional3");
    auto padd = graphp4->append_op(Add, {in_edge(IN0, popt3, OUT0)}, "padd");
    // Interface for graphp4
    graphp4->create_input_port(IN0, popt3, IN0);
    graphp4->create_output_port(OUT0, padd, OUT0);

    // Append the complex pattern to relu
    auto popt4 = graphp->append_optional(
            graphp4, {in_edge(IN0, prelu, OUT0)}, "poptional4");
    UNUSED(popt4);

    graph_t agraph;
    op_t conv1 {0, Convolution, "conv1"};
    set_conv_common_attr(conv1);
    op_t relu1 {1, ReLU, "relu1"};
    op_t conv2 {2, Convolution, "conv2"};
    set_conv_common_attr(conv2);
    op_t relu2 {3, ReLU, "relu2"};
    op_t conv3 {4, Convolution, "conv3"};
    set_conv_common_attr(conv3);
    op_t relu3 {5, ReLU, "relu3"};
    op_t add {6, Add, "add"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(12);
    conv1.add_input(lt_vec[0]);
    conv1.add_input(lt_vec[1]);
    conv1.add_output(lt_vec[2]);
    relu1.add_input(lt_vec[2]);
    relu1.add_output(lt_vec[3]);
    conv2.add_input(lt_vec[3]);
    conv2.add_input(lt_vec[4]);
    conv2.add_output(lt_vec[5]);
    relu2.add_input(lt_vec[5]);
    relu2.add_output(lt_vec[6]);
    conv3.add_input(lt_vec[6]);
    conv3.add_input(lt_vec[7]);
    conv3.add_output(lt_vec[8]);
    relu3.add_input(lt_vec[8]);
    relu3.add_output(lt_vec[9]);
    add.add_input(lt_vec[9]);
    add.add_input(lt_vec[10]);
    add.add_output(lt_vec[11]);
    ASSERT_EQ(agraph.add_op(&conv1), status::success);
    ASSERT_EQ(agraph.add_op(&relu1), status::success);
    ASSERT_EQ(agraph.add_op(&conv2), status::success);
    ASSERT_EQ(agraph.add_op(&relu2), status::success);
    ASSERT_EQ(agraph.add_op(&conv3), status::success);
    ASSERT_EQ(agraph.add_op(&relu3), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 7U);

    graph_t agraph2;
    op_t conv4 {0, Convolution, "conv4"};
    set_conv_common_attr(conv4);
    op_t bn {1, BatchNormInference, "bn"};
    bn.set_attr(op_attr::epsilon, 0.001f);

    lt_vec = create_logical_tensors(8);
    conv4.add_input(lt_vec[0]);
    conv4.add_input(lt_vec[1]);
    conv4.add_output(lt_vec[2]);
    bn.add_input(lt_vec[2]);
    bn.add_input(lt_vec[3]);
    bn.add_input(lt_vec[4]);
    bn.add_input(lt_vec[5]);
    bn.add_input(lt_vec[6]);
    bn.add_output(lt_vec[7]);
    ASSERT_EQ(agraph2.add_op(&conv4), status::success);
    ASSERT_EQ(agraph2.add_op(&bn), status::success);
    agraph2.build_graph();

    fusion_ops.clear();
    EXPECT_FALSE(match_pattern(agraph2.get_ops()[0].get(), graphp, fusion_ops));

    graph_t agraph3;
    op_t conv5 {0, Convolution, "conv5"};
    set_conv_common_attr(conv5);
    op_t relu5 {1, ReLU, "relu4"};
    op_t add2 {2, Add, "add2"};
    lt_vec = create_logical_tensors(6);
    conv5.add_input(lt_vec[0]);
    conv5.add_input(lt_vec[1]);
    conv5.add_output(lt_vec[2]);
    relu5.add_input(lt_vec[2]);
    relu5.add_output(lt_vec[3]);
    add2.add_input(lt_vec[3]);
    add2.add_input(lt_vec[4]);
    add2.add_output(lt_vec[5]);
    ASSERT_EQ(agraph3.add_op(&conv5), status::success);
    ASSERT_EQ(agraph3.add_op(&relu5), status::success);
    ASSERT_EQ(agraph3.add_op(&add2), status::success);
    agraph3.build_graph();

    fusion_ops.clear();
    EXPECT_TRUE(match_pattern(agraph3.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, ParallelMatmul) {
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    // Pattern that captures shared input to three MatMuls
    //            |--> MatMul
    //   Wildcard ----> MatMul
    //            |--> MatMul
    auto pwild = graphp->append_op(Wildcard, "pwild");
    auto pmm1 = graphp->append_op(
            MatMul, {in_edge(IN0, pwild, OUT0)}, "pmatmul1");
    auto pmm2 = graphp->append_op(
            MatMul, {in_edge(IN0, pwild, OUT0)}, "pmatmul2");
    auto pmm3 = graphp->append_op(
            MatMul, {in_edge(IN0, pwild, OUT0)}, "pmatmul3");
    UNUSED(pmm1);
    UNUSED(pmm2);
    UNUSED(pmm3);

    graph_t agraph;
    op_t relu {4, ReLU, "relu"};
    op_t matmul1 {0, MatMul, "matmul1"};
    op_t matmul2 {1, MatMul, "matmul2"};
    op_t matmul3 {2, MatMul, "matmul3"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(8);
    relu.add_input(lt_vec[7]);
    relu.add_output(lt_vec[0]);
    matmul1.add_input(lt_vec[0]);
    matmul1.add_input(lt_vec[1]);
    matmul1.add_output(lt_vec[2]);
    matmul2.add_input(lt_vec[0]);
    matmul2.add_input(lt_vec[3]);
    matmul2.add_output(lt_vec[4]);
    matmul3.add_input(lt_vec[0]);
    matmul3.add_input(lt_vec[5]);
    matmul3.add_output(lt_vec[6]);

    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);
    ASSERT_EQ(agraph.add_op(&matmul2), status::success);
    ASSERT_EQ(agraph.add_op(&matmul3), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, OptionalInput) {
    /*Pattern                  Graph
     Dq0     Dq1            Dq0     Dq1
      |      |               |       |
      |   [Reshape]*         |       |
       \    /                 \     /
       MatMul                 MatMul
         |                       |
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pdq0 = graphp->append_op(Dequantize, "pdq0");
    auto pdq1 = graphp->append_op(Dequantize, "pdq1");
    auto optbody = std::make_shared<pb_graph_t>("poptionalbody");
    auto preshape = optbody->append_op(StaticReshape, "preshape");
    optbody->create_input_port(IN0, preshape, IN0);
    optbody->create_output_port(OUT0, preshape, OUT0);
    auto popt = graphp->append_optional(
            optbody, {in_edge(IN0, pdq1, OUT0)}, "poptional");
    auto pmatmul = graphp->append_op(MatMul,
            {in_edge(IN0, pdq0, OUT0), in_edge(IN1, popt, OUT0)}, "prelu");
    UNUSED(pmatmul);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    std::vector<int64_t> zps = {0};
    std::vector<float> scales = {3.1f};
    op_t dq0 {1, Dequantize, "dq0"};
    dq0.set_attr(op_attr::scales, scales);
    dq0.set_attr(op_attr::zps, zps);
    op_t dq1 {2, Dequantize, "dq1"};
    dq1.set_attr(op_attr::scales, scales);
    dq1.set_attr(op_attr::zps, zps);

    auto lt0 = logical_tensor_init(0, impl::data_type::s8);
    auto lt1 = logical_tensor_init(1, impl::data_type::f32);
    dq0.add_input(lt0);
    dq0.add_output(lt1);
    auto lt2 = logical_tensor_init(2, impl::data_type::s8);
    auto lt3 = logical_tensor_init(3, impl::data_type::f32);
    dq1.add_input(lt2);
    dq1.add_output(lt3);
    auto lt4 = logical_tensor_init(4, impl::data_type::f32);
    matmul.add_input(lt1);
    matmul.add_input(lt3);
    matmul.add_output(lt4);

    ASSERT_EQ(agraph.add_op(&dq0), status::success);
    ASSERT_EQ(agraph.add_op(&dq1), status::success);
    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

//
// Construct a nested pattern:
// (NODE)* represents that NODE is wraped in repetition or optional
// (NODE1 | NODE2 | NODE3) represents alternation of NODE1, NODE2 and NODE3
// (Matmul -> (((ReLU | Sigmoid | Tanh)))*)*
//
TEST(PatternMatcherV2, NestedMatchingFailure) {
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");
    auto mlp_layer = std::make_shared<pb_graph_t>("pmlp");
    auto matmul_layer = mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    auto optional_activation_subgraph
            = std::make_shared<pb_graph_t>("poptional_activation_subgraph");
    auto activation = optional_activation_subgraph->append_alternation(
            {impl::op_kind::ReLU, impl::op_kind::Sigmoid, impl::op_kind::Tanh},
            "palternation");
    optional_activation_subgraph->create_input_port(0, activation, 0);
    optional_activation_subgraph->create_output_port(0, activation, 0);
    auto optional_activation
            = mlp_layer->append_optional(optional_activation_subgraph,
                    {in_edge(0, matmul_layer, 0)}, "poptional_activation");
    mlp_layer->create_input_port(0, matmul_layer, 0);
    mlp_layer->create_output_port(0, optional_activation, 0);
    pgraph->append_repetition(mlp_layer, {0, 0}, 1, 2, "prepetition");

    impl::graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(4);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), pgraph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, RepetitionWithMultipleConsumers) {
    /* pattern
       conv
        |
       relu x [1,3)
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pconv = graphp->append_op(Convolution, "pconv");
    auto repbody = std::make_shared<pb_graph_t>("prepetitionbody");
    auto prelu = repbody->append_op(ReLU, "prelu");
    repbody->create_input_port(IN0, prelu, IN0);
    repbody->create_output_port(OUT0, prelu, OUT0);
    graphp->append_repetition(repbody, {OUT0, IN0}, 1, 3,
            {in_edge(IN0, pconv, OUT0)}, "prepetition");

    /* graph
       conv
        |
       relu
        / \
   wildcard wildcard
    */
    graph_t agraph;
    op_t conv {0, Convolution, "conv"};
    set_conv_common_attr(conv);
    op_t relu {1, ReLU, "relu"};
    op_t wildcard1 {2, Wildcard, "w1"};
    op_t wildcard2 {3, Wildcard, "w2"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(8);
    conv.add_input(lt_vec[2]);
    conv.add_input(lt_vec[3]);
    conv.add_output(lt_vec[4]);
    relu.add_input(lt_vec[4]);
    relu.add_output(lt_vec[5]);
    wildcard1.add_input(lt_vec[5]);
    wildcard1.add_output(lt_vec[6]);
    wildcard2.add_input(lt_vec[5]);
    wildcard2.add_output(lt_vec[7]);

    ASSERT_EQ(agraph.add_op(&conv), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard1), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard2), status::success);
    agraph.build_graph();
    ASSERT_EQ(agraph.num_ops(), 4U);

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, MultipleConsumer) {
    /*Pattern
     Transpose
      /     \____________
   Matmul               /
                     MatMul
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto trans = graphp->append_op(StaticTranspose, "trans");
    auto mat1 = graphp->append_op(MatMul, {in_edge(IN1, trans, OUT0)}, "mat1");
    auto mat2 = graphp->append_op(MatMul, {in_edge(IN1, trans, OUT0)}, "mat2");
    UNUSED(mat1);
    UNUSED(mat2);

    graph_t agraph;
    op_t transpose {0, StaticTranspose, "transpose"};
    transpose.set_attr(op_attr::order, std::vector<int64_t> {0, 2, 1, 3});
    op_t matmul1 {1, MatMul, "matmul1"};
    op_t matmul2 {2, MatMul, "matmul2"};

    auto lt0 = logical_tensor_init(0, impl::data_type::f32);
    auto lt1 = logical_tensor_init(1, impl::data_type::f32);
    transpose.add_input(lt0);
    transpose.add_output(lt1);
    auto lt2 = logical_tensor_init(2, impl::data_type::f32);
    auto lt3 = logical_tensor_init(3, impl::data_type::f32);
    matmul1.add_input(lt2);
    matmul1.add_input(lt1);
    matmul1.add_output(lt3);
    auto lt4 = logical_tensor_init(4, impl::data_type::f32);
    auto lt5 = logical_tensor_init(5, impl::data_type::f32);
    matmul2.add_input(lt4);
    matmul2.add_input(lt1);
    matmul2.add_output(lt5);

    ASSERT_EQ(agraph.add_op(&transpose), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);
    ASSERT_EQ(agraph.add_op(&matmul2), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, MultipleConsumerDifferentPartition) {
    /*Pattern
     Matmul
      |
     Div
      |
     Add
      |
   SoftMax
      |
     Mul
    */
    /*Graph

    \   /
    Matmul
      |
     Div
      |
     Add
      |
   SoftMax
      |  \________________
     Mul                  \
                   SoftMaxBackProp
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto matmul_node = graphp->append_op(MatMul, "matmul");
    auto div_node = graphp->append_op(
            Divide, {in_edge(IN0, matmul_node, OUT0)}, "div");
    auto add_node
            = graphp->append_op(Add, {in_edge(IN0, div_node, OUT0)}, "add");
    auto softmax_node = graphp->append_op(
            SoftMax, {in_edge(IN0, add_node, OUT0)}, "softmax");
    softmax_node->allow_external_output(OUT0);
    auto mul_node = graphp->append_op(
            Multiply, {in_edge(IN0, softmax_node, OUT0)}, "mul");
    UNUSED(mul_node);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t div {1, Divide, "div"};
    op_t add {2, Add, "add"};
    op_t softmax {3, SoftMax, "softmax"};
    op_t mul {4, Multiply, "mul"};
    op_t softmaxbwd {5, SoftMaxBackprop, "softmaxbwd"};

    auto lt0 = logical_tensor_init(0, impl::data_type::f32);
    auto lt1 = logical_tensor_init(1, impl::data_type::f32);
    auto lt2 = logical_tensor_init(2, impl::data_type::f32);
    matmul.add_input(lt0);
    matmul.add_input(lt1);
    matmul.add_output(lt2);
    auto lt3 = logical_tensor_init(3, impl::data_type::f32);
    auto lt4 = logical_tensor_init(4, impl::data_type::f32);
    div.add_input(lt2);
    div.add_input(lt3);
    div.add_output(lt4);
    auto lt5 = logical_tensor_init(5, impl::data_type::f32);
    auto lt6 = logical_tensor_init(6, impl::data_type::f32);
    add.add_input(lt4);
    add.add_input(lt5);
    add.add_output(lt6);
    auto lt7 = logical_tensor_init(7, impl::data_type::f32);
    softmax.add_input(lt6);
    softmax.add_output(lt7);
    auto lt8 = logical_tensor_init(8, impl::data_type::f32);
    auto lt9 = logical_tensor_init(9, impl::data_type::f32);
    mul.add_input(lt7);
    mul.add_input(lt8);
    mul.add_output(lt9);

    auto lt10 = logical_tensor_init(10, impl::data_type::f32);
    auto lt11 = logical_tensor_init(11, impl::data_type::f32);
    softmaxbwd.add_input(lt7);
    softmaxbwd.add_input(lt10);
    softmaxbwd.add_output(lt11);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&div), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);
    ASSERT_EQ(agraph.add_op(&softmax), status::success);
    ASSERT_EQ(agraph.add_op(&mul), status::success);
    ASSERT_EQ(agraph.add_op(&softmaxbwd), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 5U);
}

TEST(PatternMatcherV2, NestedRepetitionOptional) {
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");
    auto mlp_layer = std::make_shared<pb_graph_t>("mlp_layer");
    auto matmul = mlp_layer->append_op(impl::op_kind::MatMul, "matmul");
    auto optional_add_subgraph
            = std::make_shared<pb_graph_t>("optional_add_subgraph");
    auto optional_add = optional_add_subgraph->append_op(
            impl::op_kind::Add, "optional_add");
    optional_add_subgraph->create_input_port(0, optional_add, 0);
    optional_add_subgraph->create_output_port(0, optional_add, 0);
    auto add = mlp_layer->append_optional(
            optional_add_subgraph, {in_edge(0, matmul, 0)}, "add");

    auto activation = mlp_layer->append_alternation(
            {impl::op_kind::ReLU, impl::op_kind::Sigmoid, impl::op_kind::GELU},
            {in_edge(0, add, 0)}, "activation");

    mlp_layer->create_input_port(0, matmul, 0);
    mlp_layer->create_output_port(0, activation, 0);
    pgraph->append_repetition(mlp_layer, {0, 0}, 1, 10, "rep_unit");

    impl::graph_t agraph;
    op_t matmul_op {0, MatMul, "matmul"};
    op_t add_op {1, Add, "add"};
    op_t relu {2, ReLU, "relu"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    matmul_op.add_input(lt_vec[0]);
    matmul_op.add_input(lt_vec[1]);
    matmul_op.add_output(lt_vec[2]);
    add_op.add_input(lt_vec[2]);
    add_op.add_input(lt_vec[3]);
    add_op.add_output(lt_vec[4]);
    relu.add_input(lt_vec[4]);
    relu.add_output(lt_vec[5]);

    ASSERT_EQ(agraph.add_op(&matmul_op), status::success);
    ASSERT_EQ(agraph.add_op(&add_op), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;

    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), pgraph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, RepetitionExternalOutput) {
    /*
    pattern:
          matmul                    \
         |      \(external_output)   |
      activation                     |  * [1,10)
         |      \(external_output)   /

    graph:
         matmul
          |    \
          relu  ext0
          |   \
         matmul ext1
          |    \
          relu  ext2
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto fwd_mlp_layer = std::make_shared<pb_graph_t>("fwd_mlp_layer");
    auto matmul = fwd_mlp_layer->append_op(impl::op_kind::MatMul, "matmul");
    matmul->allow_external_output(0);
    auto activation = fwd_mlp_layer->append_alternation(
            {impl::op_kind::ReLU, impl::op_kind::Sigmoid, impl::op_kind::Tanh},
            {in_edge(0, matmul, 0)}, "activation");
    activation->allow_external_output(0);
    fwd_mlp_layer->create_input_port(0, matmul, 0);
    fwd_mlp_layer->create_output_port(0, activation, 0);

    // repeat layer for [1, 10) times
    graphp->append_repetition(fwd_mlp_layer, {0, 0}, 1, 10, "rep_unit");

    graph_t agraph;
    op_t matmul0 {0, MatMul, "matmul0"};
    op_t relu0 {1, ReLU, "relu0"};
    op_t matmul1 {2, MatMul, "matmul1"};
    op_t relu1 {3, ReLU, "relu1"};

    op_t ext0 {4, StaticTranspose, "ext0"};
    ext0.set_attr(op_attr::order, std::vector<int64_t> {0, 1});
    op_t ext1 {5, StaticTranspose, "ext1"};
    ext1.set_attr(op_attr::order, std::vector<int64_t> {0, 1});
    op_t ext2 {6, StaticTranspose, "ext2"};
    ext2.set_attr(op_attr::order, std::vector<int64_t> {0, 1});

    auto lt0 = logical_tensor_init(0, impl::data_type::f32);
    auto lt1 = logical_tensor_init(1, impl::data_type::f32);
    auto lt2 = logical_tensor_init(2, impl::data_type::f32);
    matmul0.add_input(lt0);
    matmul0.add_input(lt1);
    matmul0.add_output(lt2);
    auto lt3 = logical_tensor_init(3, impl::data_type::f32);
    relu0.add_input(lt2);
    relu0.add_output(lt3);
    auto lt4 = logical_tensor_init(4, impl::data_type::f32);
    auto lt5 = logical_tensor_init(5, impl::data_type::f32);
    matmul1.add_input(lt3);
    matmul1.add_input(lt4);
    matmul1.add_output(lt5);
    auto lt6 = logical_tensor_init(6, impl::data_type::f32);
    relu1.add_input(lt5);
    relu1.add_output(lt6);
    auto lt7 = logical_tensor_init(7, impl::data_type::f32);
    auto lt8 = logical_tensor_init(8, impl::data_type::f32);
    auto lt9 = logical_tensor_init(9, impl::data_type::f32);
    ext0.add_input(lt2);
    ext0.add_output(lt7);
    ext1.add_input(lt3);
    ext1.add_output(lt8);
    ext2.add_input(lt5);
    ext2.add_output(lt9);

    ASSERT_EQ(agraph.add_op(&matmul0), status::success);
    ASSERT_EQ(agraph.add_op(&relu0), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);
    ASSERT_EQ(agraph.add_op(&relu1), status::success);
    ASSERT_EQ(agraph.add_op(&ext0), status::success);
    ASSERT_EQ(agraph.add_op(&ext1), status::success);
    ASSERT_EQ(agraph.add_op(&ext2), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 4U);
}

TEST(PatternMatcherV2, RepetitionExternalOutputSwapOrder) {
    /*
    pattern:
          matmul                    \
         |      \(external_output)   |
      activation                     |  * [1,10)
         |      \(external_output)   /

    graph:
         matmul
        /    |
      ext0  relu
           / |
       ext1 matmul
            /  |
          ext2 relu
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto fwd_mlp_layer = std::make_shared<pb_graph_t>("fwd_mlp_layer");
    auto matmul = fwd_mlp_layer->append_op(impl::op_kind::MatMul, "matmul");
    matmul->allow_external_output(0);
    auto activation = fwd_mlp_layer->append_alternation(
            {impl::op_kind::ReLU, impl::op_kind::Sigmoid, impl::op_kind::Tanh},
            {in_edge(0, matmul, 0)}, "activation");
    activation->allow_external_output(0);
    fwd_mlp_layer->create_input_port(0, matmul, 0);
    fwd_mlp_layer->create_output_port(0, activation, 0);

    // repeat layer for [1, 10) times
    graphp->append_repetition(fwd_mlp_layer, {0, 0}, 1, 10, "rep_unit");

    graph_t agraph;
    op_t matmul0 {0, MatMul, "matmul0"};
    op_t relu0 {1, ReLU, "relu0"};
    op_t matmul1 {2, MatMul, "matmul1"};
    op_t relu1 {3, ReLU, "relu1"};

    op_t ext0 {4, StaticTranspose, "ext0"};
    ext0.set_attr(op_attr::order, std::vector<int64_t> {0, 1});
    op_t ext1 {5, StaticTranspose, "ext1"};
    ext1.set_attr(op_attr::order, std::vector<int64_t> {0, 1});
    op_t ext2 {6, StaticTranspose, "ext2"};
    ext2.set_attr(op_attr::order, std::vector<int64_t> {0, 1});

    auto lt0 = logical_tensor_init(0, impl::data_type::f32);
    auto lt1 = logical_tensor_init(1, impl::data_type::f32);
    auto lt2 = logical_tensor_init(2, impl::data_type::f32);
    matmul0.add_input(lt0);
    matmul0.add_input(lt1);
    matmul0.add_output(lt2);

    auto lt7 = logical_tensor_init(7, impl::data_type::f32);
    ext0.add_input(lt2);
    ext0.add_output(lt7);

    auto lt3 = logical_tensor_init(3, impl::data_type::f32);
    relu0.add_input(lt2);
    relu0.add_output(lt3);

    auto lt8 = logical_tensor_init(8, impl::data_type::f32);
    ext1.add_input(lt3);
    ext1.add_output(lt8);

    auto lt4 = logical_tensor_init(4, impl::data_type::f32);
    auto lt5 = logical_tensor_init(5, impl::data_type::f32);
    matmul1.add_input(lt3);
    matmul1.add_input(lt4);
    matmul1.add_output(lt5);

    auto lt9 = logical_tensor_init(9, impl::data_type::f32);
    ext2.add_input(lt5);
    ext2.add_output(lt9);

    auto lt6 = logical_tensor_init(6, impl::data_type::f32);
    relu1.add_input(lt5);
    relu1.add_output(lt6);

    ASSERT_EQ(agraph.add_op(&ext0), status::success);
    ASSERT_EQ(agraph.add_op(&ext1), status::success);
    ASSERT_EQ(agraph.add_op(&ext2), status::success);
    ASSERT_EQ(agraph.add_op(&matmul0), status::success);
    ASSERT_EQ(agraph.add_op(&relu0), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);
    ASSERT_EQ(agraph.add_op(&relu1), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[3].get(), graphp, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 4U);
}

TEST(PatternMatcherV2, CyclicCheck) {
    /*
    pattern:
          matmul
           /  \(external_output)
         relu
           \  /
            add


    graph:
         matmul
          /  \
        relu  sigmoid
          \  /
           add
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pmatmul = graphp->append_op(impl::op_kind::MatMul, "pmatmul");
    pmatmul->allow_external_output(0);
    auto prelu = graphp->append_op(
            impl::op_kind::ReLU, {in_edge(0, pmatmul, 0)}, "prelu");
    auto padd = graphp->append_op(
            impl::op_kind::Add, {in_edge(0, prelu, 0)}, "padd");
    UNUSED(padd);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    op_t add {2, Add, "add"};
    op_t sigmoid {3, Sigmoid, "sigmoid"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(6);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);
    sigmoid.add_input(lt_vec[2]);
    sigmoid.add_output(lt_vec[4]);
    add.add_input(lt_vec[3]);
    add.add_input(lt_vec[4]);
    add.add_output(lt_vec[5]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&sigmoid), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
}

TEST(PatternMatcherV2, UndirectCyclicCheck) {
    /*
    pattern:
          matmul
           /  \(external_output)
         relu
           \  /
            add


    graph:
         matmul
          /  \
         |    wildcard wildcard
        relu    |     /
         |    wildcard
          \  /
           add
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto pmatmul = graphp->append_op(impl::op_kind::MatMul, "pmatmul");
    pmatmul->allow_external_output(0);
    auto prelu = graphp->append_op(
            impl::op_kind::ReLU, {in_edge(0, pmatmul, 0)}, "prelu");
    auto padd = graphp->append_op(
            impl::op_kind::Add, {in_edge(0, prelu, 0)}, "padd");
    UNUSED(padd);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    op_t add {2, Add, "add"};
    op_t wildcard {3, Wildcard, "wildcard"};
    op_t wildcard2 {4, Wildcard, "wildcard"};
    op_t wildcard3 {5, Wildcard, "wildcard"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(9);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);
    wildcard.add_input(lt_vec[2]);
    wildcard.add_output(lt_vec[4]);
    wildcard2.add_input(lt_vec[5]);
    wildcard2.add_output(lt_vec[6]);
    wildcard3.add_input(lt_vec[4]);
    wildcard3.add_input(lt_vec[6]);
    wildcard3.add_output(lt_vec[7]);
    add.add_input(lt_vec[3]);
    add.add_input(lt_vec[7]);
    add.add_output(lt_vec[8]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard2), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard3), status::success);
    ASSERT_EQ(agraph.add_op(&add), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_FALSE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
}

TEST(PatternMatcherV2, ComplexCyclicCheck) {
    /*
    pattern:
          matmul                   \
           /   \(external_output)   |
         relu                       |  * [1,10)
           \  /                     |
            add                     /

    graph:
         matmul
          /   \
        relu  sigmoid
          \        |
           add     |
            |      |
           matmul /
            |    /
           relu /
            \  /
             add
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto fwd_mlp_layer = std::make_shared<pb_graph_t>("fwd_mlp_layer");
    auto pmatmul = fwd_mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    pmatmul->allow_external_output(0);
    auto prelu = fwd_mlp_layer->append_op(
            impl::op_kind::ReLU, {in_edge(0, pmatmul, 0)}, "prelu");
    auto padd = fwd_mlp_layer->append_op(
            impl::op_kind::Add, {in_edge(0, prelu, 0)}, "padd");
    fwd_mlp_layer->create_input_port(0, pmatmul, 0);
    fwd_mlp_layer->create_output_port(0, padd, 0);

    // repeat layer for [1, 10) times
    graphp->append_repetition(fwd_mlp_layer, {0, 0}, 1, 10, "rep_unit");

    graph_t agraph;
    op_t matmul0 {0, MatMul, "matmu0"};
    op_t relu0 {1, ReLU, "relu0"};
    op_t add0 {2, Add, "add0"};
    op_t sigmoid0 {3, Sigmoid, "sigmoid0"};
    op_t matmul1 {4, MatMul, "matmul1"};
    op_t relu1 {5, ReLU, "relu1"};
    op_t add1 {6, Add, "add1"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(11);
    matmul0.add_input(lt_vec[0]);
    matmul0.add_input(lt_vec[1]);
    matmul0.add_output(lt_vec[2]);
    relu0.add_input(lt_vec[2]);
    relu0.add_output(lt_vec[3]);
    sigmoid0.add_input(lt_vec[2]);
    sigmoid0.add_output(lt_vec[4]);
    add0.add_input(lt_vec[3]);
    add0.add_input(lt_vec[5]);
    add0.add_output(lt_vec[6]);
    matmul1.add_input(lt_vec[6]);
    matmul1.add_input(lt_vec[7]);
    matmul1.add_output(lt_vec[8]);
    relu1.add_input(lt_vec[8]);
    relu1.add_output(lt_vec[9]);
    add1.add_input(lt_vec[9]);
    //cycle here
    add1.add_input(lt_vec[4]);
    add1.add_output(lt_vec[10]);

    ASSERT_EQ(agraph.add_op(&matmul0), status::success);
    ASSERT_EQ(agraph.add_op(&relu0), status::success);
    ASSERT_EQ(agraph.add_op(&sigmoid0), status::success);
    ASSERT_EQ(agraph.add_op(&add0), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);
    ASSERT_EQ(agraph.add_op(&relu1), status::success);
    ASSERT_EQ(agraph.add_op(&add1), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    // should match the 1st rep_unit
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    EXPECT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, ComplexUndirectCyclicCheck) {
    /*
    pattern:
          matmul                   \
           /   \(external_output)   |
         relu                       |  * [1,10)
           \  /                     |
            add                     /

    graph:
         matmul
          /   \
        relu  wildcard
          \        |
           add    wildcard
            |      |
           matmul wildcard
            |    /
           relu /
            \  /
             add
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto fwd_mlp_layer = std::make_shared<pb_graph_t>("fwd_mlp_layer");
    auto pmatmul = fwd_mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    pmatmul->allow_external_output(0);
    auto prelu = fwd_mlp_layer->append_op(
            impl::op_kind::ReLU, {in_edge(0, pmatmul, 0)}, "prelu");
    auto padd = fwd_mlp_layer->append_op(
            impl::op_kind::Add, {in_edge(0, prelu, 0)}, "padd");
    fwd_mlp_layer->create_input_port(0, pmatmul, 0);
    fwd_mlp_layer->create_output_port(0, padd, 0);

    // repeat layer for [1, 10) times
    graphp->append_repetition(fwd_mlp_layer, {0, 0}, 1, 10, "rep_unit");

    graph_t agraph;
    op_t matmul0 {0, MatMul, "matmu0"};
    op_t relu0 {1, ReLU, "relu0"};
    op_t add0 {2, Add, "add0"};
    op_t wildcard0 {3, Wildcard, "wildcard0"};
    op_t wildcard1 {4, Wildcard, "wildcard1"};
    op_t wildcard2 {5, Wildcard, "wildcard2"};
    op_t matmul1 {6, MatMul, "matmul1"};
    op_t relu1 {7, ReLU, "relu1"};
    op_t add1 {8, Add, "add1"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(13);
    matmul0.add_input(lt_vec[0]);
    matmul0.add_input(lt_vec[1]);
    matmul0.add_output(lt_vec[2]);
    relu0.add_input(lt_vec[2]);
    relu0.add_output(lt_vec[3]);
    wildcard0.add_input(lt_vec[2]);
    wildcard0.add_output(lt_vec[4]);
    wildcard1.add_input(lt_vec[4]);
    wildcard1.add_output(lt_vec[5]);
    wildcard2.add_input(lt_vec[5]);
    wildcard2.add_output(lt_vec[6]);
    add0.add_input(lt_vec[3]);
    add0.add_input(lt_vec[7]);
    add0.add_output(lt_vec[8]);
    matmul1.add_input(lt_vec[8]);
    matmul1.add_input(lt_vec[9]);
    matmul1.add_output(lt_vec[10]);
    relu1.add_input(lt_vec[10]);
    relu1.add_output(lt_vec[11]);
    add1.add_input(lt_vec[11]);
    //cycle here
    add1.add_input(lt_vec[6]);
    add1.add_output(lt_vec[12]);

    ASSERT_EQ(agraph.add_op(&matmul0), status::success);
    ASSERT_EQ(agraph.add_op(&relu0), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard0), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard1), status::success);
    ASSERT_EQ(agraph.add_op(&wildcard2), status::success);
    ASSERT_EQ(agraph.add_op(&add0), status::success);
    ASSERT_EQ(agraph.add_op(&matmul1), status::success);
    ASSERT_EQ(agraph.add_op(&relu1), status::success);
    ASSERT_EQ(agraph.add_op(&add1), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    // should match the 1st rep_unit
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    EXPECT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, OptionalSubgraphFailure) {
    /*
        [   \    /
            matmul
              |
        [relu, sigmoid, tanh]*[0,1] ]*[1,5]
    */
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");
    auto mlp_layer = std::make_shared<pb_graph_t>("pmlp");
    auto matmul_layer = mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    auto optional_activation_subgraph
            = std::make_shared<pb_graph_t>("poptional_activation_subgraph");
    auto activation = optional_activation_subgraph->append_alternation(
            {impl::op_kind::ReLU, impl::op_kind::Sigmoid, impl::op_kind::Tanh},
            "palternation");
    optional_activation_subgraph->create_input_port(0, activation, 0);
    optional_activation_subgraph->create_output_port(0, activation, 0);
    auto optional_activation
            = mlp_layer->append_optional(optional_activation_subgraph,
                    {in_edge(0, matmul_layer, 0)}, "poptional_activation");
    mlp_layer->create_input_port(0, matmul_layer, 0);
    mlp_layer->create_output_port(0, optional_activation, 0);
    pgraph->append_repetition(mlp_layer, {0, 0}, 1, 5, "prepetition");

    impl::graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t matmul2 {1, MatMul, "matmul2"};
    op_t matmul3 {2, MatMul, "matmul3"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(7);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    matmul2.add_input(lt_vec[2]);
    matmul2.add_input(lt_vec[3]);
    matmul2.add_output(lt_vec[4]);
    matmul3.add_input(lt_vec[4]);
    matmul3.add_input(lt_vec[5]);
    matmul3.add_output(lt_vec[6]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&matmul2), status::success);
    ASSERT_EQ(agraph.add_op(&matmul3), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), pgraph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 3U);
}

TEST(PatternMatcherV2, OptionalSubgraphFailure2) {
    /*
           [   \     /
               matmul
                 |
                / \
           relu     [Sigmoid, Tanh]
             |             |          ]*[1,5]
    */
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");
    auto mlp_layer = std::make_shared<pb_graph_t>("pmlp");
    auto matmul_layer = mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    auto relu_layer = mlp_layer->append_op(
            impl::op_kind::ReLU, {in_edge(0, matmul_layer, 0)}, "prelu");
    auto optional_activation_subgraph
            = std::make_shared<pb_graph_t>("poptional_activation_subgraph");
    auto activation = optional_activation_subgraph->append_alternation(
            {impl::op_kind::Sigmoid, impl::op_kind::Tanh}, "palternation");
    optional_activation_subgraph->create_input_port(0, activation, 0);
    optional_activation_subgraph->create_output_port(0, activation, 0);
    auto optional_activation
            = mlp_layer->append_optional(optional_activation_subgraph,
                    {in_edge(0, matmul_layer, 0)}, "poptional_activation");
    mlp_layer->create_input_port(0, matmul_layer, 0);
    mlp_layer->create_output_port(0, relu_layer, 0);
    mlp_layer->create_output_port(1, optional_activation, 0);
    pgraph->append_repetition(mlp_layer, {0, 0}, 1, 5, "prepetition");

    impl::graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(4);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), pgraph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, OptionalSubgraphFailure3) {
    /*
            [  \     /
               matmul
                 |
               relu
                 |
              [relu]*[0,1] ]*[1,5]
    */
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");
    auto mlp_layer = std::make_shared<pb_graph_t>("pmlp");
    auto matmul_layer = mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    auto relu_layer = mlp_layer->append_op(
            impl::op_kind::ReLU, {in_edge(0, matmul_layer, 0)}, "prelu");
    auto optional_relu_subgraph
            = std::make_shared<pb_graph_t>("poptional_relu_subgraph");
    auto activation
            = optional_relu_subgraph->append_op(impl::op_kind::ReLU, "prelu2");
    optional_relu_subgraph->create_input_port(0, activation, 0);
    optional_relu_subgraph->create_output_port(0, activation, 0);
    auto optional_relu = mlp_layer->append_optional(optional_relu_subgraph,
            {in_edge(0, relu_layer, 0)}, "poptional_relu");
    mlp_layer->create_input_port(0, matmul_layer, 0);
    mlp_layer->create_output_port(0, optional_relu, 0);
    pgraph->append_repetition(mlp_layer, {0, 0}, 1, 5, "prepetition");

    impl::graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(4);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), pgraph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 2U);
}

TEST(PatternMatcherV2, OptionalSubgraphFailure4) {
    /*
            [  \     /
               matmul
                 |
                add*[0,1]
                 |
              [relu]*[0,1] ]*[1,5]
    */
    auto pgraph = std::make_shared<pb_graph_t>("pgraph");
    auto mlp_layer = std::make_shared<pb_graph_t>("pmlp");
    auto matmul_layer = mlp_layer->append_op(impl::op_kind::MatMul, "pmatmul");
    auto optional_add_subgraph
            = std::make_shared<pb_graph_t>("poptional_add_subgraph");
    auto add = optional_add_subgraph->append_op(
            impl::op_kind::Add, "palternation");
    optional_add_subgraph->create_input_port(0, add, 0);
    optional_add_subgraph->create_output_port(0, add, 0);
    auto optional_add = mlp_layer->append_optional(optional_add_subgraph,
            {in_edge(0, matmul_layer, 0)}, "poptional_add");
    auto optional_activation_subgraph
            = std::make_shared<pb_graph_t>("poptional_activation_subgraph");
    auto activation = optional_activation_subgraph->append_alternation(
            {impl::op_kind::ReLU, impl::op_kind::Sigmoid, impl::op_kind::Tanh},
            "palternation");
    optional_activation_subgraph->create_input_port(0, activation, 0);
    optional_activation_subgraph->create_output_port(0, activation, 0);
    auto optional_activation
            = mlp_layer->append_optional(optional_activation_subgraph,
                    {in_edge(0, optional_add, 0)}, "poptional_activation");
    mlp_layer->create_input_port(0, matmul_layer, 0);
    mlp_layer->create_output_port(0, optional_activation, 0);
    pgraph->append_repetition(mlp_layer, {0, 0}, 1, 5, "prepetition");

    impl::graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(3);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), pgraph, fusion_ops));
    ASSERT_EQ(fusion_ops.size(), 1U);
}

TEST(PatternMatcherV2, RepetitionOportExternalOutput) {
    /*
    pattern:
        matmul                     \
          |                         |  * [1,10)
         relu                      /
          |  \(external_output)
        sigmoid
    graph:
         matmul
           |
          relu
           |  \
        matmul relu_bwd
           |
          relu
           |  \
       sigmoid relu_bwd
    */
    auto graphp = std::make_shared<pb_graph_t>("pgraph");
    auto grep = std::make_shared<pb_graph_t>("grep");
    auto pmatmul = grep->append_op(impl::op_kind::MatMul, "pmatmul");
    auto prelu = grep->append_op(
            impl::op_kind::ReLU, {in_edge(0, pmatmul, 0)}, "prelu");
    prelu->allow_external_output(0);
    grep->create_input_port(0, pmatmul, 0);
    grep->create_output_port(0, prelu, 0);
    auto prep = graphp->append_repetition(grep, {0, 0}, 1, 10, "prep");

    auto psigmoid = graphp->append_op(
            impl::op_kind::Sigmoid, {in_edge(0, prep, 0)}, "psigmoid");

    UNUSED(psigmoid);

    graph_t agraph;
    op_t matmul {0, MatMul, "matmul"};
    op_t relu {1, ReLU, "relu"};
    op_t relu_bwd {2, ReLUBackprop, "relu_bwd"};
    op_t matmul2 {3, MatMul, "matmul2"};
    op_t relu2 {4, ReLU, "relu2"};
    op_t relu_bwd2 {5, ReLUBackprop, "relu_bwd2"};
    op_t sigmoid {6, Sigmoid, "sigmoid"};

    std::vector<logical_tensor_t> lt_vec = create_logical_tensors(12);
    matmul.add_input(lt_vec[0]);
    matmul.add_input(lt_vec[1]);
    matmul.add_output(lt_vec[2]);
    relu.add_input(lt_vec[2]);
    relu.add_output(lt_vec[3]);
    relu_bwd.add_input(lt_vec[3]);
    relu_bwd.add_input(lt_vec[4]);
    relu_bwd.add_output(lt_vec[5]);
    matmul2.add_input(lt_vec[3]);
    matmul2.add_input(lt_vec[6]);
    matmul2.add_output(lt_vec[7]);
    relu2.add_input(lt_vec[7]);
    relu2.add_output(lt_vec[8]);
    sigmoid.add_input(lt_vec[8]);
    sigmoid.add_output(lt_vec[9]);
    relu_bwd2.add_input(lt_vec[8]);
    relu_bwd2.add_input(lt_vec[10]);
    relu_bwd2.add_output(lt_vec[11]);

    ASSERT_EQ(agraph.add_op(&matmul), status::success);
    ASSERT_EQ(agraph.add_op(&relu), status::success);
    ASSERT_EQ(agraph.add_op(&relu_bwd), status::success);
    ASSERT_EQ(agraph.add_op(&matmul2), status::success);
    ASSERT_EQ(agraph.add_op(&relu2), status::success);
    ASSERT_EQ(agraph.add_op(&sigmoid), status::success);
    ASSERT_EQ(agraph.add_op(&relu_bwd2), status::success);

    agraph.build_graph();

    std::vector<op_t *> fusion_ops;
    EXPECT_TRUE(match_pattern(agraph.get_ops()[0].get(), graphp, fusion_ops));
    EXPECT_EQ(fusion_ops.size(), 5U);
}