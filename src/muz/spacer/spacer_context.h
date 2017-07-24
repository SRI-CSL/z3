/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_context.h

Abstract:

    SPACER for datalog

Author:

    Anvesh Komuravelli
    Arie Gurfinkel

Revision History:

    Based on ../pdr/pdr_context.h by
     Nikolaj Bjorner (nbjorner)

--*/

#ifndef _SPACER_CONTEXT_H_
#define _SPACER_CONTEXT_H_

#ifdef _CYGWIN
#undef min
#undef max
#endif
#include <queue>
#include "spacer_manager.h"
#include "spacer_prop_solver.h"
#include "fixedpoint_params.hpp"

namespace datalog {
    class rule_set;
    class context;
};

namespace spacer {

    class pred_transformer;
    class model_node;
    class derivation;
    class model_search;
    class context;

    typedef obj_map<datalog::rule const, app_ref_vector*> rule2inst;
    typedef obj_map<func_decl, pred_transformer*> decl2rel;

  class reach_fact;
  typedef ref<reach_fact> reach_fact_ref;
  typedef sref_vector<reach_fact> reach_fact_ref_vector;

class reach_fact {
    unsigned m_ref_count;

    expr_ref m_fact;
    ptr_vector<app> m_aux_vars;

    const datalog::rule &m_rule;
    reach_fact_ref_vector m_justification;

    bool m_init;

  public:
    reach_fact (ast_manager &m, const datalog::rule &rule,
                expr* fact, const ptr_vector<app> &aux_vars,
                bool init = false) :
      m_ref_count (0), m_fact (fact, m), m_aux_vars (aux_vars),
      m_rule(rule), m_init (init) {}
    reach_fact (ast_manager &m, const datalog::rule &rule,
                expr* fact, bool init = false) :
      m_ref_count (0), m_fact (fact, m), m_rule(rule), m_init (init) {}

    bool is_init () {return m_init;}
    const datalog::rule& get_rule () {return m_rule;}

    void add_justification (reach_fact *f) {m_justification.push_back (f);}
    const reach_fact_ref_vector& get_justifications () {return m_justification;}

    expr *get () {return m_fact.get ();}
    const ptr_vector<app> &aux_vars () {return m_aux_vars;}

    void inc_ref () {++m_ref_count;}
    void dec_ref ()
    {
      SASSERT (m_ref_count > 0);
      --m_ref_count;
        if(m_ref_count == 0) { dealloc(this); }
    }
  };


class lemma;
typedef ref<lemma> lemma_ref;
typedef sref_vector<lemma> lemma_ref_vector;

// a lemma
class lemma {
    unsigned m_ref_count;

    ast_manager &m;
    expr_ref m_fml;
    app_ref_vector m_bindings;
    unsigned m_lvl;

public:
    lemma (ast_manager &manager, expr * fml, unsigned lvl) :
        m_ref_count(0), m(manager), m_fml (fml, m), m_bindings(m), m_lvl(lvl) {}

    lemma (const lemma &other)
        : m(other.m), m_fml (other.m_fml), m_bindings(other.m_bindings), m_lvl (other.m_lvl) {}

    expr * get () const {return m_fml.get ();}
    unsigned level () const {return m_lvl;}
    void set_level (unsigned lvl) { m_lvl = lvl;}
    app_ref_vector& get_bindings() { return m_bindings; }
    void add_binding(app_ref_vector& binding) {m_bindings.append(binding);}
    void mk_insts(expr_ref_vector& inst, expr* fml = nullptr);
    bool is_ground () const { return ::is_quantifier (m_fml); }

    void inc_ref () {++m_ref_count;}
    void dec_ref ()
    {
      SASSERT (m_ref_count > 0);
      --m_ref_count;
        if(m_ref_count == 0) { dealloc(this); }
    }
};

struct lemma_lt_proc : public std::binary_function<const lemma*, const lemma *, bool> {
    bool operator() (const lemma *a, const lemma *b) {
        return (a->level () < b->level ()) ||
            (a->level () == b->level () &&
             ast_lt_proc() (a->get (), b->get ()));
    }
};



//
// Predicate transformer state.
// A predicate transformer corresponds to the
// set of rules that have the same head predicates.
//

class pred_transformer {

