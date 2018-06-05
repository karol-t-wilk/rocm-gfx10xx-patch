// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_DEVICE_DETAIL_DEVICE_HISTOGRAM_HPP_
#define ROCPRIM_DEVICE_DETAIL_DEVICE_HISTOGRAM_HPP_

#include <cmath>
#include <type_traits>
#include <iterator>

#include "../../config.hpp"
#include "../../detail/various.hpp"

#include "../../intrinsics.hpp"
#include "../../functional.hpp"

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

// Special wrapper for passing fixed-length arrays (i.e. T values[Size]) into kernels
template<class T, unsigned int Size>
class fixed_array
{
// Workaround: HCC does not support passing structs with array-typed members into kernels
// Custom serializer and deserializer are used:
#ifdef ROCPRIM_HC_API

private:
    static constexpr unsigned int MaxSize = 4;
    T values[MaxSize];
    static_assert(Size <= MaxSize, "HC implementation does not support Size greater than MaxSize");

public:

    __attribute__((annotate("serialize")))
    ROCPRIM_HOST inline
    void __cxxamp_serialize(Kalmar::Serialize& s) const
    {
        for(unsigned int i = 0; i < MaxSize; i++)
        {
            s.Append(sizeof(T), &values[i]);
        }
    }

    __attribute__((annotate("user_deserialize")))
    ROCPRIM_HOST_DEVICE inline
    fixed_array(T value0, T value1, T value2, T value3)
    {
        // Implementation for MaxSize = 4 only
        values[0] = value0;
        values[1] = value1;
        values[2] = value2;
        values[3] = value3;
    }

#else

private:
    T values[Size];

#endif

public:

    ROCPRIM_HOST_DEVICE inline
    fixed_array(const T values[Size])
    {
        for(unsigned int i = 0; i < Size; i++)
        {
            this->values[i] = values[i];
        }
    }

    ROCPRIM_HOST_DEVICE inline
    T& operator[](unsigned int index)
    {
        return values[index];
    }

    ROCPRIM_HOST_DEVICE inline
    const T& operator[](unsigned int index) const
    {
        return values[index];
    }
};

template<class Level>
struct sample_to_bin_even
{
    unsigned int bins;
    Level lower_level;
    Level upper_level;
    Level scale;

    ROCPRIM_HOST_DEVICE inline
    sample_to_bin_even() = default;

    ROCPRIM_HOST_DEVICE inline
    sample_to_bin_even(unsigned int bins, Level lower_level, Level upper_level)
        : bins(bins),
          lower_level(lower_level),
          upper_level(upper_level),
          scale((upper_level - lower_level) / bins)
    {}

    template<class Sample>
    ROCPRIM_HOST_DEVICE inline
    int operator()(Sample sample) const
    {
        const Level s = static_cast<Level>(sample);
        if(s >= lower_level && s < upper_level)
        {
            return (s - lower_level) / scale;
        }
        else
        {
            return -1;
        }
    }
};

// Returns index of the first element in values that is greater than value, or count if no such element is found.
template<class T>
ROCPRIM_HOST_DEVICE inline
unsigned int upper_bound(const T * values, unsigned int count, T value)
{
    unsigned int current = 0;
    while(count > 0)
    {
        const unsigned int step = count / 2;
        const unsigned int next = current + step;
        if(value < values[next])
        {
            count = step;
        }
        else
        {
            current = next + 1;
            count -= step + 1;
        }
    }
    return current;
}

template<class Level>
struct sample_to_bin_range
{
    unsigned int bins;
    const Level * level_values;

    ROCPRIM_HOST_DEVICE inline
    sample_to_bin_range() = default;

    ROCPRIM_HOST_DEVICE inline
    sample_to_bin_range(unsigned int bins, const Level * level_values)
        : bins(bins), level_values(level_values)
    {}

    template<class Sample>
    ROCPRIM_HOST_DEVICE inline
    int operator()(Sample sample) const
    {
        const Level s = static_cast<Level>(sample);
        const unsigned int bin = upper_bound(level_values, bins + 1, s) - 1;
        if(bin < bins)
        {
            return bin;
        }
        else
        {
            return -1;
        }
    }
};

