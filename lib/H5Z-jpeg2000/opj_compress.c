#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "H5Zjpeg2000_backend.h"
#include <openjpeg.h>

/* ------------------------------------------------------------------ */
/* dtype metadata helpers                                              */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Memory-backed write stream                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t *data;
  size_t size;   /* allocated bytes */
  size_t offset; /* current write position / bytes written */
} write_stream_t;

static OPJ_SIZE_T ws_write(void *src, OPJ_SIZE_T nb, void *user_data) {
  write_stream_t *ws = (write_stream_t *)user_data;

  /* Grow buffer if needed */
  if (ws->offset + nb > ws->size) {
    size_t new_size = ws->size * 2;
    if (new_size < ws->offset + nb)
      new_size = ws->offset + nb;
    uint8_t *p = (uint8_t *)realloc(ws->data, new_size);
    if (!p)
      return (OPJ_SIZE_T)-1;
    ws->data = p;
    ws->size = new_size;
  }

  memcpy(ws->data + ws->offset, src, nb);
  ws->offset += nb;
  return nb;
}

static OPJ_OFF_T ws_skip(OPJ_OFF_T nb, void *user_data) {
  write_stream_t *ws = (write_stream_t *)user_data;

  if (nb < 0)
    return -1;

  /* Extend allocation if seeking past end */
  size_t new_off = ws->offset + (size_t)nb;
  if (new_off > ws->size) {
    uint8_t *p = (uint8_t *)realloc(ws->data, new_off);
    if (!p)
      return -1;
    ws->data = p;
    ws->size = new_off;
  }

  ws->offset = new_off;
  return nb;
}

static OPJ_BOOL ws_seek(OPJ_OFF_T pos, void *user_data) {
  write_stream_t *ws = (write_stream_t *)user_data;

  if (pos < 0)
    return OPJ_FALSE;

  size_t new_off = (size_t)pos;
  if (new_off > ws->size) {
    uint8_t *p = (uint8_t *)realloc(ws->data, new_off);
    if (!p)
      return OPJ_FALSE;
    ws->data = p;
    ws->size = new_off;
  }

  ws->offset = new_off;
  return OPJ_TRUE;
}

static opj_stream_t *stream_for_write(write_stream_t *ws, size_t initial_size) {
  ws->data = (uint8_t *)malloc(initial_size);
  ws->size = ws->data ? initial_size : 0;
  ws->offset = 0;

  if (!ws->data)
    return NULL;

  /* OPJ_FALSE = output stream */
  opj_stream_t *stream =
      opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_FALSE);
  if (!stream) {
    free(ws->data);
    ws->data = NULL;
    return NULL;
  }

  opj_stream_set_user_data(stream, ws, NULL);
  opj_stream_set_write_function(stream, ws_write);
  opj_stream_set_skip_function(stream, ws_skip);
  opj_stream_set_seek_function(stream, ws_seek);

  return stream;
}

/* ------------------------------------------------------------------ */
/* Quiet message handlers                                              */
/* ------------------------------------------------------------------ */

static void on_error(const char *msg, void *data) {
  (void)msg;
  (void)data;
}
static void on_warning(const char *msg, void *data) {
  (void)msg;
  (void)data;
}
static void on_info(const char *msg, void *data) {
  (void)msg;
  (void)data;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int compress(size_t input_nbytes, void *input_buffer, unsigned int width,
             unsigned int height, unsigned int ncomps, unsigned int dtype,
             float compression_ratio, size_t *output_nbytes,
             void **output_buffer) {
  int ret = -1;
  opj_codec_t *codec = NULL;
  opj_stream_t *stream = NULL;
  opj_image_t *image = NULL;
  write_stream_t ws = {0};
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
      OPJ_INT32 tmp;

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

  opj_set_error_handler(codec, on_error, NULL);
  opj_set_warning_handler(codec, on_warning, NULL);
  opj_set_info_handler(codec, on_info, NULL);

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

  stream = stream_for_write(&ws, initial);
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

  /* ws.offset is exactly the number of bytes written */
  *output_buffer = ws.data;
  *output_nbytes = ws.offset;
  ws.data = NULL; /* ownership transferred; skip free in cleanup */
  ret = 0;

cleanup:
  if (ws.data)
    free(ws.data);
  if (image)
    opj_image_destroy(image);
  if (stream)
    opj_stream_destroy(stream);
  if (codec)
    opj_destroy_codec(codec);

  return ret;
}
