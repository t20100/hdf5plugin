/* HDF5 compression filter plugin storing compressed chunk as Jpeg2000/part1 J2K
or JP2 code stream.

If no parameter is provided, use lossless compression.

Input HDF5 filter options (a.k.a. `cd_values`):
 - 0: Compression ratio: as a fixed point integer with 2 decimals (e.g. 1000
= 10.00:1). Pass value < 100 for lossless compression.

Stored HDF5 filter options (a.ka. `cd_values`):
- 0: RESERVED: Filter version
- 1: RESERVED: Width  // TODO or height width?
- 2: RESERVED: Height
- 3: RESERVED: Number of components
- 4: RESERVED: Data type
- 5: Compression ratio

TODO:
extra:
- 6: PSNR
or
 - 5: Mode: lossless/compression ratio/PSN
 - 6: Mode parameter
or
  - 5: Mode? J2K/JP2/HTJ2K...?
  - 6: Compression ratio
  - 7: PSNR
  - 8: Color scheme?
*/

#include "H5PLextern.h"
#include "H5Zjpeg2000_backend.h"
#include <assert.h>
#include <hdf5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define H5Z_FILTER_JPEG2000 65000
#define H5Z_FILTER_JPEG2000_VERSION 1

#define CD_INDEX_VERSION 0
#define CD_INDEX_WIDTH 1
#define CD_INDEX_HEIGHT 2
#define CD_INDEX_NCOMPS 3
#define CD_INDEX_DTYPE 4
#define CD_INDEX_RATIO 5

static herr_t set_local_jpeg2000(hid_t dcpl, hid_t type, hid_t space);
static size_t filter_jpeg2000(unsigned int flags, size_t cd_nelmts,
                              const unsigned int cd_values[], size_t nbytes,
                              size_t *buf_size, void **buf);

const H5Z_class2_t H5Z_JPEG2000[1] = {{
    H5Z_CLASS_T_VERS,                  /* H5Z_class_t version */
    (H5Z_filter_t)H5Z_FILTER_JPEG2000, /* Filter id number             */
    1,                                 /* encoder_present flag (set to true) */
    1,                                 /* decoder_present flag (set to true) */
    "jpeg2000",                        /* Filter name for debugging    */
    NULL,                              /* The "can apply" callback     */
    (H5Z_set_local_func_t)set_local_jpeg2000,
    (H5Z_func_t)filter_jpeg2000,
}};
// TODO add can_apply + check filter version number

H5PL_type_t H5PLget_plugin_type(void) { return H5PL_TYPE_FILTER; }

const void *H5PLget_plugin_info(void) { return H5Z_JPEG2000; }

