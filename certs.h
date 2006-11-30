typedef int CertLevel;

#define CERT_LEVEL_NONE 0

#define CERT_STYLE_LARGE "h2"
#define CERT_STYLE_MEDIUM "h4"
#define CERT_STYLE_SMALL "div"

typedef const char * CertStyle;

int
virgule_cert_num_levels (VirguleReq *vr);

CertLevel
virgule_cert_level_from_name (VirguleReq *vr, const char *name);

const char *
virgule_cert_level_to_name (VirguleReq *vr, CertLevel level);

CertLevel
virgule_cert_get (VirguleReq *vr, const char *issuer, const char *subject);

int
virgule_cert_set (VirguleReq *vr, const char *issuer, const char *subject, CertLevel level);

void
virgule_render_cert_level_begin (VirguleReq *vr, const char *user, CertStyle cs);

void
virgule_render_cert_level_end (VirguleReq *vr, CertStyle cs);

void
virgule_render_cert_level_text (VirguleReq *vr, const char *user);
