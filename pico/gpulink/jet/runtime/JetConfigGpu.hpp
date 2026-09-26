// JetConfigGpu.hpp - applied on top of each example's own JetConfig.hpp (as
// picojet's JetConfigPico.hpp): whole frames, no fields. The example's own
// choices (lighting, texturing, sorting, post-processing) stay; the GPU
// (jet/gpu/JetGpu.cpp) draws what they produce.
#pragma once

#undef  HALF_WIDTH_BUFFERS
#define HALF_WIDTH_BUFFERS 0

#undef  FIELD_BUFFERS
#define FIELD_BUFFERS 0

#undef  SSR_FIELD_REFLECT
#define SSR_FIELD_REFLECT 0
