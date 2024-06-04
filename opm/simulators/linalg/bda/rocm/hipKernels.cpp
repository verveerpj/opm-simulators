/*
  Copyright 2024 Equinor ASA

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>
#include <cmath>
#include <sstream>

#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <dune/common/timer.hh>

#include <opm/simulators/linalg/bda/rocm/hipKernels.hpp>

#include <opm/simulators/linalg/bda/Misc.hpp>

#include <hip/hip_runtime.h>
#include <hip/hip_version.h>

#define HIP_CHECK(STAT)                                  \
    do {                                                 \
        const hipError_t stat = (STAT);                  \
        if(stat != hipSuccess)                           \
        {                                                \
            std::ostringstream oss;                      \
            oss << "rocsparseSolverBackend::hip ";       \
            oss << "error: " << hipGetErrorString(stat); \
            OPM_THROW(std::logic_error, oss.str());      \
        }                                                \
    } while(0)

namespace Opm
{

using Opm::OpmLog;
using Dune::Timer;

// define static variables and kernels
int HipKernels::verbosity;
double* HipKernels::tmp;
bool HipKernels::initialized = false;
std::size_t HipKernels::preferred_workgroup_size_multiple = 0;

#ifdef __HIP__
/// HIP kernel to multiply vector with another vector and a scalar, element-wise
// add result to a third vector
__global__ void vmul_k(
    const double alpha,
    double const *in1,
    double const *in2,
    double *out,
    const int N)
{
    unsigned int NUM_THREADS = gridDim.x;
    int idx = blockDim.x * blockIdx.x + threadIdx.x;

    while(idx < N){
        out[idx] += alpha * in1[idx] * in2[idx];
        idx += NUM_THREADS;
    }
}

/// HIP kernel to transform blocked vector to scalar vector using pressure-weights
// every workitem handles one blockrow
__global__ void full_to_pressure_restriction_k(
    const double *fine_y,
    const double *weights,
    double *coarse_y,
    const unsigned int Nb)
{
    const unsigned int NUM_THREADS = gridDim.x;
    const unsigned int block_size = 3;
    unsigned int target_block_row = blockDim.x * blockIdx.x + threadIdx.x;

    while(target_block_row < Nb){
        double sum = 0.0;
        unsigned int idx = block_size * target_block_row;
        for (unsigned int i = 0; i < block_size; ++i) {
            sum += fine_y[idx + i] * weights[idx + i];
        }
        coarse_y[target_block_row] = sum;
        target_block_row += NUM_THREADS;
    }
}

/// HIP kernel to add the coarse pressure solution back to the finer, complete solution
// every workitem handles one blockrow
__global__ void add_coarse_pressure_correction_k(
    const double *coarse_x,
    double *fine_x,
    const unsigned int pressure_idx,
    const unsigned int Nb)
{
    const unsigned int NUM_THREADS = gridDim.x;
    const unsigned int block_size = 3;
    unsigned int target_block_row = blockDim.x * blockIdx.x + threadIdx.x;

    while(target_block_row < Nb){
        fine_x[target_block_row * block_size + pressure_idx] += coarse_x[target_block_row];
        target_block_row += NUM_THREADS;
    }
}

/// HIP kernel to prolongate vector during amg cycle
// every workitem handles one row
__global__ void prolongate_vector_k(
     const double *in,
     double *out,
     const int *cols,
     const unsigned int N)
{
    const unsigned int NUM_THREADS = gridDim.x;
    unsigned int row = blockDim.x * blockIdx.x + threadIdx.x;

    while(row < N){
        out[row] += in[cols[row]];
        row += NUM_THREADS;
    }
}

// res = rhs - mat * x
// algorithm based on:
// Optimization of Block Sparse Matrix-Vector Multiplication on Shared-MemoryParallel Architectures,
// Ryan Eberhardt, Mark Hoemmen, 2016, https://doi.org/10.1109/IPDPSW.2016.42
__global__ void residual_blocked_k(
    const double *vals,
    const int *cols,
    const int *rows,
    const int Nb,
    const double *x,
    const double *rhs,
    double *out,
    const unsigned int block_size
    )
{
    extern __shared__ double tmp[];
    const unsigned int warpsize = warpSize;
    const unsigned int bsize = blockDim.x;
    const unsigned int gid = blockDim.x * blockIdx.x + threadIdx.x;
    const unsigned int idx_b = gid / bsize;
    const unsigned int idx_t = threadIdx.x;
    unsigned int idx = idx_b * bsize + idx_t;
    const unsigned int bs = block_size;
    const unsigned int num_active_threads = (warpsize/bs/bs)*bs*bs;
    const unsigned int num_blocks_per_warp = warpsize/bs/bs;
    const unsigned int NUM_THREADS = gridDim.x;
    const unsigned int num_warps_in_grid = NUM_THREADS / warpsize;
    unsigned int target_block_row = idx / warpsize;
    const unsigned int lane = idx_t % warpsize;
    const unsigned int c = (lane / bs) % bs;
    const unsigned int r = lane % bs;
    
    // for 3x3 blocks:
    // num_active_threads: 27 (CUDA) vs 63 (ROCM)
    // num_blocks_per_warp: 3 (CUDA) vs  7 (ROCM)
    int offsetTarget = warpsize == 64 ? 48 : 32; 
    
    while(target_block_row < Nb){
        unsigned int first_block = rows[target_block_row];
        unsigned int last_block = rows[target_block_row+1];
        unsigned int block = first_block + lane / (bs*bs);
        double local_out = 0.0;

        if(lane < num_active_threads){
            for(; block < last_block; block += num_blocks_per_warp){
                double x_elem = x[cols[block]*bs + c];
                double A_elem = vals[block*bs*bs + c + r*bs];
                local_out += x_elem * A_elem;
            }
        }

        // do reduction in shared mem
        tmp[lane] = local_out; 
        
        for(unsigned int offset = 3; offset <= offsetTarget; offset <<= 1)
        {
            if (lane + offset < warpsize)
            {
                tmp[lane] += tmp[lane + offset];
            }
            __syncthreads();
        }
        
        if(lane < bs){
            unsigned int row = target_block_row*bs + lane;
            out[row] = rhs[row] - tmp[lane];
        }
        target_block_row += num_warps_in_grid;
    }
}

// KERNEL residual_k    
// res = rhs - mat * x
// algorithm based on:
// Optimization of Block Sparse Matrix-Vector Multiplication on Shared-MemoryParallel Architectures,
// Ryan Eberhardt, Mark Hoemmen, 2016, https://doi.org/10.1109/IPDPSW.2016.42
// template <unsigned shared_mem_size>
__global__ void residual_k(
    const double *vals,
    const int *cols,
    const int *rows,
    const int N,
    const double *x,
    const double *rhs,
    double *out
    )
{
    extern __shared__ double tmp[];
    const unsigned int bsize = blockDim.x;
    const unsigned int gid = blockDim.x * blockIdx.x + threadIdx.x;
    const unsigned int idx_b = gid / bsize;
    const unsigned int idx_t = threadIdx.x;
    const unsigned int num_workgroups = gridDim.x;

    int row = idx_b;

    while (row < N) {
        int rowStart = rows[row];
        int rowEnd = rows[row+1];
        int rowLength = rowEnd - rowStart;
        double local_sum = 0.0;
        for (int j = rowStart + idx_t; j < rowEnd; j += bsize) {
            int col = cols[j];
            local_sum += vals[j] * x[col];
        }

        tmp[idx_t] = local_sum;
        __syncthreads();

        int offset = bsize / 2;
        while(offset > 0) {
            if (idx_t < offset) {
                tmp[idx_t] += tmp[idx_t + offset];
            }
            __syncthreads();
            offset = offset / 2;
        }

        if (idx_t == 0) {
            out[row] = rhs[row] - tmp[idx_t];
        }

        row += num_workgroups;
    }
}

__global__ void spmv_k(
    const double *vals,
    const int *cols,
    const int *rows,
    const int N,
    const double *x,
    double *out
    )
{
    extern __shared__ double tmp[];
    const unsigned int bsize = blockDim.x;
    const unsigned int gid = blockDim.x * blockIdx.x + threadIdx.x;
    const unsigned int idx_b = gid / bsize; 
    const unsigned int idx_t = threadIdx.x;
    const unsigned int num_workgroups = gridDim.x;

    int row = idx_b;

    while (row < N) {
        int rowStart = rows[row];
        int rowEnd = rows[row+1];
        int rowLength = rowEnd - rowStart;
        double local_sum = 0.0;
        for (int j = rowStart + idx_t; j < rowEnd; j += bsize) {
            int col = cols[j];
            local_sum += vals[j] * x[col];
        }

        tmp[idx_t] = local_sum;
        __syncthreads();

        int offset = bsize / 2;
        while(offset > 0) {
            if (idx_t < offset) {
                tmp[idx_t] += tmp[idx_t + offset];
            }
            __syncthreads();
            offset = offset / 2;
        }

        if (idx_t == 0) {
            out[row] = tmp[idx_t];
        }

        row += num_workgroups;
    }
}
#endif

void HipKernels::init(int verbosity_)
{
    if (initialized) {
        OpmLog::debug("Warning HipKernels is already initialized");
        return;
    }

    verbosity = verbosity_;

    initialized = true;
}

void HipKernels::vmul(const double alpha, double* in1, double* in2, double* out, int N, hipStream_t stream)
{
    Timer t_vmul;
#ifdef __HIP__
    unsigned blockDim = 64;
    unsigned blocks = Accelerator::ceilDivision(N, blockDim);
    unsigned gridDim = blocks * blockDim;

    // dim3(N) will create a vector {N, 1, 1}
    vmul_k<<<dim3(gridDim), dim3(blockDim), 0, stream>>>(alpha, in1, in2, out, N);
    HIP_CHECK(hipStreamSynchronize(stream));
#else
    OPM_THROW(std::logic_error, "Error vmul for rocsparse only supported when compiling with hipcc");
#endif

    if (verbosity >= 4) {
        std::ostringstream oss;
        oss << std::scientific << "HipKernels vmul() time: " << t_vmul.stop() << " s";
        OpmLog::info(oss.str());
    }
}

void HipKernels::full_to_pressure_restriction(const double* fine_y, double* weights, double* coarse_y, int Nb, hipStream_t stream)
{
    Timer t;
#ifdef __HIP__
    unsigned blockDim = 64;
    unsigned blocks = Accelerator::ceilDivision(Nb, blockDim);
    unsigned gridDim = blocks * blockDim;

    // dim3(N) will create a vector {N, 1, 1}
    full_to_pressure_restriction_k<<<dim3(gridDim), dim3(blockDim), 0, stream>>>(fine_y, weights, coarse_y, Nb);
    HIP_CHECK(hipStreamSynchronize(stream));
#else
    OPM_THROW(std::logic_error, "Error full_to_pressure_restriction for rocsparse only supported when compiling with hipcc");
#endif
    
    if (verbosity >= 4) {
        std::ostringstream oss;
        oss << std::scientific << "HipKernels full_to_pressure_restriction() time: " << t.stop() << " s";
        OpmLog::info(oss.str());
    }
}

void HipKernels::add_coarse_pressure_correction(double* coarse_x, double* fine_x, int pressure_idx, int Nb, hipStream_t stream)
{
    Timer t;
#ifdef __HIP__
    unsigned blockDim = 64;
    unsigned blocks = Accelerator::ceilDivision(Nb, blockDim);
    unsigned gridDim = blocks * blockDim;

    // dim3(N) will create a vector {N, 1, 1}
    add_coarse_pressure_correction_k<<<dim3(gridDim), dim3(blockDim), 0, stream>>>(coarse_x, fine_x, pressure_idx, Nb);
    HIP_CHECK(hipStreamSynchronize(stream));
#else
    OPM_THROW(std::logic_error, "Error add_coarse_pressure_correction for rocsparse only supported when compiling with hipcc");
#endif

    if (verbosity >= 4) {
        std::ostringstream oss;
        oss << std::scientific << "HipKernels add_coarse_pressure_correction() time: " << t.stop() << " s";
        OpmLog::info(oss.str());
    }
}

void HipKernels::prolongate_vector(const double* in, double* out, const int* cols, int N, hipStream_t stream)
{
    Timer t;
    
#ifdef __HIP__
    unsigned blockDim = 64;
    unsigned blocks = Accelerator::ceilDivision(N, blockDim);
    unsigned gridDim = blocks * blockDim;
    unsigned shared_mem_size = blockDim * sizeof(double); 

    // dim3(N) will create a vector {N, 1, 1}
    prolongate_vector_k<<<dim3(gridDim), dim3(blockDim), shared_mem_size, stream>>>(in, out, cols, N);
    HIP_CHECK(hipStreamSynchronize(stream));
#else
    OPM_THROW(std::logic_error, "Error prolongate_vector for rocsparse only supported when compiling with hipcc");
#endif

    if (verbosity >= 4) {
        std::ostringstream oss;
        oss << std::scientific << "HipKernels prolongate_vector() time: " << t.stop() << " s";
        OpmLog::info(oss.str());
    }
}

void HipKernels::residual(double* vals, int* cols, int* rows, double* x, const double* rhs, double* out, int Nb, unsigned int block_size, hipStream_t stream)
{
    Timer t_residual;

#ifdef __HIP__
    unsigned blockDim = 64;
    const unsigned int num_work_groups = Accelerator::ceilDivision(Nb, blockDim);
    unsigned gridDim = num_work_groups * blockDim;
    unsigned shared_mem_size = blockDim * sizeof(double); 

    if (block_size > 1) {
        // dim3(N) will create a vector {N, 1, 1}
        residual_blocked_k<<<dim3(gridDim), dim3(blockDim), shared_mem_size, stream>>>(vals, cols, rows, Nb, x, rhs, out, block_size);
    } else {
        residual_k<<<dim3(gridDim), dim3(blockDim), shared_mem_size, stream>>>(vals, cols, rows, Nb, x, rhs, out);
    }
    HIP_CHECK(hipStreamSynchronize(stream));
#else
    OPM_THROW(std::logic_error, "Error residual for rocsparse only supported when compiling with hipcc");
#endif
    
    if (verbosity >= 4) {
        HIP_CHECK(hipStreamSynchronize(stream));
        std::ostringstream oss;
        oss << std::scientific << "HipKernels residual() time: " << t_residual.stop() << " s";
        OpmLog::info(oss.str());
    }
}

void HipKernels::spmv(double* vals, int* cols, int* rows, double* x, double* y, int Nb, unsigned int block_size, hipStream_t stream)
{//NOTE: block_size not used since I use this kernel only for block sizes 1, other uses use rocsparse!
    Timer t_spmv;
#ifdef __HIP__
    unsigned blockDim = 64;
    const unsigned int num_work_groups = Accelerator::ceilDivision(Nb, blockDim);
    unsigned gridDim = num_work_groups * blockDim;
    unsigned shared_mem_size = blockDim * sizeof(double); 

   spmv_k<<<dim3(gridDim), dim3(blockDim), shared_mem_size, stream>>>(vals, cols, rows, Nb, x, y);
    
    HIP_CHECK(hipStreamSynchronize(stream));
#else
    OPM_THROW(std::logic_error, "Error spmv for rocsparse only supported when compiling with hipcc");
#endif
    
    if (verbosity >= 4) {
        std::ostringstream oss;
        oss << std::scientific << "HipKernels spmv_blocked() time: " << t_spmv.stop() << " s";
        OpmLog::info(oss.str());
    }
}


} // namespace Opm
