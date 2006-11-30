int
article_serve (VirguleReq *vr);

int
article_recent_render (VirguleReq *vr, int n_arts_max, int start);

typedef enum {
  ARTICLE_RENDER_FULL,
  ARTICLE_RENDER_LEAD
} ArticleRenderStyle;
