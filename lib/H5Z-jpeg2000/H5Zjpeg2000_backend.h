#ifndef H5Z_JPEG2000_BACKEND_H
#define H5Z_JPEG2000_BACKEND_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H5Z_JPEG2000_BACKEND_ENV "HDF5PLUGIN_JPEG2000_BACKEND"
#define H5Z_JPEG2000_MANIFEST_ENV "HDF5PLUGIN_JPEG2000_MANIFEST"
#define H5Z_JPEG2000_MANIFEST_NAME "hdf5plugin_jpeg2000_plugins.json"

/* Backend selection is process-local and never stored in HDF5 chunks. */

/*
 * Keep backend entry points prefixed. Plain names such as compress() or
 * decompress() are too generic for HDF5 plugins loaded into a shared
 * process namespace, and become ambiguous once multiple backends exist.
 */

typedef enum {
  H5Z_J2K_DTYPE_BITFIELD = 0,
  H5Z_J2K_DTYPE_INT8 = 1,
  H5Z_J2K_DTYPE_UINT8 = 2,
  H5Z_J2K_DTYPE_INT16 = 3,
  H5Z_J2K_DTYPE_UINT16 = 4,
  H5Z_J2K_DTYPE_INT32 = 5,
  H5Z_J2K_DTYPE_UINT32 = 6,
} h5z_j2k_dtype_t;

/*
 * Backend ABI for the standalone J2K filter.  The HDF5 filter itself stays
 * backend-neutral; OpenJPEG, Kakadu and future engines live in separate shared
 * libraries loaded by the dispatcher.  This keeps the HDF5 filter loadable even
 * when optional backend dependencies are not on LD_LIBRARY_PATH.
 *
 * HTJ2K is intentionally planned as a separate HDF5 plugin rather
 * than another mode of this J2K filter.  That future plugin can use
 * the same pattern with an HTJ2K-specific backend list, e.g.
 * OpenHTJ2K, Kakadu, or Grok where supported.
 */
typedef struct {
  const char *name;
  int (*available)(void);
  int (*compress)(size_t input_nbytes, void *input_buffer, unsigned int width,
                  unsigned int height, unsigned int ncomps, unsigned int dtype,
                  float compression_ratio, size_t *output_nbytes,
                  void **output_buffer);
  int (*decompress)(size_t compressed_nbytes, void *compressed_buffer,
                    size_t *output_nbytes, void **output_buffer);
} h5z_jpeg2000_backend_t;

/**
 * Encode a raw pixel buffer to a J2K codestream in memory using the selected
 * backend.
 *
 * @param input_nbytes       byte length of the input pixel buffer
 * @param input_buffer       pointer to the input pixel buffer (interleaved
 * samples)
 * @param width              image width in pixels
 * @param height             image height in pixels
 * @param ncomps             number of components (e.g. 1=grey, 3=RGB)
 * @param dtype              sample type, one of j2k_dtype_t (unsigned int)
 * @param compression_ratio  lossy ratio (e.g. 10 = 10:1); 0 = lossless
 * @param output_nbytes      [out] byte length of the allocated output
 * codestream
 * @param output_buffer      [out] caller-owned output buffer (free() when done)
 * @return 0 on success, -1 on failure
 */
int h5z_jpeg2000_compress(size_t input_nbytes, void *input_buffer,
                          unsigned int width, unsigned int height,
                          unsigned int ncomps, unsigned int dtype,
                          float compression_ratio, size_t *output_nbytes,
                          void **output_buffer);

/**
 * Decode a raw J2K codestream from memory using the selected backend.
 *
 * @param compressed_nbytes  byte length of the input codestream
 * @param compressed_buffer  pointer to the input codestream
 * @param output_nbytes      [out] byte length of the allocated output buffer
 * @param output_buffer      [out] caller-owned output buffer (free() when done)
 * @return 0 on success, -1 on failure
 */
int h5z_jpeg2000_decompress(size_t compressed_nbytes, void *compressed_buffer,
                            size_t *output_nbytes, void **output_buffer);

const h5z_jpeg2000_backend_t *h5z_jpeg2000_select_backend(void);

int h5z_jpeg2000_openjpeg_compress(
    size_t input_nbytes, void *input_buffer, unsigned int width,
    unsigned int height, unsigned int ncomps, unsigned int dtype,
    float compression_ratio, size_t *output_nbytes, void **output_buffer);
int h5z_jpeg2000_openjpeg_decompress(size_t compressed_nbytes,
                                     void *compressed_buffer,
                                     size_t *output_nbytes,
                                     void **output_buffer);

extern const h5z_jpeg2000_backend_t h5z_jpeg2000_openjpeg_backend;

#ifdef H5Z_JPEG2000_HAVE_KAKADU
int h5z_jpeg2000_kakadu_compress(
    size_t input_nbytes, void *input_buffer, unsigned int width,
    unsigned int height, unsigned int ncomps, unsigned int dtype,
    float compression_ratio, size_t *output_nbytes, void **output_buffer);
int h5z_jpeg2000_kakadu_decompress(size_t compressed_nbytes,
                                   void *compressed_buffer,
                                   size_t *output_nbytes,
                                   void **output_buffer);

extern const h5z_jpeg2000_backend_t h5z_jpeg2000_kakadu_backend;
#endif

#ifdef __cplusplus
}
#endif

#endif
