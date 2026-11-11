/*
 * op2mtl: Metal's work for CoreAnimation, on the Adreno 430, through Mesa's fd4 Gallium driver (see op2mtl.h).
 *
 * Grown out of air_run.c (the hakios AIR front end's draw harness, MIT, with this project's patches): the
 * screen is opened the same way, shaders go AIR -> NIR -> ir3 the same way (finalize_for_gallium is that
 * file's, minus its softpipe branches), and the draw state is what air_run proved on the phone pixel for pixel
 * (G3/G4: sysmem, Metal's negative viewport y, premultiplied blending, framebuffer fetch, BGRA8 targets).
 * What is new here: CA's own VERTEX function fed from vertex buffers through the pipeline's vertex descriptor,
 * index buffers, IOSurface render targets, and state that lives across calls instead of in one main().
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "air_module.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/winsys_handle.h"
#include "freedreno/drm/freedreno_drm_public.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/u_memory.h"
#include "util/ralloc.h"
#include <xf86drm.h>

#include "op2mtl.h"

/* -- errors and logging ------------------------------------------------------------------------------- */

static __thread char g_error[256];
static void (*g_log)(const char *line);

OP2MTL_API unsigned op2mtl_version(void) { return OP2MTL_VERSION; }
OP2MTL_API const char *op2mtl_last_error(void) { return g_error; }
OP2MTL_API void op2mtl_set_log(void (*fn)(const char *line)) { g_log = fn; }

static void
say(const char *fmt, ...)
{
   char line[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof line, fmt, ap);
   va_end(ap);
   if (g_log)
      g_log(line);
   else
      fprintf(stderr, "op2mtl: %s\n", line);
}

/* set the error, say it once, return NULL/false */
static void *
fail(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(g_error, sizeof g_error, fmt, ap);
   va_end(ap);
   say("%s", g_error);
   return NULL;
}

/* -- Metal's numbers -> Gallium's --------------------------------------------------------------------- */

static enum pipe_format
pixel_format(uint32_t f)
{
   switch (f) {
   case 1:   return PIPE_FORMAT_A8_UNORM;
   case 10:  return PIPE_FORMAT_R8_UNORM;
   case 25:  return PIPE_FORMAT_R16_FLOAT;
   case 30:  return PIPE_FORMAT_R8G8_UNORM;
   case 55:  return PIPE_FORMAT_R32_FLOAT;
   case 65:  return PIPE_FORMAT_R16G16_FLOAT;
   case 70:  return PIPE_FORMAT_R8G8B8A8_UNORM;
   case 71:  return PIPE_FORMAT_R8G8B8A8_SRGB;
   case 80:  return PIPE_FORMAT_B8G8R8A8_UNORM;
   case 81:  return PIPE_FORMAT_B8G8R8A8_SRGB;
   case 90:  return PIPE_FORMAT_R10G10B10A2_UNORM;
   case 115: return PIPE_FORMAT_R16G16B16A16_FLOAT;
   case 125: return PIPE_FORMAT_R32G32B32A32_FLOAT;
   /* ASTC (the iOS 12.4 SDK's MTLPixelFormat values; fd4 samples all of them): CA's wallpaper is 4x4 LDR */
   case 204: return PIPE_FORMAT_ASTC_4x4;
   case 205: return PIPE_FORMAT_ASTC_5x4;
   case 206: return PIPE_FORMAT_ASTC_5x5;
   case 207: return PIPE_FORMAT_ASTC_6x5;
   case 208: return PIPE_FORMAT_ASTC_6x6;
   case 210: return PIPE_FORMAT_ASTC_8x5;
   case 211: return PIPE_FORMAT_ASTC_8x6;
   case 212: return PIPE_FORMAT_ASTC_8x8;
   case 213: return PIPE_FORMAT_ASTC_10x5;
   case 214: return PIPE_FORMAT_ASTC_10x6;
   case 215: return PIPE_FORMAT_ASTC_10x8;
   case 216: return PIPE_FORMAT_ASTC_10x10;
   case 217: return PIPE_FORMAT_ASTC_12x10;
   case 218: return PIPE_FORMAT_ASTC_12x12;
   case 186: return PIPE_FORMAT_ASTC_4x4_SRGB;
   case 187: return PIPE_FORMAT_ASTC_5x4_SRGB;
   case 188: return PIPE_FORMAT_ASTC_5x5_SRGB;
   case 189: return PIPE_FORMAT_ASTC_6x5_SRGB;
   case 190: return PIPE_FORMAT_ASTC_6x6_SRGB;
   case 192: return PIPE_FORMAT_ASTC_8x5_SRGB;
   case 193: return PIPE_FORMAT_ASTC_8x6_SRGB;
   case 194: return PIPE_FORMAT_ASTC_8x8_SRGB;
   case 195: return PIPE_FORMAT_ASTC_10x5_SRGB;
   case 196: return PIPE_FORMAT_ASTC_10x6_SRGB;
   case 197: return PIPE_FORMAT_ASTC_10x8_SRGB;
   case 198: return PIPE_FORMAT_ASTC_10x10_SRGB;
   case 199: return PIPE_FORMAT_ASTC_12x10_SRGB;
   case 200: return PIPE_FORMAT_ASTC_12x12_SRGB;
   default:  return PIPE_FORMAT_NONE;
   }
}

static enum pipe_format
vertex_format(uint32_t f)
{
   switch (f) {
   case 1:  return PIPE_FORMAT_R8G8_UINT;
   case 2:  return PIPE_FORMAT_R8G8B8_UINT;
   case 3:  return PIPE_FORMAT_R8G8B8A8_UINT;
   case 7:  return PIPE_FORMAT_R8G8_UNORM;
   case 8:  return PIPE_FORMAT_R8G8B8_UNORM;
   case 9:  return PIPE_FORMAT_R8G8B8A8_UNORM;
   case 13: return PIPE_FORMAT_R16G16_UINT;
   case 14: return PIPE_FORMAT_R16G16B16_UINT;
   case 15: return PIPE_FORMAT_R16G16B16A16_UINT;
   case 19: return PIPE_FORMAT_R16G16_UNORM;
   case 20: return PIPE_FORMAT_R16G16B16_UNORM;
   case 21: return PIPE_FORMAT_R16G16B16A16_UNORM;
   case 25: return PIPE_FORMAT_R16G16_FLOAT;
   case 26: return PIPE_FORMAT_R16G16B16_FLOAT;
   case 27: return PIPE_FORMAT_R16G16B16A16_FLOAT;
   case 28: return PIPE_FORMAT_R32_FLOAT;
   case 29: return PIPE_FORMAT_R32G32_FLOAT;
   case 30: return PIPE_FORMAT_R32G32B32_FLOAT;
   case 31: return PIPE_FORMAT_R32G32B32A32_FLOAT;
   case 36: return PIPE_FORMAT_R32_UINT;
   case 37: return PIPE_FORMAT_R32G32_UINT;
   case 38: return PIPE_FORMAT_R32G32B32_UINT;
   case 39: return PIPE_FORMAT_R32G32B32A32_UINT;
   case 42: return PIPE_FORMAT_B8G8R8A8_UNORM;   /* UChar4Normalized_BGRA */
   case 45: return PIPE_FORMAT_R8_UINT;
   case 47: return PIPE_FORMAT_R8_UNORM;
   case 51: return PIPE_FORMAT_R16_FLOAT;
   default: return PIPE_FORMAT_NONE;
   }
}

