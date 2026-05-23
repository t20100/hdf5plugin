#include "H5Zjpeg2000_backend.h"

#ifdef H5Z_JPEG2000_HAVE_KAKADU

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "jp2.h"
#include "kdu_compressed.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "kdu_stripe_compressor.h"
#include "kdu_stripe_decompressor.h"
#include "kdu_threads.h"
#include "kdu_utils.h"

using namespace kdu_core;
using namespace kdu_supp;

namespace {

class KduErrorHandler : public kdu_message {
public:
  explicit KduErrorHandler(bool debug_enabled) : debug(debug_enabled) {}

  void put_text(const char *string) override {
    if (string) {
      buffer.append(string);
    }
  }

  void flush(bool end_of_message) override {
    if (end_of_message) {
      if (debug) {
        fprintf(stderr, "[hdf5plugin/jpeg2000] Kakadu error: %s\n", buffer.c_str());
      }
      buffer.clear();
      throw KDU_ERROR_EXCEPTION;
    }
  }

private:
  bool debug = false;
  std::string buffer;
};

class KduWarningHandler : public kdu_message {
public:
  explicit KduWarningHandler(bool debug_enabled) : debug(debug_enabled) {}

  void put_text(const char *string) override {
    if (string) {
      buffer.append(string);
    }
  }

  void flush(bool end_of_message) override {
    if (end_of_message) {
      if (debug) {
        fprintf(stderr, "[hdf5plugin/jpeg2000] Kakadu warning: %s\n", buffer.c_str());
      }
      buffer.clear();
    }
  }

private:
  bool debug = false;
  std::string buffer;
};

bool debug_enabled() { return std::getenv("HDF5PLUGIN_JPEG2000_DEBUG") != nullptr; }

void ensure_kakadu_handlers(bool debug) {
  static bool configured = false;
  if (configured) {
    return;
  }
  configured = true;
  static KduErrorHandler err_handler(debug);
  static KduWarningHandler warn_handler(debug);
  kdu_customize_errors(&err_handler);
  kdu_customize_warnings(&warn_handler);
}

bool env_flag(const char *name, bool default_value) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return default_value;
  }
  auto equals_ci = [](const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
      if (std::tolower(static_cast<unsigned char>(*a)) !=
          std::tolower(static_cast<unsigned char>(*b))) {
        return false;
      }
      ++a;
      ++b;
    }
    return *a == '\0' && *b == '\0';
  };
  if (equals_ci(v, "1") || equals_ci(v, "true") || equals_ci(v, "yes") ||
      equals_ci(v, "on")) {
    return true;
  }
  if (equals_ci(v, "0") || equals_ci(v, "false") || equals_ci(v, "no") ||
      equals_ci(v, "off")) {
    return false;
  }
  return default_value;
}

int env_int(const char *name, int default_value) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return default_value;
  }
  char *end = nullptr;
  long n = std::strtol(v, &end, 10);
  if (end == v || (end && *end != '\0')) {
    return default_value;
  }
  if (n < 0) {
    n = 0;
  } else if (n > 1024) {
    n = 1024;
  }
  return static_cast<int>(n);
}

struct KakaduTune {
  bool force_precise = true;
  bool want_fastest = false;
  int threads = 0;
};

KakaduTune get_kakadu_tune() {
  KakaduTune t;
  t.force_precise = env_flag("HDF5PLUGIN_JPEG2000_KAKADU_PRECISE", true);
  t.want_fastest = env_flag("HDF5PLUGIN_JPEG2000_KAKADU_FAST", false);
  t.threads = env_int("HDF5PLUGIN_JPEG2000_KAKADU_THREADS", 0);
  return t;
}

class ThreadEnvGuard {
public:
  ThreadEnvGuard() = default;
  ThreadEnvGuard(const ThreadEnvGuard &) = delete;
  ThreadEnvGuard &operator=(const ThreadEnvGuard &) = delete;

  void setup(int threads) {
    if (threads <= 1) {
      return;
    }
    env.create();
    for (int i = 1; i < threads; ++i) {
      env.add_thread();
    }
    created = true;
  }

  kdu_thread_env *ptr() { return created ? &env : nullptr; }

  ~ThreadEnvGuard() {
    if (created) {
      (void)env.destroy();
    }
  }

private:
  bool created = false;
  kdu_thread_env env;
};

class MemTarget : public kdu_compressed_target_nonnative {
public:
  std::vector<kdu_byte> data;

  bool post_write(int num_bytes) override {
    size_t offset = data.size();
    data.resize(offset + static_cast<size_t>(num_bytes));
    pull_data(reinterpret_cast<kdu_byte *>(data.data()), static_cast<int>(offset), num_bytes);
    return true;
  }
};