    struct stats {
        unsigned m_num_propagations;
        unsigned m_num_invariants;
        stats() { reset(); }
        void reset() { memset(this, 0, sizeof(*this)); }
    };

    /// manager of the lemmas in all the frames
#include "spacer_legacy_frames.h"
    class frames {
    private:
        pred_transformer &m_pt;
        lemma_ref_vector m_lemmas;
        unsigned m_size;

        bool m_sorted;
        lemma_lt_proc m_lt;

        void sort ();

    public:
        frames (pred_transformer &pt) : m_pt (pt), m_size(0), m_sorted (true) {}
        ~frames() {}
        void simplify_formulas ();

        pred_transformer& pt () {return m_pt;}


        void get_frame_lemmas (unsigned level, expr_ref_vector &out)
            {
                for (unsigned i = 0, sz = m_lemmas.size (); i < sz; ++i)
                    if(m_lemmas[i]->level() == level) { out.push_back(m_lemmas[i]->get()); }
            }
        void get_frame_geq_lemmas (unsigned level, expr_ref_vector &out)
            {
                for (unsigned i = 0, sz = m_lemmas.size (); i < sz; ++i)
                    if(m_lemmas [i]->level() >= level) { out.push_back(m_lemmas[i]->get()); }
            }


        unsigned size () const {return m_size;}
        unsigned lemma_size () const {return m_lemmas.size ();}
        void add_frame () {m_size++;}
        void inherit_frames (frames &other)
            {
                for (unsigned i = 0, sz = other.m_lemmas.size (); i < sz; ++i)
                { add_lemma(other.m_lemmas [i]->get(), other.m_lemmas [i]->level(), other.m_lemmas[i]->get_bindings()); }
                m_sorted = false;
            }

        bool add_lemma (expr * lemma, unsigned level, app_ref_vector& binding);
        bool add_lemma (lemma *lem);
        void propagate_to_infinity (unsigned level);
        bool propagate_to_next_level (unsigned level);


    };



    typedef obj_map<datalog::rule const, expr*> rule2expr;
    typedef obj_map<datalog::rule const, ptr_vector<app> > rule2apps;

    manager&                     pm;        // spacer-manager
    ast_manager&                 m;         // manager
    context&                     ctx;

    func_decl_ref                m_head;    // predicate
    func_decl_ref_vector         m_sig;     // signature
    ptr_vector<pred_transformer> m_use;     // places where 'this' is referenced.
    ptr_vector<datalog::rule>    m_rules;   // rules used to derive transformer
    prop_solver                  m_solver;  // solver context
    solver*                      m_reach_ctx; // context for reachability facts
    frames                       m_frames;

    reach_fact_ref_vector        m_reach_facts; // reach facts
    /// Number of initial reachability facts
    unsigned                     m_rf_init_sz;
    obj_map<expr, datalog::rule const*> m_tag2rule; // map tag predicate to rule.
    rule2expr                    m_rule2tag;        // map rule to predicate tag.
    rule2inst                    m_rule2inst;       // map rules to instantiations of indices
    rule2expr                    m_rule2transition; // map rules to transition
    rule2apps                    m_rule2vars;       // map rule to auxiliary variables
    expr_ref                     m_transition;      // transition relation.
    expr_ref                     m_initial_state;   // initial state.
    app_ref                      m_extend_lit;      // literal to extend initial state
    bool                         m_all_init;        // true if the pt has no uninterpreted body in any rule
    ptr_vector<func_decl>        m_predicates;
    stats                        m_stats;
    stopwatch                    m_initialize_watch;
    stopwatch                    m_must_reachable_watch;



    /// Auxiliary variables to represent different disjunctive
    /// cases of must summaries. Stored over 'n' (a.k.a. new)
    /// versions of the variables
    expr_ref_vector              m_reach_case_vars;

    void init_sig();
    void ensure_level(unsigned level);
    void add_lemma_core (lemma *lemma);
    void add_lemma_from_child (pred_transformer &child, lemma *lemma, unsigned lvl);

    void mk_assumptions(func_decl* head, expr* fml, expr_ref_vector& result);

