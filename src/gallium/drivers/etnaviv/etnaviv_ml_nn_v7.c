/*
 * Copyright (c) 2023-2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * SPDX-License-Identifier: MIT
 */

#include "etnaviv_context.h"
#include "etnaviv_debug.h"
#include "etnaviv_ml_nn.h"
#include "etnaviv_screen.h"

static void *
map_resource(struct pipe_resource *resource)
{
   return etna_bo_map(etna_resource(resource)->bo);
}

static uint32_t
calculate_bias_correction(uint8_t *weights, const struct etna_operation *operation)
{
   int32_t correction = 0;

   for (unsigned i = 0; i < operation->weight_width * operation->weight_height * operation->input_channels; i++) {
      correction += (weights[i] - operation->weight_zero_point) * operation->input_zero_point;
   }

   return correction;
}


static void
append_bits(uint32_t value, size_t size, unsigned *bits_in_buffer, uint64_t *buffer, uint32_t **dest, bool do_write)
{
   *buffer |= (uint64_t)value << *bits_in_buffer;
   *bits_in_buffer += size;
   if (*bits_in_buffer >= 32) {
      if (do_write)
         **dest = *buffer & 0xffffffff;
      *dest += 1;
      *buffer >>= 32;
      *bits_in_buffer -= 32;
   }
}

struct wb_stream {
   unsigned zero_point;
   unsigned zrl_bits;
   unsigned *bits_in_buffer;
   uint64_t *buffer;
   uint32_t **map;
   bool do_write;

   unsigned accum_zeroes;
};