struct DtypeInfo {
  int precision;
  bool is_signed;
  int size;
};

bool set_dtype_info(DtypeInfo &info, int precision, bool is_signed, int size) {
  info.precision = precision;
  info.is_signed = is_signed;
  info.size = size;
  return true;
}

bool dtype_info(unsigned int dtype, DtypeInfo &info) {
  switch (static_cast<h5z_j2k_dtype_t>(dtype)) {
  case H5Z_J2K_DTYPE_INT8:
    return set_dtype_info(info, 8, true, 1);
  case H5Z_J2K_DTYPE_UINT8:
    return set_dtype_info(info, 8, false, 1);
  case H5Z_J2K_DTYPE_INT16:
    return set_dtype_info(info, 16, true, 2);
  case H5Z_J2K_DTYPE_UINT16:
    return set_dtype_info(info, 16, false, 2);
  case H5Z_J2K_DTYPE_INT32:
    return set_dtype_info(info, 32, true, 4);
  case H5Z_J2K_DTYPE_UINT32:
    return set_dtype_info(info, 32, false, 4);
  default:
    return false;
  }
}

bool set_siz_params(siz_params &siz, int width, int height, int num_comps,
                    const DtypeInfo &dtype) {
  if (width <= 0 || height <= 0 || !(num_comps == 1 || num_comps == 3)) {
    return false;
  }
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Ssize, 0, 0, height);
  siz.set(Ssize, 0, 1, width);
  siz.set(Sorigin, 0, 0, 0);
  siz.set(Sorigin, 0, 1, 0);
  siz.set(Stiles, 0, 0, height);
  siz.set(Stiles, 0, 1, width);
  siz.set(Stile_origin, 0, 0, 0);
  siz.set(Stile_origin, 0, 1, 0);

  for (int c = 0; c < num_comps; ++c) {
    siz.set(Sdims, c, 0, height);
    siz.set(Sdims, c, 1, width);
    siz.set(Sprecision, c, 0, dtype.precision);
    siz.set(Ssigned, c, 0, dtype.is_signed);
  }
  return true;
}

bool kakadu_extra_has_param(const char *param) {
  const char *extra = std::getenv("HDF5PLUGIN_JPEG2000_KAKADU_PARAMS");
  if (extra == nullptr || *extra == '\0' || param == nullptr || *param == '\0') {
    return false;
  }
  std::string s(extra);
  std::string p(param);
  for (char &ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  for (char &ch : p) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s.find(p) != std::string::npos;
}

void apply_kakadu_mode(siz_params &siz) {
  siz.parse_string("Sncap=P15");
  siz.parse_string("Cmodes=0");
}

void apply_kakadu_rate_defaults(siz_params &siz, int precision, bool rate_controlled) {
  if (!rate_controlled) {
    if (!kakadu_extra_has_param("creversible")) {
      siz.parse_string("Creversible=yes");
    }
    return;
  }
  if (!kakadu_extra_has_param("creversible")) {
    siz.parse_string("Creversible=no");
  }
  if (!kakadu_extra_has_param("qstep") && !kakadu_extra_has_param("qfactor")) {
    const double nominal_qstep = std::ldexp(1.0, -(precision + 5));
    const double grok_readable_floor = std::ldexp(1.0, -18);
    const double qstep = std::max(nominal_qstep, grok_readable_floor);
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "Qstep=%.17g", qstep);
    siz.parse_string(cmd);
  }
}

void apply_kakadu_precision_defaults(siz_params &siz, int precision) {
  const char *clevels_s = std::getenv("HDF5PLUGIN_JPEG2000_CLEVELS");
  if (precision == 32 && (clevels_s == nullptr || *clevels_s == '\0') &&
      !kakadu_extra_has_param("clevels")) {
    siz.parse_string("Clevels=3");
  }
}

void apply_kakadu_overrides(siz_params &siz) {
  const char *extra = std::getenv("HDF5PLUGIN_JPEG2000_KAKADU_PARAMS");
  if (extra != nullptr && *extra != '\0') {
    std::string s(extra);
    size_t start = 0;
    while (start < s.size()) {
      size_t end = s.find_first_of(";\n", start);
      if (end == std::string::npos) {
        end = s.size();
      }
      std::string tok = s.substr(start, end - start);
      size_t l = tok.find_first_not_of(" \t\r");
      size_t r = tok.find_last_not_of(" \t\r");
      if (l != std::string::npos && r != std::string::npos) {
        tok = tok.substr(l, r - l + 1);
      } else {
        tok.clear();
      }
      if (!tok.empty()) {
        siz.parse_string(tok.c_str());
      }
      start = end + 1;
    }
  }

  const char *clevels_s = std::getenv("HDF5PLUGIN_JPEG2000_CLEVELS");
  if (clevels_s != nullptr && *clevels_s != '\0') {
    char *end = nullptr;
    long clevels = std::strtol(clevels_s, &end, 10);
    if (end != clevels_s && end && *end == '\0') {
      if (clevels < 0) {
        clevels = 0;
      } else if (clevels > 32) {
        clevels = 32;
      }
      std::string cmd = "Clevels=" + std::to_string(clevels);
      siz.parse_string(cmd.c_str());
    }
  }
}

