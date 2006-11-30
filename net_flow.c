/* Implementation of the group trust metric as network flow in C. */

#include <glib.h>
#include "net_flow.h"

#define TWEAK TRUE

struct _NetFlowPriv {
  int n_edges;

  int *edge_src;
  int *edge_dst;
  int *edge_flow;

  int *node_sink; /* 1 if there is flow from this node to the supersink */
  int *node_flow; /* total flow through the node */
  int *node_in_degree;
  int *node_in_degree_max;
  int **node_in_edges;
  int *node_out_degree;
  int *node_out_degree_max;
  int **node_out_edges;
};

NetFlow *
virgule_net_flow_new (void)
{
  NetFlow *result = g_new (NetFlow, 1);

  result->n_nodes = 0;
  result->n_nodes_max = 16;
  result->n_succs = g_new (int, result->n_nodes_max);
  result->n_succs_max = g_new (int, result->n_nodes_max);
  result->succs = g_new (int *, result->n_nodes_max);
  result->capacity = NULL;
  result->node_id = g_hash_table_new (g_str_hash, g_str_equal);
  result->names = NULL;
  result->priv = NULL;

  return result;
}

static void
net_flow_free_helper (gpointer key, gpointer value, gpointer user_data)
{
  g_free (key);
}

void
virgule_net_flow_free (NetFlow *self)
{
  NetFlowPriv *priv;
  int i;

  g_hash_table_foreach (self->node_id, net_flow_free_helper, NULL);
  g_hash_table_destroy (self->node_id);


  for (i = 0; i < self->n_nodes; i++)
    g_free (self->succs[i]);
  g_free (self->n_succs);
  g_free (self->n_succs_max);
  g_free (self->succs);
  if (self->capacity != NULL)
    g_free (self->capacity);
  if (self->names != NULL)
    g_free (self->names);

  priv = self->priv;
  if (priv)
    {
      for (i = 0; i < self->n_nodes; i++)
	{
	  g_free (priv->node_in_edges[i]);
	  g_free (priv->node_out_edges[i]);
	}

      g_free (priv->edge_src);
      g_free (priv->edge_dst);
      g_free (priv->edge_flow);


      g_free (priv->node_sink);
      g_free (priv->node_flow);
      g_free (priv->node_in_degree);
      g_free (priv->node_in_degree_max);
      g_free (priv->node_in_edges);
      g_free (priv->node_out_degree);
      g_free (priv->node_out_degree_max);
      g_free (priv->node_out_edges);
      g_free (priv);
    }

  g_free (self);
}

gint
virgule_net_flow_find_node (NetFlow *self, const char *name)
{
  gpointer resultptr;

  if (g_hash_table_lookup_extended (self->node_id,
				    name,
				    NULL,
				    &resultptr))
    return GPOINTER_TO_INT(resultptr);
  resultptr = GINT_TO_POINTER (self->n_nodes);
  g_hash_table_insert (self->node_id, g_strdup (name), resultptr);

  if (self->n_nodes == self->n_nodes_max)
    {
      self->n_nodes_max <<= 1;
      self->n_succs = g_realloc (self->n_succs,
				 sizeof(int) * self->n_nodes_max);
      self->n_succs_max = g_realloc (self->n_succs_max,
				     sizeof(int) * self->n_nodes_max);
      self->succs = g_realloc (self->succs, sizeof(int *) * self->n_nodes_max);
      if (self->capacity != NULL)
	self->capacity = g_realloc (self->capacity,
				    sizeof(int) * self->n_nodes_max);
    }
  self->n_succs[self->n_nodes] = 0;
  self->n_succs_max[self->n_nodes] = 16;
  self->succs[self->n_nodes] = g_new (int, self->n_succs_max[self->n_nodes]);

  return self->n_nodes++;
}

static void
net_flow_node_name_helper (gpointer key, gpointer value, gpointer user_data)
{
  NetFlow *self = user_data;

  self->names[GPOINTER_TO_INT (value)] = key;
}