template<class T, unsigned int Size>
struct sample_vector
{
    T values[Size];
};

// TODO: add overloads for cases when SampleIterator is a pointer
// reinterpret_cast<const sample_vector_type *>(samples)

template<
    unsigned int ItemsPerThread,
    unsigned int Channels,
    class Sample,
    class SampleIterator
>
ROCPRIM_DEVICE inline
void load_samples(unsigned int flat_id,
                  SampleIterator samples,
                  sample_vector<Sample, Channels> (&values)[ItemsPerThread])
{
    Sample tmp[Channels * ItemsPerThread];
    block_load_direct_blocked(
        flat_id,
        samples,
        tmp
    );
    for(unsigned int i = 0; i < ItemsPerThread; i++)
    {
        for(unsigned int channel = 0; channel < Channels; channel++)
        {
            values[i].values[channel] = tmp[i * Channels + channel];
        }
    }
}

template<
    unsigned int ItemsPerThread,
    unsigned int Channels,
    class Sample,
    class SampleIterator
>
ROCPRIM_DEVICE inline
void load_samples(unsigned int flat_id,
                  SampleIterator samples,
                  sample_vector<Sample, Channels> (&values)[ItemsPerThread],
                  unsigned int valid_count)
{
    Sample tmp[Channels * ItemsPerThread];
    block_load_direct_blocked(
        flat_id,
        samples,
        tmp,
        valid_count * Channels
    );
    for(unsigned int i = 0; i < ItemsPerThread; i++)
    {
        for(unsigned int channel = 0; channel < Channels; channel++)
        {
            values[i].values[channel] = tmp[i * Channels + channel];
        }
    }
}

template<
    unsigned int BlockSize,
    unsigned int ActiveChannels,
    class Counter
>
ROCPRIM_DEVICE inline
void init_histogram(fixed_array<Counter *, ActiveChannels> histogram,
                    fixed_array<unsigned int, ActiveChannels> bins)
{
    const unsigned int flat_id = ::rocprim::detail::block_thread_id<0>();
    const unsigned int block_id = ::rocprim::detail::block_id<0>();

    const unsigned int index = block_id * BlockSize + flat_id;
    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        if(index < bins[channel])
        {
            histogram[channel][index] = 0;
        }
    }
}

template<
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class SampleToBinOp
>
ROCPRIM_DEVICE inline
void histogram_shared(SampleIterator samples,
                      unsigned int columns,
                      unsigned int rows,
                      unsigned int row_stride,
                      fixed_array<Counter *, ActiveChannels> histogram,
                      fixed_array<SampleToBinOp, ActiveChannels> sample_to_bin_op,
                      fixed_array<unsigned int, ActiveChannels> bins,
                      unsigned int * block_histogram_start)
{
    using sample_type = typename std::iterator_traits<SampleIterator>::value_type;
    using sample_vector_type = sample_vector<sample_type, Channels>;

    constexpr unsigned int items_per_block = BlockSize * ItemsPerThread;

    const unsigned int flat_id = ::rocprim::detail::block_thread_id<0>();
    const unsigned int block_id0 = ::rocprim::detail::block_id<0>();
    const unsigned int block_id1 = ::rocprim::detail::block_id<1>();
    const unsigned int grid_size0 = ::rocprim::detail::grid_size<0>();
    const unsigned int grid_size1 = ::rocprim::detail::grid_size<1>();
    const unsigned int rows_per_block = ::rocprim::detail::ceiling_div(rows, grid_size1);

    unsigned int * block_histogram[ActiveChannels];
    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        block_histogram[channel] = block_histogram_start;
        block_histogram_start += bins[channel];
        for(unsigned int bin = flat_id; bin < bins[channel]; bin += BlockSize)
        {
            block_histogram[channel][bin] = 0;
        }
    }
    ::rocprim::syncthreads();

    const unsigned int start_row = block_id1 * rows_per_block;
    const unsigned int end_row = ::rocprim::min(rows, start_row + rows_per_block);
    for(unsigned int row = start_row; row < end_row; row++)
    {
        SampleIterator row_samples = samples + row * row_stride;

        unsigned int block_offset = block_id0 * items_per_block;
        while(block_offset < columns)
        {
            sample_vector_type values[ItemsPerThread];

            unsigned int valid_count;
            if(block_offset + items_per_block <= columns)
            {
                valid_count = items_per_block;
                load_samples(flat_id, row_samples + Channels * block_offset, values);
            }
            else
            {
                valid_count = columns - block_offset;
                load_samples(flat_id, row_samples + Channels * block_offset, values, valid_count);
            }

            for(unsigned int i = 0; i < ItemsPerThread; i++)
            {
                if(flat_id * ItemsPerThread + i < valid_count)
                {
                    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
                    {
                        const int bin = sample_to_bin_op[channel](values[i].values[channel]);
                        if(bin != -1)
                        {
                            ::rocprim::detail::atomic_add(&block_histogram[channel][bin], 1);
                        }
                    }
                }
            }

            block_offset += grid_size0 * items_per_block;
        }
    }
    ::rocprim::syncthreads();

    for(unsigned int channel = 0; channel < ActiveChannels; channel++)
    {
        for(unsigned int bin = flat_id; bin < bins[channel]; bin += BlockSize)
        {
            if(block_histogram[channel][bin] > 0)
            {
                ::rocprim::detail::atomic_add(&histogram[channel][bin], block_histogram[channel][bin]);
            }
        }
    }
}

