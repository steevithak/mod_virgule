typedef struct {
  char *name;
  int (*func)(VirguleReq *vr, xmlNode *params);
} xmlrpc_method;