/**
 * net_flow_node_name: Find name of numbered node.
 * @self: The #NetFlow context.
 * @node: The node number.
 *
 * Finds the name of the numbered node. Note: this is intended primarily
 * for debugging and is certainly not coded for speed.
 *
 * Not used in the live code.
 *
 * Return value: The name of the node, or NULL if not found.
 **/
char *
virgule_net_flow_node_name (NetFlow *self, int node)
{

  /* todo: doesn't gracefully handle new names after first call of this
     method. */
  if (!self->names)
    {
      self->names = g_new (char *, self->n_nodes);
      g_hash_table_foreach (self->node_id, net_flow_node_name_helper, self);
    }

  return self->names[node];
}

void
virgule_net_flow_add_edge (NetFlow *self, const char *src, const char *dst)
{
  gint src_id;
  gint dst_id;

  src_id = virgule_net_flow_find_node (self, src);
  dst_id = virgule_net_flow_find_node (self, dst);
  if (self->n_succs[src_id] == self->n_succs_max[src_id])
    {
      self->n_succs_max[src_id] <<= 1;
      self->succs[src_id] = g_realloc (self->succs[src_id],
				       self->n_succs_max[src_id] *
				       sizeof(int));
    }
  self->succs[src_id][self->n_succs[src_id]++] = dst_id;
}

static void
net_flow_assign_capacities (NetFlow *self, gint seed, const int *caps, int n_caps)
{
  int i, j, k;
  gint *node_list;
  int beg_nl, end_nl;
  int new_end_nl;
  int cap, cap_sum, iter_num;
  int *capacity;

  g_return_if_fail (self->capacity == NULL);
  g_return_if_fail (self->n_nodes > 0);

  capacity = g_new (int, self->n_nodes);
  for (i = 0; i < self->n_nodes; i++)
    capacity[i] = -1;

  node_list = g_new (int, self->n_nodes);
  node_list[0] = seed;
  beg_nl = 0;
  end_nl = 1;
  cap = caps[0];
  capacity[seed] = cap;
  cap_sum = cap;

  /* breadth first search */
  for (iter_num = 1; end_nl > beg_nl; iter_num++)
    {
      new_end_nl = end_nl;
      if (iter_num < n_caps)
	cap = caps[iter_num];
      for (j = beg_nl; j < end_nl; j++)
	{
	  int ix = node_list[j];
	  int *succs = self->succs[ix];
	  int n_succs = self->n_succs[ix];
	  for (k = 0; k < n_succs; k++)
	    {
	      int succ = succs[k];
	      if (capacity[succ] == -1)
		{
		  capacity[succ] = cap;
		  cap_sum += cap;
		  node_list[new_end_nl++] = succ;
		}
	    }
	}
      beg_nl = end_nl;
      end_nl = new_end_nl;
    }

  /* can access average capacity here == cap_sum / self->n_nodes */

  g_free (node_list);

  self->capacity = capacity;
}

/* Do a greedy assignment into a tree. Return a predecessor represenation
   of the tree (newly alloc'ed). */
/*
 *  Appears to be called only in this source file. Should this be static?
 */
