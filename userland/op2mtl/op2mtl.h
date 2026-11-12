/*
 * op2mtl: the OnePlus 2's Metal driver core (G5), a C API over Mesa's freedreno (fd4) Gallium driver and the
 * AIR -> NIR front end. OP2Metal.bundle (the Metal plug-in CoreAnimation loads) is a thin shim over this:
 * it dlopens /usr/local/lib/op2mtl.dylib and maps each Metal call onto one of these.
 *
 * Numbers are METAL's own enums (MTLPixelFormat, MTLVertexFormat, MTLBlendFactor, ...), so the shim passes
 * them through unchanged; the conversion to Gallium is in one place, op2mtl.c.
 *
 * Execution model (first milestone): encoding goes straight into one Gallium context under the device's lock;
 * op2mtl_commit flushes and returns a fence, op2mtl_wait waits for it. Every call is safe from any thread.
 * Every failure is reported by a NULL / false return and a sentence in op2mtl_last_error().
 */
#ifndef OP2MTL_H
#define OP2MTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OP2MTL_API __attribute__((visibility("default")))
#define OP2MTL_MAX_ATTRIBS 16
#define OP2MTL_MAX_BUFFERS 31
#define OP2MTL_MAX_COLOR 4
#define OP2MTL_MAX_FC 32

typedef struct op2mtl_device op2mtl_device;
typedef struct op2mtl_library op2mtl_library;
typedef struct op2mtl_pipeline op2mtl_pipeline;
typedef struct op2mtl_buffer op2mtl_buffer;
typedef struct op2mtl_texture op2mtl_texture;
typedef struct op2mtl_sampler op2mtl_sampler;

/* the version of this API: OP2Metal refuses a library with another one */
#define OP2MTL_VERSION 1
OP2MTL_API unsigned op2mtl_version(void);
OP2MTL_API const char *op2mtl_last_error(void);
/* a log sink (OP2Metal routes it to the kernel log); NULL = stderr */
OP2MTL_API void op2mtl_set_log(void (*fn)(const char *line));

/* -- device ------------------------------------------------------------------------------------------- */
OP2MTL_API op2mtl_device *op2mtl_device_open(void);
OP2MTL_API const char *op2mtl_device_name(op2mtl_device *dev);

/* -- libraries and pipelines ---------------------------------------------------------------------------- */
/* a .metallib in memory (copied) */
OP2MTL_API op2mtl_library *op2mtl_library_new(op2mtl_device *dev, const void *data, size_t size);
OP2MTL_API bool op2mtl_library_has(op2mtl_library *lib, const char *function);
OP2MTL_API void op2mtl_library_free(op2mtl_library *lib);

struct op2mtl_constant {          /* one MTLFunctionConstantValues entry, by index */
   uint32_t index;
   uint64_t bits;                 /* the value's bits, as Metal stores them (bool/int/uint/float/half) */
};

struct op2mtl_function {
   op2mtl_library *library;
   const char *name;
   unsigned num_constants;
   struct op2mtl_constant constants[OP2MTL_MAX_FC];
};

struct op2mtl_vertex_attribute {  /* MTLVertexAttributeDescriptor, by attribute index */
   uint32_t format;               /* MTLVertexFormat; 0 = invalid (unused) */
   uint32_t offset;
   uint32_t buffer_index;         /* Metal [[buffer(n)]] */
};

struct op2mtl_vertex_layout {     /* MTLVertexBufferLayoutDescriptor, by buffer index */
   uint32_t stride;               /* 0 = no layout at this index */
   uint32_t step_function;        /* MTLVertexStepFunction: 0 constant, 1 per vertex, 2 per instance */
   uint32_t step_rate;
};

struct op2mtl_color_attachment {  /* MTLRenderPipelineColorAttachmentDescriptor */
   uint32_t pixel_format;         /* MTLPixelFormat; 0 = no attachment */
   bool blending;
   uint32_t rgb_op, alpha_op;                 /* MTLBlendOperation */
   uint32_t src_rgb, dst_rgb, src_alpha, dst_alpha;   /* MTLBlendFactor */
   uint32_t write_mask;           /* MTLColorWriteMask: R=8 G=4 B=2 A=1 */
};

struct op2mtl_pipeline_desc {
   struct op2mtl_function vertex, fragment;
   struct op2mtl_vertex_attribute attributes[OP2MTL_MAX_ATTRIBS];
   struct op2mtl_vertex_layout layouts[OP2MTL_MAX_BUFFERS];
   struct op2mtl_color_attachment color[OP2MTL_MAX_COLOR];
   uint32_t sample_count;         /* 1 (MSAA is refused for now) */
};

OP2MTL_API op2mtl_pipeline *op2mtl_pipeline_new(op2mtl_device *dev, const struct op2mtl_pipeline_desc *desc);
OP2MTL_API void op2mtl_pipeline_free(op2mtl_pipeline *p);

