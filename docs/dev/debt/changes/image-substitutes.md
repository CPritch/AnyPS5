# shader

## add Functional

- [Null T# substitutes](../../../core/shader/recompiler/Optimization/src/DescriptorBindingBuilder.cpp): a null T# with SEL_0 selectors takes the specialization of the first float, unconverted XYZW T# its slot had and reads the null binding of that T#'s float heap, which must return (0,0,0,0) as the console does. That read was measured only on an NVIDIA GeForce RTX 5060 Laptop GPU (driver 620.73.04), for image_sample_lz through a 2D and a 1D view, with the null descriptor and with the RGBA8 placeholder (`APS5_NO_NULL_DESCRIPTOR`), with and without the GPU heap. A slot first bound with an integer T# keeps no substitute: the R32_UINT and R32_SINT placeholders read W as 1, and the recompiler does not know which of the two the driver binds. A null T# of an image that is loaded, queried, compared, fmask-expanded, gathered in 3D or sampled through a table or unnormalized S# keeps its own specialization.
