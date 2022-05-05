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

#include <algorithm>
#include <vector>

#include <oneapi/dnnl/dnnl_debug.h>

#include "dnnl_graph_common.hpp"
#ifdef DNNL_GRAPH_WITH_SYCL
#include "dnnl_sycl.hpp"
#endif
#include "utils/timer.hpp"

namespace benchdnnext {

bool check_graph_creation_status(const graph_prb_t *prb, res_t *res) {
    if (prb->ctor_status == fill_status::UNSUPPORTED_CONFIG) {
        res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
        return false;
    } else if (prb->ctor_status == fill_status::UNSUPPORTED_OP
            || prb->ctor_status == fill_status::UNKNOWN_ERROR) {
        res->state = UNIMPLEMENTED;
        return false;
    }
    return true;
}

void check_known_skipped_case_graph_common(
        const std::vector<dnnl_data_type_t> &v_dt, const std::string &tag,
        const dir_t &dir, res_t *res) {
    // tag::undef not supported for now
    if (tag == tag::undef) {
        res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
        return;
    }
}

void check_graph_eltwise_post_ops(const attr_t &attr, res_t *res) {
    for (const auto &e : attr.post_ops.entry) {
        if (!e.is_eltwise_kind()) continue;

        if (convert_alg_kind(e.eltwise.alg)
                == dnnl::graph::op::kind::LastSymbol) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
            return;
        }

        check_graph_eltwise_params(
                res, e.kind, e.eltwise.alpha, e.eltwise.beta);
    }
}

// Due to differences between oneDNN and oneDNN graph APIs we need
// to skip cases in which elementwise parameters cannot be set. For
// example, oneDNN graph API doesn't have alpha parameter for ReLU,
// while oneDNN does. Another example is Swish, which is represented
// in oneDNN graph by Multiply+Sigmoid - Sigmoid doesn't accept any
// param, so alpha is fixed and equal to 1.0.
void check_graph_eltwise_params(res_t *res,
        const attr_t::post_ops_t::kind_t alg, const float alpha,
        const float beta) {
    using alg_t = attr_t::post_ops_t::kind_t;

    constexpr float eps = 1.0e-05;
    if (alg == alg_t::RELU || alg == alg_t::RELU_DST) {
        const float expected_alpha = 0.0;
        if (std::fabs(expected_alpha - alpha) > eps) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
            return;
        }
    } else if (alg == alg_t::SWISH) {
        const float expected_alpha = 1.0;
        if (std::fabs(expected_alpha - alpha) > eps) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
            return;
        }
    }
}

float get_post_eltwise_scale(
        const std::vector<attr_t::post_ops_t::entry_t> &post_ops) noexcept {
    for (const auto &po : post_ops) {
        if (po.is_eltwise_kind()) return po.eltwise.scale;
    }
    return 1.f;
}

dnnl::graph::logical_tensor::data_type convert_dt(
        const dnnl_data_type_t dt) noexcept {
    using graph_dt = dnnl::graph::logical_tensor::data_type;

    switch (dt) {
        case dnnl_f16: return graph_dt::f16;
        case dnnl_bf16: return graph_dt::bf16;
        case dnnl_f32: return graph_dt::f32;
        case dnnl_s32: return graph_dt::s32;
        case dnnl_s8: return graph_dt::s8;
        case dnnl_u8: return graph_dt::u8;
        case dnnl_data_type_undef:
        default: return graph_dt::undef;
    }
}

dnnl_data_type_t convert_dt(
        const dnnl::graph::logical_tensor::data_type dt) noexcept {
    using graph_dt = dnnl::graph::logical_tensor::data_type;

    switch (dt) {
        case graph_dt::f16: return dnnl_f16;
        case graph_dt::bf16: return dnnl_bf16;
        case graph_dt::f32: return dnnl_f32;
        case graph_dt::s32: return dnnl_s32;
        case graph_dt::s8: return dnnl_s8;
        case graph_dt::u8: return dnnl_u8;
        case graph_dt::undef:
        default: return dnnl_data_type_undef;
    }
}

