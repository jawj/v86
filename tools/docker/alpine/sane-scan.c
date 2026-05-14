/*
 * Copyright 2026 George MacKerron
 * Released under GPL v3: https://opensource.org/license/gpl-3.0
 * 
 * sane-scan: long-lived SANE daemon
 *
 * Holds one SANE_Handle for the lifetime of the process. Reads commands
 * line-by-line from stdin; writes one response line per command on
 * stdout. JSON arrays (from `options` / `set`) never contain raw newlines
 * (newlines in string values are escaped to "\n"), so newline alone is a
 * sufficient response delimiter.
 *
 * Usage: sane-scan
 *
 * On startup, calls sane_init + sane_get_devices, opens the first device, and
 * writes a one-line JSON banner to stdout describing it:
 *   {"device":"...","vendor":"...","model":"...","type":"..."}
 * On failure before the loop starts, writes "ERR ..." instead and exits.
 * The banner doubles as the READY signal — JS reads it before sending commands.
 *
 * Commands (one per line):
 *   options                              Dump JSON array of all options.
 *   set <name> <value>                   Set one option. Value:
 *                                          - bool: "true" / "false"
 *                                          - int / fixed: number, or
 *                                            comma-separated for arrays
 *                                          - string: rest-of-line, raw
 *                                        Response: updated options JSON.
 *   scan [<tlx> <tly> <brx> <bry>]       If 4 numbers given, set tl-x/y, br-x/y
 *                                        (mm). Then sane_start, write 12-byte
 *                                        header + raw pixels to /dev/hvc0,
 *                                        sane_cancel. Stdout: "OK" or "ERR".
 *   preview                              Snapshot all settable+active option
 *                                        values, set preview=true, scan,
 *                                        sane_cancel, set preview=false,
 *                                        restore snapshot. Stdout: "OK"/"ERR".
 *   exit                                 Clean shutdown.
 *
 * Scan output format on /dev/hvc0:
 *   bytes 0-3:   width        (uint32 LE)
 *   bytes 4-7:   height       (uint32 LE, 0 = unknown)
 *   byte  8:     depth        (uint8: 1, 8, or 16)
 *   byte  9:     channels     (uint8: 1 or 3)
 *   byte  10:    little_endian (uint8: 1 = LE pixel data, 0 = BE)
 *   byte  11:    reserved     (0)
 *   bytes 12+:   raw pixel data
 *
 * Compile: gcc -o sane-scan sane-scan.c -lsane -s -march=i686 -Os -flto
 */

#include <sane/sane.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ── JSON output helpers ───────────────────────────────────────────── */

static void json_string(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    switch (*s) {
    case '"':  fputs("\\\"", f); break;
    case '\\': fputs("\\\\", f); break;
    case '\n': fputs("\\n", f);  break;
    case '\r': fputs("\\r", f);  break;
    case '\t': fputs("\\t", f);  break;
    default:   fputc(*s, f);
    }
  }
  fputc('"', f);
}

static double fixed_to_double(SANE_Fixed v) {
  return v / (double)(1 << SANE_FIXED_SCALE_SHIFT);
}

static SANE_Fixed double_to_fixed(double v) {
  return (SANE_Fixed)(v * (1 << SANE_FIXED_SCALE_SHIFT) + 0.5);
}

static void fprint_number(FILE *f, SANE_Value_Type type, SANE_Word w) {
  if (type == SANE_TYPE_FIXED)
    fprintf(f, "%g", fixed_to_double(w));
  else
    fprintf(f, "%d", w);
}

/* ── option lookup ─────────────────────────────────────────────────── */

static int find_option(SANE_Handle h, int n, const char *name) {
  for (int i = 1; i < n; i++) {
    const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, i);
    if (d && d->name && strcmp(d->name, name) == 0)
      return i;
  }
  return -1;
}

static int get_option_count(SANE_Handle h) {
  SANE_Int n;
  if (sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &n, NULL)
      != SANE_STATUS_GOOD)
    return 0;
  return n;
}