static unsigned
blend_factor(uint32_t f)
{
   static const unsigned map[] = {
      [0] = PIPE_BLENDFACTOR_ZERO, [1] = PIPE_BLENDFACTOR_ONE,
      [2] = PIPE_BLENDFACTOR_SRC_COLOR, [3] = PIPE_BLENDFACTOR_INV_SRC_COLOR,
      [4] = PIPE_BLENDFACTOR_SRC_ALPHA, [5] = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
      [6] = PIPE_BLENDFACTOR_DST_COLOR, [7] = PIPE_BLENDFACTOR_INV_DST_COLOR,
      [8] = PIPE_BLENDFACTOR_DST_ALPHA, [9] = PIPE_BLENDFACTOR_INV_DST_ALPHA,
      [10] = PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE,
      [11] = PIPE_BLENDFACTOR_CONST_COLOR, [12] = PIPE_BLENDFACTOR_INV_CONST_COLOR,
      [13] = PIPE_BLENDFACTOR_CONST_ALPHA, [14] = PIPE_BLENDFACTOR_INV_CONST_ALPHA,
   };
   return f < ARRAY_SIZE(map) ? map[f] : PIPE_BLENDFACTOR_ONE;
}

static unsigned
blend_op(uint32_t o)
{
   static const unsigned map[] = { PIPE_BLEND_ADD, PIPE_BLEND_SUBTRACT, PIPE_BLEND_REVERSE_SUBTRACT,
                                   PIPE_BLEND_MIN, PIPE_BLEND_MAX };
   return o < ARRAY_SIZE(map) ? map[o] : PIPE_BLEND_ADD;
}

static unsigned
write_mask(uint32_t m)   /* Metal: R=8 G=4 B=2 A=1; Gallium: R=1 G=2 B=4 A=8 */
{
   return ((m & 8) ? PIPE_MASK_R : 0) | ((m & 4) ? PIPE_MASK_G : 0) |
          ((m & 2) ? PIPE_MASK_B : 0) | ((m & 1) ? PIPE_MASK_A : 0);
}

static unsigned
wrap_mode(uint32_t a)
{
   static const unsigned map[] = {
      [0] = PIPE_TEX_WRAP_CLAMP_TO_EDGE, [1] = PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE,
      [2] = PIPE_TEX_WRAP_REPEAT, [3] = PIPE_TEX_WRAP_MIRROR_REPEAT,
      [4] = PIPE_TEX_WRAP_CLAMP_TO_BORDER, [5] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
   };
   return a < ARRAY_SIZE(map) ? map[a] : PIPE_TEX_WRAP_CLAMP_TO_EDGE;
}

static bool
primitive(uint32_t p, enum mesa_prim *out)
{
   static const enum mesa_prim map[] = { MESA_PRIM_POINTS, MESA_PRIM_LINES, MESA_PRIM_LINE_STRIP,
                                         MESA_PRIM_TRIANGLES, MESA_PRIM_TRIANGLE_STRIP };
   if (p >= ARRAY_SIZE(map))
      return false;
   *out = map[p];
   return true;
}

/* -- objects ------------------------------------------------------------------------------------------ */

#define STAGES 2   /* 0 vertex, 1 fragment */
#define MAX_TEX 16
#define MAX_FENCES 64

struct op2mtl_library {
   op2mtl_device *dev;
   void *mem;
   void *data;
   size_t size;
   struct air_metallib lib;
};

struct op2mtl_buffer {
   op2mtl_device *dev;
   struct pipe_resource *res;
   struct pipe_transfer *xfer;
   void *map;
   size_t size;
};

struct op2mtl_texture {
   op2mtl_device *dev;
   struct pipe_resource *res;
   struct pipe_sampler_view *view;
   struct pipe_surface *surf;
   uint32_t w, h;
};

struct op2mtl_sampler {
   op2mtl_device *dev;
   void *cso;
};

#define VB_ZERO (-2)                  /* vb_metal[]: the device's zero buffer, not a Metal buffer */

struct op2mtl_pipeline {
   op2mtl_device *dev;
   void *mem;                         /* the translated shaders' info lives here */
   void *vs, *fs, *blend, *velems;
   unsigned num_vb;                   /* gallium vertex buffer slots */
   int vb_metal[OP2MTL_MAX_BUFFERS];  /* slot -> Metal buffer index */
   uint32_t ubo[STAGES], ssbo[STAGES];        /* Metal buffer indices read as constant / device buffers */
   uint32_t textures[STAGES];
   uint32_t samplers[STAGES];
   void *inline_sampler[STAGES][PIPE_MAX_SAMPLERS];
   unsigned num_color;
};

struct binding {
   op2mtl_buffer *buf;
   size_t offset;
   struct pipe_resource *bytes;       /* set_bytes' own copy */
   size_t bytes_len;
};

struct op2mtl_device {
   pthread_mutex_t lock;
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   const nir_shader_compiler_options *nir_opts[STAGES];
   void *dsa, *rast[2];               /* rasterizer without / with scissor */
   void *default_sampler;
   struct pipe_resource *zero_vb;     /* 16 zero bytes: attributes the vertex descriptor leaves out read 0 */
   struct pipe_resource *zero_ubo;    /* 4 KB of zeros: a constant buffer the shader reads and CA did not bind */
   uint32_t zero_ubo_said[STAGES];    /* which indices have been reported */
   struct pipe_sampler_view *zero_view;   /* 1x1 transparent black: a texture slot the shader samples, unbound */
   uint32_t zero_view_said[STAGES];

   bool in_pass;
   struct pipe_framebuffer_state fb;
   op2mtl_pipeline *pipeline;
   struct binding bind[STAGES][OP2MTL_MAX_BUFFERS];
   op2mtl_texture *tex[STAGES][MAX_TEX];
   op2mtl_sampler *smp[STAGES][MAX_TEX];
   struct pipe_viewport_state vp;
   struct pipe_scissor_state scissor;
   bool scissor_on;

   uint64_t fence_seq;
   struct pipe_fence_handle *fences[MAX_FENCES];
   uint64_t fence_ids[MAX_FENCES];
};

/* -- device ------------------------------------------------------------------------------------------- */

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* air_run's finalize_for_gallium for a screen that is freedreno (its softpipe branches dropped) */
static void
finalize_for_gallium(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_system_values);
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   /* interpolated-input intrinsics, or every varying is FLAT on ir3 (air_run.c says how that was found) */
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4,
            nir_lower_io_use_interpolated_input_intrinsics);
}

/* ir3's compile errors (tools/mesa-op2/ir3-context-error-returns.patch), in this library's log */
extern void (*ir3_op2_error_hook)(const char *msg);
static void
ir3_error(const char *msg)
{
   say("ir3: %s", msg);
}