dnnl::graph::op::kind convert_alg_kind(
        const dnnl_alg_kind_t kind, bool is_fwd) noexcept {
    using graph_op = dnnl::graph::op::kind;
    // all options could be easily added later
    if (is_fwd) {
        switch (kind) {
            case dnnl_eltwise_abs: return graph_op::Abs;
            case dnnl_eltwise_clip_v2: return graph_op::HardTanh;
            case dnnl_eltwise_elu: return graph_op::Elu;
            case dnnl_eltwise_exp: return graph_op::Exp;
            case dnnl_eltwise_gelu_erf: return graph_op::GELU;
            case dnnl_eltwise_hardswish: return graph_op::HardSwish;
            case dnnl_eltwise_log: return graph_op::Log;
            case dnnl_eltwise_logistic: return graph_op::Sigmoid;
            case dnnl_eltwise_logsigmoid: return graph_op::SoftPlus;
            case dnnl_eltwise_pow: return graph_op::Pow;
            case dnnl_eltwise_relu: return graph_op::ReLU;
            case dnnl_eltwise_soft_relu: return graph_op::SoftPlus;
            case dnnl_eltwise_round: return graph_op::Round;
            case dnnl_eltwise_sqrt: return graph_op::Sqrt;
            case dnnl_eltwise_square: return graph_op::Square;
            case dnnl_eltwise_tanh: return graph_op::Tanh;
            case dnnl_binary_add: return graph_op::Add;
            case dnnl_binary_div: return graph_op::Divide;
            case dnnl_binary_max: return graph_op::Maximum;
            case dnnl_binary_min: return graph_op::Minimum;
            case dnnl_binary_mul: return graph_op::Multiply;
            case dnnl_binary_sub: return graph_op::Subtract;
            case dnnl_reduction_norm_lp_power_p_sum: return graph_op::ReduceL1;
            case dnnl_reduction_norm_lp_sum: return graph_op::ReduceL2;
            case dnnl_reduction_max: return graph_op::ReduceMax;
            case dnnl_reduction_mean: return graph_op::ReduceMean;
            case dnnl_reduction_min: return graph_op::ReduceMin;
            case dnnl_reduction_mul: return graph_op::ReduceProd;
            case dnnl_reduction_sum: return graph_op::ReduceSum;
            // TODO (damianszw): find nicer way to tell about unsupported type
            case dnnl_eltwise_bounded_relu:
            case dnnl_eltwise_clip:
            case dnnl_eltwise_clip_v2_use_dst_for_bwd:
            case dnnl_eltwise_elu_use_dst_for_bwd:
            case dnnl_eltwise_exp_use_dst_for_bwd:
            case dnnl_eltwise_gelu_tanh:
            case dnnl_eltwise_linear:
            case dnnl_eltwise_logistic_use_dst_for_bwd:
            case dnnl_eltwise_mish:
            case dnnl_eltwise_relu_use_dst_for_bwd:
            case dnnl_eltwise_sqrt_use_dst_for_bwd:
            case dnnl_eltwise_swish:
            case dnnl_eltwise_tanh_use_dst_for_bwd:
            case dnnl_reduction_norm_lp_power_p_max:
            case dnnl_reduction_norm_lp_max:
            default: return graph_op::LastSymbol;
        }
    } else {
        switch (kind) {
            case dnnl_eltwise_clip_v2:
            case dnnl_eltwise_clip_v2_use_dst_for_bwd:
                return graph_op::HardTanhBackprop;
            case dnnl_eltwise_elu:
            case dnnl_eltwise_elu_use_dst_for_bwd: return graph_op::EluBackprop;
            case dnnl_eltwise_gelu_erf: return graph_op::GELUBackprop;
            case dnnl_eltwise_hardswish: return graph_op::HardSwishBackprop;
            case dnnl_eltwise_logistic:
            case dnnl_eltwise_logistic_use_dst_for_bwd:
                return graph_op::SigmoidBackprop;
            case dnnl_eltwise_logsigmoid: return graph_op::SoftPlusBackprop;
            case dnnl_eltwise_relu:
            case dnnl_eltwise_relu_use_dst_for_bwd:
                return graph_op::ReLUBackprop;
            case dnnl_eltwise_soft_relu: return graph_op::SoftPlusBackprop;
            case dnnl_eltwise_sqrt:
            case dnnl_eltwise_sqrt_use_dst_for_bwd:
                return graph_op::SqrtBackprop;
            case dnnl_eltwise_tanh:
            case dnnl_eltwise_tanh_use_dst_for_bwd:
                return graph_op::TanhBackprop;
            // Don't support for now
            case dnnl_eltwise_abs:
            case dnnl_eltwise_exp:
            case dnnl_eltwise_log:
            case dnnl_eltwise_pow:
            case dnnl_eltwise_round:
            case dnnl_eltwise_square:
            case dnnl_binary_add:
            case dnnl_binary_div:
            case dnnl_binary_max:
            case dnnl_binary_min:
            case dnnl_binary_mul:
            case dnnl_binary_sub:
            case dnnl_reduction_norm_lp_power_p_sum:
            case dnnl_reduction_norm_lp_sum:
            case dnnl_reduction_max:
            case dnnl_reduction_mean:
            case dnnl_reduction_min:
            case dnnl_reduction_mul:
            case dnnl_reduction_sum:
            // TODO (damianszw): find nicer way to tell about unsupported type
            case dnnl_eltwise_bounded_relu:
            case dnnl_eltwise_clip:
            case dnnl_eltwise_exp_use_dst_for_bwd:
            case dnnl_eltwise_gelu_tanh:
            case dnnl_eltwise_linear:
            case dnnl_eltwise_mish:
            case dnnl_eltwise_swish:
            case dnnl_reduction_norm_lp_power_p_max:
            case dnnl_reduction_norm_lp_max:
            default: return graph_op::LastSymbol;
        }
    }
}

std::string convert_tag(const std::string &tag, bool activation_tag) noexcept {
    if (tag == "abx") return (activation_tag) ? "NCX" : "OIX";
    if (tag == "axb") return "NXC";
    if (tag == "xba") return "XIO";
    // default cases
    return (activation_tag) ? "NXC" : "XIO";
}

dims_t convert_bin_policy(const dims_t &lhs_dims, const attr_t::policy_t policy,
        const std::string &data_format) noexcept {
    using bin_pol = attr_t::policy_t;

    auto rhs_dims = dims_t(lhs_dims.size(), 1);

    switch (policy) {
        case bin_pol::PER_TENSOR: rhs_dims = lhs_dims; break;
        case bin_pol::PER_OC:
            if (data_format == "NCX")
                rhs_dims[1] = lhs_dims[1];
            else
                rhs_dims.back() = lhs_dims.back(); // "NXC" case
            break;
        case bin_pol::PER_DIM_0: rhs_dims[0] = lhs_dims[0]; break;
        case bin_pol::PER_DIM_1: rhs_dims[1] = lhs_dims[1]; break;
        case bin_pol::PER_DIM_01:
            rhs_dims[0] = lhs_dims[0];
            rhs_dims[1] = lhs_dims[1];
            break;
        case bin_pol::COMMON:
        default: break;
    }

    return rhs_dims;
}

