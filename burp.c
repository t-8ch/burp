#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>

#include "aur.h"
#include "util.h"

struct category_t {
  const char *name;
  const char *id;
};

/* This list must be sorted */
static const struct category_t categories[] = {
  { "daemons",      "2" },
  { "devel",        "3" },
  { "editors",      "4" },
  { "emulators",    "5" },
  { "fonts",       "20" },
  { "games",        "6" },
  { "gnome",        "7" },
  { "i18n",         "8" },
  { "kde",          "9" },
  { "kernels",     "19" },
  { "lib",         "10" },
  { "modules",     "11" },
  { "multimedia",  "12" },
  { "network",     "13" },
  { "office",      "14" },
  { "science",     "15" },
  { "system",      "16" },
  { "x11",         "17" },
  { "xfce",        "18" },
};

static char *arg_domain = "aur.archlinux.org";
static char *arg_username;
static char *arg_password;
static char *arg_cookiefile;
static const char *arg_category = "1";
static bool arg_persist_cookies;

static int category_compare(const void *a, const void *b) {
  const struct category_t *left = a;
  const struct category_t *right = b;
  return strcmp(left->name, right->name);
}

static const char *category_validate(const char *cat) {
  struct category_t key = { cat, NULL };
  struct category_t *res;

  res = bsearch(&key, categories, ARRAYSIZE(categories),
      sizeof(struct category_t), category_compare);

  return res ? res->id : NULL;
}

static char *find_config_file(void) {
  char *var, *out;

  var = getenv("XDG_CONFIG_HOME");
  if (var) {
    if (asprintf(&out, "%s/burp/burp.conf", var) < 0) {
      fprintf(stderr, "error: failed to allocate memory\n");
      return NULL;
    }
    return out;
  }

  var = getenv("HOME");
  if (var) {
    if (asprintf(&out, "%s/.config/burp/burp.conf", var) < 0){
      fprintf(stderr, "error: failed to allocate memory\n");
      return NULL;
    }
    return out;
  }

  return NULL;
}


static char *shell_expand(const char *in) {
  wordexp_t wexp;
  char *out = NULL;

  if (wordexp(in, &wexp, WRDE_NOCMD) < 0)
    return NULL;

  out = strdup(wexp.we_wordv[0]);
  wordfree(&wexp);
  if (out == NULL)
    return NULL;

  return out;
}

static size_t strtrim(char *str) {
  char *left = str, *right;

  if (!str || *str == '\0')
    return 0;

  while (isspace((unsigned char)*left))
    left++;

  if (left != str) {
    memmove(str, left, (strlen(left) + 1));
    left = str;
  }

  if (*str == '\0')
    return 0;

  right = (char*)rawmemchr(str, '\0') - 1;
  while (isspace((unsigned char)*right))
    right--;

  *++right = '\0';

  return right - left;
}

static int read_config_file(void) {
  _cleanup_fclose_ FILE *fp = NULL;
  char *config_path = NULL;
  char line[BUFSIZ];
  int lineno = 0;

  config_path = find_config_file();
  if (config_path == NULL) {
    fprintf(stderr, "warning: unable to determine location of config file. "
       "Skipping.\n");
    return 0;
  }

  fp = fopen(config_path, "r");
  if (fp == NULL) {
    if (errno != ENOENT) {
      fprintf(stderr, "error: failed to open %s: %s\n", config_path,
          strerror(errno));
      return -errno;
    }

    /* ignore error when file isn't found */
    return 0;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *key, *value;
    size_t len;

    ++lineno;

    len = strtrim(line);
    if (len == 0 || line[0] == '#')
      continue;

    key = value = line;
    strsep(&value, "=");
    strtrim(key);
    strtrim(value);

    if (streq(key, "User")) {
      char *v = strdup(value);
      if (v == NULL)
        fprintf(stderr, "error: failed to allocate memory\n");
      else
        arg_username = v;
    } else if (streq(key, "Password")) {
      char *v = strdup(value);
      if (v == NULL)
        fprintf(stderr, "error: failed to allocate memory\n");
      else
        arg_password = v;
    } else if (streq(key, "Cookies")) {
      char *v = shell_expand(value);
      if (v == NULL)
        fprintf(stderr, "error: failed to allocate memory\n");
      else
        arg_cookiefile = v;
    } else if (streq(key, "Persist")) {
      arg_persist_cookies = true;
    }
  }

  return 0;
}

static void usage_categories(void) {
  fprintf(stderr, "Valid categories:\n");
  for (size_t i = 0; i < ARRAYSIZE(categories); ++i)
    fprintf(stderr, "\t%s\n", categories[i].name);
}