int *
virgule_net_flow_assign_tree (NetFlow *self, gint seed, const int *caps, int n_caps)
{
  int n_nodes = self->n_nodes;
  int *pred;
  int *n_children;
  int *n_children_max;
  int **children;
  int *start_assign;
  int *resid_cap;
  int i;
  int level;
  int *child_ix;

  net_flow_assign_capacities (self, seed, caps, n_caps);

  pred = g_new (int, n_nodes);
  n_children = g_new (int, n_nodes);
  n_children_max = g_new (int, n_nodes);
  children = g_new (int *, n_nodes);
  start_assign = g_new (int, n_nodes);
  resid_cap = g_new (int, n_nodes);
  child_ix = g_new (int, n_caps);

  for (i = 0; i < n_nodes; i++)
    {
      resid_cap[i] = self->capacity[i];
      pred[i] = -1;
      children[i] = NULL;
    }

  pred[seed] = seed;
  resid_cap[seed]--;

  for (level = 0; level < n_caps; level++)
    {
      int cur_assign = 0;
      int cur_node = seed;
      int cur_depth = 0;
      int cap = resid_cap[seed];
      child_ix[cur_depth] = 0;
      start_assign[0] = cur_assign;

      while (TRUE) {
	if (cur_depth == level)
	  {
	    /* add children of cur_node to tree, respecting capacity
	       constraint */
	    int n_succ = self->n_succs[cur_node];
	    int *succ = self->succs[cur_node];
	    int j;

	    n_children[cur_node] = 0;
	    n_children_max[cur_node] = 16;
	    children[cur_node] = g_new (int, n_children_max[cur_node]);
	    for (j = 0; cap > 0 && j < n_succ; j++)
	      {
		int j_ix = succ[j];
		if (pred[j_ix] == -1)
		  {
		    pred[j_ix] = cur_node;
		    if (n_children[cur_node] == n_children_max[cur_node])
		      {
			n_children_max[cur_node] <<= 1;
			children[cur_node] = g_realloc (children[cur_node],
							n_children_max[cur_node] *
							sizeof (int));
		      }
		    children[cur_node][n_children[cur_node]++] = j_ix;
		    resid_cap[j_ix]--;
		    cur_assign++;
		    cap--;
		  }

	      }

	    resid_cap[cur_node] = cap;

	    if (level == 0)
	      break;

	    /* traverse up the stack */
	    while (cur_depth > 0)
	      {
		cur_depth--;
		cur_node = pred[cur_node];
		resid_cap[cur_node] -= cur_assign - start_assign[cur_depth];
		
		start_assign[cur_depth] = cur_assign;
		cap = resid_cap[cur_node];
		if (child_ix[cur_depth] < n_children[cur_node])
		  break;
	      }
	    if (cur_depth == 0 && child_ix[0] == n_children[cur_node])
	      break;
	  }
	else 
	  {
	    /* not at the bottom level, go to next */
	    if (child_ix[cur_depth] < n_children[cur_node])
	      {
		cur_node = children[cur_node][child_ix[cur_depth]];
		if (cap > resid_cap[cur_node])
		  cap = resid_cap[cur_node];
		else
		  resid_cap[cur_node] = cap;
		child_ix[cur_depth]++;
		cur_depth++;
		child_ix[cur_depth] = 0;
		start_assign[cur_depth] = cur_assign;
	      }
	    else
	      {
		/* no children remaining at this level, up one */
		if (cur_depth == 0)
		  break;
		cur_depth--;
		cur_node = pred[cur_node];
		resid_cap[cur_node] -= cur_assign - start_assign[cur_depth];

		start_assign[cur_depth] = cur_assign;
		cap = resid_cap[cur_node];
	      }
	  }
      }
    }

  g_free (child_ix);
  g_free (resid_cap);
  g_free (start_assign);
  g_free (n_children_max);
  g_free (n_children);
  for (i = 0; i < n_nodes; i++)
    {
      if (children[i] != NULL)
	g_free (children[i]);
    }
  g_free (children);

  return pred;
}

/*
 *  Appears to be called only in this source file. Should this be static?
 */
int
virgule_net_flow_sanity_check_tree (NetFlow *self, int seed, int *pred)
{
  int n_nodes = self->n_nodes;
  int *caps;
  int node;
  int result = 0;
  int cap;

  caps = g_new (int, n_nodes);
  for (node = 0; node < n_nodes; node++)
    {
      cap = self->capacity[node];
      if (cap < 0)
	cap = 0;
      caps[node] = cap;
    }

  for (node = 0; node < n_nodes; node++)
    {
      if (pred[node] >= 0)
	{
	  int n;
	  int last = -1;

	  for (n = node; n != last; n = pred[n])
	    {
	      caps[n]--;
	      last = n;
	    }
	}
    }

  for (node = 0; node < n_nodes; node++)
    if (caps[node] < 0)
      {
	cap = self->capacity[node];
	if (cap < 0)
	  cap = 0;
	g_warning ("Node %d flow %d over capacity %d\n",
		   node, cap - caps[node], cap);
	result = -1;
      }

  g_free (caps);
  return result;
}

