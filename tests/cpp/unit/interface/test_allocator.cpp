/*******************************************************************************
* Copyright 2020-2021 Intel Corporation
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

#include <gtest/gtest.h>

#include <mutex>
#include <thread>

#include "interface/allocator.hpp"
#include "interface/c_types_map.hpp"

#include "cpp/unit/utils.hpp"

TEST(allocator_test, default_cpu_allocator) {
    dnnl::graph::impl::allocator_t *alloc
            = dnnl::graph::impl::allocator_t::create();

    dnnl::graph::impl::allocator_t::attribute_t attr {
            dnnl::graph::impl::allocator_lifetime::persistent, 4096};
    void *mem_ptr = alloc->allocate(static_cast<size_t>(16));
    ASSERT_NE(mem_ptr, nullptr);
    alloc->deallocate(mem_ptr);
    alloc->release();
}

TEST(allocator_test, create_attr) {
    dnnl::graph::impl::allocator_t::attribute_t attr {
            dnnl::graph::impl::allocator_lifetime::output, 1024};

    ASSERT_EQ(attr.data.alignment, 1024);
    ASSERT_EQ(attr.data.type, dnnl::graph::impl::allocator_lifetime::output);
}

#ifndef NDEBUG
TEST(allocator_test, monitor) {
    using namespace dnnl::graph::impl;

    const size_t temp_size = 1024, persist_size = 512;

    allocator_t *alloc = allocator_t::create();
    std::vector<void *> persist_bufs;
    std::mutex m;

    auto callee = [&]() {
        // allocate persistent buffer
        void *p_buf = alloc->allocate(persist_size,
                allocator_t::attribute_t {
                        allocator_lifetime::persistent, 4096});
        {
            std::lock_guard<std::mutex> lock(m);
            persist_bufs.emplace_back(p_buf);
        }

        // allocate temporary buffer
        void *t_buf = alloc->allocate(temp_size,
                allocator_t::attribute_t {allocator_lifetime::temp, 4096});
        for (size_t i = 0; i < temp_size; i++) {
            char *ptr = (char *)t_buf + i;
            *ptr = *ptr + 2;
        }
        // deallocate temporary buffer
        alloc->deallocate(t_buf);
    };

    // single thread
    for (size_t iter = 0; iter < 4; iter++) {
        allocator_t::monitor_t::reset_peak_temp_memory(alloc);
        ASSERT_EQ(allocator_t::monitor_t::get_peak_temp_memory(alloc), 0);

        callee(); // call the callee to do memory operation

        ASSERT_EQ(
                allocator_t::monitor_t::get_peak_temp_memory(alloc), temp_size);
        ASSERT_EQ(allocator_t::monitor_t::get_total_persist_memory(alloc),
                persist_size * (iter + 1));
    }

    for (auto p_buf : persist_bufs) {
        alloc->deallocate(p_buf);
    }
    persist_bufs.clear();

    // multiple threads
    auto thread_func = [&]() {
        allocator_t::monitor_t::reset_peak_temp_memory(alloc);
        ASSERT_EQ(allocator_t::monitor_t::get_peak_temp_memory(alloc), 0);
        callee();
        ASSERT_EQ(
                allocator_t::monitor_t::get_peak_temp_memory(alloc), temp_size);
    };

    std::thread t1(thread_func);
    std::thread t2(thread_func);

    t1.join();
    t2.join();

    // two threads allocated persist buffer
    ASSERT_EQ(allocator_t::monitor_t::get_total_persist_memory(alloc),
            persist_size * 2);

    for (auto p_buf : persist_bufs) {
        alloc->deallocate(p_buf);
    }
    persist_bufs.clear();

    alloc->release();
}
#endif