    // Initialization
    void init_rules(decl2rel const& pts, expr_ref& init, expr_ref& transition);
    void init_rule(decl2rel const& pts, datalog::rule const& rule, vector<bool>& is_init,
                   ptr_vector<datalog::rule const>& rules, expr_ref_vector& transition);
    void init_atom(decl2rel const& pts, app * atom, app_ref_vector& var_reprs, expr_ref_vector& conj, unsigned tail_idx);

    void simplify_formulas(tactic& tac, expr_ref_vector& fmls);

    // Debugging
    bool check_filled(app_ref_vector const& v) const;

    void add_premises(decl2rel const& pts, unsigned lvl, datalog::rule& rule, expr_ref_vector& r);

    expr* mk_fresh_reach_case_var ();

public:
    pred_transformer(context& ctx, manager& pm, func_decl* head);
    ~pred_transformer();

    inline bool use_native_mbp ();
    reach_fact *get_reach_fact (expr *v)
        {
            for (unsigned i = 0, sz = m_reach_facts.size (); i < sz; ++i)
                if(v == m_reach_facts [i]->get()) { return m_reach_facts[i]; }
            return NULL;
        }

    void add_rule(datalog::rule* r) { m_rules.push_back(r); }
    void add_use(pred_transformer* pt) { if(!m_use.contains(pt)) { m_use.insert(pt); } }
    void initialize(decl2rel const& pts);

    func_decl* head() const { return m_head; }
    ptr_vector<datalog::rule> const& rules() const { return m_rules; }
    func_decl* sig(unsigned i) const { return m_sig[i]; } // signature
    func_decl* const* sig() { return m_sig.c_ptr(); }
    unsigned  sig_size() const { return m_sig.size(); }
    expr*  transition() const { return m_transition; }
    expr*  initial_state() const { return m_initial_state; }
    expr*  rule2tag(datalog::rule const* r) { return m_rule2tag.find(r); }
    unsigned get_num_levels() { return m_frames.size (); }
    expr_ref get_cover_delta(func_decl* p_orig, int level);
    void     add_cover(unsigned level, expr* property);
    expr_ref get_reachable ();

    std::ostream& display(std::ostream& strm) const;

    void collect_statistics(statistics& st) const;
    void reset_statistics();

    bool is_must_reachable (expr* state, model_ref* model = 0);
    /// \brief Returns reachability fact active in the given model
    /// all determines whether initial reachability facts are included as well
    reach_fact *get_used_reach_fact (model_evaluator_util& mev, bool all = true);
    /// \brief Returns reachability fact active in the origin of the given model
    reach_fact* get_used_origin_reach_fact (model_evaluator_util &mev, unsigned oidx);
    expr_ref get_origin_summary (model_evaluator_util &mev,
                                 unsigned level, unsigned oidx, bool must,
                                 const ptr_vector<app> **aux);

    void remove_predecessors(expr_ref_vector& literals);
    void find_predecessors(datalog::rule const& r, ptr_vector<func_decl>& predicates) const;
    void find_predecessors(vector<std::pair<func_decl*, unsigned> >& predicates) const;
    datalog::rule const* find_rule(model &mev, bool& is_concrete,
                                   vector<bool>& reach_pred_used,
                                   unsigned& num_reuse_reach);
    expr* get_transition(datalog::rule const& r) { return m_rule2transition.find(&r); }
    ptr_vector<app>& get_aux_vars(datalog::rule const& r) { return m_rule2vars.find(&r); }

    bool propagate_to_next_level(unsigned level);
    void propagate_to_infinity(unsigned level);
    /// \brief  Add a lemma to the current context and all users
    bool add_lemma(expr * lemma, unsigned lvl, app_ref_vector& binding);
    bool add_lemma(expr * lemma, unsigned lvl) {
        app_ref_vector binding(m);
        return add_lemma(lemma, lvl, binding);
    }
    bool add_lemma(lemma* lem) {return m_frames.add_lemma(lem);}
    expr* get_reach_case_var (unsigned idx) const;
    bool has_reach_facts () const { return !m_reach_facts.empty () ;}

    /// initialize reachability facts using initial rules
    void init_reach_facts ();
    void add_reach_fact (reach_fact *fact);  // add reachability fact
    reach_fact* get_last_reach_fact () const { return m_reach_facts.back (); }
    expr* get_last_reach_case_var () const;


