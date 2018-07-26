#ifndef GROUPBY_COMPUTE_API_H
#define GROUPBY_COMPUTE_API_H

#include <cuda_runtime.h>
#include <limits>
#include <memory>
#include "groupby_kernels.cuh"
#include <cub/util_allocator.cuh>
#include <cub/device/device_radix_sort.cuh>

// The occupancy of the hash table determines it's capacity. A value of 50 implies
// 50% occupancy, i.e., hash_table_size == 2 * input_size
constexpr unsigned int DEFAULT_HASH_TABLE_OCCUPANCY{50};

constexpr unsigned int THREAD_BLOCK_SIZE{256};

/* --------------------------------------------------------------------------*/
/** 
* @Synopsis Performs the groupby operation for a *SINGLE* 'groupby' column and
* and a single aggregation column.
* 
* @Param[in] in_groupby_column The column to groupby. These act as keys into the hash table
* @Param[in] in_aggregation_column The column to perform the aggregation on. These act as the hash table values
* @Param[in] in_column_size The size of the groupby and aggregation columns
* @Param[out] out_groupby_column Preallocated output buffer that will hold every unique value from the input
*                                groupby column
* @Param[out] out_aggregation_column Preallocated output buffer for the resultant aggregation column that 
*                                     corresponds to the out_groupby_column where entry 'i' is the aggregation 
*                                     for the group out_groupby_column[i] 
* @Param out_size The size of the output
* @Param aggregation_op The aggregation operation to perform 
* 
* @Returns   
*/
/* ----------------------------------------------------------------------------*/
template<typename groupby_type,
         typename aggregation_type,
         typename size_type,
         typename aggregation_operation>
cudaError_t GroupbyHash(const groupby_type * const in_groupby_column,
                        const aggregation_type * const in_aggregation_column,
                        const size_type in_column_size,
                        groupby_type * out_groupby_column,
                        aggregation_type * out_aggregation_column,
                        size_type * out_size,
                        aggregation_operation aggregation_op,
                        bool sort_result = false)
{

  using map_type = concurrent_unordered_map<groupby_type, 
                                            aggregation_type, 
                                            std::numeric_limits<groupby_type>::max(), 
                                            default_hash<groupby_type>, 
                                            equal_to<aggregation_type>, 
                                            legacy_allocator<thrust::pair<groupby_type, aggregation_type> > >;

  cudaError_t error{cudaSuccess};

  // Inputs cannot be null
  if(in_groupby_column == nullptr || in_aggregation_column == nullptr)
    return cudaErrorNotPermitted;

  // Input size cannot be 0 or negative
  if(in_column_size <= 0)
    return cudaErrorNotPermitted;

  // Output buffers must already be allocated
  if(out_groupby_column == nullptr || out_aggregation_column == nullptr)
    return cudaErrorNotPermitted;

  std::unique_ptr<map_type> the_map;

  // The hash table occupancy and the input size determines the size of the hash table
  // e.g., for a 50% occupancy, the size of the hash table is twice that of the input
  const size_type hash_table_size = (in_column_size / DEFAULT_HASH_TABLE_OCCUPANCY) * 100;

  const dim3 build_grid_size ((in_column_size + THREAD_BLOCK_SIZE - 1) / THREAD_BLOCK_SIZE, 1, 1);
  const dim3 block_size (THREAD_BLOCK_SIZE, 1, 1);

  // Initialize the hash table with the aggregation operation functor's identity value
  the_map.reset(new map_type(hash_table_size, aggregation_operation::IDENTITY));

  //the_map->prefetch(0);

  error = cudaGetLastError();
  if(error != cudaSuccess)
    return error;

  // Inserts (groupby_column[i], aggregation_column[i]) as a key-value pair into the
  // hash table. When a given key already exists in the table, the aggregation operation
  // is computed between the new and existing value, and the result is stored back.
  build_aggregation_table<<<build_grid_size, block_size>>>(the_map.get(), 
                                                           in_groupby_column, 
                                                           in_aggregation_column,
                                                           in_column_size,
                                                           aggregation_op);
  error = cudaGetLastError();
  if(error != cudaSuccess)
    return error;

  // Used by threads to coordinate where to write their results
  unsigned int * global_write_index{nullptr};
  cudaMallocManaged(&global_write_index, sizeof(unsigned int));
  cudaMemset(global_write_index, 0, sizeof(unsigned int));
  //*global_write_index = 0;

  error = cudaGetLastError();
  if(error != cudaSuccess)
    return error;

  const dim3 extract_grid_size ((the_map->size() + THREAD_BLOCK_SIZE - 1) / THREAD_BLOCK_SIZE, 1, 1);

  // Extracts every non-empty key and value into separate contiguous arrays,
  // which provides the result of the groupby operation
  extract_groupby_result<<<extract_grid_size, block_size>>>(the_map.get(),
                                                            the_map->size(),
                                                            out_groupby_column,
                                                            out_aggregation_column,
                                                            global_write_index);
  error = cudaDeviceSynchronize();
  if(error != cudaSuccess)
    return error;

  // At the end of the extraction kernel, the global write index will be equal to
  // the size of the output. Update the output size.
  *out_size = *global_write_index;
  cudaFree(global_write_index);

  // Optionally sort the groupby/aggregation result columns
  if(true == sort_result)
  {
    // Allocate double buffers needed for the cub Radix Sort
    groupby_type * groupby_result_alt;
    cudaMalloc(&groupby_result_alt, *out_size * sizeof(groupby_type));

    aggregation_type * aggregation_result_alt;
    cudaMalloc(&aggregation_result_alt, *out_size * sizeof(aggregation_type));

    cub::DoubleBuffer<groupby_type>     d_keys(out_groupby_column, groupby_result_alt);
    cub::DoubleBuffer<aggregation_type> d_vals(out_aggregation_column, aggregation_result_alt);

    // When called with temp_storage == nullptr, simply returns the required allocation size in
    // temp_storage_bytes
    void *d_temp_storage = nullptr;
    size_t temp_storage_bytes = 0;
    cub::DeviceRadixSort::SortPairs(d_temp_storage, temp_storage_bytes, d_keys, d_vals, *out_size);
    
    // allocate temp storage here and call sort again to actually sort arrays
    cudaMalloc(&d_temp_storage, temp_storage_bytes);
    cub::DeviceRadixSort::SortPairs(d_temp_storage, temp_storage_bytes, d_keys, d_vals, *out_size);

    error = cudaDeviceSynchronize();
    if(error != cudaSuccess)
      return error;

    // Update output pointers with sorted result
    // TODO Find a better way to do this. 
    // Sorted output may be in a different buffer than what was originally passed in... need to copy it 
    cudaMemcpy(out_groupby_column, d_keys.Current(), *out_size * sizeof(groupby_type), cudaMemcpyDefault);
    cudaMemcpy(out_aggregation_column, d_vals.Current(), *out_size * sizeof(aggregation_type), cudaMemcpyDefault);

    // Free work buffers
    cudaFree(d_temp_storage);
    
    // FIXME These currently cause segfaults
    cudaFree(groupby_result_alt);
    cudaFree(aggregation_result_alt);
  }

  return error;
}
#endif







