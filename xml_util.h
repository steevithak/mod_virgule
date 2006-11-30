xmlNode *
xml_find_child (xmlNode *n, const char *tag);

xmlNode *
xml_ensure_child (xmlNode *n, const char *tag);

char *
xml_get_string_contents (xmlNode *n);

char *
xml_get_prop (apr_pool_t *p, xmlNodePtr node, const xmlChar *name);

char *
xml_find_child_string (xmlNode *n, const char *tag, char *def);