OP2MTL_API op2mtl_device *
op2mtl_device_open(void)
{
   ir3_op2_error_hook = ir3_error;
   /* GMEM is OCMEM on this SoC and the kernel allocates none: sysmem rendering only (G3) */
   setenv("FD_MESA_DEBUG", "sysmem", 0);
   glsl_type_singleton_init_or_ref();

   int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return fail("device: the msm device (OP2GPU) did not open");
   static const driOptionDescription freedreno_options[] = {
#include "freedreno/driinfo_freedreno.h"
   };
   static driOptionCache info, cache;
   static struct pipe_screen_config config;
   driParseOptionInfo(&info, freedreno_options, ARRAY_SIZE(freedreno_options));
   driParseConfigFiles(&cache, &info, 0, "msm", NULL, NULL, NULL, 0, NULL, 0);
   config.options = &cache;
   config.options_info = &info;

   op2mtl_device *dev = CALLOC_STRUCT(op2mtl_device);
   pthread_mutex_init(&dev->lock, NULL);
   dev->screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
   if (!dev->screen)
      return fail("device: the msm device opened but gave no screen");
   dev->pipe = dev->screen->context_create(dev->screen, NULL, 0);
   if (!dev->pipe)
      return fail("device: %s gave no context", dev->screen->get_name(dev->screen));
   dev->nir_opts[0] = dev->screen->get_compiler_options(dev->screen, PIPE_SHADER_IR_NIR, PIPE_SHADER_VERTEX);
   dev->nir_opts[1] = dev->screen->get_compiler_options(dev->screen, PIPE_SHADER_IR_NIR, PIPE_SHADER_FRAGMENT);

   dev->dsa = dev->pipe->create_depth_stencil_alpha_state(dev->pipe,
      &(struct pipe_depth_stencil_alpha_state){ 0 });
   for (unsigned s = 0; s < 2; s++)
      dev->rast[s] = dev->pipe->create_rasterizer_state(dev->pipe, &(struct pipe_rasterizer_state){
         .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
         .depth_clip_near = 1, .depth_clip_far = 1, .clip_halfz = 1, .scissor = s,
         /* Metal's provoking vertex is a triangle's FIRST (Chlorophyll 130eca2: flat colours swapped between a
          * quad's two triangles), and its lines and points are one pixel: Gallium's 0 drew none, and CA draws
          * its mask coverage as lines (Chlorophyll 175571a: every mask empty) */
         .flatshade_first = 1, .line_width = 1.0f, .point_size = 1.0f,
      });
   dev->default_sampler = dev->pipe->create_sampler_state(dev->pipe, &(struct pipe_sampler_state){
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE, .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE, .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST, .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .max_lod = 1000.0f,
   });
   static const float zeros[4];
   dev->zero_vb = pipe_buffer_create(dev->screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_IMMUTABLE, sizeof zeros);
   if (dev->zero_vb)
      pipe_buffer_write(dev->pipe, dev->zero_vb, 0, sizeof zeros, zeros);
   static const uint8_t zero4k[4096];
   dev->zero_ubo = pipe_buffer_create(dev->screen, PIPE_BIND_CONSTANT_BUFFER, PIPE_USAGE_IMMUTABLE, sizeof zero4k);
   if (dev->zero_ubo)
      pipe_buffer_write(dev->pipe, dev->zero_ubo, 0, sizeof zero4k, zero4k);
   struct pipe_resource *zt = dev->screen->resource_create(dev->screen, &(struct pipe_resource){
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8A8_UNORM, .width0 = 1, .height0 = 1, .depth0 = 1,
      .array_size = 1, .bind = PIPE_BIND_SAMPLER_VIEW });
   if (zt) {
      static const uint32_t black;
      dev->pipe->texture_subdata(dev->pipe, zt, 0, PIPE_MAP_WRITE,
                                 &(struct pipe_box){ .width = 1, .height = 1, .depth = 1 }, &black, 4, 4);
      struct pipe_sampler_view vt;
      u_sampler_view_default_template(&vt, zt, zt->format);
      dev->zero_view = dev->pipe->create_sampler_view(dev->pipe, zt, &vt);
      pipe_resource_reference(&zt, NULL);   /* the view keeps it */
   }
   say("device: %s, %s", dev->screen->get_name(dev->screen), dev->screen->get_vendor(dev->screen));
   return dev;
}

OP2MTL_API const char *
op2mtl_device_name(op2mtl_device *dev)
{
   return dev->screen->get_name(dev->screen);
}

/* -- libraries ---------------------------------------------------------------------------------------- */

OP2MTL_API op2mtl_library *
op2mtl_library_new(op2mtl_device *dev, const void *data, size_t size)
{
   op2mtl_library *lib = CALLOC_STRUCT(op2mtl_library);
   lib->dev = dev;
   lib->mem = ralloc_context(NULL);
   lib->data = ralloc_size(lib->mem, size);
   memcpy(lib->data, data, size);
   lib->size = size;
   const char *err = NULL;
   if (size < 4 || memcmp(data, "MTLB", 4) || !air_metallib_parse(lib->mem, lib->data, size, &lib->lib, &err)) {
      ralloc_free(lib->mem);
      FREE(lib);
      return fail("library: not a metallib (%s)", err ? err : "no MTLB header");
   }
   return lib;
}

static const struct air_metallib_function *
library_find(op2mtl_library *lib, const char *name)
{
   for (unsigned i = 0; i < lib->lib.num_functions; i++)
      if (!strcmp(lib->lib.functions[i].name, name))
         return &lib->lib.functions[i];
   return NULL;
}

OP2MTL_API bool
op2mtl_library_has(op2mtl_library *lib, const char *function)
{
   return library_find(lib, function) != NULL;
}

OP2MTL_API void
op2mtl_library_free(op2mtl_library *lib)
{
   if (!lib)
      return;
   ralloc_free(lib->mem);
   FREE(lib);
}

/* -- pipelines ---------------------------------------------------------------------------------------- */

static nir_shader *
translate(op2mtl_device *dev, void *mem, const struct op2mtl_function *f, gl_shader_stage want,
          struct air_shader_info *info)
{
   if (!f->library || !f->name)
      return fail("pipeline: no %s function", gl_shader_stage_name(want));
   const struct air_metallib_function *fn = library_find(f->library, f->name);
   if (!fn)
      return fail("pipeline: the library has no function %s", f->name);
   struct air_constant_value *fc = NULL;
   unsigned num_fc = air_module_function_constants(mem, fn->bitcode, fn->bitcode_size, &fc);
   /* a value for a constant this function does not declare is fine: CA hands both stages one set */
   for (unsigned i = 0; i < f->num_constants && i < OP2MTL_MAX_FC; i++)
      for (unsigned j = 0; j < num_fc; j++)
         if (fc[j].index == f->constants[i].index) {
            fc[j].bits = f->constants[i].bits;
            fc[j].supplied = true;
         }
   struct air_to_nir_options opts = { .constants = fc, .num_constants = num_fc };
   const char *err = NULL;
   unsigned s = want == MESA_SHADER_VERTEX ? 0 : 1;
   nir_shader *nir = air_to_nir(mem, fn->bitcode, fn->bitcode_size, NULL, &opts, dev->nir_opts[s], info, &err);
   if (!nir)
      return fail("pipeline: %s: %s", f->name, err ? err : "translation failed");
   if (info->stage != want)
      return fail("pipeline: %s is a %s function, wanted %s", f->name, gl_shader_stage_name(info->stage),
                  gl_shader_stage_name(want));
   return nir;   /* lowered by finalize_for_gallium once the stages are linked (link_varyings) */
}