std::string convert_attr_policy(attr_t::policy_t policy) noexcept {
    std::string ret_policy;
    switch (policy) {
        case attr_t::policy_t::PER_DIM_0:
        case attr_t::policy_t::PER_OC:
        case attr_t::policy_t::PER_DIM_1: ret_policy = "per_channel"; break;
        case attr_t::policy_t::COMMON: ret_policy = "per_tensor"; break;
        default: assert(!"policy not supported for now."); SAFE_V(FAIL);
    }
    return ret_policy;
}

std::map<std::string, float> convert_eltw_entry(
        const dnnl::graph::op::kind op_kind,
        const attr_t::post_ops_t::entry_t &entry) noexcept {
    using graph_op = dnnl::graph::op::kind;

    std::map<std::string, float> attrs;
    // all options could be easily added later
    switch (op_kind) {
        case graph_op::Elu: attrs["alpha"] = entry.eltwise.alpha; return attrs;
        case graph_op::HardTanh:
            attrs["min"] = entry.eltwise.alpha;
            attrs["max"] = entry.eltwise.beta;
            return attrs;
        default: return attrs;
    }
}

bool should_handle_swish(graph_prb_t &p, const dnnl_alg_kind_t kind) noexcept {
    using op = dnnl::graph::op;
    static const std::vector<op::kind> possible_base_ops
            = {op::kind::Convolution, op::kind::MatMul};

    const bool valid_base_op
            = std::find(possible_base_ops.cbegin(), possible_base_ops.cend(),
                      p.get_main_op_kind())
            != possible_base_ops.cend();
    const bool is_bias = p.has_post_bia();
    const bool is_swish = kind == dnnl_eltwise_swish;

    return valid_base_op && is_bias && is_swish;
}

int scale_bia(
        dnn_mem_t &dst, dnn_mem_t &src, const std::vector<float> &scales) {
    if (scales.empty()) {
        dst = std::move(src);
        return OK;
    }
    float eps = 1.e-9;
    std::vector<float> bia_scales(scales.size(), 0.f);
    std::transform(scales.begin(), scales.end(), bia_scales.begin(),
            [eps](const float scale) { return 1.f / (scale + eps); });
    const int bia_mask = bia_scales.size() == 1 ? 0 : 1;
    dnnl_primitive_attr_t bia_attr = nullptr;
    dnnl_primitive_attr_create(&bia_attr);
    dnnl_primitive_attr_set_output_scales(
            bia_attr, bia_scales.size(), bia_mask, bia_scales.data());
    SAFE(dst.reorder(src, bia_attr), CRIT);
    dnnl_primitive_attr_destroy(bia_attr);

    return OK;
}

dnnl_format_tag_t dnnl_fmt_str2tag(const std::string &fmt_str) {
    dnnl_format_tag_t tag = dnnl_format_tag_undef;
    for (int i = 0; i < dnnl_format_tag_last; ++i) {
        tag = static_cast<dnnl_format_tag_t>(i);
        if (dnnl_fmt_tag2str(tag) == fmt_str) break;
    }
    if (tag == dnnl_format_tag_undef)
        []() {
            SAFE(FAIL, CRIT);
            return 0;
        }();
    return tag;
};

dims_t calculate_strides(dims_t dims, dt dtype, const std::string &tag) {
    dims_t strides(dims.size(), 0);
    dnnl_dims_t dnnl_dims = {0};
    std::copy(dims.begin(), dims.end(), dnnl_dims);
    auto md = dnn_mem_t::init_md(
            (int)dims.size(), dnnl_dims, convert_dt(dtype), tag);
    std::copy(std::begin(md.format_desc.blocking.strides),
            std::begin(md.format_desc.blocking.strides) + dims.size(),
            std::begin(strides));
    return strides;
}

// Get indices, on which post binary ops are located.
std::vector<size_t> get_post_bin_indices(
        const std::vector<attr_t::post_ops_t::entry_t> &po_entry) {
    std::vector<size_t> post_bin_indexes {};
    for (size_t idx = 0; idx < po_entry.size(); idx++) {
        if (po_entry[idx].is_binary_kind()) post_bin_indexes.push_back(idx);
    }
    return post_bin_indexes;
}

dnn_mem_t make_dnn_mem(const dnnl::graph::logical_tensor &lt,
        const dnnl::graph::logical_tensor::data_type &graph_dt,
        const char *atag) {
    using graph_layout = dnnl::graph::logical_tensor::layout_type;

    const auto &dnnl_test_engine = ::get_test_engine();
    const auto dims = lt.get_dims();
    const int ndims = static_cast<int>(dims.size());
    std::string valid_tag = atag ? normalize_tag(atag, ndims) : "abx";

    // NOTE: oneDNN Graph cannot get the concrete format from any-format logical
    //   tensor. Given that some tags in benchdnn is any by default, we should
    //   consider any to be default plain format for oneDNN Graph.
    if (valid_tag == tag::any) valid_tag = normalize_tag("abx", ndims);

    dnnl_dims_t dnnl_dims = {0};
    std::copy(dims.begin(), dims.end(), dnnl_dims);

    const auto ltype = lt.get_layout_type();
    if (graph_layout::undef != ltype) {
        return dnn_mem_t(ndims, dnnl_dims, convert_dt(graph_dt), valid_tag,
                dnnl_test_engine);
    } else {
        []() {
            SAFE(FAIL, CRIT);
            return 0;
        }();
        return dnn_mem_t();
    }
}