/* -- memory ------------------------------------------------------------------------------------------- */
OP2MTL_API op2mtl_buffer *op2mtl_buffer_new(op2mtl_device *dev, size_t size);
OP2MTL_API void *op2mtl_buffer_contents(op2mtl_buffer *buf);   /* CPU pointer, valid for the buffer's life */
OP2MTL_API size_t op2mtl_buffer_length(op2mtl_buffer *buf);
OP2MTL_API void op2mtl_buffer_free(op2mtl_buffer *buf);

/* usage: MTLTextureUsage bits (1 read, 2 write, 4 render target) */
OP2MTL_API op2mtl_texture *op2mtl_texture_new(op2mtl_device *dev, uint32_t pixel_format, uint32_t width,
                                              uint32_t height, uint32_t mip_levels, uint32_t usage);
/* an IOSurface by IOSurfaceGetID(): the kernel imports its pages (kOP2Surface); stride = bytes per row */
OP2MTL_API op2mtl_texture *op2mtl_texture_from_iosurface(op2mtl_device *dev, uint32_t surface_id,
                                                         uint32_t pixel_format, uint32_t width,
                                                         uint32_t height, uint32_t stride);
OP2MTL_API bool op2mtl_texture_replace(op2mtl_texture *tex, uint32_t level, uint32_t x, uint32_t y,
                                       uint32_t w, uint32_t h, const void *bytes, size_t bytes_per_row);
OP2MTL_API bool op2mtl_texture_read(op2mtl_texture *tex, uint32_t level, uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, void *bytes, size_t bytes_per_row);
OP2MTL_API void op2mtl_texture_free(op2mtl_texture *tex);

struct op2mtl_sampler_desc {      /* MTLSamplerDescriptor */
   uint32_t min_filter, mag_filter;   /* MTLSamplerMinMagFilter: 0 nearest, 1 linear */
   uint32_t mip_filter;               /* MTLSamplerMipFilter: 0 not mipmapped, 1 nearest, 2 linear */
   uint32_t s, t, r;                  /* MTLSamplerAddressMode */
   bool normalized;                   /* normalizedCoordinates */
   float lod_min, lod_max;
};
OP2MTL_API op2mtl_sampler *op2mtl_sampler_new(op2mtl_device *dev, const struct op2mtl_sampler_desc *desc);
OP2MTL_API void op2mtl_sampler_free(op2mtl_sampler *s);

/* -- a render pass ------------------------------------------------------------------------------------ */
struct op2mtl_pass_attachment {
   op2mtl_texture *texture;       /* NULL = no attachment */
   uint32_t load_action;          /* MTLLoadAction: 0 don't care, 1 load, 2 clear */
   double clear[4];
};

OP2MTL_API bool op2mtl_pass_begin(op2mtl_device *dev, const struct op2mtl_pass_attachment color[OP2MTL_MAX_COLOR]);
OP2MTL_API void op2mtl_set_pipeline(op2mtl_device *dev, op2mtl_pipeline *p);
/* MTLViewport (origin x/y, width, height, znear, zfar) and MTLScissorRect */
OP2MTL_API void op2mtl_set_viewport(op2mtl_device *dev, double x, double y, double w, double h, double zn, double zf);
OP2MTL_API void op2mtl_set_scissor(op2mtl_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* stage: 0 vertex, 1 fragment; a buffer may serve as stage-in vertex data AND a constant buffer at once */
OP2MTL_API void op2mtl_set_buffer(op2mtl_device *dev, unsigned stage, unsigned index, op2mtl_buffer *buf, size_t offset);
OP2MTL_API void op2mtl_set_buffer_offset(op2mtl_device *dev, unsigned stage, unsigned index, size_t offset);
/* setVertexBytes / setFragmentBytes: copied */
OP2MTL_API void op2mtl_set_bytes(op2mtl_device *dev, unsigned stage, unsigned index, const void *bytes, size_t length);
OP2MTL_API void op2mtl_set_texture(op2mtl_device *dev, unsigned stage, unsigned index, op2mtl_texture *tex);
OP2MTL_API void op2mtl_set_sampler(op2mtl_device *dev, unsigned stage, unsigned index, op2mtl_sampler *s);
/* MTLPrimitiveType; MTLIndexType 0 = uint16, 1 = uint32 */
OP2MTL_API bool op2mtl_draw(op2mtl_device *dev, uint32_t primitive, uint32_t vertex_start, uint32_t vertex_count,
                            uint32_t instance_count);
OP2MTL_API bool op2mtl_draw_indexed(op2mtl_device *dev, uint32_t primitive, uint32_t index_count,
                                    uint32_t index_type, op2mtl_buffer *index_buffer, size_t index_offset,
                                    uint32_t instance_count);
OP2MTL_API void op2mtl_pass_end(op2mtl_device *dev);

/* -- submission --------------------------------------------------------------------------------------- */
/* everything encoded so far goes to the GPU; returns a fence (0 = nothing to wait for) */
OP2MTL_API uint64_t op2mtl_commit(op2mtl_device *dev);
/* true when it completed; timeout_ns 0 = poll */
OP2MTL_API bool op2mtl_wait(op2mtl_device *dev, uint64_t fence, uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif
#endif
