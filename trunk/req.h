/* A data structure that persists across requests. */
typedef struct _VirguleReq VirguleReq;

struct _VirguleReq {
  virgule_private_t *priv;
  request_rec *r;
  Buffer *b;   /* main buffer */
  Buffer *tb;  /* template buffer */
  Buffer *hb;  /* header buffer */
  Db *db;
  char *uri;
  const char *u; /* authenticated username */
  char *args;
//  int sitemap_rendered; /* TRUE if the sitemap has already been rendered */
  char *prefix; /* Prefix of <Location> directive, to be added to links */
  apr_table_t *render_data;
};

int
virgule_send_response (VirguleReq *vr);

apr_table_t *
virgule_get_args_table (VirguleReq *vr);

char *
virgule_req_get_tmetric (VirguleReq *vr);

const char *
virgule_req_get_tmetric_level (VirguleReq *vr, const char *u);

int
virgule_req_ok_to_post (VirguleReq *vr);

int
virgule_req_ok_to_reply (VirguleReq *vr);

int
virgule_req_ok_to_create_project (VirguleReq *vr);

int
virgule_req_ok_to_syndicate_blog (VirguleReq *vr);