dnn_mem_t make_dnn_mem(const dnnl::graph::logical_tensor &lt,
        const dims_t &dims,
        const dnnl::graph::logical_tensor::data_type &graph_dt,
        const char *atag) {
    dnnl::graph::logical_tensor new_lt(
            lt.get_id(), lt.get_data_type(), dims, lt.get_layout_type());
    return make_dnn_mem(new_lt, graph_dt, atag);
}

dnn_mem_t make_dnn_mem(const dnnl::graph::logical_tensor &lt,
        const dims_t &dims, const std::string &atag) {
    dnnl::graph::logical_tensor new_lt(
            lt.get_id(), lt.get_data_type(), dims, lt.get_layout_type());
    return make_dnn_mem(new_lt, atag);
}

dnn_mem_t make_dnn_mem(
        const dnnl::graph::logical_tensor &lt, const std::string &tag) {
    return make_dnn_mem(lt, tag.empty() ? nullptr : tag.c_str());
}

dnn_mem_t make_dnn_mem(const dnnl::graph::logical_tensor &lt, const char *tag) {
    return make_dnn_mem(lt, lt.get_data_type(), tag);
}

void compiled_partition_executor(dnnl::graph::compiled_partition &cp,
        dnnl::graph::stream &stream,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs) {
    if (get_test_engine_kind() == dnnl_cpu) {
#ifdef DNNL_GRAPH_CPU_SYCL
        dnnl::graph::sycl_interop::execute(cp, stream, inputs,
                const_cast<std::vector<dnnl::graph::tensor> &>(outputs));
#else
        cp.execute(stream, inputs, outputs);
#endif
    } else {
#ifdef DNNL_GRAPH_GPU_SYCL
        dnnl::graph::sycl_interop::execute(cp, stream, inputs,
                const_cast<std::vector<dnnl::graph::tensor> &>(outputs));
#else
        assert(!"GPU only support DPCPP runtime now");
#endif
    }
}

int execute_and_wait(perf_function_t &exec_func,
        const dnnl::graph::engine &engine,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs) {
    dnnl::graph::stream stream {get_test_stream()};
    BENCHDNNEXT_SAFE(exec_func(stream, inputs, outputs), CRIT);
    BENCHDNNEXT_SAFE(stream.wait(), CRIT);

    return OK;
}

int execute_and_wait(dnnl::graph::compiled_partition &cp,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs, res_t *res) {
    perf_function_t perf_func
            = std::bind(&compiled_partition_executor, cp, std::placeholders::_1,
                    std::placeholders::_2, std::placeholders::_3);
    const dnnl::graph::engine &engine = get_test_engine();

    int status = execute_and_wait(perf_func, engine, inputs, outputs);
    if (res) res->state = EXECUTED;

    return status;
};

inline int measure_perf_individual(timer::timer_t &t,
        dnnl::graph::stream &stream, perf_function_t &perf_func,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs) {
    t.reset();
    while (true) {
        BENCHDNNEXT_SAFE(perf_func(stream, inputs, outputs), WARN);
        t.stamp();
        if (should_stop(t)) break;
    }
    return OK;
}

int measure_perf(timer::timer_t &t, perf_function_t &perf_func,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs) {
    if (is_bench_mode(PERF)) {
        dnnl::graph::stream stream = get_test_stream();
        return measure_perf_individual(t, stream, perf_func, inputs, outputs);
    } else {
        return OK;
    }
}

int measure_perf(timer::timer_t &t, dnnl::graph::compiled_partition &cp,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs) {
    perf_function_t perf_func
            = std::bind(&compiled_partition_executor, cp, std::placeholders::_1,
                    std::placeholders::_2, std::placeholders::_3);

    return measure_perf(t, perf_func, inputs, outputs);
}

int measure_perf(timer::timer_t &t, dnnl::graph::compiled_partition &cp,
        const std::vector<dnnl::graph::tensor> &inputs,
        const std::vector<dnnl::graph::tensor> &outputs, res_t *res) {
    perf_function_t perf_func
            = std::bind(&compiled_partition_executor, cp, std::placeholders::_1,
                    std::placeholders::_2, std::placeholders::_3);
    int status = measure_perf(t, perf_func, inputs, outputs);
    if (res) res->state = EXECUTED;

    return status;
}

int measure_partition_compl(timer::timer_t &ct,
        const dnnl::graph::partition &par,
        const std::vector<dnnl::graph::logical_tensor> &inputs,
        const std::vector<dnnl::graph::logical_tensor> &outputs,
        const dnnl::graph::engine &engine) {
    ct.reset();
    while (true) {
        par.compile(inputs, outputs, engine);
        ct.stamp();
        if (should_stop_ctime(ct)) break;
    }

    return OK;
}