static int
net_flow_n_edges (NetFlow *self)
{
  int i;
  int result;

  result = 0;
  for (i = 0; i < self->n_nodes; i++)
    result += self->n_succs[i];
  return result;
}

/* This method initializes the graph data structures used for network
   flow computation, based on the successor list created by repeated
   invocation of add_edge(). */

static void
net_flow_init_graph (NetFlow *self)
{
  NetFlowPriv *priv;
  int i;
  int n_edges;
  int e = 0; /* allocation index for new edges */
  int src;

  priv = g_new (NetFlowPriv, 1);

  n_edges = net_flow_n_edges (self);

  priv->edge_src = g_new (int, n_edges);
  priv->edge_dst = g_new (int, n_edges);
  priv->edge_flow = g_new (int, n_edges);

  priv->node_sink = g_new (int, self->n_nodes);
  priv->node_flow = g_new (int, self->n_nodes);
  priv->node_in_degree = g_new (int, self->n_nodes);
  priv->node_in_degree_max = g_new (int, self->n_nodes);
  priv->node_in_edges = g_new (int *, self->n_nodes);
  priv->node_out_degree = g_new (int, self->n_nodes);
  priv->node_out_degree_max = g_new (int, self->n_nodes);
  priv->node_out_edges = g_new (int *, self->n_nodes);

  for (i = 0; i < self->n_nodes; i++)
    {
      priv->node_sink[i] = 0;
      priv->node_flow[i] = 0;
      priv->node_in_degree[i] = 0;
      priv->node_in_degree_max[i] = 16;
      priv->node_in_edges[i] = g_new (int, priv->node_in_degree_max[i]);
      priv->node_out_degree[i] = 0;
      priv->node_out_degree_max[i] = 16;
      priv->node_out_edges[i] = g_new (int, priv->node_out_degree_max[i]);
    }

  for (src = 0; src < self->n_nodes; src++)
    {
      int n_succ = self->n_succs[src];
      int *succ = self->succs[src];
      int j;

      for (j = 0; j < n_succ; j++)
	{
	  int dst = succ[j];

	  if (src != dst)
	    {
	      int degree_src, degree_dst;

	      priv->edge_src[e] = src;
	      priv->edge_dst[e] = dst;
	      priv->edge_flow[e] = 0;

	      degree_src = priv->node_out_degree[src];
	      if (degree_src == priv->node_out_degree_max[src])
		priv->node_out_edges[src] =
		  g_realloc (priv->node_out_edges[src],
			     sizeof (int) *
			     (priv->node_out_degree_max[src] <<= 1));
	      priv->node_out_edges[src][degree_src] = e;
	      priv->node_out_degree[src]++;

	      degree_dst = priv->node_in_degree[dst];
	      if (degree_dst == priv->node_in_degree_max[dst])
		priv->node_in_edges[dst] =
		  g_realloc (priv->node_in_edges[dst],
			     sizeof (int) *
			     (priv->node_in_degree_max[dst] <<= 1));
	      priv->node_in_edges[dst][degree_dst] = e;
	      priv->node_in_degree[dst]++;

	      e++;
	    }
	}
    }

  /* e <= n_edges, because e doesn't count self-edges */
  priv->n_edges = e;
  self->priv = priv;
}

/**
 * net_flow_from_tree: Set up flows in graph based on tree assignment.
 * @self: The #NetFlow context.
 * @pred_list: The tree assignment, in predecessor form.
 **/
static void
net_flow_from_tree (NetFlow *self, int *pred_list)
{
  NetFlowPriv *priv = self->priv;
  int node;
  int n_nodes = self->n_nodes;

  for (node = 0; node < n_nodes; node++)
    {
      int last = -1;
      int next;

      if (pred_list[node] != -1)
	{
	  int ix;
	  priv->node_sink[node] = 1;
	  for (ix = node; ix != last; ix = next)
	    {
	      next = pred_list[ix];
	      priv->node_flow[ix]++;
	      if (ix != next)
		{
		  /* find edge from next to ix and increment flow */
		  int *edges;
		  int n_edges;
		  int j;

		  edges = priv->node_in_edges[ix];
		  n_edges = priv->node_in_degree[ix];
		  for (j = 0; j < n_edges; j++)
		    {
		      int edge = edges[j];
		      if (priv->edge_src[edge] == next)
			{
			  priv->edge_flow[edge]++;
			  break;
			}
		    }
		}
	      last = ix;
	    }
	}
    }
}