static herr_t set_local_jpeg2000(hid_t dcpl, hid_t type, hid_t space) {
  herr_t result;
  unsigned int flags;
  size_t cd_nelmts = 8;
  unsigned cd_values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t input_cd_nelmts = 1;
  unsigned input_cd_values[] = {0};

  /* Get user input and move them after reserved slots */
  result =
      H5Pget_filter_by_id2(dcpl, H5Z_FILTER_JPEG2000, &flags, &input_cd_nelmts,
                           input_cd_values, 0, NULL, NULL);
  if (result < 0) {
    fprintf(stderr, "H5Pget_filter_by_id2 failed\n");
    return -1;
  }
  cd_values[CD_INDEX_RATIO] = input_cd_values[0];

  cd_values[CD_INDEX_VERSION] = H5Z_FILTER_JPEG2000_VERSION;

  /* Retrieve width, height, number of components */
  int ndims;
  hsize_t dims[H5S_MAX_RANK], dims_used[H5S_MAX_RANK];

  ndims = H5Sget_simple_extent_dims(space, dims, NULL);
  if (ndims < 0) {
    fprintf(stderr, "H5Sget_simple_extent_dims failed\n");
    return -1;
  }

  /* computed used (e.g. non-unity) dimensions in chunk */
  int ndims_used = 0;
  int dim_index;
  for (dim_index = 0; dim_index < ndims; dim_index++) {
    if (dims[dim_index] <= 1)
      continue;
    dims_used[ndims_used] = dims[dim_index];
    ndims_used++;
  }
  if (ndims_used == 2) {
    cd_values[CD_INDEX_WIDTH] = dims_used[1];
    cd_values[CD_INDEX_HEIGHT] = dims_used[0];
    cd_values[CD_INDEX_NCOMPS] = 1;
  } else if (ndims_used == 3) {
    cd_values[CD_INDEX_WIDTH] = dims_used[1];
    cd_values[CD_INDEX_HEIGHT] = dims_used[0];
    // TODO check number of components is supported
    cd_values[CD_INDEX_NCOMPS] = dims_used[2];
  } else {
    fprintf(stderr, "Unsupported number of dimensions\n");
    return -1;
  }

  /* Retrieve data type */
  int dtype = 0;
  H5T_class_t dclass;
  dclass = H5Tget_class(type);
  if (dclass < 0) {
    fprintf(stderr, "H5Tget_class failed\n");
    return -1;
  }
  size_t dsize;
  dsize = H5Tget_size(type);
  if (dsize == 0) {
    fprintf(stderr, "H5Tget_size failed\n");
    return -1;
  }
  if (dclass == H5T_BITFIELD) {
    dtype = H5Z_J2K_DTYPE_BITFIELD;
  } else if (dclass == H5T_INTEGER) {
    H5T_sign_t sign;
    sign = H5Tget_sign(type);
    if (sign == H5T_SGN_ERROR) {
      fprintf(stderr, "H5Tget_sign failed\n");
      return -1;
    }

    if (dsize == sizeof(uint8_t)) {
      dtype = (sign == H5T_SGN_2) ? H5Z_J2K_DTYPE_INT8 : H5Z_J2K_DTYPE_UINT8;
    } else if (dsize == sizeof(uint16_t)) {
      dtype = (sign == H5T_SGN_2) ? H5Z_J2K_DTYPE_INT16 : H5Z_J2K_DTYPE_UINT16;
    } else if (dsize == sizeof(uint32_t)) {
      dtype = (sign == H5T_SGN_2) ? H5Z_J2K_DTYPE_INT32 : H5Z_J2K_DTYPE_UINT32;
    } else {
      fprintf(stderr, "Unsupported datatype size\n");
      return -1;
    }
  } else {
    fprintf(stderr, "Unsupported datatype class: %d\n", dclass);
    return -1;
  }
  cd_values[CD_INDEX_DTYPE] = dtype;

  // TODO validate user defined parameters

  result =
      H5Pmodify_filter(dcpl, H5Z_FILTER_JPEG2000, flags, cd_nelmts, cd_values);
  if (result < 0) {
    fprintf(stderr, "H5Pmodify_filter failed\n");
    return -1;
  }

  return 1;
}

static size_t filter_jpeg2000(unsigned int flags, size_t cd_nelmts,
                              const unsigned int cd_values[], size_t nbytes,
                              size_t *buf_size, void **buf) {
  int result;
  size_t output_size;
  void *output_buffer = NULL;
  void *input_buffer = *buf;

  if (flags & H5Z_FLAG_REVERSE) { /** Decompress data **/
    result = decompress(nbytes, input_buffer, &output_size, &output_buffer);
    if (result < 0) {
      fprintf(stderr, "decompress failed\n");
      if (output_buffer) {
        free(output_buffer);
      }
      return 0;
    }

  } else { /** Compress data **/
    result = compress(nbytes, input_buffer, cd_values[CD_INDEX_WIDTH],
                      cd_values[CD_INDEX_HEIGHT], cd_values[CD_INDEX_NCOMPS],
                      cd_values[CD_INDEX_DTYPE],
                      cd_values[CD_INDEX_RATIO] /
                          100.0, // convert fixed point to float
                      &output_size, &output_buffer);
    if (result < 0) {
      fprintf(stderr, "compress failed\n");
      if (output_buffer) {
        free(output_buffer);
      }
      return 0;
    }
  }

  free(*buf);
  *buf = output_buffer;
  *buf_size = output_size;
  return output_size;
}