    lbool is_reachable(model_node& n, expr_ref_vector* core, model_ref *model,
                       unsigned& uses_level, bool& is_concrete,
                       datalog::rule const*& r,
                       vector<bool>& reach_pred_used,
                       unsigned& num_reuse_reach);
    bool is_invariant(unsigned level, expr* lemma,
                      unsigned& solver_level, expr_ref_vector* core = 0);
    bool check_inductive(unsigned level, expr_ref_vector& state,
                         unsigned& assumes_level);

    expr_ref get_formulas(unsigned level, bool add_axioms);

    void simplify_formulas();

    expr_ref get_propagation_formula(decl2rel const& pts, unsigned level);

    context& get_context () const {return ctx;}
    manager& get_manager() const { return pm; }
    ast_manager& get_ast_manager() const { return m; }

    void add_premises(decl2rel const& pts, unsigned lvl, expr_ref_vector& r);

    void close(expr* e);

    app_ref_vector& get_inst(datalog::rule const* r) { return *m_rule2inst.find(r);}

    void inherit_properties(pred_transformer& other);

    void ground_free_vars(expr* e, app_ref_vector& vars, ptr_vector<app>& aux_vars,
                          bool is_init);

    /// \brief Adds a given expression to the set of initial rules
    app* extend_initial (expr *e);

    /// \brief Returns true if the obligation is already blocked by current lemmas
    bool is_blocked (model_node &n, unsigned &uses_level);
    /// \brief Returns true if the obligation is already blocked by current quantified lemmas
    bool is_qblocked (model_node &n);

};

  typedef ref<model_node> model_node_ref;

  /**
   * A node in the search tree.
   */
  class model_node {
      friend class context;
    unsigned m_ref_count;
    /// parent node
    model_node_ref          m_parent;
    /// predicate transformer
    pred_transformer&       m_pt;
    /// post-condition decided by this node
    expr_ref                m_post;
    // if m_post is not ground, then m_binding is an instantiation for
    // all quantified variables
    app_ref_vector          m_binding;
    /// new post to be swapped in for m_post
    expr_ref                m_new_post;
    /// level at which to decide the post
    unsigned                m_level;

    unsigned                m_depth;

    /// whether a concrete answer to the post is found
    bool                    m_open;
    /// whether to use farkas generalizer to construct a lemma blocking this node
    bool                    m_use_farkas;

    unsigned                m_weakness;
    /// derivation representing the position of this node in the parent's rule
    scoped_ptr<derivation>   m_derivation;

    ptr_vector<model_node>  m_kids;
  public:
      model_node (model_node* parent, pred_transformer& pt,
                  unsigned level, unsigned depth=0);

      ~model_node() {if(m_parent) { m_parent->erase_child(*this); }}

      unsigned weakness() {return m_weakness;}
      void bump_weakness() {m_weakness++;}
      void reset_weakness() {m_weakness=0;}

      void inc_level () {m_level++; m_depth++;reset_weakness();}

      void set_derivation (derivation *d) {m_derivation = d;}
      bool has_derivation () const {return (bool)m_derivation;}
      derivation &get_derivation() const {return *m_derivation.get ();}
      void reset_derivation () {set_derivation (NULL);}
      /// detaches derivation from the node without deallocating
      derivation* detach_derivation () {return m_derivation.detach ();}

      model_node* parent () const { return m_parent.get (); }

      pred_transformer& pt () const { return m_pt; }
      ast_manager& get_ast_manager () const { return m_pt.get_ast_manager (); }
      manager& get_manager () const { return m_pt.get_manager (); }
      context& get_context () const {return m_pt.get_context ();}

      unsigned level () const { return m_level; }
      unsigned depth () const {return m_depth;}

      bool use_farkas_generalizer () const {return m_use_farkas;}
      void set_farkas_generalizer (bool v) {m_use_farkas = v;}

      expr* post () const { return m_post.get (); }
      void set_post(expr *post);
      void set_post(expr *post, app_ref_vector const &b);

      /// indicate that a new post should be set for the node
      void new_post(expr *post) {if(post != m_post) {m_new_post = post;}}
      /// true if the node needs to be updated outside of the priority queue
      bool is_dirty () {return m_new_post;}
      /// clean a dirty node
      void clean();

