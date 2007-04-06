xmlDoc *
virgule_db_xml_get (apr_pool_t *p, Db *db, const char *key);

int
virgule_db_xml_put (apr_pool_t *p, Db *db, const char *key, xmlDoc *val);

xmlDoc *
virgule_db_xml_doc_new (apr_pool_t *p);

void
virgule_db_xml_free (apr_pool_t *p, xmlDoc *doc);
