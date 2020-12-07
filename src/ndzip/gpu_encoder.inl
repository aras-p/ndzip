#pragma once

#include "common.hh"

#include <stdexcept>
#include <vector>

#include <SYCL/sycl.hpp>


#define NDZIP_WARP_SIZE (size_t{32})


namespace ndzip::detail::gpu {

template<typename Profile>
using global_data_read_accessor
        = sycl::accessor<typename Profile::data_type, 1, sycl::access::mode::read>;

template<typename T>
using global_write_accessor = sycl::accessor<T, 1, sycl::access::mode::discard_write>;

template<typename Profile>
using global_bits_write_accessor
        = sycl::accessor<typename Profile::bits_type, 1, sycl::access::mode::discard_read_write>;

template<typename T>
using local_accessor
        = sycl::accessor<T, 1, sycl::access::mode::read_write, sycl::access::target::local>;

template<typename Profile>
using local_bits_accessor = local_accessor<typename Profile::bits_type>;

template<unsigned Dims, typename U, typename T>
U extent_cast(const T &e) {
    U v;
    for (unsigned i = 0; i < Dims; ++i) {
        v[i] = e[i];
    }
    return v;
}

template<typename U, unsigned Dims>
U extent_cast(const extent<Dims> &e) {
    return extent_cast<Dims, U>(e);
}

template<typename T, int Dims>
T extent_cast(const sycl::range<Dims> &r) {
    return extent_cast<static_cast<unsigned>(Dims), T>(r);
}

template<typename T, int Dims>
T extent_cast(const sycl::id<Dims> &r) {
    return extent_cast<static_cast<unsigned>(Dims), T>(r);
}

template<typename U, typename T>
[[gnu::always_inline]] U bit_cast(T v) {
    static_assert(std::is_pod_v<T> && std::is_pod_v<U> && sizeof(U) == sizeof(T));
    U cast;
    __builtin_memcpy(&cast, &v, sizeof cast);
    return cast;
}

template<typename Profile>
size_t global_offset(size_t local_offset, extent<Profile::dimensions> global_size) {
    size_t global_offset = 0;
    size_t global_stride = 1;
    for (unsigned d = 0; d < Profile::dimensions; ++d) {
        global_offset += global_stride * (local_offset % Profile::hypercube_side_length);
        local_offset /= Profile::hypercube_side_length;
        global_stride *= global_size[Profile::dimensions - 1 - d];
    }
    return global_offset;
}

template<typename Profile>
void load_hypercube(/* global */ const typename Profile::data_type *__restrict data,
        /* local */ typename Profile::bits_type *__restrict cube,
        extent<Profile::dimensions> data_size, sycl::nd_item<2> item) {
    auto side_length = Profile::hypercube_side_length;
    auto hc_index = item.get_global_id(0);
    auto hc_size = ipow(side_length, Profile::dimensions);
    auto hc_offset = detail::extent_from_linear_id(hc_index, data_size / side_length) * side_length;
    auto n_threads = item.get_local_range(1);
    auto tid = item.get_local_id(1);

    size_t global_idx
            = linear_offset(hc_offset, data_size) + global_offset<Profile>(tid, data_size);
    size_t global_stride = global_offset<Profile>(n_threads, data_size);
    for (size_t local_idx = tid; local_idx < hc_size; local_idx += n_threads) {
        cube[local_idx] = bit_cast<typename Profile::bits_type>(data[global_idx]);
        global_idx += global_stride;
    }
}


template<typename Profile>
void block_transform(/* local */ typename Profile::bits_type *x, sycl::nd_item<2> item) {
    using saf = sycl::access::fence_space;

    constexpr auto n = Profile::hypercube_side_length;
    constexpr auto dims = Profile::dimensions;
    constexpr auto hc_size = ipow(n, dims);

    auto n_threads = item.get_local_range(1);
    auto tid = item.get_local_id(1);

    for (size_t i = tid; i < hc_size; i += n_threads) {
        x[i] = rotate_left_1(x[i]);
    }

    item.barrier(saf::local_space);

    if constexpr (dims == 1) {
        if (tid == 0) { block_transform_step(x, n, 1); }
    } else if constexpr (dims == 2) {
        for (size_t i = tid; i < n; i += n_threads) {
            const auto ii = n * i;
            block_transform_step(x + ii, n, 1);
        }
        item.barrier(saf::local_space);
        for (size_t i = tid; i < n; i += n_threads) {
            block_transform_step(x + i, n, n);
        }
    } else if constexpr (dims == 3) {
        for (size_t i = tid; i < n; i += n_threads) {
            const auto ii = n * n * i;
            for (size_t j = 0; j < n; ++j) {
                block_transform_step(x + ii + j, n, n);
            }
        }
        item.barrier(saf::local_space);
        for (size_t i = tid; i < n * n; i += n_threads) {
            const auto ii = n * i;
            block_transform_step(x + ii, n, 1);
        }
        item.barrier(saf::local_space);
        for (size_t i = tid; i < n * n; i += n_threads) {
            block_transform_step(x + i, n, n * n);
        }
    }

    item.barrier(saf::local_space);

    for (size_t i = tid; i < hc_size; i += n_threads) {
        x[i] = complement_negative(x[i]);
    }
}


template<typename Profile>
void inverse_block_transform(/* local */ typename Profile::bits_type *x, sycl::nd_item<2> item) {
    using saf = sycl::access::fence_space;

    constexpr auto n = Profile::hypercube_side_length;
    constexpr auto dims = Profile::dimensions;
    constexpr auto hc_size = ipow(n, dims);

    auto n_threads = item.get_local_range(1);
    auto tid = item.get_local_id(1);

    for (size_t i = tid; i < hc_size; i += n_threads) {
        x[i] = complement_negative(x[i]);
    }

    item.barrier(saf::local_space);

    if (dims == 1) {
        if (tid == 0) { inverse_block_transform_step(x, n, 1); }
    } else if (dims == 2) {
        for (size_t i = tid; i < n; i += n_threads) {
            inverse_block_transform_step(x + i, n, n);
        }
        item.barrier(saf::local_space);
        for (size_t i = tid; i < n; i += n_threads) {
            auto ii = i * n;
            inverse_block_transform_step(x + ii, n, 1);
        }
    } else if (dims == 3) {
        for (size_t i = tid; i < n * n; i += n_threads) {
            inverse_block_transform_step(x + i, n, n * n);
        }
        item.barrier(saf::local_space);
        for (size_t i = tid; i < n * n; i += n_threads) {
            auto ii = i * n;
            inverse_block_transform_step(x + ii, n, 1);
        }
        item.barrier(saf::local_space);
        for (size_t i = tid; i < n; i += n_threads) {
            auto ii = i * n * n;
            for (size_t j = 0; j < n; ++j) {
                inverse_block_transform_step(x + ii + j, n, n);
            }
        }
    }

    item.barrier(saf::local_space);

    for (size_t i = tid; i < hc_size; i += n_threads) {
        x[i] = rotate_right_1(x[i]);
    }
}


template<typename Bits>
void transpose_bits(/* local */ Bits *cube, sycl::nd_item<2> item) {
    using saf = sycl::access::fence_space;

    constexpr auto n_threads = NDZIP_WARP_SIZE;
    constexpr auto cols_per_thread = bitsof<Bits> / n_threads;
    auto tid = item.get_local_id(1);

    Bits columns[cols_per_thread] = {0};
    for (size_t k = 0; k < bitsof<Bits>; ++k) {
        auto row = cube[k];
        for (size_t c = 0; c < cols_per_thread; ++c) {
            size_t i = c * n_threads + tid;
            columns[c] |= ((row >> (bitsof<Bits> - 1 - i)) & Bits{1}) << (bitsof<Bits> - 1 - k);
        }
    }

    item.barrier(saf::local_space);

    for (size_t c = 0; c < cols_per_thread; ++c) {
        size_t i = c * n_threads + tid;
        cube[i] = columns[c];
    }
}


template<typename Bits>
size_t compact_zero_words(/* global */ Bits *__restrict out, /* local */ const Bits *__restrict in,
        /* local */ Bits *__restrict scratch, sycl::nd_item<2> item) {
    using saf = sycl::access::fence_space;

    constexpr auto n_columns = bitsof<Bits>;
    constexpr auto n_threads = NDZIP_WARP_SIZE;
    auto tid = item.get_local_id(1);

    for (size_t i = tid; i < n_columns; i += n_threads) {
        scratch[i] = i ? in[i - 1] != 0 : 1;
        // TODO this is dumb, we can just OR values before transposition
        scratch[2 * n_columns + i] = Bits{in[i] != 0} << i;
    }

    item.barrier(saf::local_space);

    // Hillis-Steele (short-span) prefix sum
    size_t pout = 0;
    size_t pin;
    for (size_t offset = 1; offset < n_columns; offset <<= 1u) {
        pout = 1 - pout;
        pin = 1 - pout;
        for (size_t i = tid; i < n_columns; i += n_threads) {
            if (i >= offset) {
                scratch[pout * n_columns + i]
                        = scratch[pin * n_columns + i] + scratch[pin * n_columns + i - offset];
            } else {
                scratch[pout * n_columns + i] = scratch[pin * n_columns + i];
            }
        }
        item.barrier(saf::local_space);
    }

    // Header reduction - TODO merge with prefix sum loop, or better yet, move into a all_zero check
    for (size_t offset = n_columns / 2; offset > 0; offset /= 2) {
        for (size_t i = tid; i < n_columns; i += n_threads) {
            if (i < offset) { scratch[2 * n_columns + i] |= scratch[2 * n_columns + i + offset]; }
        }
        item.barrier(saf::local_space);
    }

    if (tid == 0) { out[0] = scratch[2 * n_columns]; }

    for (size_t i = tid; i < n_columns; i += n_threads) {
        if (in[i] != 0) { out[scratch[pout * n_columns + i]] = in[i]; }
    }

    return 0;  // TODO
}


template<typename Profile>
size_t zero_bit_encode(/* global */ typename Profile::bits_type *__restrict stream,
        /* local */ typename Profile::bits_type *__restrict cube,
        /* local */ typename Profile::bits_type *__restrict scratch, sycl::nd_item<2> item) {
    using bits_type = typename Profile::bits_type;
    using saf = sycl::access::fence_space;

    auto side_length = Profile::hypercube_side_length;
    auto hc_size = detail::ipow(side_length, Profile::dimensions);

    for (size_t offset = 0; offset < hc_size; offset += bitsof<bits_type>) {
        transpose_bits(cube + offset, item);
    }

    item.barrier(saf::local_space);

    auto out = stream;
    for (size_t offset = 0; offset < hc_size; offset += bitsof<bits_type>) {
        out += compact_zero_words(out, cube + offset, scratch, item);
    }

    return out - stream;
}

template<typename Profile, typename BlockCompactionAccessor, typename CompressedBlocksAccessor>
void store_compressed_block(BlockCompactionAccessor block_compaction_acc,
        CompressedBlocksAccessor compressed_blocks_acc, sycl::nd_item<2> nd_item) {
}


// SYCL kernel names
template<typename, unsigned>
class block_compression_kernel;

template<typename, unsigned>
class length_sum_kernel;

template<typename, unsigned>
class block_compaction_kernel;

template<typename, unsigned>
class border_compaction_kernel;

}  // namespace ndzip::detail::gpu