/**
 * net_flow_augment: Add a unit augmenting path to the flow.
 * @self: The #NetFlow context.
 * @seed: The seed, i.e. first node in the path.
 *
 * Do see sim/NetFlow.java for more implementation comments.
 *
 * Return value: TRUE if an augmenting path exists.
 **/
static gboolean
net_flow_augment (NetFlow *self, int seed)
{
  NetFlowPriv *priv = self->priv;
  int n_nodes = self->n_nodes;
  gboolean *visited_in = g_new (gboolean, n_nodes);
  gboolean *visited_out = g_new (gboolean, n_nodes);
  int *queue = g_new (int, n_nodes * 2);
  gboolean *queue_dir = g_new (gboolean, n_nodes * 2); /* out = true */
  int *pred = g_new (int, n_nodes * 2);
  int q_beg, q_end;
  gboolean result = FALSE;
  int i;

  for (i = 0; i < n_nodes; i++)
    {
      visited_in[i] = FALSE;
      visited_out[i] = FALSE;
    }

  /* visit seed-in */
  q_end = 0;
  queue[q_end] = seed;
  queue_dir[q_end] = FALSE;
  q_end++;
  visited_in[seed] = TRUE;
#ifdef VERBOSE
  g_print ("net_flow_augment\n");
#endif

  for (q_beg = 0; q_beg < q_end; q_beg++)
    {
      int node = queue[q_beg];
      gboolean node_dir = queue_dir[q_beg];

#ifdef VERBOSE
      g_print ("visit node %d%c\n", node, node_dir ? 'o' : 'i');
#endif

      if ((node_dir == !TWEAK) && priv->node_sink[node] == 0)
	{
	  /* found a path to the supersink, now actually augment */
	  int q_ptr;


#ifdef VERBOSE
	  g_print ("augment:");
#endif

	  priv->node_sink[node] = 1;
	  q_ptr = q_beg;
	  while (q_ptr != 0)
	    {
	      int pred_node;

	      node_dir = queue_dir[q_ptr];
	      q_ptr = pred[q_ptr];
	      pred_node = queue[q_ptr];
#ifdef VERBOSE
	      if (node_dir)
		g_print (" %di->%do:", pred_node, node);
	      else
		g_print (" %do->%di:", pred_node, node);
#endif
	      if (pred_node == node)
		{
		  if (node_dir)
		    /* edge from in to out */
		    {
#ifdef VERBOSE
		      g_print (" %d++", node);
#endif
		      priv->node_flow[node]++;
		    }
		  else
		    /* edge from out to in (ie reversing) */
		    {
#ifdef VERBOSE
		      g_print (" %d--", node);
#endif
		      priv->node_flow[node]--;
		    }
		}
	      else
		{
		  if (!node_dir)
		    {
		      /* find the edge from pred_node to node and
                         increment flow */
		      int *edges = priv->node_in_edges[node];
		      int degree = priv->node_in_degree[node];
		      int edge = -1;
		      int j;

		      for (j = 0; j < degree; j++)
			{
			  edge = edges[j];
			  if (priv->edge_src[edge] == pred_node)
			    break;
			}
#ifdef VERBOSE
		      g_print (" (%do->%di)++", pred_node, node);
#endif
		      priv->edge_flow[edge]++;
		    }
		  else
		    {
		      /* find the edge from node to pred_node and
			 decrement flow */
		      int *edges = priv->node_out_edges[node];
		      int degree = priv->node_out_degree[node];
		      int edge = -1;
		      int j;

		      for (j = 0; j < degree; j++)
			{
			  edge = edges[j];
			  if (priv->edge_dst[edge] == pred_node)
			    break;
			}
#ifdef VERBOSE
		      g_print (" (%do->%di)--", node, pred_node);
#endif
		      priv->edge_flow[edge]--;
		    }
		  node = pred_node;
		} /* if (pred_node == node) */
	    } /* while (q_ptr != 0) */
#ifdef VERBOSE
	  g_print ("\n");
#endif
	  result = TRUE;
	  break;
	}
      else
	{
	  /* have not found supersink, trace edges from current node */
	  if (!node_dir && priv->node_flow[node] < self->capacity[node] &&
	      !visited_out[node])
	    {
	      queue[q_end] = node;
	      queue_dir[q_end] = TRUE;
	      pred[q_end] = q_beg;
	      q_end++;
	      visited_out[node] = TRUE;
#ifdef VERBOSE
	      g_print ("add %do to queue (from %di)\n", node, node);
#endif
	    }
	  else if (node_dir && priv->node_flow[node] > 0 && !visited_in[node])
	    {
	      /* backwards flow from out to in; residual graph */
	      queue[q_end] = node;
	      queue_dir[q_end] = FALSE;
	      pred[q_end] = q_beg;
	      q_end++;
	      visited_in[node] = TRUE;
#ifdef VERBOSE
	      g_print ("add %di to queue (from %do)\n", node, node);
#endif
	    }

	  /* now, follow the incident edges (in original graph) */
	  if (node_dir)
	    {
	      /* outgoing edges */
	      int *edges = priv->node_out_edges[node];
	      int degree = priv->node_out_degree[node];
	      int j;

	      for (j = 0; j < degree; j++)
		{
		  int edge = edges[j];
		  int dst = priv->edge_dst[edge];

		  if (!visited_in[dst])
		    {
		      queue[q_end] = dst;
		      queue_dir[q_end] = FALSE;
		      pred[q_end] = q_beg;
		      q_end++;
#ifdef VERBOSE
		      g_print ("add %di to queue\n", dst);
#endif
		      visited_in[dst] = TRUE;
		    }
		}
	    }
	  else
	    {
	      /* ingoing edges */
	      int *edges = priv->node_in_edges[node];
	      int degree = priv->node_in_degree[node];
	      int j;

	      for (j = 0; j < degree; j++)
		{
		  int edge = edges[j];
		  int src = priv->edge_src[edge];

		  if (!visited_out[src] && priv->edge_flow[edge] > 0)
		    {
		      queue[q_end] = src;
		      queue_dir[q_end] = TRUE;
		      pred[q_end] = q_beg;
		      q_end++;
#ifdef VERBOSE
		      g_print ("add %do to queue\n", src);
#endif
		      visited_out[src] = TRUE;
		    }
		}
	    } /* if (node_dir) */

	} /* if ((node_dir) == !TWEAK ...) */
    } /* for (q_beg = 0...) */

  g_free (pred);
  g_free (queue_dir);
  g_free (queue);
  g_free (visited_out);
  g_free (visited_in);

  return result;
}

