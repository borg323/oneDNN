/*******************************************************************************
* Copyright 2022 Intel Corporation
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

#include "gpu/jit/conv/slm_reduce_builder.hpp"

#include <algorithm>

#include "gpu/jit/conv/builder_utils.hpp"
#include "gpu/jit/conv/message_support.hpp"
#include "gpu/jit/conv/reduce_support.hpp"
#include "gpu/jit/conv/utils.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

slm_reduce_builder_t::slm_reduce_builder_t(const hw_config_t &hw_cfg,
        ir_context_t &ir_ctx, const constraint_set_t &cset,
        const grid_info_t &tg_grid, const expr_t &reg_buf,
        const layout_t &reg_layout, const tensor_t &thr_tile, int dim)
    : hw_cfg_(hw_cfg)
    , tg_grid_(tg_grid)
    , reg_buf_(reg_buf)
    , reg_layout_(reg_layout)
    , thr_tile_(thr_tile)
    , dim_(dim) {
    ir_assert((dim_ >= 0) && (dim_ <= 2));
    ir_assert(tg_grid_.dim(dim_) > 1);

    tmp_reg_buf_ = ir_ctx.create_tmp_var(type_t::byte_ptr());
    slm_buf_ = ir_ctx.create_tmp_var(type_t::byte_ptr(), "reduce_slm");
    tg_ndims_ = (dim_ != 2) ? dim_ + 1 : tg_grid_.ndims();

    build(ir_ctx, cset);
}

void slm_reduce_builder_t::build(
        ir_context_t &ir_ctx, const constraint_set_t &cset) {
    int ndims = reg_layout_.ndims();

    // Create SLM layout to store all intermediate buffers from the thread
    // group.
    layout_t slm_layout(reg_layout_.type(), ndims + tg_ndims_,
            reg_layout_.offset(), reg_layout_.blocks());
    for (int i = tg_ndims_ - 1; i >= 0; i--) {
        slm_layout = slm_layout.add_outer_block(ndims + i, tg_grid_.dim(i));
    }

    slm_buf_size_ = slm_layout.size();

    // Write thread tile to SLM.
    std::vector<dim_t> write_dims = reg_layout_.dims();
    std::vector<expr_t> write_start(ndims + tg_ndims_, 0);
    write_dims.resize(ndims + tg_ndims_, 1);
    for (int i = tg_ndims_ - 1; i >= 0; i--) {
        write_start[ndims + i] = tg_grid_.idx(i);
    }
    auto write_tile = tensor_t(write_dims, write_start);
    auto write = make_access_builder(hw_cfg_, ir_ctx, cset,
            view_t(slm_layout.map(write_tile)), slm_buf_, reg_buf_,
            send_op_t::store, send_address_t::slm);
    store_stmt_ = write.stmt();

    auto &write_layout = write.reg_layout();
    ir_assert(write_layout == reg_layout_) << "Incompatible layouts.";

    // Redistribute the layout to read/reduce all k-axis tiles from every
    // thread.
    auto local_thr_tile = reg_layout_.split(tg_grid_.sub_grid({dim_}));
    reg_layout_ = reg_layout_.map(tensor_t(local_thr_tile.dims()));

    std::vector<dim_t> read_dims(ndims + tg_ndims_, 1);
    std::vector<expr_t> read_start(ndims + tg_ndims_);
    for (int i = 0; i < ndims; i++) {
        read_dims[i] = local_thr_tile(i);
        read_start[i] = local_thr_tile.start(i);
    }
    read_dims[ndims + dim_] = tg_grid_.dim(dim_);
    for (int i = 0; i < tg_ndims_; i++) {
        read_start[ndims + i] = (i == dim_) ? 0 : tg_grid_.idx(i);
    }
    tensor_t read_tile(read_dims, read_start);
    auto read = make_access_builder(hw_cfg_, ir_ctx, cset,
            view_t(slm_layout.map(read_tile)), slm_buf_, tmp_reg_buf_,
            send_op_t::load, send_address_t::slm);

    load_stmt_ = load_stmt_.append(
            create_zero_out_stmt(hw_cfg_.hw(), reg_buf_, reg_layout_.size()));
    load_stmt_ = load_stmt_.append(read.stmt());

    tmp_reg_buf_size_ = std::max(tmp_reg_buf_size_, read.reg_buf_size());

    auto read_layout = read.reg_layout();
    load_stmt_ = load_stmt_.append(create_reduce_stmt(read_layout, reg_layout_,
            tmp_reg_buf_, reg_buf_, tensor_t(), reduction_mask()));

    allocs_.push_back(
            alloc_t::make(slm_buf_, slm_buf_size_, alloc_kind_t::slm));
    allocs_.push_back(
            alloc_t::make(tmp_reg_buf_, tmp_reg_buf_size_, alloc_kind_t::grf));

    if (!thr_tile_.is_empty()) {
        thr_tile_ = thr_tile_.create_sub_tensor(local_thr_tile);
    }
}

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl