/* A data structure that persists across requests. */
typedef struct _VirguleReq VirguleReq;

struct _VirguleReq {
  virgule_private_t *priv;
  request_rec *r;
  Buffer *b;
  Db *db;
  char *uri;
  const char *u; /* authenticated username */
  char *args;
  DbLock *lock;
  int raw; /* TRUE if there are no enclosing <blockquote>'s */
  int sitemap_rendered; /* TRUE if the sitemap has already been rendered */
  char *tmetric;
  char *prefix; /* Prefix of <Location> directive, to be added to links */
  apr_table_t *render_data;
  apr_array_header_t *topics; /* array of topic info */
};

int
send_response (VirguleReq *vr);

apr_table_t *
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
