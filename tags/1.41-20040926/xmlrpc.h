#define XMLRPC_NO_PARAMS	""

int
xmlrpc_serve (VirguleReq *vr);

int
xmlrpc_unmarshal_params (VirguleReq *vr, xmlNode *params,
			 const char *types, ...);
int
xmlrpc_auth_user (VirguleReq *vr, const char *cookie);

int
xmlrpc_response (VirguleReq *vr, const char *types, ...);

int
xmlrpc_fault (VirguleReq *vr, int code, const char *fmt, ...);
