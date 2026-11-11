/*
 * op2mtl_test: drive op2mtl the way CoreAnimation will, on the phone, and say what came back.
 *
 *   op2mtl_test [metallib] [WxH]
 *
 * One render pass into a BGRA8 target cleared to blue, CA's real pipeline (ca_uber_vertex_lp0 +
 * ca_uber_fragment_lp0_cp0_fo0 with the function constants backboardd asked for, logs/ca-fc-captured.txt),
 * fed the way Metal feeds it: a vertex descriptor over ONE interleaved vertex buffer, an index buffer, an
 * indexed draw of a quad covering the middle of the target. What is checked, and why it is evidence:
 *   - the draw completes (fence) and the GPU does not fault (the kernel logs SMMU faults with gpu_xl=1)
 *   - the quad lands where its vertices say: pixels inside it differ from the clear colour, pixels outside do
 *     not (a vertex function that ignored its stage-in data, or a wrong viewport, moves or loses the quad)
 *   - the colour inside is reported for every corner, so a varying that does not interpolate shows
 * The vertex function's transform is read from its constant buffer 1 (the first 64 bytes, a 4x4 matrix,
 * logs/ca-sweep-430-real: load_ubo(1, 0x0..0x30)); it is the identity here, so positions are clip space.
 * What the fragment function does with its constants is not known yet: they get air_run's byte pattern.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "op2mtl.h"

static void *
slurp(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *p = malloc((size_t)n);
   *size = fread(p, 1, (size_t)n, f);
   fclose(f);
   return p;
}

/* backboardd's constants for both functions (logs/ca-fc-captured.txt) */
static const struct op2mtl_constant kFC[] = {
   { 0, 0x81b00 }, { 1, 0x40 }, { 2, 0x810 }, { 3, 0x9c00 }, { 4, 0x9c00 }, { 5, 0x9c00 },
   { 6, 0 }, { 7, 0 }, { 8, 0 }, { 9, 0 }, { 10, 0 }, { 11, 0 },
};

struct vertex {          /* one interleaved vertex: the stage-in attributes 0..6 */
   float position[4];    /* attribute 0, float4 */
   float tc0[2], tc1[2], tc2[2];   /* attributes 1..3, float2 */
   float color0[4];      /* attribute 4, float4 */
   float color1[4];      /* attribute 5, float4 */
   float size;           /* attribute 6, float */
};