/* ── single-option SET ─────────────────────────────────────────────── */

/* parse "name value" and apply. value semantics depend on option type.
 * writes any error to stdout (so it lands in the JS-side response).
 * returns 0 on success, -1 on failure. */
static int apply_one_option(SANE_Handle h, const char *line) {
  const char *space = strchr(line, ' ');
  if (!space) {
    fprintf(stderr, "sane-scan: set: missing value\n");
    return -1;
  }
  size_t name_len = space - line;
  if (name_len == 0 || name_len >= 256) {
    fprintf(stderr, "sane-scan: set: bad name\n");
    return -1;
  }
  char name[256];
  memcpy(name, line, name_len);
  name[name_len] = 0;
  const char *value = space + 1;

  int n = get_option_count(h);
  int idx = find_option(h, n, name);
  if (idx < 0) {
    fprintf(stderr, "sane-scan: set: unknown option '%s'\n", name);
    return -1;
  }
  const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, idx);
  if (!SANE_OPTION_IS_ACTIVE(d->cap)) {
    fprintf(stderr, "sane-scan: set: '%s' is inactive\n", name);
    return -1;
  }
  if (!SANE_OPTION_IS_SETTABLE(d->cap)) {
    fprintf(stderr, "sane-scan: set: '%s' is not settable\n", name);
    return -1;
  }

  SANE_Status st;
  SANE_Int info = 0;

  if (d->type == SANE_TYPE_BOOL) {
    SANE_Bool v = (strcmp(value, "true") == 0) ? SANE_TRUE : SANE_FALSE;
    st = sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &v, &info);

  } else if (d->type == SANE_TYPE_INT || d->type == SANE_TYPE_FIXED) {
    int count = d->size / sizeof(SANE_Word);
    SANE_Word *words = malloc(count * sizeof(SANE_Word));
    if (!words) { fprintf(stderr, "sane-scan: set: oom\n"); return -1; }
    int got = 0;
    const char *p = value;
    while (got < count && *p) {
      char *end;
      double dv = strtod(p, &end);
      if (end == p) break;
      words[got++] = (d->type == SANE_TYPE_FIXED) ? double_to_fixed(dv) : (SANE_Word)dv;
      p = end;
      while (*p == ',' || *p == ' ') p++;
    }
    if (got != count) {
      fprintf(stderr, "sane-scan: set: '%s' expected %d values, got %d\n", name, count, got);
      free(words);
      return -1;
    }
    st = sane_control_option(h, idx, SANE_ACTION_SET_VALUE, words, &info);
    free(words);

  } else if (d->type == SANE_TYPE_STRING) {
    char val[1024];
    snprintf(val, sizeof val, "%s", value);
    st = sane_control_option(h, idx, SANE_ACTION_SET_VALUE, val, &info);

  } else {
    fprintf(stderr, "sane-scan: set: '%s' has unsupported type\n", name);
    return -1;
  }

  if (st != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: set '%s': %s\n", name, sane_strstatus(st));
    return -1;
  }
  return 0;
}

/* ── option dump ───────────────────────────────────────────────────── */

static void print_constraint(FILE *f, const SANE_Option_Descriptor *d) {
  int i;
  switch (d->constraint_type) {
  case SANE_CONSTRAINT_RANGE:
    fprintf(f, "\"constraint\":{\"type\":\"range\",\"min\":");
    fprint_number(f, d->type, d->constraint.range->min);
    fprintf(f, ",\"max\":");
    fprint_number(f, d->type, d->constraint.range->max);
    fprintf(f, ",\"step\":");
    fprint_number(f, d->type, d->constraint.range->quant);
    fputc('}', f);
    break;
  case SANE_CONSTRAINT_WORD_LIST:
    fprintf(f, "\"constraint\":{\"type\":\"list\",\"values\":[");
    for (i = 1; i <= d->constraint.word_list[0]; i++) {
      if (i > 1) fputc(',', f);
      fprint_number(f, d->type, d->constraint.word_list[i]);
    }
    fputs("]}", f);
    break;
  case SANE_CONSTRAINT_STRING_LIST:
    fprintf(f, "\"constraint\":{\"type\":\"list\",\"values\":[");
    for (i = 0; d->constraint.string_list[i]; i++) {
      if (i > 0) fputc(',', f);
      json_string(f, d->constraint.string_list[i]);
    }
    fputs("]}", f);
    break;
  default:
    fprintf(f, "\"constraint\":null");
  }
}