      void reset () {clean (); m_derivation = NULL; m_open = true;}

      bool is_closed () const { return !m_open; }
      void close();

      void add_child (model_node &v) {m_kids.push_back (&v);}
      void erase_child (model_node &v) {m_kids.erase (&v);}

      bool is_ground () { return m_binding.empty (); }
      app_ref_vector const &get_binding() const {return m_binding;}
      /*
       * Return skolem variables that appear in post
       */
      void get_skolems(app_ref_vector& v);

      void inc_ref () {++m_ref_count;}
      void dec_ref ()
          {
              --m_ref_count;
              if(m_ref_count == 0) { dealloc(this); }
          }

  };


  struct model_node_lt :
    public std::binary_function<const model_node*, const model_node*, bool>
  {bool operator() (const model_node *pn1, const model_node *pn2) const;};

  struct model_node_gt :
    public std::binary_function<const model_node*, const model_node*, bool> {
    model_node_lt lt;
    bool operator() (const model_node *n1, const model_node *n2) const
    {return lt(n2, n1);}
  };

  struct model_node_ref_gt :
    public std::binary_function<const model_node_ref&, const model_ref &, bool> {
    model_node_gt gt;
    bool operator() (const model_node_ref &n1, const model_node_ref &n2) const
    {return gt (n1.get (), n2.get ());}
  };


  /**
   */
  class derivation {
    /// a single premise of a derivation
    class premise {
      pred_transformer &m_pt;
      /// origin order in the rule
      unsigned m_oidx;
      /// summary fact corresponding to the premise
      expr_ref m_summary;
      ///  whether this is a must or may premise
      bool m_must;
      app_ref_vector m_ovars;

    public:
      premise (pred_transformer &pt, unsigned oidx, expr *summary, bool must,
               const ptr_vector<app> *aux_vars = NULL);
      premise (const premise &p);

      bool is_must () {return m_must;}
      expr * get_summary () {return m_summary.get ();}
      app_ref_vector &get_ovars () {return m_ovars;}
      unsigned get_oidx () {return m_oidx;}
      pred_transformer &pt () {return m_pt;}

      /// \brief Updated the summary.
      /// The new summary is over n-variables.
      void set_summary (expr * summary, bool must,
                        const ptr_vector<app> *aux_vars = NULL);
    };


    /// parent model node
    model_node&                         m_parent;

    /// the rule corresponding to this derivation
    const datalog::rule &m_rule;

    /// the premises
    vector<premise>                     m_premises;
    /// pointer to the active premise
    unsigned                            m_active;
    // transition relation over origin variables
    expr_ref                            m_trans;
    //  implicitly existentially quantified variables in m_trans
    app_ref_vector                      m_evars;
    /// -- create next child using given model as the guide
    /// -- returns NULL if there is no next child
    model_node* create_next_child (model_evaluator_util &mev);
  public:
    derivation (model_node& parent, datalog::rule const& rule,
                expr *trans, app_ref_vector const &evars);
    void add_premise (pred_transformer &pt, unsigned oidx,
                      expr * summary, bool must, const ptr_vector<app> *aux_vars = NULL);

    /// creates the first child. Must be called after all the premises
    /// are added. The model must be valid for the premises
    /// Returns NULL if no child exits
    model_node *create_first_child (model_evaluator_util &mev);

    /// Create the next child. Must summary of the currently active
    /// premise must be consistent with the transition relation
    model_node *create_next_child ();

    datalog::rule const& get_rule () const { return m_rule; }
    model_node& get_parent () const { return m_parent; }
    ast_manager &get_ast_manager () const {return m_parent.get_ast_manager ();}
    manager &get_manager () const {return m_parent.get_manager ();}
    context &get_context() const {return m_parent.get_context();}
  };


  class model_search {
    model_node_ref  m_root;
    unsigned m_max_level;
    unsigned m_min_depth;

    std::priority_queue<model_node_ref, std::vector<model_node_ref>,
                        model_node_ref_gt>     m_obligations;

  public:
    model_search(): m_root(NULL), m_max_level(0), m_min_depth(0) {}
    ~model_search();

