#ifndef TENSOR_HPP
#define TENSOR_HPP

#include <complex>
#include <unsupported/Eigen/CXX11/Tensor>

namespace Eigen
{
// define Tensor3dXf, Tensor3dXcf for spectrograms etc.
typedef Tensor<float, 3, RowMajor> Tensor3dXf;
typedef Tensor<std::complex<float>, 3, RowMajor> Tensor3dXcf;
} // namespace Eigen

#endif // TENSOR_HPP