/*
 * Fragment inputs take the slot of the vertex output with the same AIR link string (`user(locnN)'). The front end
 * numbers varyings by order on each side, and QuartzCore's shared vertex functions write more varyings than each
 * fragment function reads, so by order the fragment side reads the wrong ones: Chlorophyll fb5d79f found its
 * colours coming from the wrong slot, our round g5b-7 photo shows an orange second hand drawn yellow-green. The
 * fragment side's names are those strings (tools/mesa-op2/air-varying-link-name.patch).
 */
static void
link_varyings(nir_shader *fs, const struct air_shader_info *vinfo, const struct air_shader_info *finfo,
              const char *vname, const char *fname)
{
   nir_foreach_variable_with_modes(var, fs, nir_var_shader_in) {
      if (var->data.location < VARYING_SLOT_VAR0)
         continue;
      const char *link = NULL;
      for (unsigned i = 0; i < finfo->num_varyings; i++)
         if (finfo->varyings[i].slot == (unsigned)var->data.location)
            link = finfo->varyings[i].name;
      if (!link)
         continue;
      int to = -1;
      for (unsigned i = 0; i < vinfo->num_varyings; i++)
         if (vinfo->varyings[i].name && !strcmp(vinfo->varyings[i].name, link))
            to = (int)vinfo->varyings[i].slot;
      if (to < 0) {
         say("pipeline: %s reads varying %s, which %s does not write", fname, link, vname);
         continue;
      }
      var->data.location = to;
   }
}

static void
note_resources(op2mtl_pipeline *p, unsigned s, const struct air_shader_info *info)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->index >= 32)
         continue;
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER: p->ubo[s] |= 1u << r->index; break;
      case AIR_RESOURCE_DEVICE_BUFFER:   p->ssbo[s] |= 1u << r->index; break;
      case AIR_RESOURCE_TEXTURE:         if (!r->null_texture) p->textures[s] |= 1u << r->index; break;
      case AIR_RESOURCE_SAMPLER:
         p->samplers[s] |= 1u << r->index;
         if (r->inline_sampler && r->index < PIPE_MAX_SAMPLERS && !p->inline_sampler[s][r->index]) {
            const struct air_sampler_state *a = &r->sampler;
            static const unsigned wrap[] = {
               [AIR_SAMPLER_CLAMP_TO_ZERO] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
               [AIR_SAMPLER_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
               [AIR_SAMPLER_REPEAT] = PIPE_TEX_WRAP_REPEAT,
               [AIR_SAMPLER_MIRRORED_REPEAT] = PIPE_TEX_WRAP_MIRROR_REPEAT,
               [AIR_SAMPLER_CLAMP_TO_BORDER] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
            };
            struct pipe_sampler_state st = {
               .wrap_s = wrap[a->address[0] % ARRAY_SIZE(wrap)],
               .wrap_t = wrap[a->address[1] % ARRAY_SIZE(wrap)],
               .wrap_r = wrap[a->address[2] % ARRAY_SIZE(wrap)],
               .mag_img_filter = a->mag_filter == AIR_SAMPLER_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR,
               .min_img_filter = a->min_filter == AIR_SAMPLER_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR,
               .min_mip_filter = a->mip_filter == AIR_SAMPLER_MIP_LINEAR ? PIPE_TEX_MIPFILTER_LINEAR :
                                 a->mip_filter == AIR_SAMPLER_MIP_NEAREST ? PIPE_TEX_MIPFILTER_NEAREST :
                                 PIPE_TEX_MIPFILTER_NONE,
               .unnormalized_coords = a->pixel_coord,
               .min_lod = _mesa_half_to_float(a->lod_min),
               .max_lod = _mesa_half_to_float(a->lod_max),
            };
            p->inline_sampler[s][r->index] = p->dev->pipe->create_sampler_state(p->dev->pipe, &st);
         }
         break;
      default:
         break;
      }
   }
}

OP2MTL_API void
op2mtl_pipeline_free(op2mtl_pipeline *p)
{
   if (!p)
      return;
   struct pipe_context *pipe = p->dev->pipe;
   pthread_mutex_lock(&p->dev->lock);
   if (p->dev->pipeline == p)
      p->dev->pipeline = NULL;
   if (p->vs) pipe->delete_vs_state(pipe, p->vs);
   if (p->fs) pipe->delete_fs_state(pipe, p->fs);
   if (p->blend) pipe->delete_blend_state(pipe, p->blend);
   if (p->velems) pipe->delete_vertex_elements_state(pipe, p->velems);
   for (unsigned s = 0; s < STAGES; s++)
      for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
         if (p->inline_sampler[s][i])
            pipe->delete_sampler_state(pipe, p->inline_sampler[s][i]);
   pthread_mutex_unlock(&p->dev->lock);
   ralloc_free(p->mem);
   FREE(p);
}

