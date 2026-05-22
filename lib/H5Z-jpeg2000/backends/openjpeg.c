#include "H5Zjpeg2000_backend.h"

static int openjpeg_available(void) { return 1; }

const h5z_jpeg2000_backend_t h5z_jpeg2000_openjpeg_backend = {
    "openjpeg",
    openjpeg_available,
    h5z_jpeg2000_openjpeg_compress,
    h5z_jpeg2000_openjpeg_decompress,
};