static void print_value(FILE *f, SANE_Handle h, int idx,
                        const SANE_Option_Descriptor *d) {
  if (d->type == SANE_TYPE_BUTTON || d->type == SANE_TYPE_GROUP ||
      !SANE_OPTION_IS_ACTIVE(d->cap)) {
    fprintf(f, "\"value\":null");
    return;
  }
  if (d->type == SANE_TYPE_STRING) {
    char buf[d->size];
    if (sane_control_option(h, idx, SANE_ACTION_GET_VALUE, buf, NULL)
        == SANE_STATUS_GOOD) {
      fprintf(f, "\"value\":");
      json_string(f, buf);
    } else {
      fprintf(f, "\"value\":null");
    }
  } else if (d->type == SANE_TYPE_BOOL) {
    SANE_Bool v;
    if (sane_control_option(h, idx, SANE_ACTION_GET_VALUE, &v, NULL)
        == SANE_STATUS_GOOD)
      fprintf(f, "\"value\":%s", v ? "true" : "false");
    else
      fprintf(f, "\"value\":null");
  } else {
    int count = d->size / sizeof(SANE_Word);
    SANE_Word vals[count];
    if (sane_control_option(h, idx, SANE_ACTION_GET_VALUE, vals, NULL)
        == SANE_STATUS_GOOD) {
      if (count == 1) {
        fprintf(f, "\"value\":");
        fprint_number(f, d->type, vals[0]);
      } else {
        fprintf(f, "\"value\":[");
        for (int i = 0; i < count; i++) {
          if (i > 0) fputc(',', f);
          fprint_number(f, d->type, vals[i]);
        }
        fputc(']', f);
      }
    } else {
      fprintf(f, "\"value\":null");
    }
  }
}

static int dump_options(SANE_Handle h, FILE *out) {
  int n = get_option_count(h);
  if (!n) return -1;

  int first = 1;
  fputc('[', out);
  for (int i = 1; i < n; i++) {
    const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, i);
    if (!d || !d->name || !d->name[0])
      continue;
    if (!first) fputc(',', out);
    first = 0;

    fprintf(out, "{\"name\":");
    json_string(out, d->name);
    fprintf(out, ",\"title\":");
    json_string(out, d->title ? d->title : d->name);
    fprintf(out, ",\"type\":\"%s\"",
            d->type == SANE_TYPE_BOOL   ? "bool" :
            d->type == SANE_TYPE_INT    ? "int" :
            d->type == SANE_TYPE_FIXED  ? "fixed" :
            d->type == SANE_TYPE_STRING ? "string" :
            d->type == SANE_TYPE_BUTTON ? "button" : "group");
    fprintf(out, ",\"unit\":\"%s\"",
            d->unit == SANE_UNIT_PIXEL       ? "pixel" :
            d->unit == SANE_UNIT_BIT         ? "bit" :
            d->unit == SANE_UNIT_MM          ? "mm" :
            d->unit == SANE_UNIT_DPI         ? "dpi" :
            d->unit == SANE_UNIT_PERCENT     ? "percent" :
            d->unit == SANE_UNIT_MICROSECOND ? "us" : "none");
    if (d->type == SANE_TYPE_INT || d->type == SANE_TYPE_FIXED)
      fprintf(out, ",\"size\":%d", (int)(d->size / sizeof(SANE_Word)));
    else if (d->type == SANE_TYPE_STRING)
      fprintf(out, ",\"size\":%d", (int)d->size);
    fprintf(out, ",\"active\":%s",
            SANE_OPTION_IS_ACTIVE(d->cap) ? "true" : "false");
    fprintf(out, ",\"settable\":%s",
            SANE_OPTION_IS_SETTABLE(d->cap) ? "true" : "false");
    fputc(',', out);
    print_constraint(out, d);
    fputc(',', out);
    print_value(out, h, i, d);
    fputc('}', out);
  }
  fputs("]\n", out);
  return 0;
}