static void
wb_stream_flush_zeroes(struct wb_stream *wb_stream)
{
   if (wb_stream->accum_zeroes == 0)
      return;

   append_bits(wb_stream->accum_zeroes - 1, wb_stream->zrl_bits, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
   wb_stream->accum_zeroes = 0;
   append_bits(wb_stream->zero_point, 8, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
}

static void
wb_stream_write(struct wb_stream *wb_stream, unsigned value)
{
   unsigned max_zeroes = (1 << wb_stream->zrl_bits) - 1;

   if (wb_stream->zrl_bits == 0) {
      append_bits(value, 8, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
      return;
   }

   if (wb_stream->accum_zeroes == max_zeroes) {
      append_bits(max_zeroes, wb_stream->zrl_bits, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
      wb_stream->accum_zeroes = 0;
      append_bits(value, 8, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
      return;
   }

   if (value == wb_stream->zero_point) {
      wb_stream->accum_zeroes++;
      return;
   }

   append_bits(wb_stream->accum_zeroes, wb_stream->zrl_bits, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
   wb_stream->accum_zeroes = 0;
   append_bits(value, 8, wb_stream->bits_in_buffer, wb_stream->buffer, wb_stream->map, wb_stream->do_write);
}

static unsigned
write_core_6(struct etna_ml_subgraph *subgraph, uint32_t *map, unsigned core, const struct etna_operation *operation, unsigned zrl_bits)
{
   struct pipe_context *pctx = subgraph->base.context;
   unsigned nn_core_count = etna_ml_get_core_info(etna_context(pctx))->nn_core_count;
   unsigned input_channels = operation->addition ? 1 : operation->input_channels;
   unsigned output_channels = operation->addition ? 1 : operation->output_channels;
   unsigned cores_used = MIN2(output_channels, nn_core_count);
   unsigned kernels_per_core = DIV_ROUND_UP(output_channels, cores_used);
   uint8_t *input = map_resource(operation->weight_tensor);
   uint32_t *biases = map_resource(operation->bias_tensor);
   unsigned out_values_per_channel = operation->output_width * operation->output_height;
   unsigned stride = MIN2(input_channels, 6);
   unsigned superblocks = etna_ml_calculate_tiling(etna_context(pctx), operation, NULL, NULL);
   uint8_t *weights_maps[DIV_ROUND_UP(kernels_per_core, superblocks)];
   uint32_t *initial_ptr = map;
   bool do_write = initial_ptr != NULL;
   uint64_t buffer = 0;
   unsigned bits_in_buffer = 0;
   struct wb_stream wb_stream = {
      .zero_point = operation->weight_zero_point,
      .zrl_bits = zrl_bits,
      .bits_in_buffer = &bits_in_buffer,
      .buffer = &buffer,
      .map = &map,
      .do_write = do_write,
   };

   ML_DBG("%s core %d zrl_bits %d\n", __func__, core, zrl_bits);

   append_bits(zrl_bits, 8, &bits_in_buffer, &buffer, &map, do_write);
   append_bits(kernels_per_core, 16, &bits_in_buffer, &buffer, &map, do_write);

   for (unsigned superblock = 0; superblock < superblocks; superblock++) {

      unsigned kernels_in_superblock = DIV_ROUND_UP(kernels_per_core, superblocks);
      if (superblock == superblocks - 1)
         kernels_in_superblock = kernels_per_core - kernels_in_superblock * (superblocks - 1);

      for (unsigned kernel = 0; kernel < kernels_in_superblock; kernel++) {
         unsigned out_channel = core * kernels_in_superblock + kernel + superblock * DIV_ROUND_UP(kernels_per_core, superblocks) * cores_used;
         weights_maps[kernel] = input + out_channel * operation->weight_width * operation->weight_height * input_channels;
      }

      for (unsigned block = 0; block < DIV_ROUND_UP(input_channels, stride); block++) {
         for (unsigned kernel = 0; kernel < kernels_in_superblock; kernel++) {
            unsigned out_channel = core * kernels_in_superblock + kernel + superblock * DIV_ROUND_UP(kernels_per_core, superblocks) * cores_used;

            if (block == 0) {
               wb_stream_write(&wb_stream, weights_maps[kernel][0]);

               uint32_t corr = calculate_bias_correction(weights_maps[kernel], operation);
               wb_stream_flush_zeroes(&wb_stream);
               append_bits(biases[out_channel] - corr, 32, &bits_in_buffer, &buffer, &map, do_write);

               for (int i = 1; i < stride; i++) {
                  wb_stream_write(&wb_stream, weights_maps[kernel][i]);
               }
            } else {
               for (int i = 0; i < stride; i++) {
                  if (i + block * stride < input_channels)
                     wb_stream_write(&wb_stream, weights_maps[kernel][i + block * stride]);
               }
            }
            if (block == DIV_ROUND_UP(input_channels, stride) - 1) {
               wb_stream_flush_zeroes(&wb_stream);
               append_bits(out_values_per_channel * out_channel, 32, &bits_in_buffer, &buffer, &map, do_write);
            }
         }
      }
   }

   wb_stream_flush_zeroes(&wb_stream);

   if (bits_in_buffer > 0)
      append_bits(0, 32 - bits_in_buffer, &bits_in_buffer, &buffer, &map, do_write);

   return (uint8_t *)map - (uint8_t *)initial_ptr - 1;
}

static unsigned
write_core_interleaved(struct etna_ml_subgraph *subgraph, uint32_t *map, unsigned core, const struct etna_operation *operation, unsigned zrl_bits)
{
   struct pipe_context *pctx = subgraph->base.context;
   unsigned nn_core_count = etna_ml_get_core_info(etna_context(pctx))->nn_core_count;
   unsigned input_channels = operation->addition ? 1 : operation->input_channels;
   unsigned output_channels = operation->addition ? 1 : operation->output_channels;
   unsigned cores_used = MIN2(output_channels, nn_core_count);
   unsigned kernels_per_core = DIV_ROUND_UP(output_channels, cores_used);
   uint8_t *input = map_resource(operation->weight_tensor);
   uint32_t *biases = map_resource(operation->bias_tensor);
   unsigned out_values_per_channel = operation->output_width * operation->output_height;
   unsigned superblocks = etna_ml_calculate_tiling(etna_context(pctx), operation, NULL, NULL);
   uint8_t (*weights_map)[input_channels][operation->weight_width][operation->weight_height] = (void *)input;
   uint32_t *initial_ptr = map;
   bool do_write = initial_ptr != NULL;
   uint64_t buffer = 0;
   unsigned bits_in_buffer = 0;
   struct wb_stream wb_stream = {
      .zero_point = operation->weight_zero_point,
      .zrl_bits = zrl_bits,
      .bits_in_buffer = &bits_in_buffer,
      .buffer = &buffer,
      .map = &map,
      .do_write = do_write,
   };

   ML_DBG("%s core %d zrl_bits %d map %p\n", __func__, core, zrl_bits, map);

   append_bits(zrl_bits, 8, &bits_in_buffer, &buffer, &map, do_write);
   append_bits(kernels_per_core, 16, &bits_in_buffer, &buffer, &map, do_write);

   for (unsigned superblock = 0; superblock < superblocks; superblock++) {

      unsigned kernels_in_superblock = DIV_ROUND_UP(kernels_per_core, superblocks);
      if (superblock == superblocks - 1)
         kernels_in_superblock = kernels_per_core - kernels_in_superblock * (superblocks - 1);

      for (unsigned z = 0; z < input_channels; z++) {
         for (unsigned kernel = 0; kernel < kernels_in_superblock; kernel++) {
            unsigned out_channel = core * kernels_in_superblock + kernel + superblock * DIV_ROUND_UP(kernels_per_core, superblocks) * cores_used;

            for (unsigned block = 0; block < DIV_ROUND_UP(operation->weight_width, 2); block++) {
               unsigned stride = operation->weight_height;
               if (operation->weight_height > 3)
                  stride = 3;
               for (unsigned x = block * 2; x < (block + 1) * 2; x++ ) {
                  if (x >= operation->weight_width)
                     break;
                  for (unsigned y = 0; y < stride; y++) {
                     wb_stream_write(&wb_stream, weights_map[out_channel][z][x][y]);
                     if (x == 0 && y == 0 && z == 0) {
                        uint32_t corr = calculate_bias_correction((uint8_t *)weights_map[out_channel], operation);
                        wb_stream_flush_zeroes(&wb_stream);
                        append_bits(biases[out_channel] - corr, 32, &bits_in_buffer, &buffer, &map, do_write);
                     }
                  }
               }
               if (operation->weight_height > 3) {
                  for (unsigned x = block * 2; x < (block + 1) * 2; x++ ) {
                     if (x >= operation->weight_width)
                        break;
                     for (unsigned y = stride; y < operation->weight_width; y++) {
                        wb_stream_write(&wb_stream, weights_map[out_channel][z][x][y]);
                     }
                  }
               }
            }

            if (z == input_channels - 1) {
               wb_stream_flush_zeroes(&wb_stream);
               append_bits(out_values_per_channel * out_channel, 32, &bits_in_buffer, &buffer, &map, do_write);
            }
         }
         if (superblock == superblocks - 1)
            wb_stream_flush_zeroes(&wb_stream);
      }
   }

   wb_stream_flush_zeroes(&wb_stream);

   if (bits_in_buffer > 0)
      append_bits(0, 32 - bits_in_buffer, &bits_in_buffer, &buffer, &map, do_write);

   return (uint8_t *)map - (uint8_t *)initial_ptr;
}

static unsigned
write_core_sequential(struct etna_ml_subgraph *subgraph, uint32_t *map, unsigned core, const struct etna_operation *operation, unsigned zrl_bits)
{
   struct pipe_context *pctx = subgraph->base.context;
   unsigned nn_core_count = etna_ml_get_core_info(etna_context(pctx))->nn_core_count;
   unsigned output_channels = operation->addition ? 1 : operation->output_channels;
   unsigned cores_used = MIN2(output_channels, nn_core_count);
   unsigned kernels_per_core = DIV_ROUND_UP(output_channels, cores_used);
   uint8_t *input = map_resource(operation->weight_tensor);
   uint32_t *biases = map_resource(operation->bias_tensor);
   unsigned out_values_per_channel = operation->output_width * operation->output_height;
   unsigned superblocks = etna_ml_calculate_tiling(etna_context(pctx), operation, NULL, NULL);
   uint32_t *initial_ptr = map;
   bool do_write = initial_ptr != NULL;
   uint64_t buffer = 0;
   unsigned bits_in_buffer = 0;
   struct wb_stream wb_stream = {
      .zero_point = operation->weight_zero_point,
      .zrl_bits = zrl_bits,
      .bits_in_buffer = &bits_in_buffer,
      .buffer = &buffer,
      .map = &map,
      .do_write = do_write,
   };

   ML_DBG("%s core %d zrl_bits %d superblocks %d\n", __func__, core, zrl_bits, superblocks);

   append_bits(zrl_bits, 8, &bits_in_buffer, &buffer, &map, do_write);
   append_bits(kernels_per_core, 16, &bits_in_buffer, &buffer, &map, do_write);

   for (unsigned superblock = 0; superblock < superblocks; superblock++) {

      unsigned kernels_in_superblock = DIV_ROUND_UP(kernels_per_core, superblocks);
      if (superblock == superblocks - 1)
         kernels_in_superblock = kernels_per_core - kernels_in_superblock * (superblocks - 1);

      for (unsigned kernel = 0; kernel < kernels_in_superblock; kernel++) {
         unsigned out_channel = core * kernels_in_superblock + kernel + superblock * DIV_ROUND_UP(kernels_per_core, superblocks) * cores_used;

         uint8_t (*weights_map)[operation->weight_height] = (void*) input + out_channel * operation->weight_width * operation->weight_height;

         for (unsigned block = 0; block < DIV_ROUND_UP(operation->weight_width, 2); block++) {
            unsigned stride = operation->weight_height;
            if ((operation->depthwise || operation->input_width > 64) && \
               operation->weight_height > 3)
               stride = 3;
            for (unsigned x = block * 2; x < (block + 1) * 2; x++ ) {
               if (x >= operation->weight_width)
                  break;
               for (unsigned y = 0; y < stride; y++) {

                  wb_stream_write(&wb_stream, weights_map[x][y]);
                  if (x == 0 && y == 0) {
                     uint32_t corr = calculate_bias_correction((uint8_t *)weights_map, operation);
                     wb_stream_flush_zeroes(&wb_stream);
                     append_bits(biases[out_channel] - corr, 32, &bits_in_buffer, &buffer, &map, do_write);
                  }
               }
            }
            if ((operation->depthwise || operation->input_width > 64) && \
               operation->weight_height > 3) {
               for (unsigned x = block * 2; x < (block + 1) * 2; x++ ) {
                  if (x >= operation->weight_width)
                     break;
                  for (unsigned y = stride; y < operation->weight_width; y++) {
                     wb_stream_write(&wb_stream, weights_map[x][y]);
                  }
               }
            }
         }
         wb_stream_flush_zeroes(&wb_stream);
         if (operation->addition)
            append_bits(operation->addition_offset, 32, &bits_in_buffer, &buffer, &map, do_write);
         else
            append_bits(out_values_per_channel * out_channel, 32, &bits_in_buffer, &buffer, &map, do_write);
      }
   }

   wb_stream_flush_zeroes(&wb_stream);

   if (bits_in_buffer > 0)
      append_bits(0, 32 - bits_in_buffer, &bits_in_buffer, &buffer, &map, do_write);

   return (uint8_t *)map - (uint8_t *)initial_ptr - 1;
}

static unsigned
calculate_weight_bo_size(struct etna_ml_subgraph *subgraph, const struct etna_operation *operation)
{
   struct pipe_context *context = subgraph->base.context;
   struct etna_context *ctx = etna_context(context);
   unsigned nn_core_count = etna_ml_get_core_info(ctx)->nn_core_count;
   unsigned header_size = ALIGN(nn_core_count * 4, 64);
   unsigned input_channels = operation->addition ? 1 : operation->input_channels;
   unsigned output_channels = operation->addition ? 1 : operation->output_channels;
   unsigned cores_used = MIN2(output_channels, nn_core_count);
   unsigned kernels_per_core = DIV_ROUND_UP(output_channels, cores_used);
   unsigned weights_size;
   unsigned core_size;
   unsigned core_size_aligned;
   unsigned compressed_size_aligned;

   weights_size = operation->weight_width * operation->weight_height * input_channels;
   core_size = 1 + 2 + (weights_size + 4 + 4) * kernels_per_core;
   core_size_aligned = ALIGN(core_size, 64);
   compressed_size_aligned = header_size + core_size_aligned * cores_used;

   return compressed_size_aligned;
}

static unsigned
calculate_zrl_bits(struct etna_ml_subgraph *subgraph, const struct etna_operation *operation)
{
   struct pipe_context *context = subgraph->base.context;
   struct etna_context *ctx = etna_context(context);
   unsigned nn_core_count = etna_ml_get_core_info(ctx)->nn_core_count;
   unsigned max_zrl_bits = etna_ml_get_core_info(ctx)->nn_zrl_bits;
   unsigned header_size = ALIGN(nn_core_count * 4, 64);
   unsigned input_channels = operation->addition ? 1 : operation->input_channels;
   unsigned output_channels = operation->addition ? 1 : operation->output_channels;
   unsigned cores_used = MIN2(output_channels, nn_core_count);
   unsigned best_compressed_size;
   unsigned best_zrl_bits;

   /* These are very unlikely to have enough zeroes for compression to be useful. */
   if (operation->addition ||
       operation->pointwise) {

      return 0;
   }

   /* This calculation can be really slow. Start from max_zrl_bits as big
    * buffers will benefit the most from high zero compression.
    */
   best_compressed_size = UINT_MAX;
   best_zrl_bits = 0;
   for (int zrl_bits = max_zrl_bits; zrl_bits >= 0; zrl_bits--) {

      unsigned compressed_size = header_size;
      for (unsigned core = 0; core < cores_used; core++) {

         unsigned actual_size;
         if (operation->pointwise && output_channels > 8)
            actual_size = write_core_6(subgraph, NULL, core, operation, zrl_bits);
         else if (input_channels > 1)
            actual_size = write_core_interleaved(subgraph, NULL, core, operation, zrl_bits);
         else
            actual_size = write_core_sequential(subgraph, NULL, core, operation, zrl_bits);

         compressed_size += actual_size;
      }

      /* If more bits don't compress further, then stop */
      if (compressed_size <= best_compressed_size) {
         best_compressed_size = compressed_size;
         best_zrl_bits = zrl_bits;
      } else
         break;
   }

   return best_zrl_bits;
}

struct etna_bo *
etna_ml_create_coeffs_v7(struct etna_ml_subgraph *subgraph, const struct etna_operation *operation, unsigned *cache_size)
{
   struct pipe_context *context = subgraph->base.context;
   struct etna_context *ctx = etna_context(context);
   unsigned nn_core_count = etna_ml_get_core_info(ctx)->nn_core_count;
   unsigned header_size = ALIGN(nn_core_count * 4, 64);
   unsigned input_channels = operation->addition ? 1 : operation->input_channels;
   unsigned output_channels = operation->addition ? 1 : operation->output_channels;
   unsigned cores_used = MIN2(output_channels, nn_core_count);
   unsigned zrl_bits;
   unsigned max_core_size = 0;
   unsigned bo_size;

   bo_size = calculate_weight_bo_size(subgraph, operation);
   zrl_bits = calculate_zrl_bits(subgraph, operation);

   struct etna_bo *compressed = etna_bo_new(ctx->screen->dev,
                                            bo_size,
                                            DRM_ETNA_GEM_CACHE_WC);

   etna_bo_cpu_prep(compressed, DRM_ETNA_PREP_WRITE);

   uint32_t *map = etna_bo_map(compressed);
   memset(map, 0, bo_size);

   uint32_t *header = map;
   map += header_size / 4;

   for (unsigned core = 0; core < cores_used; core++) {

      unsigned actual_size;
      if (operation->pointwise && output_channels > 8)
         actual_size = write_core_6(subgraph, map, core, operation, zrl_bits);
      else if (input_channels > 1)
         actual_size = write_core_interleaved(subgraph, map, core, operation, zrl_bits);
      else
         actual_size = write_core_sequential(subgraph, map, core, operation, zrl_bits);

      actual_size = ALIGN(actual_size, 64);
      max_core_size = MAX2(actual_size, max_core_size);

      header[core] = actual_size;

      map += actual_size / 4;
   }

   etna_bo_cpu_fini(compressed);

   *cache_size = max_core_size * cores_used;

   return compressed;
}