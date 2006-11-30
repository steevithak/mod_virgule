xmlDoc *
db_xml_get (pool *p, Db *db, const char *key);

int
db_xml_put (pool *p, Db *db, const char *key, xmlDoc *val);

xmlDoc *
db_xml_doc_new (pool *p);

void
db_xml_free (pool *p, Db *db, xmlDoc *doc);