/**
 * net_flow_max_flow: Compute a maximum flow.
 * @self: The #NetFlow context.
 * @seed: The seed.
 * @caps: The capacities.
 * @n_caps: The number of elements in @caps.
 *
 * Computes a network flow, storing the results in the priv element
 * of @self.
 **/
void
virgule_net_flow_max_flow (NetFlow *self, gint seed, const int *caps, int n_caps)
{
  int n_aug;
  int *pred_list;
  int i, n_nodes;
  NetFlowPriv *priv;

  net_flow_init_graph (self);
  priv = self->priv;
  pred_list = virgule_net_flow_assign_tree (self, seed, caps, n_caps);
#if 1
  virgule_net_flow_sanity_check_tree (self, seed, pred_list);
  /* if enabled, start from tree flow rather than zero */
  net_flow_from_tree (self, pred_list);
#endif
  g_free (pred_list);

  if (TWEAK)
    {
      n_nodes = self->n_nodes;
      for (i = 0; i < n_nodes; i++)
	{
	  priv->node_flow[i] -= priv->node_sink[i];
	  self->capacity[i]--;
	}
    }

  virgule_net_flow_sanity_check (self, seed);

  n_aug = 0;
  while (net_flow_augment (self, seed))
    {
      n_aug++;
#ifdef VERBOSE
      if (n_aug % 100 == 0)
	g_print ("%d augmentations\n", n_aug);
#endif	
    }
#ifdef VERBOSE
  g_print ("total flow %d with %d augmentations\n",
	   priv->node_flow[seed],
	   n_aug);
#endif

  virgule_net_flow_sanity_check (self, seed);
}

