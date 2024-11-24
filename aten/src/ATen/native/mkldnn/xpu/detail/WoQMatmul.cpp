#include <c10/xpu/XPUFunctions.h>

#include <ATen/native/mkldnn/xpu/detail/Attr.h>
#include <ATen/native/mkldnn/xpu/detail/Utils.h>

#include <oneapi/dnnl/dnnl.hpp>
#include <cstdint>


namespace at::native::onednn {

inline Tensor resize_as_onednn_mat1(const Tensor& mat1, const Tensor& output) {
  auto output_ = output.flatten(0, -2);
  int n = output_.sizes()[1];
  auto sizes = mat1.sym_sizes().vec();
  sizes[sizes.size() - 1] = n;
  return output.view_symint(sizes);
}

sycl::event woq_matmul_int4(
    Tensor& result, //torchao: [M, K]
    const Tensor& mat1_, //torchao: [M, K]
    const Tensor& mat2_, //torchao quantized weight, [K/8, N]
    const Tensor& scale, //torchao: [K/group_size, N]
    const Tensor& zp, //torchao: [K/group_size, N]
    int64_t group_size,
    Attr attr,
    const std::vector<sycl::event>& deps) {
  size_t dims = result.dim();
  TORCH_CHECK(
      dims == 2 || dims == 3,
      "oneDNN matmul only works with 2D or 3D, got ",
      dims);
  TORCH_CHECK(result.defined(), "oneDNN matmul result should be defined");

  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto engine_index = curDevice.index();
  auto stream = GpuStreamManager::Instance().get_stream();

  // make them all contiguous
  Tensor m1 = is_onednn_matmul_strides(mat1_) ? mat1_ : mat1_.contiguous();
  //m2_ may be a 4 dims fake tensor in torchAO with shape {N / 8, K / (16 * innerKTiles), 32, innerKTiles / 2}
  //Tensor m2 = mat2_.flatten(0, -2); //ToDo: change to the fke shape: mat2_.flatten(0, -2); // N1
  Tensor m2 = is_onednn_matmul_strides(mat2_) ? mat2_ : mat2_.contiguous();
  Tensor scale_ = is_onednn_matmul_strides(scale) ? scale : scale.contiguous();
  Tensor zp_ = is_onednn_matmul_strides(zp) ? zp : zp.contiguous();
  Tensor dst = is_onednn_matmul_strides(result, true) ? result : result.contiguous();
  int m = m1.size(-2); // M
  int n = dst.size(-1);  //m2.size(0) * kNTileSize; 
  int k = m1.size(-1); // K1
  int64_t mb = 1;

  // Construct usr md from input
  // xxx_usr_md would describe the real layout of inputs
  auto m1_usr_dt = get_onednn_dtype(m1); // e.g., half <==> f16
  auto m2_usr_dt = get_onednn_dtype(m2); //int32 tensor, pack 8 int4
  auto scale_usr_dt = get_onednn_dtype(scale_); // bf16
  //   auto zp_usr_dt = dnnl::memory::data_type::s4; // int32, representing 8xint4
  auto zp_usr_dt = get_onednn_dtype(zp_); // bf16
  auto dst_usr_dt = get_onednn_dtype(dst); // bf16

  dnnl::memory::dims m1_usr_dims, m2_usr_dims, scale_usr_dims, zp_usr_dims, dst_usr_dims;
  dnnl::memory::dims m1_usr_strides, m2_usr_strides, scale_usr_strides, zp_usr_strides, dst_usr_strides;
  const uint64_t compressed_k = (uint64_t)(k / 8);
  const uint64_t num_groups = (uint64_t)(k / group_size); 
    //wei: {compressed_k, n}:   shape:{n/8, 32, xxx , innerKTiles/2}
  m1_usr_dims = {m, k};
  m1_usr_strides = {m1.stride(0), m1.stride(1)};
  m2_usr_dims = {compressed_k, n};
  m2_usr_strides = {1, compressed_k}; // k dim contiguous, 4bit pack into s32
  scale_usr_dims = {num_groups, n}; 
  scale_usr_strides = {scale_.stride(1), scale_.stride(0)};//why? 
  zp_usr_dims = {1};
  zp_usr_strides = {1};
  dst_usr_dims = {m, n};
  dst_usr_strides = {dst.stride(0), dst.stride(1)};

  dnnl::memory::desc m1_usr_md, m2_usr_md, scale_usr_md, zp_usr_md, dst_usr_md;

  m1_usr_md = dnnl::memory::desc(m1_usr_dims, m1_usr_dt, m1_usr_strides);
  m2_usr_md = dnnl::memory::desc(m2_usr_dims, m2_usr_dt, m2_usr_strides);
  scale_usr_md = dnnl::memory::desc(scale_usr_dims, scale_usr_dt, scale_usr_strides);
  zp_usr_md = dnnl::memory::desc(zp_usr_dims, zp_usr_dt, zp_usr_strides);
  dst_usr_md = dnnl::memory::desc(dst_usr_dims, dst_usr_dt, dst_usr_strides);

  // create usr memory
  auto dst_usr_m = make_onednn_memory(dst_usr_md, engine, dst.data_ptr());
  auto scale_usr_m = make_onednn_memory(scale_usr_md, engine, scale.data_ptr());
  auto zp_usr_m = make_onednn_memory(zp_usr_md, engine, zp.data_ptr());

  // Construct md for primitive creation
  // The xxx_md describes what kinds of matmul the oneDNN does.
  // The problem for this op is [m, k] x [k, n] => [m, n] matmul.  
  auto m1_dt = m1_usr_dt; //bf16
  // Tell oneDNN the weight dtype we want manipulate is u4,
  // library needs infer how to unpack u4 data based on the m2_usr_md (s32).
  auto m2_dt = dnnl::memory::data_type::u4; 
  auto scale_dt = scale_usr_dt; //bf16
  // Tell oneDNN the zp dtype we want manipulate is u4,
  // library needs infer how to unpack u4 data based on the m2_usr_md (s32).
  auto zp_dt = dnnl::memory::data_type::u4;
  auto dst_dt = dst_usr_dt;

  dnnl::memory::desc m1_md, m2_md, scale_md, zp_md, dst_md;
  dnnl::memory::dims m1_dims, m2_dims, scale_dims, zp_dims, dst_dims;
  dnnl::memory::dims m1_strides, m2_strides, scale_strides, zp_strides, dst_strides;

  m1_dims = m1_usr_dims; // {m, k}
  m1_strides = m1_usr_strides; // {k, 1}
  m2_dims = {k, n}; 
  m2_strides = {n, 1};
  scale_dims = scale_usr_dims; // {k//group_size, n}
  scale_strides = scale_usr_strides; 
  zp_dims = {1};
  zp_usr_dims = {1}; //{compressed_k, n};
  zp_strides = {1};
  dst_dims = dst_usr_dims;
  dst_strides = dst_usr_strides;

  m1_md = dnnl::memory::desc(m1_dims, m1_dt, m1_strides);
  m2_md = dnnl::memory::desc(m2_dims, m2_dt, m2_strides);
  scale_md = dnnl::memory::desc(scale_dims, scale_dt, scale_strides);
  zp_md = dnnl::memory::desc(zp_dims, zp_dt, zp_strides);
  dst_md = dnnl::memory::desc(dst_dims, dst_dt, dst_strides);
  
  std::unordered_map<int, dnnl::memory> args;
  dnnl::post_ops po = attr.extract_post_ops(dst);

  dnnl::matmul matmul_p;
  dnnl::matmul::primitive_desc matmul_pd;

  auto m1_usr_m = make_onednn_memory(m1_usr_md, engine, m1.data_ptr());
  auto m2_usr_m = make_onednn_memory(m2_usr_md, engine, m2.data_ptr());
  
  void* handle_b = m2_usr_m.get_data_handle();
  // reinterpret m2_usr_memory as u4
  dnnl::memory m2_u4_m(
      {{k, n}, dnnl::memory::data_type::u4, dnnl::memory::format_tag::ba},
      engine,
      handle_b);

  dnnl::primitive_attr pattr;
  pattr.set_post_ops(po);
  pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

  // Set scales with multiple scales along K dimension and with groups along  K.
  pattr.set_scales(
     DNNL_ARG_WEIGHTS,
     /* mask */ (1 << 0) + (1 << 1),
     {group_size, 1},
     scale_dt);
  // Set a single zero point with s8 data type.
   pattr.set_zero_points(
       DNNL_ARG_WEIGHTS,
       /* mask */ 0, //(1 << 0) + (1 << 1),
       {}, //{group_size, 1},
       dnnl::memory::data_type::s8);
  // Set fpmath mode with `apply_to_int=true` to apply fpmath mode behavior to
  // integral primitives (in this example, matmul).
  pattr.set_fpmath_mode(dnnl::fpmath_mode::bf16, true);

  matmul_pd = dnnl::matmul::primitive_desc(
        engine, m1_md, m2_u4_m.get_desc(), dst_md, pattr);
  matmul_p = dnnl::matmul(matmul_pd);

  dnnl::memory m1_m = m1_usr_m, m2_m = m2_u4_m, dst_m = dst_usr_m;
  dnnl::memory scale_m = scale_usr_m; // zp_m = zp_u4_m;
  Tensor m1_, m2_, zp_new, dst_;


  int scratchpad_size = matmul_pd.scratchpad_desc().get_size();
  Tensor scratchpad_tensor = at::empty(
      {scratchpad_size}, m1.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_memory = make_onednn_memory(
      matmul_pd.scratchpad_desc(), engine, scratchpad_tensor.data_ptr());
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_memory});

  if (attr.with_binary())
    attr.construct_post_binary(matmul_pd, args);

  args.insert({DNNL_ARG_SRC, m1_m});
  args.insert({DNNL_ARG_WEIGHTS, m2_u4_m});
  args.insert({DNNL_ARG_DST, dst_m});
  args.insert({DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS, scale_m});
  args.insert({DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_WEIGHTS, zp_usr_m});

  sycl::event matmul_event = dnnl::sycl_interop::execute(matmul_p, stream, args, deps);
  return matmul_event;
}
}