/* ── snapshot / restore for preview ────────────────────────────────── */

typedef struct {
  int idx;
  size_t size;
  void *buf;
} SavedOpt;

static SavedOpt *snapshot_options(SANE_Handle h, int *out_count) {
  *out_count = 0;
  int n = get_option_count(h);
  if (n <= 0) return NULL;

  SavedOpt *list = malloc(sizeof(SavedOpt) * n);
  if (!list) return NULL;

  int count = 0;
  for (int i = 1; i < n; i++) {
    const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, i);
    if (!d || !d->name || !d->name[0]) continue;
    if (d->type == SANE_TYPE_BUTTON || d->type == SANE_TYPE_GROUP) continue;
    if (!SANE_OPTION_IS_ACTIVE(d->cap) || !SANE_OPTION_IS_SETTABLE(d->cap)) continue;
    /* skip preview itself: we'll explicitly toggle it */
    if (strcmp(d->name, "preview") == 0) continue;

    void *buf = malloc(d->size);
    if (!buf) continue;
    if (sane_control_option(h, i, SANE_ACTION_GET_VALUE, buf, NULL) == SANE_STATUS_GOOD) {
      list[count].idx = i;
      list[count].size = d->size;
      list[count].buf = buf;
      count++;
    } else {
      free(buf);
    }
  }
  *out_count = count;
  return list;
}

static void restore_options(SANE_Handle h, SavedOpt *list, int count) {
  for (int i = 0; i < count; i++) {
    /* skip if option became inactive (very rare after preview=false) */
    const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, list[i].idx);
    if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || !SANE_OPTION_IS_SETTABLE(d->cap)) continue;
    SANE_Int info;
    SANE_Status st = sane_control_option(h, list[i].idx, SANE_ACTION_SET_VALUE,
                                         list[i].buf, &info);
    if (st != SANE_STATUS_GOOD) {
      fprintf(stderr, "sane-scan: restore '%s' failed: %s\n",
              d->name ? d->name : "?", sane_strstatus(st));
    }
  }
}

static void free_snapshot(SavedOpt *list, int count) {
  if (!list) return;
  for (int i = 0; i < count; i++) free(list[i].buf);
  free(list);
}

/* ── one-shot helpers for preview/scan flow ────────────────────────── */

static SANE_Status set_bool_option(SANE_Handle h, const char *name, SANE_Bool v) {
  int n = get_option_count(h);
  int idx = find_option(h, n, name);
  if (idx < 0) return SANE_STATUS_INVAL;
  SANE_Int info;
  return sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &v, &info);
}

static SANE_Status set_geometry_value(SANE_Handle h, const char *name, double mm) {
  int n = get_option_count(h);
  int idx = find_option(h, n, name);
  if (idx < 0) return SANE_STATUS_INVAL;
  const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, idx);
  SANE_Word w = (d->type == SANE_TYPE_FIXED) ? double_to_fixed(mm) : (SANE_Word)mm;
  SANE_Int info;
  return sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &w, &info);
}

/* Set "resolution" to the lowest value the backend reports as supported.
   Used for previews: the user's chosen DPI is irrelevant for an on-screen
   thumbnail, and at high DPI a preview can take many seconds. */