bool has_jp2_signature(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 12) {
    return false;
  }
  static const uint8_t sig[12] = {
      0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
  return std::memcmp(data, sig, sizeof(sig)) == 0;
}

int kakadu_available() { return 1; }

} // namespace

extern "C" int h5z_jpeg2000_kakadu_compress(
    size_t input_nbytes, void *input_buffer, unsigned int width,
    unsigned int height, unsigned int ncomps, unsigned int dtype,
    float compression_ratio, size_t *output_nbytes, void **output_buffer) {
  if (!input_buffer || !output_nbytes || !output_buffer) {
    return -1;
  }
  *output_nbytes = 0;
  *output_buffer = nullptr;

  const bool debug = debug_enabled();
  ensure_kakadu_handlers(debug);
  const KakaduTune tune = get_kakadu_tune();

  DtypeInfo di;
  if (!dtype_info(dtype, di)) {
    return -1;
  }
  if (!(ncomps == 1 || ncomps == 3)) {
    return -1;
  }
  const size_t expected = static_cast<size_t>(width) * height * ncomps * di.size;
  if (expected != input_nbytes) {
    return -1;
  }

  siz_params siz;
  if (!set_siz_params(siz, static_cast<int>(width), static_cast<int>(height),
                      static_cast<int>(ncomps), di)) {
    return -1;
  }

  MemTarget target;
  kdu_codestream codestream;
  try {
    std::unique_ptr<cod_params> cod_holder;
    if (siz.access_cluster(COD_params) == nullptr) {
      cod_holder.reset(new cod_params());
      cod_holder->link(&siz, -1, -1, 0, 0);
    }
    apply_kakadu_mode(siz);
    siz.finalize_all();
    codestream.create(&siz, &target);

    const bool rate_controlled = compression_ratio > 1.0f;
    apply_kakadu_rate_defaults(*codestream.access_siz(), di.precision, rate_controlled);
    apply_kakadu_precision_defaults(*codestream.access_siz(), di.precision);
    apply_kakadu_overrides(*codestream.access_siz());
    codestream.access_siz()->finalize_all();

    kdu_stripe_compressor compressor;
    ThreadEnvGuard thread_env;
    thread_env.setup(tune.threads);
    kdu_thread_env *env_ptr = thread_env.ptr();
    if (rate_controlled) {
      kdu_long layer_size = static_cast<kdu_long>(
          static_cast<double>(input_nbytes) / static_cast<double>(compression_ratio));
      compressor.start(codestream, 1, &layer_size, nullptr, 0, false,
                       tune.force_precise, true, 0.0, 0, tune.want_fastest, env_ptr);
    } else {
      compressor.start(codestream, 0, nullptr, nullptr, 0, false,
                       tune.force_precise, true, 0.0, 0, tune.want_fastest, env_ptr);
    }

    std::vector<int> stripe_heights(ncomps, static_cast<int>(height));
    std::vector<int> sample_gaps(ncomps, static_cast<int>(ncomps));
    std::vector<int> row_gaps(ncomps, static_cast<int>(width * ncomps));
    std::vector<int> precisions(ncomps, di.precision);
    std::unique_ptr<bool[]> is_signed(new bool[ncomps]);
    for (unsigned int c = 0; c < ncomps; ++c) {
      is_signed[c] = di.is_signed;
    }

    bool needs_more = false;
    if (di.size == 1 && !di.is_signed) {
      std::vector<kdu_byte *> stripe_bufs(ncomps);
      for (unsigned int c = 0; c < ncomps; ++c) {
        stripe_bufs[c] = reinterpret_cast<kdu_byte *>(input_buffer) + c;
      }
      needs_more = compressor.push_stripe(stripe_bufs.data(), stripe_heights.data(),
                                          sample_gaps.data(), row_gaps.data(),
                                          precisions.data());
    } else if (di.size == 1 && di.is_signed) {
      const int8_t *input = reinterpret_cast<const int8_t *>(input_buffer);
      std::vector<kdu_int16> tmp(static_cast<size_t>(width) * height * ncomps);
      for (size_t i = 0; i < tmp.size(); ++i) {
        tmp[i] = static_cast<kdu_int16>(input[i]);
      }
      needs_more = compressor.push_stripe(tmp.data(), stripe_heights.data(), nullptr,
                                          nullptr, nullptr, precisions.data(),
                                          is_signed.get());
    } else if (di.size == 2) {
      needs_more = compressor.push_stripe(reinterpret_cast<kdu_int16 *>(input_buffer),
                                          stripe_heights.data(), nullptr, nullptr,
                                          nullptr, precisions.data(), is_signed.get());
    } else if (di.size == 4 && di.is_signed) {
      needs_more = compressor.push_stripe(reinterpret_cast<kdu_int32 *>(input_buffer),
                                          stripe_heights.data(), nullptr, nullptr,
                                          nullptr, precisions.data(), is_signed.get());
    } else if (di.size == 4) {
      const uint32_t *input32 = reinterpret_cast<const uint32_t *>(input_buffer);
      std::vector<kdu_int32> tmp(static_cast<size_t>(width) * height * ncomps);
      constexpr int64_t kBias32 = int64_t{1} << 31;
      for (size_t i = 0; i < tmp.size(); ++i) {
        tmp[i] = static_cast<kdu_int32>(static_cast<int64_t>(input32[i]) - kBias32);
      }
      needs_more = compressor.push_stripe(tmp.data(), stripe_heights.data(), nullptr,
                                          nullptr, nullptr, precisions.data(),
                                          is_signed.get());
    }
    (void)needs_more;

    compressor.finish();
    codestream.destroy();
  } catch (kdu_exception) {
    if (debug) {
      fprintf(stderr, "[hdf5plugin/jpeg2000] Kakadu encoder exception\n");
    }
    return -1;
  }

  void *out = std::malloc(target.data.size());
  if (out == nullptr) {
    return -1;
  }
  std::memcpy(out, target.data.data(), target.data.size());
  *output_buffer = out;
  *output_nbytes = target.data.size();
  return 0;
}

