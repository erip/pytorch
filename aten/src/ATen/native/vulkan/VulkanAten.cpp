
#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/NativeFunctions.h>
#include <ATen/OpaqueTensorImpl.h>
#include <ATen/native/UpSample.h>
#include <ATen/native/utils/ParamUtils.h>
#include <ATen/native/vulkan/VulkanCommon.h>
#include <ATen/native/vulkan/VulkanDebugUtils.h>

namespace at {
namespace native {

template <typename T>
struct CAFFE2_API IntrusivePtrTargetWrapper : c10::intrusive_ptr_target {
 private:
  T target_;

 public:
  IntrusivePtrTargetWrapper() = delete;
  IntrusivePtrTargetWrapper(const T& target) : target_(target) {}
  IntrusivePtrTargetWrapper(T&& target) : target_(std::move(target)) {}

  T& get_target() {
    return target_;
  }
};

using VTensor = at::native::vulkan::VulkanTensor;

using VTensorWrapper = IntrusivePtrTargetWrapper<VTensor>;
using VTensorWrapperPtr = c10::intrusive_ptr<VTensorWrapper>;
using VulkanTensorImpl = OpaqueTensorImpl<VTensorWrapperPtr>;
using VulkanTensor = Tensor;

std::ostream& operator<<(std::ostream& s, const VulkanTensor& phvTensor) {
  s << "VulkanTensor{...}";
  return s;
}

Tensor new_with_vtensor_vulkan(VTensor&& vt, const TensorOptions& options) {
  COUT_FLF;

  auto dims = vt.sizes();
  VTensorWrapperPtr handle = c10::make_intrusive<VTensorWrapper>(std::move(vt));
  return detail::make_tensor<VulkanTensorImpl>(
      DispatchKeySet(DispatchKey::VulkanTensorId),
      options.dtype(),
      at::Device(at::kVULKAN),
      handle,
      std::vector<int64_t>(dims.begin(), dims.end()));
}

VTensor& vtensor_from_vulkan(const VulkanTensor& vulkan_tensor) {
  COUT_FLF;

  AT_ASSERTM(
      vulkan_tensor.is_vulkan(), "vulkan_to_dense expects Vulkan tensor input");
  TORCH_INTERNAL_ASSERT(at::impl::variable_excluded_from_dispatch());
  VulkanTensorImpl* phvImpl =
      static_cast<VulkanTensorImpl*>(vulkan_tensor.unsafeGetTensorImpl());
  return phvImpl->unsafe_opaque_handle()->get_target();
}

VTensor vtensor_view_from_dense(const Tensor& tensor) {
  COUT_FLF;

  AT_ASSERTM(
      tensor.device().type() == DeviceType::CPU,
      "vtensor_view_from_dense expects CPU tensor input");
  AT_ASSERTM(
      tensor.layout() == Layout::Strided,
      "vtensor_view_from_dense expects dense tensor input");
  AT_ASSERTM(
      tensor.scalar_type() == ScalarType::Float,
      "vtensor_view_from_dense expects float tensor input");
  TORCH_INTERNAL_ASSERT(at::impl::variable_excluded_from_dispatch());
  assert(false); // Not implemented
  return VTensor{tensor.sizes().vec()};
}

Tensor empty_vulkan(
    IntArrayRef sizes,
    const TensorOptions& options,
    c10::optional<c10::MemoryFormat> optional_memory_format) {
  COUT_FLF;

  TORCH_CHECK(
      !options.has_memory_format(),
      "'memory_format' argument is incompatible with vulkan tensor");
  TORCH_CHECK(
      !optional_memory_format.has_value(),
      "'memory_format' argument is incompatible with vulkan tensor");

  auto sizesVec = sizes.vec();
  VTensor vt{sizesVec};
  return new_with_vtensor_vulkan(std::move(vt), options);
}

Tensor vulkan_to_dense(const Tensor& vulkan_tensor) {
  COUT_FLF;

  VTensor& vtensor = vtensor_from_vulkan(vulkan_tensor);
  auto dims = vtensor.sizes();
  COUT_FLF;
  Tensor cpu_tensor = at::empty(
      std::vector<int64_t>(dims.begin(), dims.end()),
      vulkan_tensor.options()
          .device(at::Device(at::kCPU))
          .layout(c10::kStrided));
  float* tensorOutputData = cpu_tensor.template data_ptr<float>();
  vtensor.copyDataToHost(tensorOutputData);
  return cpu_tensor;
}

Tensor dense_to_vulkan(const Tensor& cpu_tensor) {
  COUT_FLF;

  AT_ASSERTM(
      cpu_tensor.device().type() == DeviceType::CPU,
      "dense_to_vulkan expects CPU tensor input");
  AT_ASSERTM(
      cpu_tensor.layout() == Layout::Strided,
      "dense_to_vulkan_expects strided tensor input");
  AT_ASSERTM(
      cpu_tensor.scalar_type() == ScalarType::Float,
      "dense_to_vulkan expects float tensor input");
  AT_ASSERTM(cpu_tensor.dim() == 4, "dense_to_vulkan expects tensor dim == 4");
  auto cpu_tensor_cont = cpu_tensor.contiguous();
  auto sizes = cpu_tensor_cont.sizes();
  // IKTODO: Channels first assumption only, support channels last
  float* dataNCHW = cpu_tensor_cont.template data_ptr<float>();

  at::native::vulkan::debug::vk_print_tensor_data(
      "dense_to_vulkan", dataNCHW, sizes);

  Tensor vulkan_tensor =
      empty_vulkan(cpu_tensor_cont.sizes(), cpu_tensor_cont.options(), {});

  VTensor& vtensor = vtensor_from_vulkan(vulkan_tensor);
  vtensor.setDataFromHost(dataNCHW);

  return vulkan_tensor;
}

Tensor upsample_nearest2d_vulkan(
    const Tensor& input,
    IntArrayRef outputSizes,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  COUT_FLF;
  std::cout << SFLF << " outputSizes:" << outputSizes;
  VTensor& x = vtensor_from_vulkan(input);
  auto inputSizes = input.sizes();
  COUT_FLF;

  auto in = inputSizes[0];
  auto ic = inputSizes[1];
  auto ih = inputSizes[2];
  auto iw = inputSizes[3];

  auto oh = outputSizes[0];
  auto ow = outputSizes[1];

  COUT_FLF;
  const float height_scale = compute_scales_value<float>(scales_h, ih, oh);
  const float width_scale = compute_scales_value<float>(scales_w, iw, ow);
  std::cout << " height_scale:" << height_scale
            << " width_scale:" << width_scale << std::endl;

  COUT_FLF;
  Tensor output = empty_vulkan({in, ic, oh, ow}, input.options(), {});
  std::cout << " output tensor sizes: " << output.sizes() << std::endl;

  COUT_FLF;
  VTensor& y = vtensor_from_vulkan(output);
  y.allocateStorage();

  at::native::vulkan::gl::upsample_nearest2d(
      y, x, ih, iw, oh, ow, in, ic, height_scale, width_scale);
  return output;
}

Tensor vulkan_add(const Tensor& self, const Tensor& other, Scalar alpha) {
  COUT_FLF;

  VTensor& x = vtensor_from_vulkan(self);
  VTensor& y = vtensor_from_vulkan(other);
  float a = alpha.to<float>();

  VTensor output = VTensor{self.sizes().vec()};
  output.allocateStorage();

  at::native::vulkan::gl::add(output, x, y, a);

  return new_with_vtensor_vulkan(std::move(output), self.options());
}

at::Tensor vulkan_convolution(
    const at::Tensor& input, // VULKAN
    const at::Tensor& weight, // CPU
    const at::Tensor& bias, // CPU
    IntArrayRef padding,
    IntArrayRef stride,
    IntArrayRef dilation,
    int64_t groups) {
  COUT_FLF;
  auto isizes = input.sizes();
  assert(isizes.size() == 4);
  auto wsizes = weight.sizes();
  assert(wsizes.size() == 4);

  int64_t N = isizes[0];
  int64_t C = isizes[1];
  int64_t H = isizes[2];
  int64_t W = isizes[3];

  int64_t OC = wsizes[0];
  assert(wsizes[1] == C);
  int64_t KH = wsizes[2];
  int64_t KW = wsizes[3];

  int64_t PY = padding[0];
  int64_t PX = padding[1];

  int64_t SY = stride[0];
  int64_t SX = stride[1];

  int64_t DY = dilation[0];
  int64_t DX = dilation[1];

  const int KWE = (KW - 1) * DX + 1;
  const int KHE = (KH - 1) * DY + 1;
  const int64_t OW = ((W - KWE + 2 * PX) / SX) + 1;
  const int64_t OH = ((H - KHE + 2 * PY) / SY) + 1;

  auto osizes = std::vector<int64_t>{N, OC, OH, OW};

  const VTensor& vinput = vtensor_from_vulkan(input);
  VTensor voutput = VTensor{osizes};
  voutput.allocateStorage();

  float* biasData{};
  COUT_FLF;
  if (bias.defined()) {
    std::cout << "bias defined" << std::endl;
    biasData = bias.template data_ptr<float>();
  } else {
    std::cout << "bias NOT defined" << std::endl;
    biasData = (float*)std::malloc(sizeof(float) * OC);
    std::memset(biasData, 0, sizeof(float) * OC);
  }

  float* weightData = weight.template data_ptr<float>();

  at::native::vulkan::gl::conv2d(
      voutput,
      vinput,
      weightData,
      KH,
      KW,
      biasData,
      SY,
      SX,
      PY,
      PX,
      DY,
      DX,
      groups);

  return new_with_vtensor_vulkan(std::move(voutput), input.options());
}

} // namespace native
} // namespace at
