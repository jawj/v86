/*
 * sane-opts: dump SANE device options as JSON
 * Usage: sane-opts <device-name>
 * Compile: gcc -o sane-opts sane-opts.c -lsane -s
 */

#include <sane/sane.h>
#include <stdio.h>
#include <string.h>

static void json_string(const char *s) {
  putchar('"');
  for (; *s; s++) {
    switch (*s) {
    case '"':  fputs("\\\"", stdout); break;
    case '\\': fputs("\\\\", stdout); break;
    case '\n': fputs("\\n", stdout);  break;
    case '\r': fputs("\\r", stdout);  break;
    case '\t': fputs("\\t", stdout);  break;
    default:   putchar(*s);
    }
  }
  putchar('"');
}

static const char *type_name(SANE_Value_Type t) {
  switch (t) {
  case SANE_TYPE_BOOL:   return "bool";
  case SANE_TYPE_INT:    return "int";
  case SANE_TYPE_FIXED:  return "fixed";
  case SANE_TYPE_STRING: return "string";
  case SANE_TYPE_BUTTON: return "button";
  case SANE_TYPE_GROUP:  return "group";
  default:               return "unknown";
  }
}

static const char *unit_name(SANE_Unit u) {
  switch (u) {
  case SANE_UNIT_NONE:        return "none";
  case SANE_UNIT_PIXEL:       return "pixel";
  case SANE_UNIT_BIT:         return "bit";
  case SANE_UNIT_MM:          return "mm";
  case SANE_UNIT_DPI:         return "dpi";
  case SANE_UNIT_PERCENT:     return "percent";
  case SANE_UNIT_MICROSECOND: return "us";
  default:                    return "unknown";
  }
}

static double fixed_to_double(SANE_Fixed f) {
  return f / (double)(1 << SANE_FIXED_SCALE_SHIFT);
}

static void print_number(SANE_Value_Type type, SANE_Word w) {
  if (type == SANE_TYPE_FIXED)
    printf("%g", fixed_to_double(w));
  else
    printf("%d", w);
}

static void print_constraint(const SANE_Option_Descriptor *d) {
  int i;
  switch (d->constraint_type) {
  case SANE_CONSTRAINT_RANGE:
    printf("\"constraint\":{\"type\":\"range\",\"min\":");
    print_number(d->type, d->constraint.range->min);
    printf(",\"max\":");
    print_number(d->type, d->constraint.range->max);
    printf(",\"step\":");
    print_number(d->type, d->constraint.range->quant);
    putchar('}');
    break;
  case SANE_CONSTRAINT_WORD_LIST:
    printf("\"constraint\":{\"type\":\"list\",\"values\":[");
    for (i = 1; i <= d->constraint.word_list[0]; i++) {
      if (i > 1) putchar(',');
      print_number(d->type, d->constraint.word_list[i]);
    }
    fputs("]}", stdout);
    break;
  case SANE_CONSTRAINT_STRING_LIST:
    printf("\"constraint\":{\"type\":\"list\",\"values\":[");
    for (i = 0; d->constraint.string_list[i]; i++) {
      if (i > 0) putchar(',');
      json_string(d->constraint.string_list[i]);
    }
    fputs("]}", stdout);
    break;
  default:
    printf("\"constraint\":null");
  }
}

static void print_current_value(SANE_Handle h, int idx,
                                const SANE_Option_Descriptor *d) {
  if (d->type == SANE_TYPE_BUTTON || d->type == SANE_TYPE_GROUP) {
    printf("\"value\":null");
    return;
  }
  if (!SANE_OPTION_IS_ACTIVE(d->cap)) {
    printf("\"value\":null");
    return;
  }

  if (d->type == SANE_TYPE_STRING) {
    char buf[d->size];
    if (sane_control_option(h, idx, SANE_ACTION_GET_VALUE, buf, NULL)
        == SANE_STATUS_GOOD) {
      printf("\"value\":");
      json_string(buf);
    } else {
      printf("\"value\":null");
    }
  } else if (d->type == SANE_TYPE_BOOL) {
    SANE_Bool v;
    if (sane_control_option(h, idx, SANE_ACTION_GET_VALUE, &v, NULL)
        == SANE_STATUS_GOOD)
      printf("\"value\":%s", v ? "true" : "false");
    else
      printf("\"value\":null");
  } else {
    /* INT or FIXED — could be a single value or an array (e.g. gamma table) */
    int count = d->size / sizeof(SANE_Word);
    SANE_Word vals[count];
    if (sane_control_option(h, idx, SANE_ACTION_GET_VALUE, vals, NULL)
        == SANE_STATUS_GOOD) {
      if (count == 1) {
        printf("\"value\":");
        print_number(d->type, vals[0]);
      } else {
        printf("\"value\":[");
        for (int i = 0; i < count; i++) {
          if (i > 0) putchar(',');
          print_number(d->type, vals[i]);
        }
        putchar(']');
      }
    } else {
      printf("\"value\":null");
    }
  }
}

int main(int argc, char **argv) {
  SANE_Int version;
  SANE_Handle handle;
  SANE_Status status;
  const SANE_Option_Descriptor *d;
  int i, n, first = 1;

  if (argc != 2) {
    fprintf(stderr, "usage: sane-opts <device>\n");
    return 1;
  }

  if (sane_init(&version, NULL) != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane_init failed\n");
    return 1;
  }

  status = sane_open(argv[1], &handle);
  if (status != SANE_STATUS_GOOD) {
    fprintf(stderr, "sane_open: %s\n", sane_strstatus(status));
    sane_exit();
    return 1;
  }

  /* option 0 is always the option count */
  if (sane_control_option(handle, 0, SANE_ACTION_GET_VALUE, &n, NULL)
      != SANE_STATUS_GOOD) {
    fprintf(stderr, "cannot read option count\n");
    sane_close(handle);
    sane_exit();
    return 1;
  }

  putchar('[');
  for (i = 1; i < n; i++) {
    d = sane_get_option_descriptor(handle, i);
    if (!d || !d->name || !d->name[0])
      continue;

    if (!first) putchar(',');
    first = 0;

    printf("{\"name\":");
    json_string(d->name);
    printf(",\"title\":");
    json_string(d->title ? d->title : d->name);
    printf(",\"type\":\"%s\"", type_name(d->type));
    printf(",\"unit\":\"%s\"", unit_name(d->unit));
    if (d->type == SANE_TYPE_INT || d->type == SANE_TYPE_FIXED)
      printf(",\"size\":%d", (int)(d->size / sizeof(SANE_Word)));
    else if (d->type == SANE_TYPE_STRING)
      printf(",\"size\":%d", (int)d->size);
    printf(",\"active\":%s", SANE_OPTION_IS_ACTIVE(d->cap) ? "true" : "false");
    printf(",\"settable\":%s", SANE_OPTION_IS_SETTABLE(d->cap) ? "true" : "false");
    putchar(',');
    print_constraint(d);
    putchar(',');
    print_current_value(handle, i, d);
    putchar('}');
  }
  puts("]");

  sane_close(handle);
  sane_exit();
  return 0;
}
