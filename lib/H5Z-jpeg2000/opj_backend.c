#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "H5Zjpeg2000_backend.h"
#include <openjpeg.h>

/* ================================================================== */
/* Internal helpers: memory-backed streams, quiet handlers             */
/* ================================================================== */

/* ---- Unified memory-backed stream ---- */

typedef struct {
  uint8_t *data;      /* buffer (mutable for write, NULL for read) */
  const uint8_t *rdonly; /* original read-only pointer (set by stream_from_memory) */
  size_t size;        /* total buffer size */
  size_t offset;      /* current read/write position */
} mem_stream_t;

/* Read callbacks */

static OPJ_SIZE_T mem_read(void *dst, OPJ_SIZE_T nb, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;
  const uint8_t *src = ms->rdonly ? ms->rdonly : ms->data;
  size_t remaining = ms->size - ms->offset;

  if (remaining == 0)
    return (OPJ_SIZE_T)-1; /* signal EOF */

  if (nb > (OPJ_SIZE_T)remaining)
    nb = (OPJ_SIZE_T)remaining;

  memcpy(dst, src + ms->offset, nb);
  ms->offset += nb;
  return nb;
}

static OPJ_OFF_T mem_skip(OPJ_OFF_T nb, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;

  if (nb < 0)
    return -1;

  size_t skip = (size_t)nb;
  if (ms->offset + skip > ms->size)
    skip = ms->size - ms->offset;

  ms->offset += skip;
  return (OPJ_OFF_T)skip;
}

static OPJ_BOOL mem_seek(OPJ_OFF_T pos, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;

  if (pos < 0 || (size_t)pos > ms->size)
    return OPJ_FALSE;

  ms->offset = (size_t)pos;
  return OPJ_TRUE;
}

/* Create an input (read) stream wrapping a memory buffer (zero-copy). */

static opj_stream_t *stream_from_memory(const void *buf, size_t len,
                                        mem_stream_t *ms) {
  memset(ms, 0, sizeof(*ms));
  ms->data = NULL;
  ms->rdonly = (const uint8_t *)buf;
  ms->size = len;

  opj_stream_t *stream =
      opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE /* input */);
  if (!stream)
    return NULL;

  opj_stream_set_user_data(stream, ms, NULL);
  opj_stream_set_user_data_length(stream, (OPJ_UINT64)len);
  opj_stream_set_read_function(stream, mem_read);
  opj_stream_set_skip_function(stream, mem_skip);
  opj_stream_set_seek_function(stream, mem_seek);

  return stream;
}

/* Write callbacks */

static OPJ_SIZE_T ws_write(void *src, OPJ_SIZE_T nb, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;

  /* Grow buffer if needed */
  if (ms->offset + nb > ms->size) {
    size_t new_size = ms->size * 2;
    if (new_size < ms->offset + nb)
      new_size = ms->offset + nb;
    uint8_t *p = (uint8_t *)realloc(ms->data, new_size);
    if (!p)
      return (OPJ_SIZE_T)-1;
    ms->data = p;
    ms->size = new_size;
  }

  memcpy(ms->data + ms->offset, src, nb);
  ms->offset += nb;
  return nb;
}

static OPJ_OFF_T ws_skip(OPJ_OFF_T nb, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;

  if (nb < 0)
    return -1;

  /* Extend allocation if seeking past end */
  size_t new_off = ms->offset + (size_t)nb;
  if (new_off > ms->size) {
    uint8_t *p = (uint8_t *)realloc(ms->data, new_off);
    if (!p)
      return -1;
    ms->data = p;
    ms->size = new_off;
  }

  ms->offset = new_off;
  return nb;
}

static OPJ_BOOL ws_seek(OPJ_OFF_T pos, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;

  if (pos < 0)
    return OPJ_FALSE;

  size_t new_off = (size_t)pos;
  if (new_off > ms->size) {
    uint8_t *p = (uint8_t *)realloc(ms->data, new_off);
    if (!p)
      return OPJ_FALSE;
    ms->data = p;
    ms->size = new_off;
  }

  ms->offset = new_off;
  return OPJ_TRUE;
}