#define BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC( \
        container, id, dtype, dims, alogical_tensor) \
    if (lt::opaque == alogical_tensor.get_layout_type()) { \
        container.emplace(id, dtype, dims, alogical_tensor.get_layout_id()); \
    } else if (lt::strided == alogical_tensor.get_layout_type()) { \
        container.emplace(id, dtype, dims, alogical_tensor.get_strides()); \
    } else { \
        return fill_status::UNKNOWN_ERROR; \
    }

fill_status_t po_handlers_t::bias_po_handler_t::operator()(graph_prb_t &p,
        const std::string &dst_dataf,
        const dnnl::graph::logical_tensor::data_type bia_dt) {
    using op = dnnl::graph::op;

    const auto &dst_lt = p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"];
    const auto dst_dims = dst_lt.get_dims();
    const auto dst_dt = dst_lt.get_data_type();
    const dim_t channels = (dst_dataf == "NCX") ? dst_dims[1] : dst_dims.back();
    const dims_t bia_dims = {channels};

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["bias"].push_back(TENSOR_ID);
    const std::string BIA_SRC {TENSOR_ID + "_SRC"};
    const std::string BIA_DST {TENSOR_ID + "_DST"};

    p.tensor_descs_.emplace(BIA_SRC, bia_dt, bia_dims, lt::strided,
            tensor_descs_t::property_type::constant);
    BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC(
            p.tensor_descs_, BIA_DST, dst_dt, dst_dims, dst_lt);

    op bias(new_op_id, op::kind::BiasAdd,
            {p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"],
                    p.tensor_descs_[BIA_SRC]},
            {p.tensor_descs_[BIA_DST]}, "bias");

    bias.set_attr("data_format", dst_dataf);

    p.ops_.emplace_back(bias);
    p.curr_out_map_ids_.assign({TENSOR_ID});

    return fill_status::DONE;
}