    void reset();
    model_node * top ();
    void pop () {m_obligations.pop ();}
    void push (model_node &n) {m_obligations.push (&n);}

    void inc_level ()
    {
      SASSERT (!m_obligations.empty () || m_root);
        m_max_level++;
        m_min_depth++;
        if(m_root && m_obligations.empty()) { m_obligations.push(m_root); }
    }

    model_node& get_root() const { return *m_root.get (); }
    void set_root(model_node& n);
    bool is_root (model_node& n) const {return m_root.get () == &n;}

    unsigned max_level () {return m_max_level;}
    unsigned min_depth () {return m_min_depth;}
    unsigned size () {return m_obligations.size ();}


    //std::ostream& display(std::ostream& out) const;
    expr_ref get_trace(context const& ctx);
  };


    // 'state' is unsatisfiable at 'level' with 'core'.
    // Minimize or weaken core.
    class core_generalizer {
    protected:
        context& m_ctx;
    public:
        typedef vector<std::pair<expr_ref_vector,unsigned> > cores;
        core_generalizer(context& ctx): m_ctx(ctx) {}
        virtual ~core_generalizer() {}
        virtual void operator()(model_node& n, expr_ref_vector& core, unsigned& uses_level) = 0;
    virtual void operator()(model_node& n, expr_ref_vector const& core, unsigned uses_level, cores& new_cores)
    {
            new_cores.push_back(std::make_pair(core, uses_level));
            if (!core.empty()) {
                (*this)(n, new_cores.back().first, new_cores.back().second);
            }
        }
        virtual void collect_statistics(statistics& st) const {}
        virtual void reset_statistics() {}
    };


    // AK: need to clean this up!
    class context {

        struct stats {
            unsigned m_num_queries;
            unsigned m_num_reach_queries;
            unsigned m_num_reuse_reach;
            unsigned m_max_query_lvl;
            unsigned m_max_depth;
            unsigned m_cex_depth;
            unsigned m_expand_node_undef;
            unsigned m_num_lemmas;
            unsigned m_num_restarts;
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
        };

        // stat watches
        stopwatch m_solve_watch;
        stopwatch m_propagate_watch;
        stopwatch m_reach_watch;
        stopwatch m_is_reach_watch;
        stopwatch m_create_children_watch;
        stopwatch m_init_rules_watch;

        fixedpoint_params const&    m_params;
        ast_manager&         m;
        datalog::context*    m_context;
        manager              m_pm;
        decl2rel             m_rels;         // Map from relation predicate to fp-operator.
        func_decl_ref        m_query_pred;
        pred_transformer*    m_query;
        mutable model_search m_search;
        lbool                m_last_result;
        unsigned             m_inductive_lvl;
        unsigned             m_expanded_lvl;
        ptr_vector<core_generalizer>  m_core_generalizers;
        stats                m_stats;
        model_converter_ref  m_mc;
        proof_converter_ref  m_pc;
        bool                 m_use_native_mbp;
        bool                 m_ground_cti;
        bool                 m_instantiate;
        bool                 m_use_qlemmas;
        bool                 m_weak_abs;
        bool                 m_use_restarts;
        unsigned             m_restart_initial_threshold;

        // Utility: Quantified Lemmas
        app_ref_vector m_skolems;
        void ensure_skolems(ptr_vector<sort>& sorts);

        // Functions used by search.
        lbool solve_core (unsigned from_lvl = 0);
        bool check_reachability ();
        bool propagate(unsigned min_prop_lvl, unsigned max_prop_lvl,
                       unsigned full_prop_lvl);
        bool is_reachable(model_node &n);
        lbool expand_node(model_node& n);
        reach_fact *mk_reach_fact (model_node& n, model_evaluator_util &mev,
                                   datalog::rule const& r);
        bool create_children(model_node& n, datalog::rule const& r,
                             model_evaluator_util &model,
                             const vector<bool>& reach_pred_used);
        expr_ref mk_sat_answer() const;
        expr_ref mk_unsat_answer() const;

        // Generate inductive property
        void get_level_property(unsigned lvl, expr_ref_vector& res,
                                vector<relation_info> & rs) const;


        // Initialization
        class classifier_proc;
        void init_core_generalizers(datalog::rule_set& rules);