template<
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    unsigned int Channels,
    unsigned int ActiveChannels,
    class SampleIterator,
    class Counter,
    class SampleToBinOp
>
ROCPRIM_DEVICE inline
void histogram_global(SampleIterator samples,
                      unsigned int columns,
                      unsigned int row_stride,
                      fixed_array<Counter *, ActiveChannels> histogram,
                      fixed_array<SampleToBinOp, ActiveChannels> sample_to_bin_op,
                      fixed_array<unsigned int, ActiveChannels> bins_bits)
{
    using sample_type = typename std::iterator_traits<SampleIterator>::value_type;
    using sample_vector_type = sample_vector<sample_type, Channels>;

    constexpr unsigned int items_per_block = BlockSize * ItemsPerThread;

    const unsigned int flat_id = ::rocprim::detail::block_thread_id<0>();
    const unsigned int block_id0 = ::rocprim::detail::block_id<0>();
    const unsigned int block_id1 = ::rocprim::detail::block_id<1>();
    const unsigned int block_offset = block_id0 * items_per_block;

    samples += block_id1 * row_stride + Channels * block_offset;

    sample_vector_type values[ItemsPerThread];
    unsigned int valid_count;
    if(block_offset + items_per_block <= columns)
    {
        valid_count = items_per_block;
        load_samples(flat_id, samples, values);
    }
    else
    {
        valid_count = columns - block_offset;
        load_samples(flat_id, samples, values, valid_count);
    }

    for(unsigned int i = 0; i < ItemsPerThread; i++)
    {
        for(unsigned int channel = 0; channel < ActiveChannels; channel++)
        {
            const int bin = sample_to_bin_op[channel](values[i].values[channel]);
            if(bin != -1)
            {
                const unsigned int pos = flat_id * ItemsPerThread + i;
                unsigned long long same_bin_lanes_mask = ::rocprim::ballot(pos < valid_count);
                for(unsigned int b = 0; b < bins_bits[channel]; b++)
                {
                    const unsigned int bit_set = bin & (1u << b);
                    const unsigned long long bit_set_mask = ::rocprim::ballot(bit_set);
                    same_bin_lanes_mask &= (bit_set ? bit_set_mask : ~bit_set_mask);
                }
                const unsigned int same_bin_count = ::rocprim::bit_count(same_bin_lanes_mask);
                const unsigned int prev_same_bin_count = ::rocprim::masked_bit_count(same_bin_lanes_mask);
                if(prev_same_bin_count == 0)
                {
                    // Write the number of lanes having this bin,
                    // if the current lane is the first (and maybe only) lane with this bin.
                    ::rocprim::detail::atomic_add(&histogram[channel][bin], same_bin_count);
                }
            }
        }
    }
}

} // end of detail namespace

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_DEVICE_DETAIL_DEVICE_HISTOGRAM_HPP_