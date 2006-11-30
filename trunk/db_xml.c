/* Access to the database with XML. */

#include "httpd.h"

#include "db.h"

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include "db_xml.h"

static void
db_xml_cleanup (void *data)
{
  xmlDoc *doc = (xmlDoc *)data;

  xmlFreeDoc (doc);
}

xmlDoc *
db_xml_get (pool *p, Db *db, const char *key)
{
  int val_size;
  char *val = db_get_p (p, db, key, &val_size);
  xmlDoc *result;

  if (val == NULL)
    return NULL;
  result = xmlParseMemory (val, val_size);
  if (result != NULL)
    ap_register_cleanup (p, result, db_xml_cleanup, ap_null_cleanup);
  return result;
}

int
db_xml_put (pool *p, Db *db, const char *key, xmlDoc *val)
{
  xmlChar *buf;
  int buf_size;
  int status;

  xmlIndentTreeOutput = 1;
  xmlDocDumpFormatMemory (val, &buf, &buf_size, 1);
  status = db_put (db, key, buf, buf_size);
  xmlFree (buf);
  return status;
}

xmlDoc *
db_xml_doc_new (pool *p)
{
  xmlDoc *result = xmlNewDoc ("1.0");
  ap_register_cleanup (p, result, db_xml_cleanup, ap_null_cleanup);
  return result;
}

/* Calling this is optional. */
void
db_xml_free (pool *p, Db *db, xmlDoc *doc)
{
  xmlFreeDoc (doc);
  ap_kill_cleanup (p, doc, db_xml_cleanup);
}
