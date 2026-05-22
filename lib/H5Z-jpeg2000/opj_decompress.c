#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "H5Zjpeg2000_backend.h"
#include <openjpeg.h>

/* ------------------------------------------------------------------ */
/* Internal image layout descriptor                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t num_comps;
  uint32_t prec;
  h5z_j2k_dtype_t sample_type;
  size_t sample_size;
} image_info_t;
/* ------------------------------------------------------------------ */
/* Memory-backed stream helpers                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  const uint8_t *data;
  size_t size;
  size_t offset;
} mem_stream_t;

static OPJ_SIZE_T mem_read(void *dst, OPJ_SIZE_T nb, void *user_data) {
  mem_stream_t *ms = (mem_stream_t *)user_data;
  size_t remaining = ms->size - ms->offset;

  if (remaining == 0)
    return (OPJ_SIZE_T)-1; /* signal EOF */

  if (nb > remaining)
    nb = (OPJ_SIZE_T)remaining;

  memcpy(dst, ms->data + ms->offset, nb);
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

static opj_stream_t *stream_from_memory(const void *buf, size_t len,
                                        mem_stream_t *ms) {
  ms->data = (const uint8_t *)buf;
  ms->size = len;
  ms->offset = 0;

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

/* ------------------------------------------------------------------ */
/* Quiet message handlers                                              */
/* ------------------------------------------------------------------ */

/*
static void on_error  (const char *msg, void *data) { fprintf(stderr, msg);}
static void on_warning(const char *msg, void *data) { fprintf(stderr, msg);}
static void on_info   (const char *msg, void *data) { fprintf(stderr, msg); }
*/
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
/* Sample packing: OPJ_INT32[] → typed output buffer                  */
/* ------------------------------------------------------------------ */

/**
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

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int decompress(size_t compressed_nbytes, void *compressed_buffer,
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

  opj_set_error_handler(codec, on_error, NULL);
  opj_set_warning_handler(codec, on_warning, NULL);
  opj_set_info_handler(codec, on_info, NULL);

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
