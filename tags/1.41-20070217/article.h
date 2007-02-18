int
virgule_article_serve (VirguleReq *vr);

int
virgule_article_recent_render (VirguleReq *vr, int n_arts_max, int start);

typedef enum {
  ARTICLE_RENDER_FULL,
  ARTICLE_RENDER_LEAD
} ArticleRenderStyle;

typedef struct {
  char *name;
  char *iconURL;
} ArticleTopic;