static void minimize_resolution(SANE_Handle h) {
  int n = get_option_count(h);
  int idx = find_option(h, n, "resolution");
  if (idx < 0) return;
  const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, idx);
  if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || !SANE_OPTION_IS_SETTABLE(d->cap)) return;
  if (d->type != SANE_TYPE_INT && d->type != SANE_TYPE_FIXED) return;

  SANE_Word w;
  if (d->constraint_type == SANE_CONSTRAINT_RANGE && d->constraint.range) {
    w = d->constraint.range->min;
  } else if (d->constraint_type == SANE_CONSTRAINT_WORD_LIST && d->constraint.word_list) {
    int count = d->constraint.word_list[0];
    if (count <= 0) return;
    w = d->constraint.word_list[1];
    for (int i = 2; i <= count; i++)
      if (d->constraint.word_list[i] < w) w = d->constraint.word_list[i];
  } else {
    return;
  }

  SANE_Int info;
  SANE_Status st = sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &w, &info);
  if (st != SANE_STATUS_GOOD)
    fprintf(stderr, "sane-scan: failed to minimize resolution: %s\n", sane_strstatus(st));
}

/* SANE has no "reset geometry" call; the convention is to read each option's
   range constraint and write min (top-left) / max (bottom-right). */
static void maximize_geometry(SANE_Handle h) {
  static const struct { const char *name; int use_max; } geos[] = {
    {"tl-x", 0}, {"tl-y", 0}, {"br-x", 1}, {"br-y", 1},
  };
  int n = get_option_count(h);
  for (int g = 0; g < 4; g++) {
    int idx = find_option(h, n, geos[g].name);
    if (idx < 0) continue;
    const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, idx);
    if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || !SANE_OPTION_IS_SETTABLE(d->cap)) continue;
    if (d->constraint_type != SANE_CONSTRAINT_RANGE || !d->constraint.range) continue;
    SANE_Word w = geos[g].use_max ? d->constraint.range->max : d->constraint.range->min;
    SANE_Int info;
    SANE_Status st = sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &w, &info);
    if (st != SANE_STATUS_GOOD)
      fprintf(stderr, "sane-scan: failed to maximize %s: %s\n",
              geos[g].name, sane_strstatus(st));
  }
}

/* ── scan: write 12-byte header + binary data to /dev/hvc0 ─────────── */