extern "C" int h5z_jpeg2000_kakadu_decompress(size_t compressed_nbytes,
                                               void *compressed_buffer,
                                               size_t *output_nbytes,
                                               void **output_buffer) {
  if (!compressed_buffer || compressed_nbytes == 0 || !output_nbytes || !output_buffer) {
    return -1;
  }
  *output_nbytes = 0;
  *output_buffer = nullptr;

  const bool debug = debug_enabled();
  ensure_kakadu_handlers(debug);
  const KakaduTune tune = get_kakadu_tune();

  kdu_compressed_source_buffered source;
  source.open(reinterpret_cast<kdu_byte *>(compressed_buffer), compressed_nbytes);

  kdu_codestream codestream;
  jp2_family_src family;
  jp2_source jp2;

  try {
    const bool is_jp2 = has_jp2_signature(reinterpret_cast<const uint8_t *>(compressed_buffer),
                                          compressed_nbytes);
    if (is_jp2) {
      family.open(&source);
      jp2.open(&family);
      int hdr = jp2.read_header(true);
      if (hdr <= 0) {
        return -1;
      }
      codestream.create(&jp2);
    } else {
      codestream.create(&source);
    }

    int num_comps = codestream.get_num_components(true);
    if (!(num_comps == 1 || num_comps == 3)) {
      codestream.destroy();
      return -1;
    }

    kdu_dims dims;
    codestream.get_dims(0, dims, true);
    int width = dims.size.x;
    int height = dims.size.y;
    for (int c = 1; c < num_comps; ++c) {
      kdu_dims cdims;
      codestream.get_dims(c, cdims, true);
      if (cdims.size.x != width || cdims.size.y != height) {
        codestream.destroy();
        return -1;
      }
    }

    int precision = codestream.get_bit_depth(0, true);
    bool is_signed = codestream.get_signed(0, true);
    for (int c = 1; c < num_comps; ++c) {
      if (codestream.get_bit_depth(c, true) != precision ||
          codestream.get_signed(c, true) != is_signed) {
        codestream.destroy();
        return -1;
      }
    }
    if (!(precision == 8 || precision == 16 || precision == 32)) {
      codestream.destroy();
      return -1;
    }

    int typesize = (precision <= 8) ? 1 : ((precision <= 16) ? 2 : 4);
    size_t expected = static_cast<size_t>(width) * height * num_comps * typesize;
    void *out = std::malloc(expected);
    if (out == nullptr) {
      codestream.destroy();
      return -1;
    }

    static thread_local kdu_stripe_decompressor *tls_decompressor =
        new kdu_stripe_decompressor();
    kdu_stripe_decompressor &decompressor = *tls_decompressor;
    ThreadEnvGuard thread_env;
    thread_env.setup(tune.threads);
    decompressor.start(codestream, tune.force_precise, tune.want_fastest, thread_env.ptr());

    std::vector<int> stripe_heights(num_comps, 0);
    std::vector<int> precisions(num_comps, precision);
    std::unique_ptr<bool[]> is_signed_flags(new bool[num_comps]);
    for (int c = 0; c < num_comps; ++c) {
      is_signed_flags[c] = is_signed;
    }

    int requested_stripe_height = env_int("HDF5PLUGIN_JPEG2000_KAKADU_DECODE_STRIPE_HEIGHT", 128);
    int max_stripe_height =
        (requested_stripe_height > 0 && requested_stripe_height < height) ? requested_stripe_height : height;
    int rows_done = 0;
    bool needs_more = true;

    std::vector<kdu_int16> signed8;
    std::vector<kdu_uint16> decoded16;
    std::vector<kdu_int32> decoded32;
    if (typesize == 1 && is_signed) {
      signed8.resize(static_cast<size_t>(width) * height * num_comps);
    } else if (typesize == 2 && !is_signed) {
      decoded16.resize(expected / typesize);
    } else if (typesize == 4) {
      decoded32.resize(expected / typesize);
    }

    while (rows_done < height) {
      int rows = height - rows_done;
      if (rows > max_stripe_height) {
        rows = max_stripe_height;
      }
      for (int c = 0; c < num_comps; ++c) {
        stripe_heights[c] = rows;
      }

      size_t row_offset = static_cast<size_t>(rows_done) * width * num_comps;
      if (typesize == 1 && !is_signed) {
        uint8_t *stripe_output = reinterpret_cast<uint8_t *>(out) + row_offset;
        needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                              nullptr, nullptr, nullptr,
                                              precisions.data());
      } else if (typesize == 1) {
        kdu_int16 *stripe_output = signed8.data() + row_offset;
        needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                              nullptr, nullptr, nullptr,
                                              precisions.data(), is_signed_flags.get());
      } else if (typesize == 2 && is_signed) {
        kdu_int16 *stripe_output = reinterpret_cast<kdu_int16 *>(out) + row_offset;
        needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                              nullptr, nullptr, nullptr,
                                              precisions.data(), is_signed_flags.get());
      } else if (typesize == 2) {
        kdu_int16 *stripe_output = reinterpret_cast<kdu_int16 *>(decoded16.data()) + row_offset;
        needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                              nullptr, nullptr, nullptr,
                                              precisions.data(), is_signed_flags.get());
      } else {
        kdu_int32 *stripe_output = decoded32.data() + row_offset;
        needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                              nullptr, nullptr, nullptr,
                                              precisions.data(), is_signed_flags.get());
      }
      rows_done += rows;
      if (!needs_more) {
        break;
      }
    }

    if (rows_done != height) {
      std::free(out);
      decompressor.reset();
      codestream.destroy();
      return -1;
    }
    decompressor.reset();

    if (typesize == 1 && is_signed) {
      int8_t *dst = reinterpret_cast<int8_t *>(out);
      for (size_t i = 0; i < signed8.size(); ++i) {
        dst[i] = static_cast<int8_t>(signed8[i]);
      }
    } else if (typesize == 2 && !is_signed) {
      std::memcpy(out, decoded16.data(), expected);
    } else if (typesize == 4 && is_signed) {
      std::memcpy(out, decoded32.data(), expected);
    } else if (typesize == 4) {
      uint32_t *dst = reinterpret_cast<uint32_t *>(out);
      constexpr int64_t kBias32 = int64_t{1} << 31;
      for (size_t i = 0; i < decoded32.size(); ++i) {
        dst[i] = static_cast<uint32_t>(static_cast<int64_t>(decoded32[i]) + kBias32);
      }
    }

    codestream.destroy();
    if (is_jp2) {
      jp2.close();
      family.close();
    }

    *output_buffer = out;
    *output_nbytes = expected;
    return 0;
  } catch (kdu_exception) {
    if (debug) {
      fprintf(stderr, "[hdf5plugin/jpeg2000] Kakadu decoder exception\n");
    }
    return -1;
  }
}

extern "C" const h5z_jpeg2000_backend_t h5z_jpeg2000_kakadu_backend = {
    "kakadu",
    kakadu_available,
    h5z_jpeg2000_kakadu_compress,
    h5z_jpeg2000_kakadu_decompress,
};

#endif