int
main(int argc, char **argv)
{
   const char *path = argc > 1 ? argv[1] : "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
   unsigned W = 1080, H = 1920;
   if (argc > 2)
      sscanf(argv[2], "%ux%u", &W, &H);
   setvbuf(stdout, NULL, _IOLBF, 0);
   printf("op2mtl_test: API %u, %s, %ux%u\n", op2mtl_version(), path, W, H);

   op2mtl_device *dev = op2mtl_device_open();
   if (!dev) {
      printf("op2mtl_test: FAIL device: %s\n", op2mtl_last_error());
      return 1;
   }
   printf("op2mtl_test: device %s\n", op2mtl_device_name(dev));
   size_t size = 0;
   void *data = slurp(path, &size);
   op2mtl_library *lib = data ? op2mtl_library_new(dev, data, size) : NULL;
   if (!lib) {
      printf("op2mtl_test: FAIL library: %s\n", data ? op2mtl_last_error() : "unreadable");
      return 1;
   }

   struct op2mtl_pipeline_desc d;
   memset(&d, 0, sizeof d);
   d.vertex = (struct op2mtl_function){ .library = lib, .name = "ca_uber_vertex_lp0",
                                        .num_constants = sizeof kFC / sizeof kFC[0] };
   memcpy(d.vertex.constants, kFC, sizeof kFC);
   d.fragment = d.vertex;
   d.fragment.name = "ca_uber_fragment_lp0_cp0_fo0";
   static const struct { unsigned fmt, off; } attr[7] = {
      { 31, 0 }, { 29, 16 }, { 29, 24 }, { 29, 32 }, { 31, 40 }, { 31, 56 }, { 28, 72 },
   };
   for (unsigned i = 0; i < 7; i++)
      d.attributes[i] = (struct op2mtl_vertex_attribute){ .format = attr[i].fmt, .offset = attr[i].off,
                                                          .buffer_index = 0 };
   d.layouts[0] = (struct op2mtl_vertex_layout){ .stride = sizeof(struct vertex), .step_function = 1,
                                                 .step_rate = 1 };
   d.color[0] = (struct op2mtl_color_attachment){ .pixel_format = 80 /* BGRA8Unorm */, .write_mask = 15 };
   d.sample_count = 1;
   op2mtl_pipeline *p = op2mtl_pipeline_new(dev, &d);
   if (!p) {
      printf("op2mtl_test: FAIL pipeline: %s\n", op2mtl_last_error());
      return 1;
   }
   printf("op2mtl_test: pipeline made (CA's vertex + fragment functions, %zu-byte vertices)\n",
          sizeof(struct vertex));

   /* the quad: clip x,y in [-0.5, 0.5] -> the middle half of the target; a colour per corner */
   static const float corner[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } };
   op2mtl_buffer *vb = op2mtl_buffer_new(dev, 4 * sizeof(struct vertex));
   op2mtl_buffer *ib = op2mtl_buffer_new(dev, 6 * sizeof(uint16_t));
   op2mtl_buffer *ubo = op2mtl_buffer_new(dev, 256);
   if (!vb || !ib || !ubo) {
      printf("op2mtl_test: FAIL buffers: %s\n", op2mtl_last_error());
      return 1;
   }
   struct vertex *v = op2mtl_buffer_contents(vb);
   memset(v, 0, 4 * sizeof *v);
   for (unsigned i = 0; i < 4; i++) {
      v[i].position[0] = corner[i][0];
      v[i].position[1] = corner[i][1];
      v[i].position[3] = 1.0f;
      v[i].color0[0] = 0.25f;
      v[i].color0[1] = 0.5f;
      v[i].color0[2] = 0.75f;
      v[i].color0[3] = 1.0f;
      v[i].color1[3] = 1.0f;
      v[i].size = 1.0f;
   }
   static const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
   memcpy(op2mtl_buffer_contents(ib), idx, sizeof idx);
   float *m = op2mtl_buffer_contents(ubo);
   memset(m, 0, 256);
   m[0] = m[5] = m[10] = m[15] = 1.0f;   /* identity */
   uint8_t pattern[256];
   for (unsigned i = 0; i < sizeof pattern; i++)
      pattern[i] = (uint8_t)(i * 7 + 13);

   op2mtl_texture *rt = op2mtl_texture_new(dev, 80, W, H, 1, 1 | 4);
   if (!rt) {
      printf("op2mtl_test: FAIL target: %s\n", op2mtl_last_error());
      return 1;
   }
   struct op2mtl_pass_attachment att[OP2MTL_MAX_COLOR];
   memset(att, 0, sizeof att);
   att[0] = (struct op2mtl_pass_attachment){ .texture = rt, .load_action = 2, .clear = { 0, 0, 1, 1 } };
   if (!op2mtl_pass_begin(dev, att)) {
      printf("op2mtl_test: FAIL pass: %s\n", op2mtl_last_error());
      return 1;
   }
   op2mtl_set_pipeline(dev, p);
   op2mtl_set_viewport(dev, 0, 0, W, H, 0, 1);
   op2mtl_set_buffer(dev, 0, 0, vb, 0);   /* stage-in */
   op2mtl_set_buffer(dev, 0, 1, ubo, 0);  /* the vertex function's transform */
   /* the vertex function also reads constant buffers 2, 3, 5 (on the phone: vs ubo 0x2e): zeros, so nothing
    * there can move the quad; what CA keeps in them is for the shim's trace to say */
   static const uint8_t zeros[256];
   for (unsigned i = 2; i < 8; i++)
      op2mtl_set_bytes(dev, 0, i, zeros, sizeof zeros);
   for (unsigned i = 0; i < 8; i++)        /* every fragment constant buffer it might read */
      op2mtl_set_bytes(dev, 1, i, pattern, sizeof pattern);
   bool drew = op2mtl_draw_indexed(dev, 3 /* triangles */, 6, 0 /* uint16 */, ib, 0, 1);
   op2mtl_pass_end(dev);
   if (!drew) {
      printf("op2mtl_test: FAIL draw: %s\n", op2mtl_last_error());
      return 1;
   }
   uint64_t fence = op2mtl_commit(dev);
   bool done = op2mtl_wait(dev, fence, 2000000000ull);
   printf("op2mtl_test: fence %llu %s\n", (unsigned long long)fence, done ? "completed" : "DID NOT COMPLETE");
   if (!done)
      return 1;

   uint8_t *px = malloc((size_t)W * H * 4);
   if (!op2mtl_texture_read(rt, 0, 0, 0, W, H, px, (size_t)W * 4)) {
      printf("op2mtl_test: FAIL readback: %s\n", op2mtl_last_error());
      return 1;
   }
   /* inside = the middle half; outside = the rest */
   unsigned in_changed = 0, in_total = 0, out_changed = 0, out_total = 0;
   for (unsigned y = 0; y < H; y++)
      for (unsigned x = 0; x < W; x++) {
         const uint8_t *q = px + ((size_t)y * W + x) * 4;
         bool clear = q[0] == 255 && q[1] == 0 && q[2] == 0 && q[3] == 255;   /* B G R A of (0,0,1,1) */
         bool inside = x >= W / 4 + 2 && x < 3 * W / 4 - 2 && y >= H / 4 + 2 && y < 3 * H / 4 - 2;
         bool outside = x < W / 4 - 2 || x >= 3 * W / 4 + 2 || y < H / 4 - 2 || y >= 3 * H / 4 + 2;
         if (inside) { in_total++; in_changed += !clear; }
         if (outside) { out_total++; out_changed += !clear; }
      }
   printf("op2mtl_test: inside the quad %u of %u pixels drawn; outside %u of %u (should be 0)\n",
          in_changed, in_total, out_changed, out_total);
   static const unsigned probe[5][2] = { { 0, 0 }, { 1, 1 }, { 3, 1 }, { 1, 3 }, { 2, 2 } };   /* quarters */
   for (unsigned i = 0; i < 5; i++) {
      unsigned x = probe[i][0] * W / 4 + (probe[i][0] == 3 ? -4 : 4), y = probe[i][1] * H / 4 + (probe[i][1] == 3 ? -4 : 4);
      if (probe[i][0] == 2) { x = W / 2; y = H / 2; }
      const uint8_t *q = px + ((size_t)y * W + x) * 4;
      printf("op2mtl_test: pixel (%u, %u) B G R A = %u %u %u %u\n", x, y, q[0], q[1], q[2], q[3]);
   }
   bool pass = in_changed == in_total && out_changed == 0;
   printf("op2mtl_test: %s\n", pass ? "PASS: CA's pipeline drew the quad where its vertices put it"
                                   : "FAIL: the quad is not where its vertices put it");
   return pass ? 0 : 1;
}
