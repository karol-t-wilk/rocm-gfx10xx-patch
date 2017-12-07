// MIT License
//
// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <iostream>
#include <vector>
#include <algorithm>

// Google Test
#include <gtest/gtest.h>
// HC API
#include <hcc/hc.hpp>
// rocPRIM
#include <warp/warp_scan.hpp>

#include "test_utils.hpp"

namespace rp = rocprim;

template<typename WarpSizeWrapper>
class RocprimWarpScanShuffleBasedTests : public ::testing::Test {
public:
    static constexpr unsigned int warp_size = WarpSizeWrapper::value;
};

TYPED_TEST_CASE(RocprimWarpScanShuffleBasedTests, WarpSizes);

TYPED_TEST(RocprimWarpScanShuffleBasedTests, InclusiveScanInt)
{
    // logical warp side for warp primitive, execution warp size is always rp::warp_size()
    constexpr size_t logical_warp_size = TestFixture::warp_size;
    const size_t block_size = std::max<size_t>(rp::warp_size(), 4 * logical_warp_size);
    const size_t size = block_size * 4;

    // Given warp size not supported
    if(logical_warp_size > rp::warp_size() || !rp::detail::is_power_of_two(logical_warp_size))
    {
        return;
    }

    // Generate data
    std::vector<int> output = get_random_data<int>(size, -100, 100);

    // Calulcate expected results on host
    std::vector<int> expected(output.size(), 0);
    for(size_t i = 0; i < output.size() / logical_warp_size; i++)
    {
        for(size_t j = 0; j < logical_warp_size; j++)
        {
            auto idx = i * logical_warp_size + j;
            expected[idx] = output[idx] + expected[j > 0 ? idx-1 : idx];
        }
    }

    hc::array_view<int, 1> d_output(output.size(), output.data());
    hc::parallel_for_each(
        hc::extent<1>(output.size()).tile(block_size),
        [=](hc::tiled_index<1> i) [[hc]]
        {
            int value = d_output[i];
            rp::warp_scan<int, logical_warp_size> wscan;
            wscan.inclusive_scan(value, value);
            d_output[i] = value;
        }
    );

    d_output.synchronize();
    for(int i = 0; i < output.size(); i++)
    {
        EXPECT_EQ(output[i], expected[i]);
    }
}

TYPED_TEST(RocprimWarpScanShuffleBasedTests, InclusiveScanReduceInt)
{
    // logical warp side for warp primitive, execution warp size is always rp::warp_size()
    constexpr size_t logical_warp_size = TestFixture::warp_size;
    const size_t block_size = std::max<size_t>(rp::warp_size(), 4 * logical_warp_size);
    const size_t size = block_size * 4;

    // Given warp size not supported
    if(logical_warp_size > rp::warp_size() || !rp::detail::is_power_of_two(logical_warp_size))
    {
        return;
    }

    // Generate data
    std::vector<int> output = get_random_data<int>(size, -100, 100);
    std::vector<int> output_reductions(size / logical_warp_size);

    // Calulcate expected results on host
    std::vector<int> expected(output.size(), 0);
    std::vector<int> expected_reductions(output.size(), 0);
    for(size_t i = 0; i < output.size() / logical_warp_size; i++)
    {
        for(size_t j = 0; j < logical_warp_size; j++)
        {
            auto idx = i * logical_warp_size + j;
            expected[idx] = output[idx] + expected[j > 0 ? idx-1 : idx];
        }
        expected_reductions[i] = expected[(i+1) * logical_warp_size - 1];
    }

    hc::array_view<int, 1> d_output(output.size(), output.data());
    hc::array_view<int, 1> d_output_r(
        output_reductions.size(), output_reductions.data()
    );
    hc::parallel_for_each(
        hc::extent<1>(output.size()).tile(block_size),
        [=](hc::tiled_index<1> i) [[hc]]
        {
            int value = d_output[i];
            int reduction;
            rp::warp_scan<int, logical_warp_size> wscan;
            wscan.inclusive_scan(value, value, reduction);
            d_output[i] = value;
            if(i.local[0]%logical_warp_size == 0)
            {
                d_output_r[i.global[0]/logical_warp_size] = reduction;
            }
        }
    );

    d_output.synchronize();
    for(int i = 0; i < output.size(); i++)
    {
        EXPECT_EQ(output[i], expected[i]);
    }

    d_output_r.synchronize();
    for(int i = 0; i < output_reductions.size(); i++)
    {
        EXPECT_EQ(output_reductions[i], expected_reductions[i]);
    }
}