static int do_scan(SANE_Handle h) {
  FILE *out = fopen("/dev/hvc0", "wb");
  if (!out) {
    fprintf(stderr, "sane-scan: cannot open /dev/hvc0\n");
    return -1;
  }

  /* /dev/hvc0 is a TTY; default line discipline has OPOST|ONLCR, which expands
     every 0x0A byte in the binary pixel stream to 0x0D 0x0A. At 16-bit depth the
     low byte cycles through all 256 values, so 0x0A occurs ~1/256 bytes and the
     row-shift garbles every scan. Switch to raw mode to pass bytes through. */
  int hvc_fd = fileno(out);
  struct termios t;
  if (tcgetattr(hvc_fd, &t) == 0) {
    cfmakeraw(&t);
    if (tcsetattr(hvc_fd, TCSANOW, &t) != 0)
      fprintf(stderr, "sane-scan: tcsetattr(/dev/hvc0) failed\n");
  }

  SANE_Status status = sane_start(h);
  if (status != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: sane_start: %s\n", sane_strstatus(status));
    sane_cancel(h);
    fclose(out);
    return -1;
  }

  SANE_Parameters params;
  status = sane_get_parameters(h, &params);
  if (status != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: sane_get_parameters: %s\n", sane_strstatus(status));
    sane_cancel(h);
    fclose(out);
    return -1;
  }

  uint8_t header[12];
  uint32_t width  = (uint32_t)params.pixels_per_line;
  uint32_t height = params.lines > 0 ? (uint32_t)params.lines : 0;
  uint8_t  depth  = (uint8_t)params.depth;
  uint8_t  channels;

  switch (params.format) {
  case SANE_FRAME_GRAY:  channels = 1; break;
  case SANE_FRAME_RGB:   channels = 3; break;
  default:
    fprintf(stderr, "sane-scan: unsupported frame format %d\n", params.format);
    sane_cancel(h);
    fclose(out);
    return -1;
  }

  memcpy(header + 0, &width, 4);  /* LE on x86 */
  memcpy(header + 4, &height, 4);
  header[8]  = depth;
  header[9]  = channels;
  header[10] = 1;                 /* little-endian (x86) */
  header[11] = 0;

  if (fwrite(header, 1, 12, out) != 12) {
    fprintf(stderr, "sane-scan: failed to write header\n");
    sane_cancel(h);
    fclose(out);
    return -1;
  }
  fflush(out);

  /* Backends may pad rows: bytes_per_line ≥ pixels_per_line × channels × depth/8.
     Read one row at a time and write only the meaningful prefix so the JS side's
     row-stride assumption matches the bytes it sees. */
  size_t out_row = (depth == 1)
    ? ((size_t)width + 7) / 8
    : (size_t)width * channels * (depth == 16 ? 2 : 1);
  size_t stride = params.bytes_per_line > 0 && (size_t)params.bytes_per_line >= out_row
    ? (size_t)params.bytes_per_line
    : out_row;

  uint8_t *row_buf = (uint8_t *)malloc(stride);
  if (!row_buf) {
    fprintf(stderr, "sane-scan: malloc(%zu) failed\n", stride);
    sane_cancel(h);
    fclose(out);
    return -1;
  }

  size_t pos = 0;
  SANE_Int bytes_read;
  while (1) {
    status = sane_read(h, row_buf + pos, (SANE_Int)(stride - pos), &bytes_read);
    if (status == SANE_STATUS_EOF) break;
    if (status != SANE_STATUS_GOOD) {
      fprintf(stderr, "sane-scan: sane_read: %s\n", sane_strstatus(status));
      free(row_buf);
      sane_cancel(h);
      fclose(out);
      return -1;
    }
    pos += (size_t)bytes_read;
    if (pos == stride) {
      if (fwrite(row_buf, 1, out_row, out) != out_row) {
        fprintf(stderr, "sane-scan: write error\n");
        free(row_buf);
        sane_cancel(h);
        fclose(out);
        return -1;
      }
      fflush(out);
      pos = 0;
    }
  }
  free(row_buf);

  /* SANE spec: cancel at the end of every scan, including a clean EOF, or
     some backends keep the device in a "scanning" state. */
  sane_cancel(h);
  fclose(out);
  return 0;
}

/* ── command handlers ──────────────────────────────────────────────── */

static int handle_scan_with_geometry(SANE_Handle h, const char *args) {
  double tlx, tly, brx, bry;
  int matched = sscanf(args, " %lf %lf %lf %lf", &tlx, &tly, &brx, &bry);
  if (matched == 4) {
    if (set_geometry_value(h, "tl-x", tlx) != SANE_STATUS_GOOD)
      fprintf(stderr, "sane-scan: failed to set tl-x = %g\n", tlx);
    if (set_geometry_value(h, "tl-y", tly) != SANE_STATUS_GOOD)
      fprintf(stderr, "sane-scan: failed to set tl-y = %g\n", tly);
    if (set_geometry_value(h, "br-x", brx) != SANE_STATUS_GOOD)
      fprintf(stderr, "sane-scan: failed to set br-x = %g\n", brx);
    if (set_geometry_value(h, "br-y", bry) != SANE_STATUS_GOOD)
      fprintf(stderr, "sane-scan: failed to set br-y = %g\n", bry);
  }
  return do_scan(h);
}

static int handle_preview(SANE_Handle h) {
  int snap_count = 0;
  SavedOpt *snap = snapshot_options(h, &snap_count);

  SANE_Status st = set_bool_option(h, "preview", SANE_TRUE);
  if (st != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: failed to set preview=true: %s\n", sane_strstatus(st));
    free_snapshot(snap, snap_count);
    return -1;
  }

  maximize_geometry(h);
  minimize_resolution(h);
  int ret = do_scan(h);

  /* always try to clear preview and restore the user's options, even if scan failed */
  set_bool_option(h, "preview", SANE_FALSE);
  restore_options(h, snap, snap_count);
  free_snapshot(snap, snap_count);

  return ret;
}

