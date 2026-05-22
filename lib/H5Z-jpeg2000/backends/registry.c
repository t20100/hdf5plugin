#include "H5Zjpeg2000_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MANIFEST_BACKENDS 16
#define MAX_BACKEND_NAME_LEN 64

static const h5z_jpeg2000_backend_t *const available_backends[] = {
    &h5z_jpeg2000_openjpeg_backend,
    NULL,
};

typedef struct {
  char names[MAX_MANIFEST_BACKENDS][MAX_BACKEND_NAME_LEN];
  int count;
  char error[256];
} manifest_priority_t;

static const h5z_jpeg2000_backend_t *find_backend(const char *name) {
  for (int i = 0; available_backends[i] != NULL; i++) {
    const h5z_jpeg2000_backend_t *backend = available_backends[i];
    if (strcmp(name, backend->name) == 0) {
      return backend;
    }
  }
  return NULL;
}

static const h5z_jpeg2000_backend_t *first_available_backend(void) {
  for (int i = 0; available_backends[i] != NULL; i++) {
    const h5z_jpeg2000_backend_t *backend = available_backends[i];
    if (backend->available()) {
      return backend;
    }
  }
  return NULL;
}

static void skip_json_ws(const char *text, size_t len, size_t *pos) {
  while (*pos < len && (text[*pos] == ' ' || text[*pos] == '\n' ||
                        text[*pos] == '\r' || text[*pos] == '\t')) {
    (*pos)++;
  }
}

static int parse_json_string_at(const char *text, size_t len, size_t *pos,
                                char *out, size_t out_size) {
  size_t out_pos = 0;
  skip_json_ws(text, len, pos);
  if (*pos >= len || text[*pos] != '"') {
    return 0;
  }
  (*pos)++;
  while (*pos < len) {
    char c = text[(*pos)++];
    if (c == '"') {
      if (out_size > 0) {
        out[out_pos < out_size ? out_pos : out_size - 1] = '\0';
      }
      return 1;
    }
    if (c == '\\') {
      if (*pos >= len) {
        return 0;
      }
      c = text[(*pos)++];
      switch (c) {
      case '"':
      case '\\':
      case '/':
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      default:
        return 0;
      }
    }
    if (out_pos + 1 < out_size) {
      out[out_pos++] = c;
    }
  }
  return 0;
}

static size_t find_json_key(const char *text, size_t len, const char *key) {
  char quoted_key[128];
  int written = snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
  if (written <= 0 || (size_t)written >= sizeof(quoted_key)) {
    return (size_t)-1;
  }

  const size_t key_len = (size_t)written;
  for (size_t pos = 0; pos + key_len <= len; pos++) {
    if (memcmp(text + pos, quoted_key, key_len) != 0) {
      continue;
    }
    size_t after_key = pos + key_len;
    skip_json_ws(text, len, &after_key);
    if (after_key < len && text[after_key] == ':') {
      return after_key + 1;
    }
  }
  return (size_t)-1;
}

static int parse_json_string_field(const char *text, size_t len, const char *key,
                                   char *out, size_t out_size) {
  size_t pos = find_json_key(text, len, key);
  if (pos == (size_t)-1) {
    return 0;
  }
  return parse_json_string_at(text, len, &pos, out, out_size);
}

static int append_manifest_backend(manifest_priority_t *priority,
                                   const char *name) {
  if (name[0] == '\0') {
    return 1;
  }
  if (priority->count >= MAX_MANIFEST_BACKENDS) {
    snprintf(priority->error, sizeof(priority->error),
             "too many backend entries in jpeg2000 manifest");
    return 0;
  }
  snprintf(priority->names[priority->count], MAX_BACKEND_NAME_LEN, "%s", name);
  priority->count++;
  return 1;
}

static int parse_json_string_array_field(const char *text, size_t len,
                                         const char *key,
                                         manifest_priority_t *priority) {
  size_t pos = find_json_key(text, len, key);
  if (pos == (size_t)-1) {
    return 1;
  }
  skip_json_ws(text, len, &pos);
  if (pos >= len || text[pos] != '[') {
    snprintf(priority->error, sizeof(priority->error),
             "manifest field '%s' is not an array", key);
    return 0;
  }
  pos++;
  while (pos < len) {
    skip_json_ws(text, len, &pos);
    if (pos < len && text[pos] == ']') {
      return 1;
    }
    char value[MAX_BACKEND_NAME_LEN] = {0};
    if (!parse_json_string_at(text, len, &pos, value, sizeof(value))) {
      snprintf(priority->error, sizeof(priority->error),
               "manifest field '%s' contains a non-string entry", key);
      return 0;
    }
    if (!append_manifest_backend(priority, value)) {
      return 0;
    }
    skip_json_ws(text, len, &pos);
    if (pos < len && text[pos] == ',') {
      pos++;
      continue;
    }
    if (pos < len && text[pos] == ']') {
      return 1;
    }
    snprintf(priority->error, sizeof(priority->error),
             "manifest field '%s' has invalid array syntax", key);
    return 0;
  }
  snprintf(priority->error, sizeof(priority->error),
           "manifest field '%s' has an unterminated array", key);
  return 0;
}

