typedef struct {
  double confidence;
  double rating;
  double rating_sq;
} EigenVecEl;

HashTable *
eigen_local_load (apr_pool_t *p, VirguleReq *vr, const char *dbkey);

void
eigen_local_store (VirguleReq *vr, HashTable *ht, const char *dbkey);

int
eigen_set_local (VirguleReq *vr, const char *subj, double rating);

HashTable *
eigen_vec_load (apr_pool_t *p, VirguleReq *vr, const char *dbkey);

int
eigen_crank (apr_pool_t *p, VirguleReq *vr, const char *u);

int
eigen_report (VirguleReq *vr, const char *u);