/* Create an output (write) stream backed by a growable buffer. */

static opj_stream_t *stream_for_write(mem_stream_t *ms, size_t initial_size) {
  memset(ms, 0, sizeof(*ms));
  ms->rdonly = NULL;
  ms->data = (uint8_t *)malloc(initial_size);
  ms->size = ms->data ? initial_size : 0;

  if (!ms->data)
    return NULL;

  opj_stream_t *stream =
      opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_FALSE /* output */);
  if (!stream) {
    free(ms->data);
    ms->data = NULL;
    ms->size = 0;
    return NULL;
  }

  opj_stream_set_user_data(stream, ms, NULL);
  opj_stream_set_write_function(stream, ws_write);
  opj_stream_set_skip_function(stream, ws_skip);
  opj_stream_set_seek_function(stream, ws_seek);

  return stream;
}

/* Retrieve the owned output buffer after a write stream is destroyed. */

static inline void *mem_stream_get_data(mem_stream_t *ms, size_t *out_nbytes) {
  void *buf = ms->data;
  if (out_nbytes)
    *out_nbytes = ms->offset;
  ms->data = NULL;
  ms->size = 0;
  ms->offset = 0;
  return buf;
}

/* Free the buffer if it was allocated by stream_for_write. */

static inline void mem_stream_free_data(mem_stream_t *ms) {
  if (ms->data) {
    free(ms->data);
    ms->data = NULL;
  }
  ms->size = 0;
  ms->offset = 0;
  ms->rdonly = NULL;
}

/* ---- Quiet message handlers ---- */

static void opj_on_error(const char *msg, void *data) {
  (void)msg;
  (void)data;
}
static void opj_on_warning(const char *msg, void *data) {
  (void)msg;
  (void)data;
}
static void opj_on_info(const char *msg, void *data) {
  (void)msg;
  (void)data;
}

static inline void opj_set_quiet_handlers(opj_codec_t *codec) {
  opj_set_error_handler(codec, opj_on_error, NULL);
  opj_set_warning_handler(codec, opj_on_warning, NULL);
  opj_set_info_handler(codec, opj_on_info, NULL);
}

/* ================================================================== */
/* Compress helpers                                                    */
/* ================================================================== */

typedef struct {
  unsigned int prec; /* bits per sample */
  unsigned int sgnd; /* 1 = signed, 0 = unsigned */
  size_t size;       /* bytes per sample */
} dtype_info_t;

static int dtype_info(unsigned int dtype, dtype_info_t *out) {
  switch ((h5z_j2k_dtype_t)dtype) {
  case H5Z_J2K_DTYPE_INT8:
    out->prec = 8;
    out->sgnd = 1;
    out->size = sizeof(int8_t);
    return 0;
  case H5Z_J2K_DTYPE_UINT8:
    out->prec = 8;
    out->sgnd = 0;
    out->size = sizeof(uint8_t);
    return 0;
  case H5Z_J2K_DTYPE_INT16:
    out->prec = 16;
    out->sgnd = 1;
    out->size = sizeof(int16_t);
    return 0;
  case H5Z_J2K_DTYPE_UINT16:
    out->prec = 16;
    out->sgnd = 0;
    out->size = sizeof(uint16_t);
    return 0;
  case H5Z_J2K_DTYPE_INT32:
    out->prec = 32;
    out->sgnd = 1;
    out->size = sizeof(int32_t);
    return 0;
  case H5Z_J2K_DTYPE_UINT32:
    out->prec = 32;
    out->sgnd = 0;
    out->size = sizeof(uint32_t);
    return 0;
  default:
    return -1;
  }
}

/* ================================================================== */
/* Decompress helpers                                                  */
/* ================================================================== */

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t num_comps;
  uint32_t prec;
  h5z_j2k_dtype_t sample_type;
  size_t sample_size;
} image_info_t;

