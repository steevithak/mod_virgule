struct _FeedItem {
  time_t post_time;
  time_t update_time;
  char *blogauthor;
  char *bloglink;
  char *link;
  char *id;
  xmlNode *title;
  xmlNode *content;
};

typedef struct _FeedItem FeedItem;

int
virgule_update_aggregator_list (VirguleReq *vr);

int
virgule_aggregator_serve (VirguleReq *vr);