OP2MTL_API op2mtl_pipeline *
op2mtl_pipeline_new(op2mtl_device *dev, const struct op2mtl_pipeline_desc *d)
{
   if (d->sample_count > 1)
      return fail("pipeline: %u samples: MSAA is not done yet", d->sample_count);
   op2mtl_pipeline *p = CALLOC_STRUCT(op2mtl_pipeline);
   p->dev = dev;
   p->mem = ralloc_context(NULL);
   for (unsigned i = 0; i < OP2MTL_MAX_BUFFERS; i++)
      p->vb_metal[i] = -1;

   pthread_mutex_lock(&dev->lock);
   struct air_shader_info vinfo, finfo;
   nir_shader *vs = translate(dev, p->mem, &d->vertex, MESA_SHADER_VERTEX, &vinfo);
   nir_shader *fs = vs ? translate(dev, p->mem, &d->fragment, MESA_SHADER_FRAGMENT, &finfo) : NULL;
   if (!vs || !fs)
      goto bad;
   link_varyings(fs, &vinfo, &finfo, d->vertex.name, d->fragment.name);
   finalize_for_gallium(vs);
   finalize_for_gallium(fs);

   /* the vertex descriptor -> vertex elements, one per stage-in attribute, in driver_location order */
   struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
   unsigned num_ve = 0;
   memset(ve, 0, sizeof ve);
   for (unsigned i = 0; i < vinfo.num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &vinfo.vertex_attributes[i];
      if (a->driver_location >= PIPE_MAX_ATTRIBS || a->index >= OP2MTL_MAX_ATTRIBS) {
         fail("pipeline: %s: attribute %u out of range", d->vertex.name, a->index);
         goto bad;
      }
      const struct op2mtl_vertex_attribute *m = &d->attributes[a->index];
      enum pipe_format f = vertex_format(m->format);
      if (f == PIPE_FORMAT_NONE && m->format) {
         fail("pipeline: %s: attribute %u (%s) has MTLVertexFormat %u, which is not mapped",
              d->vertex.name, a->index, a->name ? a->name : "?", m->format);
         goto bad;
      }
      if (f == PIPE_FORMAT_NONE) {
         /* the shader declares it, the vertex descriptor leaves it out (CA: `size` of ca_uber_vertex_lp0):
          * every vertex reads 0, from the device's zero buffer with stride 0 */
         int zslot = -1;
         for (unsigned k = 0; k < p->num_vb; k++)
            if (p->vb_metal[k] == VB_ZERO)
               zslot = (int)k;
         if (zslot < 0) {
            zslot = (int)p->num_vb++;
            p->vb_metal[zslot] = VB_ZERO;
         }
         ve[a->driver_location] = (struct pipe_vertex_element){
            .src_format = (uint8_t)PIPE_FORMAT_R32G32B32A32_FLOAT, .vertex_buffer_index = (uint8_t)zslot };
         num_ve = MAX2(num_ve, a->driver_location + 1);
         continue;
      }
      if (m->buffer_index >= OP2MTL_MAX_BUFFERS) {
         fail("pipeline: attribute %u reads buffer %u", a->index, m->buffer_index);
         goto bad;
      }
      int slot = -1;
      for (unsigned k = 0; k < p->num_vb; k++)
         if (p->vb_metal[k] == (int)m->buffer_index)
            slot = (int)k;
      if (slot < 0) {
         slot = (int)p->num_vb++;
         p->vb_metal[slot] = (int)m->buffer_index;
      }
      const struct op2mtl_vertex_layout *l = &d->layouts[m->buffer_index];
      struct pipe_vertex_element *e = &ve[a->driver_location];
      e->src_offset = (uint16_t)m->offset;
      e->vertex_buffer_index = (uint8_t)slot;
      e->src_format = (uint8_t)f;
      e->src_stride = l->step_function == 0 ? 0 : l->stride;
      e->instance_divisor = l->step_function == 2 ? MAX2(l->step_rate, 1) : 0;
      num_ve = MAX2(num_ve, a->driver_location + 1);
   }
   p->velems = dev->pipe->create_vertex_elements_state(dev->pipe, num_ve, ve);

   note_resources(p, 0, &vinfo);
   note_resources(p, 1, &finfo);

   struct pipe_blend_state bs = { .independent_blend_enable = 1 };
   for (unsigned i = 0; i < OP2MTL_MAX_COLOR; i++) {
      const struct op2mtl_color_attachment *c = &d->color[i];
      if (!c->pixel_format)
         continue;
      if (pixel_format(c->pixel_format) == PIPE_FORMAT_NONE) {
         fail("pipeline: colour attachment %u has MTLPixelFormat %u, which is not mapped", i, c->pixel_format);
         goto bad;
      }
      p->num_color = i + 1;
      bs.rt[i].colormask = write_mask(c->write_mask);
      if (c->blending) {
         bs.rt[i].blend_enable = 1;
         bs.rt[i].rgb_func = blend_op(c->rgb_op);
         bs.rt[i].alpha_func = blend_op(c->alpha_op);
         bs.rt[i].rgb_src_factor = blend_factor(c->src_rgb);
         bs.rt[i].rgb_dst_factor = blend_factor(c->dst_rgb);
         bs.rt[i].alpha_src_factor = blend_factor(c->src_alpha);
         bs.rt[i].alpha_dst_factor = blend_factor(c->dst_alpha);
      }
   }
   p->blend = dev->pipe->create_blend_state(dev->pipe, &bs);

   p->vs = dev->pipe->create_vs_state(dev->pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = vs });
   p->fs = dev->pipe->create_fs_state(dev->pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = fs });
   pthread_mutex_unlock(&dev->lock);
   if (!p->vs || !p->fs) {
      op2mtl_pipeline_free(p);
      return fail("pipeline: %s / %s: the driver refused the shaders", d->vertex.name, d->fragment.name);
   }
   say("pipeline: %s + %s: %u attributes in %u vertex buffers, vs ubo 0x%x, fs ubo 0x%x tex 0x%x smp 0x%x, "
       "%u colour", d->vertex.name, d->fragment.name, num_ve, p->num_vb, p->ubo[0], p->ubo[1],
       p->textures[1], p->samplers[1], p->num_color);
   return p;

bad:
   pthread_mutex_unlock(&dev->lock);
   op2mtl_pipeline_free(p);
   return NULL;
}

/* -- buffers ------------------------------------------------------------------------------------------ */

OP2MTL_API op2mtl_buffer *
op2mtl_buffer_new(op2mtl_device *dev, size_t size)
{
   if (!size)
      size = 4;
   op2mtl_buffer *b = CALLOC_STRUCT(op2mtl_buffer);
   b->dev = dev;
   b->size = size;
   pthread_mutex_lock(&dev->lock);
   b->res = pipe_buffer_create(dev->screen,
                               PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
                               PIPE_BIND_SHADER_BUFFER, PIPE_USAGE_DEFAULT, (unsigned)size);
   /* freedreno has no persistent maps, but an UNSYNCHRONIZED map of a linear buffer IS the BO's own CPU
    * mapping, which lives as long as the BO: Metal's `contents` pointer, kept until the buffer is freed */
   if (b->res)
      b->map = dev->pipe->buffer_map(dev->pipe, b->res, 0,
                                     PIPE_MAP_READ | PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED,
                                     &(struct pipe_box){ .width = (int)size, .height = 1, .depth = 1 },
                                     &b->xfer);
   pthread_mutex_unlock(&dev->lock);
   if (!b->res || !b->map) {
      op2mtl_buffer_free(b);
      return fail("buffer: %zu bytes could not be made or mapped", size);
   }
   return b;
}

OP2MTL_API void *op2mtl_buffer_contents(op2mtl_buffer *b) { return b->map; }
OP2MTL_API size_t op2mtl_buffer_length(op2mtl_buffer *b) { return b->size; }

OP2MTL_API void
op2mtl_buffer_free(op2mtl_buffer *b)
{
   if (!b)
      return;
   pthread_mutex_lock(&b->dev->lock);
   for (unsigned s = 0; s < STAGES; s++)
      for (unsigned i = 0; i < OP2MTL_MAX_BUFFERS; i++)
         if (b->dev->bind[s][i].buf == b)
            b->dev->bind[s][i].buf = NULL;
   if (b->xfer)
      b->dev->pipe->buffer_unmap(b->dev->pipe, b->xfer);
   pipe_resource_reference(&b->res, NULL);
   pthread_mutex_unlock(&b->dev->lock);
   FREE(b);
}

/* -- textures ----------------------------------------------------------------------------------------- */

static op2mtl_texture *
texture_wrap(op2mtl_device *dev, struct pipe_resource *res, uint32_t w, uint32_t h)
{
   op2mtl_texture *t = CALLOC_STRUCT(op2mtl_texture);
   t->dev = dev;
   t->res = res;
   t->w = w;
   t->h = h;
   struct pipe_sampler_view vt;
   u_sampler_view_default_template(&vt, res, res->format);
   t->view = dev->pipe->create_sampler_view(dev->pipe, res, &vt);
   if (res->bind & PIPE_BIND_RENDER_TARGET)
      t->surf = dev->pipe->create_surface(dev->pipe, res, &(struct pipe_surface){ .format = res->format });
   return t;
}