static void usage(void) {
  fprintf(stderr, "burp %s\n"
  "Usage: burp [options] targets...\n\n"
  " Options:\n"
  "  -h, --help                Shows this help message.\n"
  "  -u, --user                AUR login username.\n"
  "  -p, --password            AUR login password.\n", BURP_VERSION);
  fprintf(stderr,
  "  -c CAT, --category=CAT    Assign the uploaded package with category CAT.\n"
  "                              This will default to the current category\n"
  "                              for pre-existing packages and 'None' for new\n"
  "                              packages. -c help will give a list of valid\n"
  "                              categories.\n");
  fprintf(stderr,
  /* leaving --domain undocumented for now */
  /* "      --domain=DOMAIN       Domain of the AUR (default: aur.archlinux.org)\n" */
  "  -C FILE, --cookies=FILE   Use FILE to store cookies rather than the default\n"
  "                              temporary file. Useful with the -k option.\n"
  "  -k, --keep-cookies        Cookies will be persistent and reused for logins.\n"
  "                              If you specify this option, you must also provide\n"
  "                              a path to a cookie file.\n"
  /* "  -v, --verbose             be more verbose. Pass twice for debug info.\n\n" */
  "  burp also honors a config file. See burp(1) for more information.\n\n");
}

static int parseargs(int argc, char **argv) {
  static struct option option_table[] = {
    {"cookies",       required_argument,  0, 'C'},
    {"category",      required_argument,  0, 'c'},
    {"help",          no_argument,        0, 'h'},
    {"keep-cookies",  no_argument,        0, 'k'},
    {"password",      required_argument,  0, 'p'},
    {"user",          required_argument,  0, 'u'},
    /* {"verbose",       no_argument,        0, 'v'}, */
    {"domain",        required_argument,  0, 128},
    {NULL, 0, NULL, 0}
  };

  for (;;) {
    int opt = getopt_long(argc, argv, "C:c:hkp:u:", option_table, NULL);
    if (opt < 0)
      break;

    switch (opt) {
    case 'C':
      arg_cookiefile = optarg;
      break;
    case 'c':
      arg_category = category_validate(optarg);
      if (arg_category == NULL) {
        fprintf(stderr, "error: invalid category %s\n", optarg);
        usage_categories();
        return -EINVAL;
      }
      break;
    case 'h':
      usage();
      return 0;
    case 'k':
      arg_persist_cookies = true;
      break;
    case 'p':
      arg_password = optarg;
      break;
    case 'u':
      arg_username = optarg;
      break;
    case 128:
      arg_domain = optarg;
      break;
    default:
      return -EINVAL;
    }
  }

  return 0;
}

static int make_login_error(int err) {
  switch (-err) {
  case EBADR:
    fprintf(stderr, "error: insufficient credentials provided to login.\n");
    break;
  case EACCES:
    fprintf(stderr, "error: bad username or password.\n");
    break;
  case EKEYEXPIRED:
    fprintf(stderr, "error: required login cookie has expired.\n");
    break;
  case EKEYREJECTED:
    fprintf(stderr, "error: login cookie not accepted.\n");
    break;
  default:
    fprintf(stderr, "error: failed to login to AUR: %s\n", strerror(-err));
    break;
  }

  return EXIT_FAILURE;
}

int main(int argc, char *argv[]) {
  struct aur_t *aur;
  int r;

  if (read_config_file() < 0)
    return EXIT_FAILURE;

  if (parseargs(argc, argv) < 0)
    return EXIT_FAILURE;

  argc -= optind - 1;
  argv += optind - 1;

  r = aur_new(&aur, arg_domain, true);
  if (r < 0) {
    fprintf(stderr, "error: failed to create AUR client: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }

  if (arg_username)
    aur_set_username(aur, arg_username);
  if (arg_password)
    aur_set_password(aur, arg_password);
  if (arg_cookiefile)
    aur_set_cookies(aur, arg_cookiefile);
  if (arg_persist_cookies)
    aur_set_persist_cookies(aur, arg_persist_cookies);

  r = aur_login(aur, false);
  if (r < 0) {
    switch (r) {
    case -EKEYEXPIRED:
      /* cookie expired */
      fprintf(stderr, "warning: Your cookie has expired -- using password login\n");
    /* fallthrough */
    case -ENOKEY:
      /* cookie not found */
      r = aur_login(aur, true);
      break;
    }

    if (r < 0)
      return make_login_error(r);
  }

  for (int i = 1; i < argc; ++i) {
    _cleanup_free_ char *error = NULL;
    int k = aur_upload(aur, argv[i], arg_category, &error);
    if (k == 0)
      printf("success: uploaded %s\n", argv[i]);
    else {
      fprintf(stderr, "failed to upload %s: %s\n", argv[i],
          error ? error : strerror(-k));
      if (r == 0)
        r = k;
    }
  }

  aur_free(aur);

  return r == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