/*
 * Pack decoded OPJ_INT32 samples into the appropriate typed output buffer.
 *
 * OpenJPEG always decodes into OPJ_INT32 arrays internally.
 * For unsigned images the values are in [0, 2^prec - 1].
 * For signed images the values are in [-2^(prec-1), 2^(prec-1) - 1].
 *
 * We down-cast to the natural storage type (uint8 / uint16 / int32)
 * without further shifting or scaling; the caller receives the raw
 * reconstructed sample values exactly as encoded.
 */

static int pack_samples(const opj_image_t *image, const image_info_t *info,
                        void **output_buffer, size_t *output_nbytes) {
  size_t npixels = (size_t)info->width * info->height;
  size_t nsamples = npixels * info->num_comps;
  size_t nbytes = nsamples * info->sample_size;

  void *out = malloc(nbytes);
  if (!out)
    return -1;

  for (uint32_t c = 0; c < info->num_comps; c++) {
    const OPJ_INT32 *src = image->comps[c].data;

    for (size_t px = 0; px < npixels; px++) {
      size_t idx = px * info->num_comps + c;

      switch (info->sample_type) {
      case H5Z_J2K_DTYPE_INT8:
        ((int8_t *)out)[idx] = (int8_t)src[px];
        break;
      case H5Z_J2K_DTYPE_UINT8:
        ((uint8_t *)out)[idx] = (uint8_t)src[px];
        break;
      case H5Z_J2K_DTYPE_INT16:
        ((int16_t *)out)[idx] = (int16_t)src[px];
        break;
      case H5Z_J2K_DTYPE_UINT16:
        ((uint16_t *)out)[idx] = (uint16_t)src[px];
        break;
      case H5Z_J2K_DTYPE_INT32:
        ((int32_t *)out)[idx] = (int32_t)src[px];
        break;
      case H5Z_J2K_DTYPE_UINT32:
        ((uint32_t *)out)[idx] = (uint32_t)src[px];
        break;
      }
    }
  }

  *output_buffer = out;
  *output_nbytes = nbytes;
  return 0;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

int h5z_jpeg2000_compress(size_t input_nbytes, void *input_buffer, unsigned int width,
             unsigned int height, unsigned int ncomps, unsigned int dtype,
             float compression_ratio, size_t *output_nbytes,
             void **output_buffer) {
  int ret = -1;
  opj_codec_t *codec = NULL;
  opj_stream_t *stream = NULL;
  opj_image_t *image = NULL;
  mem_stream_t ms = {0};
  dtype_info_t di;

  if (!input_buffer || !output_nbytes || !output_buffer)
    return -1;

  *output_nbytes = 0;
  *output_buffer = NULL;

  /* ---- 1. Resolve dtype ---- */
  if (dtype_info(dtype, &di) != 0) {
    fprintf(stderr, "dtype_info error\n");
    return -1;
  }

  size_t npixels = (size_t)width * height;
  size_t expected = npixels * ncomps * di.size;
  if (input_nbytes != expected) {
    fprintf(stderr, "input_nbytes != expected: %zu != %zu\n", input_nbytes,
            expected);
    return -1;
  }

  /* ---- 2. Build opj_image_t from raw buffer ---- */
  /*
   * OpenJPEG always uses OPJ_INT32 for component data internally.
   * We unpack the interleaved input into per-component planar arrays.
   */
  opj_image_cmptparm_t *cmptparms =
      (opj_image_cmptparm_t *)calloc(ncomps, sizeof(*cmptparms));
  if (!cmptparms) {
    fprintf(stderr, "calloc error\n");
    return -1;
  }

  for (unsigned int c = 0; c < ncomps; c++) {
    cmptparms[c].prec = di.prec;
    cmptparms[c].sgnd = di.sgnd;
    cmptparms[c].dx = 1;
    cmptparms[c].dy = 1;
    cmptparms[c].w = width;
    cmptparms[c].h = height;
  }

  image = opj_image_create(ncomps, cmptparms, OPJ_CLRSPC_GRAY);
  free(cmptparms);
  if (!image) {
    fprintf(stderr, "opj_image_create error\n");
    goto cleanup;
  }

  /* Set image dimensions and component offsets */

  image->x0 = 0;
  image->y0 = 0;
  image->x1 = width;
  image->y1 = height;

  /* Unpack interleaved input → planar OPJ_INT32 component arrays */
  for (unsigned int c = 0; c < ncomps; c++) {
    OPJ_INT32 *dst = image->comps[c].data;

    for (size_t px = 0; px < npixels; px++) {
      size_t idx = px * ncomps + c;

      switch ((h5z_j2k_dtype_t)dtype) {
      case H5Z_J2K_DTYPE_INT8:
        dst[px] = (OPJ_INT32)((int8_t *)input_buffer)[idx];
        break;
      case H5Z_J2K_DTYPE_UINT8:
        dst[px] = (OPJ_INT32)((uint8_t *)input_buffer)[idx];
        break;
      case H5Z_J2K_DTYPE_INT16:
        dst[px] = (OPJ_INT32)((int16_t *)input_buffer)[idx];
        break;
      case H5Z_J2K_DTYPE_UINT16:
        dst[px] = (OPJ_INT32)((uint16_t *)input_buffer)[idx];
        break;
      case H5Z_J2K_DTYPE_INT32:
        dst[px] = (OPJ_INT32)((int32_t *)input_buffer)[idx];
        break;
      case H5Z_J2K_DTYPE_UINT32:
        dst[px] = (OPJ_INT32)((uint32_t *)input_buffer)[idx];
        break;
      }
    }
  }

  /* ---- 3. Set up encoder parameters ---- */
  opj_cparameters_t params;
  opj_set_default_encoder_parameters(&params);

  params.cod_format = 0; /* 0 = J2K raw codestream (no JP2 container) */

  params.tcp_numlayers = 1;
  params.cp_disto_alloc = 1;
  if (compression_ratio <= 1.0) {
    /* Lossless: use MCT only for 3-component images */
    params.irreversible = 0; /* reversible (5-3) wavelet */
    params.tcp_rates[0] = 0; /* 0 means lossless in OpenJPEG */
  } else {
    params.irreversible = 1; /* lossy: ICT + 9-7 wavelet */
    params.tcp_numlayers = 1;
    params.tcp_rates[0] = compression_ratio;
  }

  /* Always disable MCT — we encode each component independently */
  params.tcp_mct = 0;

  /* ---- 4. Create J2K encoder ---- */
  codec = opj_create_compress(OPJ_CODEC_J2K); // TODO allow JP2?
  if (!codec) {
    fprintf(stderr, "opj_create_compress error\n");
    goto cleanup;
  }

  opj_set_quiet_handlers(codec);

  if (!opj_setup_encoder(codec, &params, image)) {
    fprintf(stderr, "opj_setup_encoder error\n");
    goto cleanup;
  }

  /* ---- 5. Create write stream with a reasonable initial allocation ---- */
  /*
   * Heuristic initial size: uncompressed size / max(1, ratio/2).
   * The buffer grows automatically via realloc in ws_write().
   */
  size_t initial =
      expected / (compression_ratio > 1.0 ? compression_ratio / 2.0 : 1.0);
  if (initial < 4096)
    initial = 4096;

  stream = stream_for_write(&ms, initial);
  if (!stream) {
    fprintf(stderr, "stream_for_write error\n");
    goto cleanup;
  }

  /* ---- 6. Encode ---- */
  if (!opj_start_compress(codec, image, stream)) {
    fprintf(stderr, "opj_start_compress error\n");
    goto cleanup;
  }

  if (!opj_encode(codec, stream)) {
    fprintf(stderr, "opj_encode error\n");
    goto cleanup;
  }

  if (!opj_end_compress(codec, stream)) {
    fprintf(stderr, "opj_end_compress error\n");
    goto cleanup;
  }

  /* Transfer ownership of the output buffer */
  *output_buffer = mem_stream_get_data(&ms, output_nbytes);
  ret = 0;

cleanup:
  mem_stream_free_data(&ms);
  if (image)
    opj_image_destroy(image);
  if (stream)
    opj_stream_destroy(stream);
  if (codec)
    opj_destroy_codec(codec);

  return ret;
}

int h5z_jpeg2000_decompress(size_t compressed_nbytes, void *compressed_buffer,
               size_t *output_nbytes, void **output_buffer) {
  int ret = -1;
  opj_codec_t *codec = NULL;
  opj_stream_t *stream = NULL;
  opj_image_t *image = NULL;
  mem_stream_t ms;

  if (!compressed_buffer || compressed_nbytes == 0 || !output_nbytes ||
      !output_buffer)
    return -1;

  *output_nbytes = 0;
  *output_buffer = NULL;

  /* ---- 1. Create J2K decoder (handles J2K, JP2 and HTJ2K) ---- */
  OPJ_CODEC_FORMAT format;
  if (compressed_nbytes < 4) {
    fprintf(stderr, "Input too short to determine codestream format\n");
    return -1;
  }

  unsigned char magic[4];
  memcpy(magic, compressed_buffer, 4);
  if (magic[0] == 0xff && magic[1] == 0x4f)
    format = OPJ_CODEC_J2K;
  else if (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x00 &&
           magic[3] == 0x0C)
    // TODO check the next 8 bits of the magic
    format = OPJ_CODEC_JP2;
  else {
    fprintf(stderr, "Unknown magic\n");
    return -1;
  }
  codec = opj_create_decompress(format);
  if (!codec) {
    fprintf(stderr, "opj_create_decompress error\n");
    goto cleanup;
  }

  opj_set_quiet_handlers(codec);

  /* ---- 2. Default decoder parameters ---- */
  opj_dparameters_t params;
  opj_set_default_decoder_parameters(&params);

  if (!opj_setup_decoder(codec, &params)) {
    fprintf(stderr, "opj_setup_decoder error\n");
    goto cleanup;
  }

  /* ---- 3. Wrap input buffer in a zero-copy stream ---- */
  stream = stream_from_memory(compressed_buffer, compressed_nbytes, &ms);
  if (!stream) {
    fprintf(stderr, "stream_from_memory error\n");
    goto cleanup;
  }

  /* ---- 4. Read main header ---- */
  if (!opj_read_header(stream, codec, &image)) {
    fprintf(stderr, "opj_read_header error\n");
    goto cleanup;
  }

  /* ---- 5. Decode ---- */
  if (!opj_decode(codec, stream, image)) {
    fprintf(stderr, "opj_decode error\n");
    goto cleanup;
  }

  if (!opj_end_decompress(codec, stream)) {
    fprintf(stderr, "opj_end_decompress error\n");
    goto cleanup;
  }

  /* ---- 6. Determine output sample type from codestream precision ---- */
  /*
   * All components are assumed to share the same prec / sgnd.
   * (Mixed-precision images are exotic and rarely encountered.)
   * prec and sgnd come directly from the SIZ marker in the codestream.
   */
  image_info_t local_info;
  local_info.width = image->comps[0].w;
  local_info.height = image->comps[0].h;
  local_info.num_comps = image->numcomps;
  local_info.prec = image->comps[0].prec;

  int sgnd = image->comps[0].sgnd;

  if (local_info.prec <= 8) {
    local_info.sample_type = sgnd ? H5Z_J2K_DTYPE_INT8 : H5Z_J2K_DTYPE_UINT8;
    local_info.sample_size = sizeof(int8_t); /* same size for both */
  } else if (local_info.prec <= 16) {
    local_info.sample_type = sgnd ? H5Z_J2K_DTYPE_INT16 : H5Z_J2K_DTYPE_UINT16;
    local_info.sample_size = sizeof(int16_t);
  } else {
    local_info.sample_type = sgnd ? H5Z_J2K_DTYPE_INT32 : H5Z_J2K_DTYPE_UINT32;
    local_info.sample_size = sizeof(int32_t);
  }

  /* ---- 7. Pack into the typed output buffer ---- */
  if (pack_samples(image, &local_info, output_buffer, output_nbytes) != 0)
    goto cleanup;

  ret = 0;

cleanup:
  if (image)
    opj_image_destroy(image);
  if (stream)
    opj_stream_destroy(stream);
  if (codec)
    opj_destroy_codec(codec);

  return ret;
}