OP2MTL_API op2mtl_texture *
op2mtl_texture_new(op2mtl_device *dev, uint32_t mtl_format, uint32_t w, uint32_t h, uint32_t levels,
                   uint32_t usage)
{
   enum pipe_format f = pixel_format(mtl_format);
   if (f == PIPE_FORMAT_NONE)
      return fail("texture: MTLPixelFormat %u is not mapped", mtl_format);
   unsigned bind = PIPE_BIND_SAMPLER_VIEW;
   if ((usage & 4) && dev->screen->is_format_supported(dev->screen, f, PIPE_TEXTURE_2D, 0, 0,
                                                       PIPE_BIND_RENDER_TARGET))
      bind |= PIPE_BIND_RENDER_TARGET;
   pthread_mutex_lock(&dev->lock);
   struct pipe_resource *res = dev->screen->resource_create(dev->screen, &(struct pipe_resource){
      .target = PIPE_TEXTURE_2D, .format = f, .width0 = w, .height0 = h, .depth0 = 1, .array_size = 1,
      .last_level = levels ? levels - 1 : 0, .bind = bind,
   });
   op2mtl_texture *t = res ? texture_wrap(dev, res, w, h) : NULL;
   pthread_mutex_unlock(&dev->lock);
   if (!t)
      return fail("texture: %ux%u %s could not be made", w, h, util_format_short_name(f));
   return t;
}

