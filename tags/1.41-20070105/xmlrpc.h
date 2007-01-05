#define XMLRPC_NO_PARAMS	""

int
virgule_xmlrpc_serve (VirguleReq *vr);

int
virgule_xmlrpc_unmarshal_params (VirguleReq *vr, xmlNode *params,
			         const char *types, ...);
int
virgule_xmlrpc_auth_user (VirguleReq *vr, const char *cookie);

int
virgule_xmlrpc_response (VirguleReq *vr, const char *types, ...);

int
virgule_xmlrpc_fault (VirguleReq *vr, int code, const char *fmt, ...);
