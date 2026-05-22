#include <sys/types.h>

typedef enum {
  H5Z_J2K_DTYPE_BITFIELD = 0,
  H5Z_J2K_DTYPE_INT8 = 1,
  H5Z_J2K_DTYPE_UINT8 = 2,
  H5Z_J2K_DTYPE_INT16 = 3,
  H5Z_J2K_DTYPE_UINT16 = 4,
  H5Z_J2K_DTYPE_INT32 = 5,
  H5Z_J2K_DTYPE_UINT32 = 6,
} h5z_j2k_dtype_t;

/**
 * compress() – encode a raw pixel buffer to a J2K codestream in memory.
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
int compress(size_t compressed_nbytes, void *compressed_buffer,
             unsigned int width, unsigned int height, unsigned int ncomps,
             unsigned int dtype, float compression_ratio, size_t *output_nbytes,
             void **output_buffer);

/**
 * decompress() – decode a raw J2K / HTJ2K codestream from memory.
 *
 * @param compressed_nbytes  byte length of the input codestream
 * @param compressed_buffer  pointer to the input codestream
 * @param output_nbytes      [out] byte length of the allocated output buffer
 * @param output_buffer      [out] caller-owned output buffer (free() when done)
 * @return 0 on success, -1 on failure
 */
int decompress(size_t compressed_nbytes, void *compressed_buffer,
               size_t *output_nbytes, void **output_buffer);