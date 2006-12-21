typedef struct {
  double confidence;
  double rating;
  double rating_sq;
} EigenVecEl;

HashTable *
virgule_eigen_local_load (apr_pool_t *p, VirguleReq *vr, const char *dbkey);

void
virgule_eigen_local_store (VirguleReq *vr, HashTable *ht, const char *dbkey);

int
virgule_eigen_set_local (VirguleReq *vr, const char *subj, double rating);

HashTable *
virgule_eigen_vec_load (apr_pool_t *p, VirguleReq *vr, const char *dbkey);

int
virgule_eigen_crank (apr_pool_t *p, VirguleReq *vr, const char *u);

int
virgule_eigen_report (VirguleReq *vr, const char *u);

void
virgule_eigen_cleanup (VirguleReq *vr, const char *u);