template<typename T, unsigned Dims>
struct ndzip::gpu_encoder<T, Dims>::impl {
    sycl::queue q;
};

template<typename T, unsigned Dims>
ndzip::gpu_encoder<T, Dims>::gpu_encoder() : _pimpl(std::make_unique<impl>()) {
}

template<typename T, unsigned Dims>
ndzip::gpu_encoder<T, Dims>::~gpu_encoder() = default;


template<typename T, unsigned Dims>
size_t ndzip::gpu_encoder<T, Dims>::compress(
        const slice<const data_type, dimensions> &data, void *stream) const {
    using profile = detail::profile<T, Dims>;
    using bits_type = typename profile::bits_type;

    constexpr auto side_length = profile::hypercube_side_length;
    constexpr auto hc_size = detail::ipow(side_length, profile::dimensions);
    constexpr auto max_compressed_block_uints
            = hc_size / detail::bitsof<bits_type> * (detail::bitsof<bits_type> + 1);

    detail::file<profile> file(data.size());

    sycl::buffer<data_type, dimensions> data_buffer{
            data.data(), detail::gpu::extent_cast<sycl::range<dimensions>>(data.size())};
    sycl::buffer<bits_type, 2> compressed_blocks_buffer{
            sycl::range<2>{file.num_hypercubes(), max_compressed_block_uints}};
    sycl::buffer<detail::file_offset_type, 1> compressed_block_lengths_buffer{
            sycl::range<1>{file.num_hypercubes()}};

    /*
    _pimpl->q.submit([&](sycl::handler &cgh) {
        // global memory
        auto data_acc = data_buffer.template get_access<sycl::access::mode::read>(cgh);
        auto compressed_blocks_acc
                = compressed_blocks_buffer.template
    get_access<sycl::access::mode::discard_read_write>( cgh); auto compressed_block_lengths_acc =
    compressed_block_lengths_buffer.get_access<sycl::access::mode::discard_write>( cgh);

        // local memory
        auto block_transform_acc = sycl::accessor<bits_type, 1,
                sycl::access::mode::read_write, sycl::access::target::local>{hc_size, cgh};
        auto block_compaction_acc
                = sycl::accessor<bits_type, 1, sycl::access::mode::read_write,
                        sycl::access::target::local>{max_compressed_block_uints, cgh};
        auto data_size = data.size();

        cgh.parallel_for<detail::gpu::block_compression_kernel<T, Dims>>(
                sycl::nd_range<2>{sycl::range<2>{file.num_hypercubes(), NDZIP_WARP_SIZE},
                        sycl::range<2>{1, NDZIP_WARP_SIZE}},
                [=](sycl::nd_item<2> item) {
                    auto hc_index = item.get_global_range(0);

                    detail::gpu::load_hypercube<profile>(
                            data_acc, block_transform_acc, data_size, item);
                    detail::gpu::block_transform<profile>(block_transform_acc, item);
                    compressed_block_lengths_acc[sycl::id<1>{hc_index}]
                            = detail::gpu::zero_bit_encode<profile>(
                                    block_transform_acc, compressed_blocks_acc, item);
                    // detail::gpu::store_compressed_block<profile>(
                    //         block_compaction_acc, compressed_blocks_acc, item);
                });
    });
     */

    // Re-use stream buffer? It must receive the offsets as header anyway
    sycl::buffer<detail::file_offset_type, 1> compressed_block_offsets_buffer{
            sycl::range<1>{file.num_hypercubes() + 1}};

    _pimpl->q.submit([&](sycl::handler &cgh) {
        auto compressed_block_lengths_acc
                = compressed_block_lengths_buffer.get_access<sycl::access::mode::read>(cgh);
        auto compressed_block_offsets_acc
                = compressed_block_offsets_buffer
                          .get_access<sycl::access::mode::discard_read_write>(cgh);

        cgh.parallel_for<detail::gpu::length_sum_kernel<T, Dims>>(
                sycl::range<1>{NDZIP_WARP_SIZE}, [=](sycl::item<1> item) {
                    // parallel prefix sum lengths -> offsets
                });
    });

    sycl::buffer<std::byte, 1> stream_buffer{static_cast<std::byte *>(stream),
            sycl::range<1>{compressed_size_bound<data_type>(data.size())}};

    _pimpl->q.submit([&](sycl::handler &cgh) {
        auto compressed_block_lengths_acc
                = compressed_block_lengths_buffer.get_access<sycl::access::mode::read>(cgh);
        auto compressed_block_offsets_acc
                = compressed_block_offsets_buffer.get_access<sycl::access::mode::read>(cgh);
        auto compressed_blocks_acc
                = compressed_blocks_buffer.template get_access<sycl::access::mode::read>(cgh);
        auto stream_acc = stream_buffer.get_access<sycl::access::mode::discard_write>(cgh);

        cgh.parallel_for<detail::gpu::block_compaction_kernel<T, Dims>>(
                sycl::range<1>{file.num_hypercubes()}, [=](sycl::item<1> item) {
                    // compaction
                });
    });

    _pimpl->q.submit([&](sycl::handler &cgh) {
        auto compressed_block_offsets_acc
                = compressed_block_offsets_buffer.get_access<sycl::access::mode::read>(cgh);
        auto data_acc = stream_buffer.template get_access<sycl::access::mode::write>(cgh);
        auto stream_acc = stream_buffer.get_access<sycl::access::mode::discard_write>(cgh);

        cgh.parallel_for<detail::gpu::border_compaction_kernel<T, Dims>>(
                sycl::range<1>{NDZIP_WARP_SIZE}, [=](sycl::item<1> item) {
                    // sequentially append border slices
                });
    });

    detail::file_offset_type compressed_size;

    _pimpl->q.submit([&](sycl::handler &cgh) {
        auto compressed_block_offsets_acc
                = compressed_block_offsets_buffer.get_access<sycl::access::mode::read>(
                        cgh, sycl::range<1>{1}, sycl::id<1>{file.num_hypercubes()});
        cgh.copy(compressed_block_offsets_acc, &compressed_size);
    });

    _pimpl->q.wait();
    return compressed_size;
}


template<typename T, unsigned Dims>
size_t ndzip::gpu_encoder<T, Dims>::decompress(
        const void *stream, size_t bytes, const slice<data_type, dimensions> &data) const {
    return 0;
}


namespace ndzip {

extern template class gpu_encoder<float, 1>;
extern template class gpu_encoder<float, 2>;
extern template class gpu_encoder<float, 3>;
extern template class gpu_encoder<double, 1>;
extern template class gpu_encoder<double, 2>;
extern template class gpu_encoder<double, 3>;

}  // namespace ndzip
