typedef int CertLevel;

#define CERT_LEVEL_NONE 0

#define CERT_STYLE_LARGE "h2"
#define CERT_STYLE_MEDIUM "h4"
#define CERT_STYLE_SMALL "div"

typedef const char * CertStyle;

int
cert_num_levels (VirguleReq *vr);

CertLevel
cert_level_from_name (VirguleReq *vr, const char *name);

const char *
cert_level_to_name (VirguleReq *vr, CertLevel level);

CertLevel
cert_get (VirguleReq *vr, const char *issuer, const char *subject);

int
cert_set (VirguleReq *vr, const char *issuer, const char *subject, CertLevel level);

void
render_cert_level_begin (VirguleReq *vr, const char *user, CertStyle cs);

void
render_cert_level_end (VirguleReq *vr, CertStyle cs);

void
render_cert_level_text (VirguleReq *vr, const char *user);