OP2MTL_API op2mtl_texture *
op2mtl_texture_from_iosurface(op2mtl_device *dev, uint32_t surface_id, uint32_t mtl_format, uint32_t w,
                              uint32_t h, uint32_t stride)
{
   enum pipe_format f = pixel_format(mtl_format);
   if (f == PIPE_FORMAT_NONE)
      return fail("iosurface %u: MTLPixelFormat %u is not mapped", surface_id, mtl_format);
   /* freedreno's dmabuf import is the IOSurface import on this port (fd_bo_from_dmabuf -> hakios-drm ->
    * OP2GPU kOP2Surface): the "fd" is the IOSurface's ID */
   struct winsys_handle wh = { .type = WINSYS_HANDLE_TYPE_FD, .handle = surface_id, .stride = stride,
                               .format = f };
   pthread_mutex_lock(&dev->lock);
   struct pipe_resource *res = dev->screen->resource_from_handle(dev->screen, &(struct pipe_resource){
      .target = PIPE_TEXTURE_2D, .format = f, .width0 = w, .height0 = h, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET,
   }, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   op2mtl_texture *t = res ? texture_wrap(dev, res, w, h) : NULL;
   pthread_mutex_unlock(&dev->lock);
   if (!t)
      return fail("iosurface %u: %ux%u %s stride %u could not be imported", surface_id, w, h,
                  util_format_short_name(f), stride);
   return t;
}

OP2MTL_API bool
op2mtl_texture_replace(op2mtl_texture *t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const void *bytes, size_t bpr)
{
   pthread_mutex_lock(&t->dev->lock);
   t->dev->pipe->texture_subdata(t->dev->pipe, t->res, level, PIPE_MAP_WRITE,
                                 &(struct pipe_box){ .x = (int)x, .y = (int)y, .width = (int)w,
                                                     .height = (int)h, .depth = 1 },
                                 bytes, (unsigned)bpr, (unsigned)(bpr * h));
   pthread_mutex_unlock(&t->dev->lock);
   return true;
}

OP2MTL_API bool
op2mtl_texture_read(op2mtl_texture *t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    void *bytes, size_t bpr)
{
   pthread_mutex_lock(&t->dev->lock);
   struct pipe_transfer *xfer = NULL;
   const uint8_t *src = t->dev->pipe->texture_map(t->dev->pipe, t->res, level, PIPE_MAP_READ,
      &(struct pipe_box){ .x = (int)x, .y = (int)y, .width = (int)w, .height = (int)h, .depth = 1 }, &xfer);
   if (src) {
      size_t row = (size_t)w * util_format_get_blocksize(t->res->format);
      for (uint32_t r = 0; r < h; r++)
         memcpy((uint8_t *)bytes + r * bpr, src + r * xfer->stride, MIN2(row, bpr));
      t->dev->pipe->texture_unmap(t->dev->pipe, xfer);
   }
   pthread_mutex_unlock(&t->dev->lock);
   return src ? true : (fail("texture: could not be read back"), false);
}

OP2MTL_API void
op2mtl_texture_free(op2mtl_texture *t)
{
   if (!t)
      return;
   op2mtl_device *dev = t->dev;
   pthread_mutex_lock(&dev->lock);
   for (unsigned s = 0; s < STAGES; s++)
      for (unsigned i = 0; i < MAX_TEX; i++)
         if (dev->tex[s][i] == t)
            dev->tex[s][i] = NULL;
   pipe_sampler_view_reference(&t->view, NULL);
   pipe_surface_reference(&t->surf, NULL);
   pipe_resource_reference(&t->res, NULL);
   pthread_mutex_unlock(&dev->lock);
   FREE(t);
}

/* -- samplers ----------------------------------------------------------------------------------------- */

OP2MTL_API op2mtl_sampler *
op2mtl_sampler_new(op2mtl_device *dev, const struct op2mtl_sampler_desc *d)
{
   op2mtl_sampler *s = CALLOC_STRUCT(op2mtl_sampler);
   s->dev = dev;
   pthread_mutex_lock(&dev->lock);
   s->cso = dev->pipe->create_sampler_state(dev->pipe, &(struct pipe_sampler_state){
      .wrap_s = wrap_mode(d->s), .wrap_t = wrap_mode(d->t), .wrap_r = wrap_mode(d->r),
      .min_img_filter = d->min_filter ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = d->mag_filter ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = d->mip_filter == 2 ? PIPE_TEX_MIPFILTER_LINEAR :
                        d->mip_filter == 1 ? PIPE_TEX_MIPFILTER_NEAREST : PIPE_TEX_MIPFILTER_NONE,
      .unnormalized_coords = !d->normalized,
      .min_lod = d->lod_min, .max_lod = d->lod_max > 0 ? d->lod_max : 1000.0f,
   });
   pthread_mutex_unlock(&dev->lock);
   return s;
}

OP2MTL_API void
op2mtl_sampler_free(op2mtl_sampler *s)
{
   if (!s)
      return;
   op2mtl_device *dev = s->dev;
   pthread_mutex_lock(&dev->lock);
   for (unsigned st = 0; st < STAGES; st++)
      for (unsigned i = 0; i < MAX_TEX; i++)
         if (dev->smp[st][i] == s)
            dev->smp[st][i] = NULL;
   dev->pipe->delete_sampler_state(dev->pipe, s->cso);
   pthread_mutex_unlock(&dev->lock);
   FREE(s);
}

/* -- a render pass ------------------------------------------------------------------------------------ */

static void
set_full_viewport(op2mtl_device *dev)
{
   double w = dev->fb.width, h = dev->fb.height;
   /* Metal: origin top left, NDC y up, z in [0, 1] (clip_halfz): air_run's negative y scale */
   dev->vp = (struct pipe_viewport_state){
      .scale = { (float)(w / 2), (float)(-h / 2), 1.0f },
      .translate = { (float)(w / 2), (float)(h / 2), 0.0f },
   };
   dev->scissor_on = false;
}

OP2MTL_API bool
op2mtl_pass_begin(op2mtl_device *dev, const struct op2mtl_pass_attachment color[OP2MTL_MAX_COLOR])
{
   pthread_mutex_lock(&dev->lock);
   struct pipe_framebuffer_state fb = { 0 };
   unsigned w = ~0u, h = ~0u;
   for (unsigned i = 0; i < OP2MTL_MAX_COLOR; i++) {
      op2mtl_texture *t = color[i].texture;
      if (!t)
         continue;
      if (!t->surf) {
         pthread_mutex_unlock(&dev->lock);
         return fail("pass: colour attachment %u is not a render target texture", i), false;
      }
      fb.cbufs[i] = t->surf;
      fb.nr_cbufs = i + 1;
      w = MIN2(w, t->w);
      h = MIN2(h, t->h);
   }
   if (!fb.nr_cbufs) {
      pthread_mutex_unlock(&dev->lock);
      return fail("pass: no colour attachment"), false;
   }
   fb.width = (uint16_t)w;
   fb.height = (uint16_t)h;
   dev->fb = fb;
   dev->pipe->set_framebuffer_state(dev->pipe, &fb);
   for (unsigned i = 0; i < OP2MTL_MAX_COLOR; i++) {
      if (!color[i].texture || color[i].load_action != 2)
         continue;
      union pipe_color_union c = { .f = { (float)color[i].clear[0], (float)color[i].clear[1],
                                          (float)color[i].clear[2], (float)color[i].clear[3] } };
      dev->pipe->clear_render_target(dev->pipe, color[i].texture->surf, &c, 0, 0, w, h, false);
   }
   set_full_viewport(dev);
   dev->in_pass = true;
   pthread_mutex_unlock(&dev->lock);
   return true;
}

OP2MTL_API void
op2mtl_set_pipeline(op2mtl_device *dev, op2mtl_pipeline *p)
{
   pthread_mutex_lock(&dev->lock);
   dev->pipeline = p;
   pthread_mutex_unlock(&dev->lock);
}

OP2MTL_API void
op2mtl_set_viewport(op2mtl_device *dev, double x, double y, double w, double h, double zn, double zf)
{
   pthread_mutex_lock(&dev->lock);
   dev->vp = (struct pipe_viewport_state){
      .scale = { (float)(w / 2), (float)(-h / 2), (float)(zf - zn) },
      .translate = { (float)(x + w / 2), (float)(y + h / 2), (float)zn },
   };
   pthread_mutex_unlock(&dev->lock);
}

OP2MTL_API void
op2mtl_set_scissor(op2mtl_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
   pthread_mutex_lock(&dev->lock);
   dev->scissor = (struct pipe_scissor_state){
      .minx = (uint16_t)MIN2(x, dev->fb.width), .miny = (uint16_t)MIN2(y, dev->fb.height),
      .maxx = (uint16_t)MIN2(x + w, dev->fb.width), .maxy = (uint16_t)MIN2(y + h, dev->fb.height),
   };
   dev->scissor_on = true;
   pthread_mutex_unlock(&dev->lock);
}

static void
binding_clear_bytes(struct binding *b)
{
   pipe_resource_reference(&b->bytes, NULL);
   b->bytes_len = 0;
}

OP2MTL_API void
op2mtl_set_buffer(op2mtl_device *dev, unsigned stage, unsigned index, op2mtl_buffer *buf, size_t offset)
{
   if (stage >= STAGES || index >= OP2MTL_MAX_BUFFERS)
      return;
   pthread_mutex_lock(&dev->lock);
   struct binding *b = &dev->bind[stage][index];
   binding_clear_bytes(b);
   b->buf = buf;
   b->offset = offset;
   pthread_mutex_unlock(&dev->lock);
}

OP2MTL_API void
op2mtl_set_buffer_offset(op2mtl_device *dev, unsigned stage, unsigned index, size_t offset)
{
   if (stage >= STAGES || index >= OP2MTL_MAX_BUFFERS)
      return;
   pthread_mutex_lock(&dev->lock);
   dev->bind[stage][index].offset = offset;
   pthread_mutex_unlock(&dev->lock);
}

OP2MTL_API void
op2mtl_set_bytes(op2mtl_device *dev, unsigned stage, unsigned index, const void *bytes, size_t len)
{
   if (stage >= STAGES || index >= OP2MTL_MAX_BUFFERS || !len)
      return;
   pthread_mutex_lock(&dev->lock);
   struct binding *b = &dev->bind[stage][index];
   binding_clear_bytes(b);
   b->buf = NULL;
   b->offset = 0;
   b->bytes = pipe_buffer_create(dev->screen, PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER,
                                 PIPE_USAGE_STREAM, (unsigned)ALIGN(len, 16));
   if (b->bytes) {
      pipe_buffer_write(dev->pipe, b->bytes, 0, (unsigned)len, bytes);
      b->bytes_len = len;
   }
   pthread_mutex_unlock(&dev->lock);
}

OP2MTL_API void
op2mtl_set_texture(op2mtl_device *dev, unsigned stage, unsigned index, op2mtl_texture *t)
{
   if (stage >= STAGES || index >= MAX_TEX)
      return;
   pthread_mutex_lock(&dev->lock);
   dev->tex[stage][index] = t;
   pthread_mutex_unlock(&dev->lock);
}

OP2MTL_API void
op2mtl_set_sampler(op2mtl_device *dev, unsigned stage, unsigned index, op2mtl_sampler *s)
{
   if (stage >= STAGES || index >= MAX_TEX)
      return;
   pthread_mutex_lock(&dev->lock);
   dev->smp[stage][index] = s;
   pthread_mutex_unlock(&dev->lock);
}

/* the resource behind a binding, and its usable size from the offset */
static struct pipe_resource *
binding_res(struct binding *b, size_t *size)
{
   if (b->bytes) {
      *size = b->bytes_len;
      return b->bytes;
   }
   if (b->buf) {
      *size = b->buf->size > b->offset ? b->buf->size - b->offset : 0;
      return b->buf->res;
   }
   *size = 0;
   return NULL;
}

/* everything the current pipeline reads, bound; fails (by name) on a binding it needs and does not have */
static bool
bind_for_draw(op2mtl_device *dev)
{
   struct pipe_context *pipe = dev->pipe;
   op2mtl_pipeline *p = dev->pipeline;
   if (!dev->in_pass)
      return fail("draw: outside a render pass"), false;
   if (!p)
      return fail("draw: no render pipeline state"), false;

   pipe->bind_vs_state(pipe, p->vs);
   pipe->bind_fs_state(pipe, p->fs);
   pipe->bind_blend_state(pipe, p->blend);
   pipe->bind_depth_stencil_alpha_state(pipe, dev->dsa);
   pipe->bind_rasterizer_state(pipe, dev->rast[dev->scissor_on]);
   pipe->set_viewport_states(pipe, 0, 1, &dev->vp);
   if (dev->scissor_on)
      pipe->set_scissor_states(pipe, 0, 1, &dev->scissor);
   pipe->bind_vertex_elements_state(pipe, p->velems);

   struct pipe_vertex_buffer vb[OP2MTL_MAX_BUFFERS];
   memset(vb, 0, sizeof vb);
   for (unsigned s = 0; s < p->num_vb; s++) {
      if (p->vb_metal[s] == VB_ZERO) {
         pipe_resource_reference(&vb[s].buffer.resource, dev->zero_vb);
         continue;
      }
      struct binding *b = &dev->bind[0][p->vb_metal[s]];
      size_t size;
      struct pipe_resource *res = binding_res(b, &size);
      if (!res)
         return fail("draw: vertex buffer %d (stage-in) is not bound", p->vb_metal[s]), false;
      pipe_resource_reference(&vb[s].buffer.resource, res);   /* set_vertex_buffers takes the reference */
      vb[s].buffer_offset = (unsigned)b->offset;
   }
   pipe->set_vertex_buffers(pipe, p->num_vb, vb);

   for (unsigned st = 0; st < STAGES; st++) {
      enum pipe_shader_type ps = st == 0 ? PIPE_SHADER_VERTEX : PIPE_SHADER_FRAGMENT;
      u_foreach_bit(i, p->ubo[st]) {
         struct binding *b = &dev->bind[st][i];
         size_t size;
         struct pipe_resource *res = binding_res(b, &size);
         if (!res && dev->zero_ubo) {
            /* Metal leaves an unbound buffer undefined and CA relies on it for reads its specialisation never
             * uses (SpringBoard: vertex buffer 3): zeros, said once per stage and index */
            if (!(dev->zero_ubo_said[st] & (1u << i))) {
               dev->zero_ubo_said[st] |= 1u << i;
               say("draw: %s constant buffer %u is not bound: it reads zeros", st ? "fragment" : "vertex", i);
            }
            res = dev->zero_ubo;
            size = 4096;
         }
         if (!res)
            return fail("draw: %s constant buffer %u is not bound", st ? "fragment" : "vertex", i), false;
         pipe->set_constant_buffer(pipe, ps, i, false, &(struct pipe_constant_buffer){
            .buffer = res, .buffer_offset = (unsigned)(b->bytes || res == dev->zero_ubo ? 0 : b->offset),
            .buffer_size = (unsigned)MIN2(size, 65536) });
      }
      if (p->ssbo[st]) {
         struct pipe_shader_buffer sb[32];
         memset(sb, 0, sizeof sb);
         unsigned n = util_last_bit(p->ssbo[st]);
         u_foreach_bit(i, p->ssbo[st]) {
            struct binding *b = &dev->bind[st][i];
            size_t size;
            sb[i].buffer = binding_res(b, &size);
            sb[i].buffer_offset = (unsigned)(b->bytes ? 0 : b->offset);
            sb[i].buffer_size = (unsigned)size;
         }
         pipe->set_shader_buffers(pipe, ps, 0, n, sb, 0);
      }
      unsigned ntex = util_last_bit(p->textures[st]);
      if (ntex) {
         struct pipe_sampler_view *views[MAX_TEX] = { 0 };
         for (unsigned i = 0; i < ntex && i < MAX_TEX; i++) {
            if (dev->tex[st][i]) {
               views[i] = dev->tex[st][i]->view;
            } else if (p->textures[st] & (1u << i)) {
               /* sampled but unbound: an A4xx texture slot with no state hangs the GPU (round g5b-3,
                * SpringBoard's first draws); Metal reads zeros, so does this */
               views[i] = dev->zero_view;
               if (!(dev->zero_view_said[st] & (1u << i))) {
                  dev->zero_view_said[st] |= 1u << i;
                  say("draw: %s texture %u is not bound: it reads zeros", st ? "fragment" : "vertex", i);
               }
            }
         }
         pipe->set_sampler_views(pipe, ps, 0, MIN2(ntex, MAX_TEX), 0, false, views);
      }
      unsigned nsmp = MAX2(util_last_bit(p->samplers[st]), ntex);
      if (nsmp) {
         void *smp[MAX_TEX] = { 0 };
         for (unsigned i = 0; i < nsmp && i < MAX_TEX; i++)
            smp[i] = p->inline_sampler[st][i] ? p->inline_sampler[st][i] :
                     dev->smp[st][i] ? dev->smp[st][i]->cso : dev->default_sampler;
         pipe->bind_sampler_states(pipe, ps, 0, MIN2(nsmp, MAX_TEX), smp);
      }
   }
   return true;
}

OP2MTL_API bool
op2mtl_draw(op2mtl_device *dev, uint32_t prim, uint32_t start, uint32_t count, uint32_t instances)
{
   enum mesa_prim mode;
   if (!primitive(prim, &mode))
      return fail("draw: MTLPrimitiveType %u", prim), false;
   pthread_mutex_lock(&dev->lock);
   bool ok = bind_for_draw(dev);
   if (ok) {
      struct pipe_draw_info info = { .mode = mode, .instance_count = MAX2(instances, 1) };
      struct pipe_draw_start_count_bias sc = { .start = start, .count = count };
      dev->pipe->draw_vbo(dev->pipe, &info, 0, NULL, &sc, 1);
   }
   pthread_mutex_unlock(&dev->lock);
   return ok;
}

OP2MTL_API bool
op2mtl_draw_indexed(op2mtl_device *dev, uint32_t prim, uint32_t count, uint32_t index_type,
                    op2mtl_buffer *ib, size_t offset, uint32_t instances)
{
   enum mesa_prim mode;
   if (!primitive(prim, &mode))
      return fail("draw: MTLPrimitiveType %u", prim), false;
   if (!ib)
      return fail("draw: no index buffer"), false;
   unsigned isize = index_type ? 4 : 2;
   pthread_mutex_lock(&dev->lock);
   bool ok = bind_for_draw(dev);
   if (ok) {
      struct pipe_draw_info info = { .mode = mode, .index_size = (uint8_t)isize,
                                     .instance_count = MAX2(instances, 1) };
      info.index.resource = ib->res;
      struct pipe_draw_start_count_bias sc = { .start = (unsigned)(offset / isize), .count = count };
      dev->pipe->draw_vbo(dev->pipe, &info, 0, NULL, &sc, 1);
   }
   pthread_mutex_unlock(&dev->lock);
   return ok;
}

OP2MTL_API void
op2mtl_pass_end(op2mtl_device *dev)
{
   pthread_mutex_lock(&dev->lock);
   dev->in_pass = false;
   pthread_mutex_unlock(&dev->lock);
}

/* -- submission --------------------------------------------------------------------------------------- */

OP2MTL_API uint64_t
op2mtl_commit(op2mtl_device *dev)
{
   pthread_mutex_lock(&dev->lock);
   struct pipe_fence_handle *f = NULL;
   dev->pipe->flush(dev->pipe, &f, 0);
   uint64_t id = 0;
   if (f) {
      id = ++dev->fence_seq;
      unsigned slot = (unsigned)(id % MAX_FENCES);
      dev->screen->fence_reference(dev->screen, &dev->fences[slot], NULL);
      dev->fences[slot] = f;
      dev->fence_ids[slot] = id;
   }
   pthread_mutex_unlock(&dev->lock);
   return id;
}

OP2MTL_API bool
op2mtl_wait(op2mtl_device *dev, uint64_t id, uint64_t timeout_ns)
{
   if (!id)
      return true;
   pthread_mutex_lock(&dev->lock);
   unsigned slot = (unsigned)(id % MAX_FENCES);
   struct pipe_fence_handle *f = NULL;
   if (dev->fence_ids[slot] == id)
      dev->screen->fence_reference(dev->screen, &f, dev->fences[slot]);
   pthread_mutex_unlock(&dev->lock);
   if (!f)
      return true;   /* older than the ring: long done */
   bool done = dev->screen->fence_finish(dev->screen, NULL, f, timeout_ns);
   dev->screen->fence_reference(dev->screen, &f, NULL);
   return done;
}
