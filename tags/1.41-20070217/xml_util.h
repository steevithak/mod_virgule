xmlNode *
virgule_xml_find_child (xmlNode *n, const char *tag);

xmlNode *
virgule_xml_ensure_child (xmlNode *n, const char *tag);

char *
virgule_xml_get_string_contents (xmlNode *n);

void
virgule_xml_del_string_contents (xmlNode *n);

char *
virgule_xml_get_prop (apr_pool_t *p, xmlNodePtr node, const xmlChar *name);

char *
virgule_xml_find_child_string (xmlNode *n, const char *tag, char *def);
