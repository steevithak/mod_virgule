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
schema_render_input (pool *p, Buffer *b, SchemaField *sf, xmlNode *tree);

void
schema_render_inputs (pool *p, Buffer *b, SchemaField *sf, const char **fields, xmlNode *tree);

void
schema_put_field (pool *p, SchemaField *sf, xmlNode *tree, table *args);

void
schema_put_fields (pool *p, SchemaField *sf, const char **fields, xmlNode *tree, table *args);