/* ── main: command loop ────────────────────────────────────────────── */

static void strip_eol(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = 0;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  /* line-buffered stdout: every '\n' auto-flushes, so a one-line response
     reaches the FIFO as soon as we finish writing it (no waiting on a
     subsequent fflush call). dump_options writes a single line so this is
     enough; we still fflush explicitly after the loop body for OK/ERR
     responses that may not end in a newline if something goes wrong. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  /* stderr is unbuffered by default on POSIX. */

  fprintf(stderr, "sane-scan: starting sane_init\n");
  SANE_Int version;
  if (sane_init(&version, NULL) != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: sane_init failed\n");
    fputs("ERR sane_init failed\n", stdout);
    return 1;
  }
  fprintf(stderr, "sane-scan: sane_init OK, getting devices\n");

  /* local_only=TRUE: skip network backends; the scanner is local via USB/IP.
     SANE owns the returned array; do NOT free it. */
  const SANE_Device **devs = NULL;
  SANE_Status status = sane_get_devices(&devs, SANE_TRUE);
  if (status != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: sane_get_devices: %s\n", sane_strstatus(status));
    fprintf(stdout, "ERR sane_get_devices: %s\n", sane_strstatus(status));
    sane_exit();
    return 1;
  }
  if (!devs || !devs[0]) {
    fprintf(stderr, "sane-scan: no devices found\n");
    fputs("ERR no devices found\n", stdout);
    sane_exit();
    return 1;
  }
  const SANE_Device *dev = devs[0];
  fprintf(stderr, "sane-scan: opening %s\n", dev->name);

  SANE_Handle handle;
  status = sane_open(dev->name, &handle);
  if (status != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane-scan: sane_open: %s\n", sane_strstatus(status));
    fprintf(stdout, "ERR sane_open: %s\n", sane_strstatus(status));
    sane_exit();
    return 1;
  }
  fprintf(stderr, "sane-scan: sane_open OK, awaiting commands\n");

  /* Banner: one JSON object on its own line. Doubles as READY signal. */
  fputs("{\"device\":", stdout);
  json_string(stdout, dev->name);
  fputs(",\"vendor\":", stdout);
  json_string(stdout, dev->vendor ? dev->vendor : "");
  fputs(",\"model\":", stdout);
  json_string(stdout, dev->model ? dev->model : "");
  fputs(",\"type\":", stdout);
  json_string(stdout, dev->type ? dev->type : "");
  fputs("}\n", stdout);

  char line[1024];
  int should_exit = 0;
  while (!should_exit && fgets(line, sizeof line, stdin)) {
    strip_eol(line);

    if (strcmp(line, "options") == 0) {
      dump_options(handle, stdout);

    } else if (strncmp(line, "set ", 4) == 0) {
      apply_one_option(handle, line + 4);  /* prints ERR on failure */
      dump_options(handle, stdout);        /* always dump the (possibly-changed) state */

    } else if (strncmp(line, "scan", 4) == 0
               && (line[4] == 0 || line[4] == ' ')) {
      const char *args = (line[4] == 0) ? "" : line + 4;
      int r = handle_scan_with_geometry(handle, args);
      fputs(r == 0 ? "OK\n" : "ERR scan failed\n", stdout);

    } else if (strcmp(line, "preview") == 0) {
      int r = handle_preview(handle);
      fputs(r == 0 ? "OK\n" : "ERR preview failed\n", stdout);

    } else if (strcmp(line, "exit") == 0) {
      fputs("OK\n", stdout);
      should_exit = 1;

    } else if (line[0] != 0) {
      fprintf(stderr, "sane-scan: unknown command: %s\n", line);
      fputs("ERR unknown command\n", stdout);
    }

    fflush(stdout);
  }

  sane_close(handle);
  sane_exit();
  return 0;
}