/**
 * net_flow_sanity_check: Check that flow satisfies constraints.
 * @self: The #NetFlow context.
 * @seed: The seed.
 *
 * Checks that:
 * + For all in nodes other than seed, the sum of inedge flow is equal
 *   to supersink flow plus node flow.
 * + For all out nodes, the sum of outedge flow is equal to node flow.
 * + All flows are nonnegative.
 * + All supersink flows are either 0 or 1.
 * + All node flows are less-than-or-equal-to the capacity.
 *
 * Return value: 0 if ok.
 **/
/*
 *  Appears to be called only in this source file. Should this be static?
 */
int
virgule_net_flow_sanity_check (NetFlow *self, gint seed)
{
  int e, n, j;
  NetFlowPriv *priv = self->priv;
  int flow;
  int result = 0;

  for (e = 0; e < priv->n_edges; e++)
    {
      if (priv->edge_flow[e] < 0)
	{
	  g_warning ("Negative edge flow\n");
	  result = -1;
	}
    }

  for (n = 0; n < self->n_nodes; n++)
    {
      int cap;
      if (priv->node_sink[n] < 0 || priv->node_sink[n] > 1)
	{
	  g_warning ("Node %d flow %d to sink\n", n, priv->node_sink[n]);
	  result = -1;
	}
      if (priv->node_flow[n] < 0)
	{
	  g_warning ("Negative node %d flow %d\n", n, priv->node_flow[n]);
	  result = -1;
	}
      cap = self->capacity[n];
      if (cap < 0)
	cap =0;
      if (priv->node_flow[n] > cap)
	{
	  g_warning ("Node %d flow %d over capacity %d\n",
		     n, priv->node_flow[n], self->capacity[n]);
	  result = -1;
	}
      if (n != seed)
	{
	  int *in_edges = priv->node_in_edges[n];
	  int in_degree = priv->node_in_degree[n];

	  flow = 0;
	  for (j = 0; j < in_degree; j++)
	    {
	      e = in_edges[j];
	      if (priv->edge_dst[e] != n)
		{
		  g_warning ("Edge/node data structure inconsistency\n");
		  result = -1;
		}
	      flow += priv->edge_flow[e];
	    }
	  if (flow != priv->node_flow[n] + priv->node_sink[n])
	    {
	      g_warning ("Flow inconsistency, node %din\n", n);
	      result = -1;
	    }
	}
      {
	int *out_edges = priv->node_out_edges[n];
	int out_degree = priv->node_out_degree[n];

	flow = 0;
	for (j = 0; j < out_degree; j++)
	  {
	    e = out_edges[j];
	    if (priv->edge_src[e] != n)
	      {
		g_warning ("Edge/node data structure inconsistency\n");
		result = -1;
	      }
	    flow += priv->edge_flow[e];
	  }
	if (flow != priv->node_flow[n])
	  {
	    g_warning ("Flow inconsistency, node %dout, node_flow = %d, sum of flow is %d\n",
		       n, priv->node_flow[n], flow);
	      result = -1;
	    }
      }
    }
  return result;
}

/**
 * net_flow_extract: Extract flow.
 * @self: The #NetFlow context.
 *
 * Extracts the flow from net_flow_max_flow(). The resulting array is
 * allocated with glib, and has a 1 or 0 depending on whether the node
 * is accepted. Call this function once only per NetFlow. The result
 * will survive a net_flow_free(), and needs to be g_free()'d.
 *
 * Return value: flow array.
 **/
int *
virgule_net_flow_extract (NetFlow *self)
{
  int *result;

  result = self->priv->node_sink;
  self->priv->node_sink = NULL;
  return result;
}

