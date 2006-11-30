typedef struct _AllowedTag AllowedTag;
typedef struct _NavOption NavOption;

/* A data structure that persists across requests. */

typedef struct _VirguleReq VirguleReq;

struct _VirguleReq {
  request_rec *r;
  Buffer *b;
  Db *db;
  const char *base_uri;
  const char *site_name;
  enum {
    PROJSTYLE_RAPH,
    PROJSTYLE_NICK
  } projstyle;
  const char **cert_level_names;
  const char **seeds;
  const int *caps;
  const char **special_users;
  int allow_account_creation;
  int render_diaryratings;
  int recentlog_as_posted;
  const AllowedTag **allowed_tags;
  const NavOption **nav_options;
  char *uri;
  const char *u; /* authenticated username */
  char *args;
  DbLock *lock;
  int raw; /* TRUE if there are no enclosing <blockquote>'s */
  int sitemap_rendered; /* TRUE if the sitemap has already been rendered */
  char *tmetric;
  char *prefix; /* Prefix of <Location> directive, to be added to links */
  table *render_data;
  array_header *topics; /* array of topic info */
};

int
send_response (VirguleReq *vr);

table *
get_args_table (VirguleReq *vr);

char *
req_get_tmetric (VirguleReq *vr);

const char *
req_get_tmetric_level (VirguleReq *vr, const char *u);

int
req_ok_to_post (VirguleReq *vr);

int
req_ok_to_reply (VirguleReq *vr);

int
req_ok_to_create_project (VirguleReq *vr);