fill_status_t po_handlers_t::eltwise_po_handler_t::operator()(
        graph_prb_t &p, const attr_t::post_ops_t::entry_t &po_entry) {
    using op = dnnl::graph::op;

    const auto requested_post_op_kind = convert_alg_kind(po_entry.eltwise.alg);
    const auto is_swish = should_handle_swish(p, po_entry.eltwise.alg);
    if (requested_post_op_kind == op::kind::LastSymbol && !is_swish)
        return fill_status::UNSUPPORTED_OP;
    const auto post_op_kind
            = (is_swish) ? op::kind::Sigmoid : requested_post_op_kind;

    const auto &dst_lt = p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"];
    const auto dst_dims = dst_lt.get_dims();
    const auto dst_dt = dst_lt.get_data_type();

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["eltwise"].push_back(TENSOR_ID);
    const std::string ELT_DST {TENSOR_ID + "_DST"};

    BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC(
            p.tensor_descs_, ELT_DST, dst_dt, dst_dims, dst_lt);

    op eltwise(new_op_id, post_op_kind,
            {p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"]},
            {p.tensor_descs_[ELT_DST]}, "eltwise");

    const auto attrs = convert_eltw_entry(post_op_kind, po_entry);
    for (const auto &kv : attrs) {
        eltwise.set_attr(kv.first, kv.second);
    }
    if (po_entry.eltwise.alg == dnnl_eltwise_soft_relu) {
        eltwise.set_attr("beta", static_cast<int64_t>(1));
    } else if (po_entry.eltwise.alg == dnnl_eltwise_logsigmoid) {
        eltwise.set_attr("beta", static_cast<int64_t>(-1));
    }

    p.ops_.emplace_back(eltwise);
    p.curr_out_map_ids_.assign({TENSOR_ID});

    if (is_swish) {
        const size_t new_op_id = p.ops_.size();
        const std::string TENSOR_ID = std::to_string(new_op_id);
        p.tensor_id["binary"].push_back(TENSOR_ID);
        const std::string BIN_DST {TENSOR_ID + "_DST"};
        const std::string BIA_DST = p.tensor_id["bias"].back() + "_DST";

        p.tensor_descs_.emplace(BIN_DST, dst_dt, dst_dims, lt::strided);
        op binary(new_op_id, op::kind::Multiply,
                {p.tensor_descs_[ELT_DST], p.tensor_descs_[BIA_DST]},
                {p.tensor_descs_[BIN_DST]}, "binary");
        p.ops_.emplace_back(binary);
        p.curr_out_map_ids_.assign({TENSOR_ID});
    }

    return fill_status::DONE;
}

fill_status_t po_handlers_t::binary_po_handler_t::operator()(graph_prb_t &p,
        const std::string &dst_dataf,
        const attr_t::post_ops_t::entry_t &po_entry) {
    using op = dnnl::graph::op;

    const auto post_op_kind = convert_alg_kind(po_entry.binary.alg);
    if (post_op_kind == op::kind::LastSymbol)
        return fill_status::UNSUPPORTED_OP;

    const auto &dst_lt = p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"];
    const auto dst_dims = dst_lt.get_dims();
    const auto dst_dt = dst_lt.get_data_type();
    const auto bin_src_dims
            = convert_bin_policy(dst_dims, po_entry.binary.policy, dst_dataf);
    const auto bin_src_dt = p.with_quantization()
            ? dt::f32
            : convert_dt(po_entry.binary.src1_dt);

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["binary"].push_back(TENSOR_ID);
    const std::string BIN_SRC {TENSOR_ID + "_SRC"};
    const std::string BIN_DST {TENSOR_ID + "_DST"};

    if (bin_src_dims != dst_dims) {
        p.tensor_descs_.emplace(
                BIN_SRC, bin_src_dt, bin_src_dims, po_entry.binary.tag);
    } else {
        BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC(
                p.tensor_descs_, BIN_SRC, bin_src_dt, bin_src_dims, dst_lt);
    }
    BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC(
            p.tensor_descs_, BIN_DST, dst_dt, dst_dims, dst_lt);

    op binary(new_op_id, post_op_kind,
            {p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"],
                    p.tensor_descs_[BIN_SRC]},
            {p.tensor_descs_[BIN_DST]}, "binary");
    if (bin_src_dims == dst_dims)
        binary.set_attr("auto_broadcast", std::string("none"));

    p.ops_.emplace_back(binary);
    p.curr_out_map_ids_.assign({TENSOR_ID});

    return fill_status::DONE;
}

fill_status_t po_handlers_t::sum_po_handler_t::operator()(graph_prb_t &p) {
    using op = dnnl::graph::op;

    const auto &dst_lt = p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"];
    const auto dst_dims = dst_lt.get_dims();
    const auto dst_dt = dst_lt.get_data_type();

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["sum"].push_back(TENSOR_ID);
    const std::string SUM_SRC {TENSOR_ID + "_SRC"};
    const std::string SUM_DST {TENSOR_ID + "_DST"};

    BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC(
            p.tensor_descs_, SUM_SRC, dst_dt, dst_dims, dst_lt);
    BENCHDNN_EXTENSION_EMPLACE_TENSOR_DESC(
            p.tensor_descs_, SUM_DST, dst_dt, dst_dims, dst_lt);

    op sum(new_op_id, op::kind::Add,
            {p.tensor_descs_[p.curr_out_map_ids_.front() + "_DST"],
                    p.tensor_descs_[SUM_SRC]},
            {p.tensor_descs_[SUM_DST]}, "sum");
    sum.set_attr("auto_broadcast", std::string {"none"});
    p.ops_.emplace_back(sum);
    p.curr_out_map_ids_.assign({TENSOR_ID});

    return fill_status::DONE;
}

fill_status po_handlers_t::low_precision_handler_t::handle_low_precision_src(
        graph_prb_t &p, const low_precision_attr &lp_attr) {
    using op = dnnl::graph::op;

    const auto src_lt = p.tensor_descs_[p.tensor_id["main"].back() + "_SRC"];
    const auto src_dims = src_lt.get_dims();
    const auto src_dt = lp_attr.src_dt;

    const std::string IN_KEY = lp_attr.with_typecast ? "typecast" : "main";
    const std::string SRC = p.tensor_id[IN_KEY].back() + "_SRC";

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["dequant_src"].push_back(TENSOR_ID);
    const std::string QSRC {TENSOR_ID + "_SRC"};

    const std::string qsrc_type = src_dt == dt::u8 ? "uint8" : "int8";

    p.tensor_descs_.emplace(QSRC, src_dt, src_dims, lp_attr.stag);

    op dequant_src(new_op_id, op::kind::Dequantize, {p.tensor_descs_[QSRC]},
            {p.tensor_descs_[SRC]}, "dequant_src");
    dequant_src.set_attr("scales", std::vector<float> {1.f})
            .set_attr("zps",
                    (lp_attr.src_zp == nullptr) ? std::vector<int64_t> {0L}
                                                : *lp_attr.src_zp)
            .set_attr("qtype", std::string("per_tensor"))
            .set_attr("axis", static_cast<int64_t>(0));
    p.ops_.emplace_back(dequant_src);

    return fill_status::DONE;
}

fill_status po_handlers_t::low_precision_handler_t::handle_low_precision_srcs(
        graph_prb_t &p, const low_precision_attr &lp_attr,
        const size_t num_srcs) {
    using op = dnnl::graph::op;

    for (size_t i = 0; i < num_srcs; i++) {
        const auto SRC_I_STR = "_SRC" + std::to_string(i);
        const auto src_lt
                = p.tensor_descs_[p.tensor_id["main"].back() + SRC_I_STR];
        const auto src_dims = src_lt.get_dims();
        const auto src_dt = lp_attr.src_dt;

        const std::string IN_KEY = lp_attr.with_typecast ? "typecast" : "main";
        const std::string SRC_I = p.tensor_id[IN_KEY].back() + SRC_I_STR;

        const size_t new_op_id = p.ops_.size();
        const std::string TENSOR_ID = std::to_string(new_op_id);
        p.tensor_id["dequant_src"].push_back(TENSOR_ID);
        const std::string QSRC {TENSOR_ID + SRC_I_STR};

        const std::string qsrc_type = src_dt == dt::u8 ? "uint8" : "int8";

        p.tensor_descs_.emplace(QSRC, src_dt, src_dims, lp_attr.stag);

        op dequant_src(new_op_id, op::kind::Dequantize, {p.tensor_descs_[QSRC]},
                {p.tensor_descs_[SRC_I]}, "dequant_src" + std::to_string(i));
        dequant_src.set_attr("scales", std::vector<float> {1.f})
                .set_attr("zps",
                        (lp_attr.src_zp == nullptr) ? std::vector<int64_t> {0L}
                                                    : *lp_attr.src_zp)
                .set_attr("qtype", std::string("per_tensor"))
                .set_attr("axis", static_cast<int64_t>(0));
        p.ops_.emplace_back(dequant_src);
    }

    return fill_status::DONE;
}

fill_status po_handlers_t::low_precision_handler_t::handle_low_precision_wei(
        graph_prb_t &p, const low_precision_attr &lp_attr) {
    using op = dnnl::graph::op;

    const auto wei_lt = p.tensor_descs_[p.tensor_id["main"].back() + "_WEI"];
    auto wei_dims = wei_lt.get_dims();
    const auto wei_dt = lp_attr.wei_dt;

    const std::string IN_KEY = lp_attr.with_typecast ? "typecast" : "main";
    const std::string WEI = p.tensor_id[IN_KEY].back() + "_WEI";

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["dequant_wei"].push_back(TENSOR_ID);
    const std::string QWEI {TENSOR_ID + "_WEI"};

    const std::string qwei_type = wei_dt == dt::u8 ? "uint8" : "int8";

    if (lp_attr.wei_strides.size() > 0) {
        p.tensor_descs_.emplace(QWEI, wei_dt, wei_dims, lp_attr.wei_strides,
                tensor_descs_t::property_type::constant);
    } else {
        p.tensor_descs_.emplace(QWEI, wei_dt, wei_dims, lp_attr.wtag,
                tensor_descs_t::property_type::constant);
    }

    const std::string qtype = lp_attr.oscale_policy == policy_t::COMMON
            ? "per_tensor"
            : "per_channel";
    const int64_t count
            = lp_attr.oscale_policy == policy_t::COMMON ? 1 : lp_attr.n_oc;

    lp_attr.oscales->resize(count, 1.f);
    if (!lp_attr.def_oscales) {
        for (int64_t c = 0; c < count; ++c) {
            (*lp_attr.oscales)[c] = lp_attr.scales[c];
        }
    }

    op dequant_wei(new_op_id, op::kind::Dequantize, {p.tensor_descs_[QWEI]},
            {p.tensor_descs_[WEI]}, "dequant_wei");
    dequant_wei.set_attr("scales", *lp_attr.oscales)
            .set_attr("zps",
                    (lp_attr.wei_zp == nullptr) ? std::vector<int64_t> {0L}
                                                : *lp_attr.wei_zp)
            .set_attr("qtype", qtype)
            .set_attr("axis", static_cast<int64_t>(0));
    p.ops_.emplace_back(dequant_wei);

    return fill_status::DONE;
}

fill_status po_handlers_t::low_precision_handler_t::handle_low_precision_dst(
        graph_prb_t &p, const low_precision_attr &lp_attr) {
    using op = dnnl::graph::op;

    const auto dst_lt = p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"];
    const auto dst_dims = dst_lt.get_dims();
    const auto dst_dt = lp_attr.dst_dt;

    const std::string DST = p.curr_out_map_ids_.back() + "_DST";

    size_t new_op_id = p.ops_.size();
    std::string TENSOR_ID = std::to_string(new_op_id);
    p.tensor_id["quant_dst"].push_back(TENSOR_ID);
    const std::string QDST {TENSOR_ID + "_DST"};

    const std::string qdst_type = dst_dt == dt::u8 ? "uint8" : "int8";

    p.tensor_descs_.emplace(QDST, dst_dt, dst_dims, lp_attr.dtag);

    op quant_dst(new_op_id, op::kind::Quantize, {p.tensor_descs_[DST]},
            {p.tensor_descs_[QDST]}, "quant_dst");
    quant_dst.set_attr("scales", std::vector<float> {lp_attr.dst_scale})
            .set_attr("zps",
                    (lp_attr.dst_zp == nullptr) ? std::vector<int64_t> {0L}
                                                : *lp_attr.dst_zp)
            .set_attr("qtype", std::string("per_tensor"))
            .set_attr("axis", static_cast<int64_t>(0));
    p.ops_.emplace_back(quant_dst);

    p.curr_out_map_ids_.assign({TENSOR_ID});

    return fill_status::DONE;
}

fill_status
po_handlers_t::low_precision_handler_t::handle_low_precision_post_sum(
        graph_prb_t &p, const low_precision_attr &lp_attr,
        const std::vector<attr_t::post_ops_t::entry_t> &po_entry) {
    using op = dnnl::graph::op;

    const auto dst_lt = p.tensor_descs_[p.curr_out_map_ids_.back() + "_DST"];
    const auto dst_dims = dst_lt.get_dims();

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    const std::string QPSUM_SRC {TENSOR_ID + "_SUM_SRC1"};
    const std::string POST_SUM_SRC = p.tensor_id["sum"].back() + "_SRC";

    float sum_scale_val = 1.f;
    int64_t sum_zp_val = 0L;
    dt sum_src_dt = dt::undef;
    for (const attr_t::post_ops_t::entry_t &po : po_entry) {
        if (po.is_sum_kind()) {
            sum_scale_val = po.sum.scale;
            sum_zp_val = po.sum.zero_point;
            sum_src_dt = convert_dt(po.sum.dt);
        }
    }
    const std::vector<float> sum_scales {sum_scale_val};
    const std::vector<int64_t> sum_zp {sum_zp_val};
    if (dt::undef == sum_src_dt) sum_src_dt = lp_attr.dst_dt;

    p.tensor_descs_.emplace(QPSUM_SRC, sum_src_dt, dst_dims, lp_attr.dtag);
    op dequant_sum(p.ops_.size(), op::kind::Dequantize,
            {p.tensor_descs_[QPSUM_SRC]}, {p.tensor_descs_[POST_SUM_SRC]},
            "dequant_sum");

    dequant_sum.set_attr("scales", sum_scales).set_attr("zps", sum_zp);
    p.ops_.emplace_back(dequant_sum);
    return fill_status::DONE;
}

fill_status
po_handlers_t::low_precision_handler_t::handle_low_precision_post_bin(
        graph_prb_t &p, const low_precision_attr &lp_attr,
        const std::vector<attr_t::post_ops_t::entry_t> &po_entry) {
    using op = dnnl::graph::op;

    const size_t new_op_id = p.ops_.size();
    const std::string TENSOR_ID = std::to_string(new_op_id);
    const std::string QPBIN_SRC {TENSOR_ID + "_BIN_SRC1"};
    const std::string POST_BIN_SRC = p.tensor_id["binary"].back() + "_SRC";

    const float bin_scale_val = 1.f;
    const int64_t bin_zp_val = 0L;
    dt bin_src_dt = dt::undef;
    std::string bin_src_tag = "any";
    for (const attr_t::post_ops_t::entry_t &po : po_entry) {
        if (po.is_binary_kind()) {
            bin_src_dt = convert_dt(po.binary.src1_dt);
            bin_src_tag = po.binary.tag;
        }
    }

    const std::vector<float> bin_scales {bin_scale_val};
    const std::vector<int64_t> bin_zp {bin_zp_val};
    if (dt::undef == bin_src_dt) bin_src_dt = lp_attr.dst_dt;

    p.tensor_descs_.emplace(QPBIN_SRC, bin_src_dt,
            p.tensor_descs_[POST_BIN_SRC].get_dims(), bin_src_tag);
    op dequant_bin(p.ops_.size(), op::kind::Dequantize,
            {p.tensor_descs_[QPBIN_SRC]}, {p.tensor_descs_[POST_BIN_SRC]},
            "dequant_bin");

    dequant_bin.set_attr("scales", bin_scales).set_attr("zps", bin_zp);
    p.ops_.emplace_back(dequant_bin);

    return fill_status::DONE;
}

#ifdef DNNL_GRAPH_WITH_SYCL
void *sycl_alloc(size_t n, const void *dev, const void *ctx,
        dnnl::graph::allocator::attribute attr) {
    return cl::sycl::malloc_shared(n,
            *static_cast<const cl::sycl::device *>(dev),
            *static_cast<const cl::sycl::context *>(ctx));
}

void sycl_free(void *ptr, const void *ctx) {
    return cl::sycl::free(ptr, *static_cast<const cl::sycl::context *>(ctx));
}

const dnnl::graph::engine &get_graph_engine() {
    static auto sycl_allocator {
            dnnl::graph::sycl_interop::make_allocator(sycl_alloc, sycl_free)};
    static dnnl::engine test_eng {::get_test_engine()};
    static cl::sycl::device dev {dnnl::sycl_interop::get_device(test_eng)};
    static cl::sycl::context ctx {dnnl::sycl_interop::get_context(test_eng)};
    static dnnl::graph::engine eng {
            dnnl::graph::sycl_interop::make_engine(dev, ctx, sycl_allocator)};
    return eng;
}

dnnl::graph::stream &get_graph_stream() {
    static dnnl::engine test_eng {::get_test_engine()};
    static cl::sycl::device dev {dnnl::sycl_interop::get_device(test_eng)};
    static cl::sycl::context ctx {dnnl::sycl_interop::get_context(test_eng)};

    static cl::sycl::queue q {ctx, dev, cl::sycl::property::queue::in_order {}};

    static dnnl::graph::stream strm {
            dnnl::graph::sycl_interop::make_stream(get_graph_engine(), q)};
    return strm;
}
#endif // DNNL_GRAPH_WITH_SYCL

// Engine used to run oneDNN fusion patterns for testing.
const dnnl::graph::engine &get_test_engine() {
    using engine = dnnl::graph::engine;
    if (get_test_engine_kind() == dnnl_cpu) {
#ifdef DNNL_GRAPH_CPU_SYCL
        static engine eng(get_graph_engine());
#else
        static engine eng(engine::kind::cpu, static_cast<int>(engine_index));
#endif
        return eng;
    } else {
#ifdef DNNL_GRAPH_GPU_SYCL
        static engine eng(get_graph_engine());
#else
        assert(!"GPU only support DPCPP runtime now");
        static engine eng(engine::kind::gpu, static_cast<int>(engine_index));
#endif
        return eng;
    }
}

const dnnl::graph::stream &get_test_stream() {
    using stream = dnnl::graph::stream;
    if (get_test_engine_kind() == dnnl_cpu) {
#ifdef DNNL_GRAPH_CPU_SYCL
        static const stream strm(get_graph_stream());
#elif DNNL_GRAPH_CPU_RUNTIME == DNNL_GRAPH_RUNTIME_THREADPOOL
        static const stream strm {dnnl::graph::threadpool_interop::make_stream(
                get_test_engine(), dnnl::graph::testing::get_threadpool())};
#else
        static const stream strm(
                const_cast<dnnl::graph::engine &>(get_test_engine()));
#endif
        return strm;
    } else {
#ifdef DNNL_GRAPH_GPU_SYCL
        static const stream strm(
                const_cast<dnnl::graph::stream &>(get_graph_stream()));
#else
        assert(!"GPU only support DPCPP runtime now");
        static const stream strm(
                const_cast<dnnl::graph::engine &>(get_test_engine()));
#endif
        return strm;
    }
}

} // namespace benchdnnext