        bool check_invariant(unsigned lvl);
        bool check_invariant(unsigned lvl, func_decl* fn);

        void checkpoint();

        void init_rules(datalog::rule_set& rules, decl2rel& transformers);

        void simplify_formulas();

        void reset_core_generalizers();

        bool validate();

        unsigned get_cex_depth ();

    public:
        /**
           Initial values of predicates are stored in corresponding relations in dctx.

           We check whether there is some reachable state of the relation checked_relation.
        */
        context(
            fixedpoint_params const&  params,
            ast_manager&       m);

        ~context();

        fixedpoint_params const& get_params() const { return m_params; }
        bool use_native_mbp () {return m_use_native_mbp;}
        bool use_ground_cti () {return m_ground_cti;}
        bool use_instantiate () { return m_instantiate; }
        bool use_qlemmas () {return m_use_qlemmas; }

        ast_manager&      get_ast_manager() const { return m; }
        manager&          get_manager() { return m_pm; }
        decl2rel const&   get_pred_transformers() const { return m_rels; }
        pred_transformer& get_pred_transformer(func_decl* p) const
        { return *m_rels.find(p); }
        datalog::context& get_datalog_context() const
        { SASSERT(m_context); return *m_context; }
        expr_ref          get_answer();
        /**
         * get bottom-up (from query) sequence of ground predicate instances
         * (for e.g. P(0,1,0,0,3)) that together form a ground derivation to query
         */
        expr_ref          get_ground_sat_answer ();

        void collect_statistics(statistics& st) const;
        void reset_statistics();

        std::ostream& display(std::ostream& strm) const;

        void display_certificate(std::ostream& strm) const;

        lbool solve(unsigned from_lvl = 0);

        lbool solve_from_lvl (unsigned from_lvl);

        void reset();

        void set_query(func_decl* q) { m_query_pred = q; }

        void set_unsat() { m_last_result = l_false; }

        void set_model_converter(model_converter_ref& mc) { m_mc = mc; }

        void get_rules_along_trace (datalog::rule_ref_vector& rules);

        model_converter_ref get_model_converter() { return m_mc; }

        void set_proof_converter(proof_converter_ref& pc) { m_pc = pc; }

        void update_rules(datalog::rule_set& rules);

        void set_axioms(expr* axioms) { m_pm.set_background(axioms); }

        unsigned get_num_levels(func_decl* p);

        expr_ref get_cover_delta(int level, func_decl* p_orig, func_decl* p);

        void add_cover(int level, func_decl* pred, expr* property);

        expr_ref get_reachable (func_decl* p);

        void add_invariant (func_decl *pred, expr* property);

        model_ref get_model();

        proof_ref get_proof() const;

        model_node& get_root() const { return m_search.get_root(); }

        expr_ref get_constraints (unsigned lvl);
        void add_constraints (unsigned lvl, expr_ref c);

        struct sk_lt_proc :
        public std::binary_function<app const*, app const*, bool> {
          sk_lt_proc(app_ref_vector& skolems) : m_skolems(skolems) {}
          bool operator() (app const *a, app const *b)
          {
              bool a_skolem = a->get_decl()->get_name().str().find("zk!") != std::string::npos;
              bool b_skolem = b->get_decl()->get_name().str().find("zk!") != std::string::npos;
              if (a_skolem || b_skolem) {
                  if (a_skolem && !b_skolem)
                { return true; }
                  else if (!a_skolem && b_skolem)
                { return false; }
                  else {
                      bool a_found = false, b_found = false;
                      for (unsigned sk=0; sk < m_skolems.size(); sk++) {
                          if (m_skolems.get(sk)->hash() == a->hash()) {
                            if(b_found) { return false; }
                              a_found = true;
                          }
                          if (m_skolems.get(sk)->hash() == b->hash()) {
                            if(a_found) { return true; }
                              b_found = true;
                          }
                      }
                      SASSERT(false);
                      return false;
                  }
              }
              return a->get_id() < b->get_id();
          }

        private:
          const app_ref_vector& m_skolems;
        };
    };

    inline bool pred_transformer::use_native_mbp () {return ctx.use_native_mbp ();}
};

#endif
