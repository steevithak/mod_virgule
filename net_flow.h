typedef struct _NetFlow NetFlow;
typedef struct _NetFlowPriv NetFlowPriv;

struct _NetFlow {
  int n_nodes;
  int n_nodes_max;
  int *n_succs;
  int *n_succs_max;
  int **succs;
  int *capacity;

  GHashTable *node_id; /* maps node objects to node numbers */

  char **names; /* maps node numbers to names */

  NetFlowPriv *priv;
};

NetFlow *
virgule_net_flow_new (void);

void
virgule_net_flow_free (NetFlow *self);

gint
virgule_net_flow_find_node (NetFlow *self, const char *name);

char *
virgule_net_flow_node_name (NetFlow *self, int node);

void
virgule_net_flow_add_edge (NetFlow *self, const char *src, const char *dst);

int *
virgule_net_flow_assign_tree (NetFlow *self, gint seed, const int *caps, int n_caps);

int
virgule_net_flow_sanity_check_tree (NetFlow *self, int seed, int *pred);

void
virgule_net_flow_max_flow (NetFlow *self, gint seed, const int *caps, int n_caps);

int
virgule_net_flow_sanity_check (NetFlow *self, gint seed);

int *
virgule_net_flow_extract (NetFlow *self);
