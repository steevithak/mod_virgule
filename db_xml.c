/* Access to the database with XML. */

#include <apr.h>
#include <apr_pools.h>
#include <httpd.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include "db.h"
#include "db_xml.h"

static apr_status_t
db_xml_cleanup (void *data)
{
  xmlDoc *doc = (xmlDoc *)data;

  if (doc != NULL)
    xmlFreeDoc (doc);
  
  return APR_SUCCESS;
}

xmlDoc *
virgule_db_xml_get (apr_pool_t *p, Db *db, const char *key)
{
  int val_size;
  char *val = virgule_db_get_p (p, db, key, &val_size);
  xmlDoc *result;

  if (val == NULL)
    return NULL;
  result = xmlParseMemory (val, val_size);
  if (result != NULL)
    apr_pool_cleanup_register (p, result, db_xml_cleanup, apr_pool_cleanup_null);
  return result;
}

int
virgule_db_xml_put (apr_pool_t *p, Db *db, const char *key, xmlDoc *val)
{
  xmlChar *buf;
  int buf_size;
  int status;

  xmlIndentTreeOutput = 1;
  xmlDocDumpFormatMemory (val, &buf, &buf_size, 1);
  status = virgule_db_put (db, key, (char *)buf, buf_size);
  xmlFree (buf);
  return status;
}

xmlDoc *
virgule_db_xml_doc_new (apr_pool_t *p)
{
  xmlDoc *result = xmlNewDoc ((xmlChar *)"1.0");
  apr_pool_cleanup_register (p, result, db_xml_cleanup, apr_pool_cleanup_null);
  return result;
}

/* Optional: clean up now, don't wait for APR end of pool life cleanup */
void
virgule_db_xml_free (apr_pool_t *p, xmlDoc *doc)
{
  if (doc != NULL)
    xmlFreeDoc (doc);
  apr_pool_cleanup_kill (p, doc, db_xml_cleanup);
}