static char *read_text_file(const char *path, size_t *len) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *text = (char *)malloc((size_t)size + 1);
  if (text == NULL) {
    fclose(file);
    return NULL;
  }
  size_t read_len = fread(text, 1, (size_t)size, file);
  fclose(file);
  text[read_len] = '\0';
  *len = read_len;
  return text;
}

static int load_manifest_priority(manifest_priority_t *priority) {
  const char *path = getenv(H5Z_JPEG2000_MANIFEST_ENV);
  if (path == NULL || path[0] == '\0') {
    return 1;
  }

  size_t len = 0;
  char *text = read_text_file(path, &len);
  if (text == NULL) {
    snprintf(priority->error, sizeof(priority->error),
             "could not open jpeg2000 manifest '%s'", path);
    return 0;
  }

  char backend[MAX_BACKEND_NAME_LEN] = {0};
  if (parse_json_string_field(text, len, "backend", backend, sizeof(backend))) {
    if (!append_manifest_backend(priority, backend)) {
      free(text);
      return 0;
    }
  }

  int ok = parse_json_string_array_field(text, len, "jpeg2000", priority);
  free(text);
  return ok;
}

static const h5z_jpeg2000_backend_t *select_named_backend(const char *name) {
  const h5z_jpeg2000_backend_t *backend = find_backend(name);
  if (backend == NULL) {
    fprintf(stderr, "Unsupported jpeg2000 backend '%s'\n", name);
    return NULL;
  }
  if (!backend->available()) {
    fprintf(stderr, "Requested jpeg2000 backend '%s' is not available\n", name);
    return NULL;
  }
  return backend;
}

static const h5z_jpeg2000_backend_t *select_manifest_backend(void) {
  manifest_priority_t priority;
  memset(&priority, 0, sizeof(priority));
  if (!load_manifest_priority(&priority)) {
    fprintf(stderr, "Invalid jpeg2000 manifest: %s\n", priority.error);
    return NULL;
  }
  for (int i = 0; i < priority.count; i++) {
    const h5z_jpeg2000_backend_t *backend = find_backend(priority.names[i]);
    if (backend != NULL && backend->available()) {
      return backend;
    }
  }
  return NULL;
}

const h5z_jpeg2000_backend_t *h5z_jpeg2000_select_backend(void) {
  const char *requested = getenv(H5Z_JPEG2000_BACKEND_ENV);

  if (requested != NULL && requested[0] != '\0' && strcmp(requested, "auto") != 0) {
    return select_named_backend(requested);
  }

  const h5z_jpeg2000_backend_t *backend = select_manifest_backend();
  if (backend != NULL) {
    return backend;
  }

  backend = first_available_backend();
  if (backend == NULL) {
    fprintf(stderr, "No jpeg2000 backend is available\n");
  }
  return backend;
}

int h5z_jpeg2000_compress(size_t input_nbytes, void *input_buffer,
                          unsigned int width, unsigned int height,
                          unsigned int ncomps, unsigned int dtype,
                          float compression_ratio, size_t *output_nbytes,
                          void **output_buffer) {
  const h5z_jpeg2000_backend_t *backend = h5z_jpeg2000_select_backend();
  if (backend == NULL) {
    return -1;
  }
  return backend->compress(input_nbytes, input_buffer, width, height, ncomps,
                           dtype, compression_ratio, output_nbytes,
                           output_buffer);
}

int h5z_jpeg2000_decompress(size_t compressed_nbytes, void *compressed_buffer,
                            size_t *output_nbytes, void **output_buffer) {
  const h5z_jpeg2000_backend_t *backend = h5z_jpeg2000_select_backend();
  if (backend == NULL) {
    return -1;
  }
  return backend->decompress(compressed_nbytes, compressed_buffer, output_nbytes,
                             output_buffer);
}
