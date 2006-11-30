typedef struct _SchemaField SchemaField;

typedef enum {
  SCHEMA_TEXTAREA  = 1 << 0,
  SCHEMA_SELECT = 1 << 1
} SchemaFlags;

struct _SchemaField {
  char *name; /* name of xml property */
  char *description;
  char *prefix;
  int size;
  SchemaFlags flags;
  char **choices;
};

void
schema_render_input (apr_pool_t *p, Buffer *b, SchemaField *sf, xmlNode *tree);

void
schema_render_inputs (apr_pool_t *p, Buffer *b, SchemaField *sf, const char **fields, xmlNode *tree);

void
schema_put_field (apr_pool_t *p, SchemaField *sf, xmlNode *tree, apr_table_t *args);

void
schema_put_fields (apr_pool_t *p, SchemaField *sf, const char **fields, xmlNode *tree, apr_table_t *args);
