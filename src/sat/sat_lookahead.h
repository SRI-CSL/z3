/*++
Copyright (c) 2017 Microsoft Corporation

Module Name:

    sat_lookahead.h

Abstract:
   
    Lookahead SAT solver in the style of March.
    Thanks also to the presentation in sat11.w.    

Author:

    Nikolaj Bjorner (nbjorner) 2017-2-11

Notes:

--*/
#ifndef _SAT_LOOKAHEAD_H_
#define _SAT_LOOKAHEAD_H_

namespace sat {
    class lookahead {
        solver& s;

        struct config {
            double   m_dl_success;
            float    m_alpha;
            float    m_max_score;
            unsigned m_max_hlevel; 
            unsigned m_min_cutoff;
            unsigned m_level_cand;

            config() {
                m_max_hlevel = 50;
                m_alpha = 3.5;
                m_max_score = 20.0;
                m_min_cutoff = 30;
                m_level_cand = 600;
            }
        };

        struct statistics {
            unsigned m_propagations;
            statistics() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
        };

        config                 m_config;
        double                 m_delta_trigger; 

        literal_vector         m_trail;         // trail of units
        unsigned_vector        m_trail_lim;
        literal_vector         m_units;         // units learned during lookahead
        unsigned_vector        m_units_lim;
        vector<literal_vector> m_binary;        // literal: binary clauses
        unsigned_vector        m_binary_trail;  // trail of added binary clauses
        unsigned_vector        m_binary_trail_lim; 
        unsigned               m_qhead;         // propagation queue head
        unsigned_vector        m_qhead_lim;
        clause_vector          m_clauses;       // non-binary clauses
        clause_allocator       m_cls_allocator;        
        bool                   m_inconsistent;
        unsigned_vector        m_bstamp;        // literal: timestamp for binary implication
        vector<svector<float> >  m_H;           // literal: fitness score
        svector<float>         m_rating;        // var:     pre-selection rating
        unsigned               m_bstamp_id;     // unique id for binary implication.
        char_vector            m_assignment;    // literal: assignment 
        vector<watch_list>     m_watches;       // literal: watch structure
        indexed_uint_set       m_freevars;
        statistics             m_stats;

        void add_binary(literal l1, literal l2) {
            SASSERT(l1 != l2);
            SASSERT(~l1 != l2);
            m_binary[(~l1).index()].push_back(l2);
            m_binary[(~l2).index()].push_back(l1);
            m_binary_trail.push_back((~l1).index());
        }

        void del_binary(unsigned idx) {
            literal_vector & lits = m_binary[idx];
            literal l = lits.back();
            lits.pop_back();
            m_binary[(~l).index()].pop_back();
        }

        // -------------------------------------
        // track consequences of binary clauses
        // see also 72 - 79 in sat11.w

        void inc_bstamp() {
            ++m_bstamp_id;
            if (m_bstamp_id == 0) {
                ++m_bstamp_id;
                m_bstamp.fill(0);
            }
        }
        void set_bstamp(literal l) {
            m_bstamp[l.index()] = m_bstamp_id;
        }
        void set_bstamps(literal l) {
            inc_bstamp();
            set_bstamp(l);
            literal_vector const& conseq = m_binary[l.index()];
            for (unsigned i = 0; i < conseq.size(); ++i) {
                set_bstamp(conseq[i]);
            }
        }
        bool is_stamped(literal l) const { return m_bstamp[l.index()] == m_bstamp_id; }

        /**
           \brief add one-step transitive closure of binary implications
                  return false if we learn a unit literal.
           \pre all implicants of ~u are stamped.
                u \/ v is true
        **/

        bool add_tc1(literal u, literal v) {
            unsigned sz = m_binary[v.index()].size();
            for (unsigned i = 0; i < sz; ++i) {
                literal w = m_binary[v.index()][i];
                // ~v \/ w
                if (!is_fixed(w)) {
                    if (is_stamped(~w)) {
                        // u \/ v, ~v \/ w, u \/ ~w => u is unit
                        assign(u);        
                        return false;
                    }
                    add_binary(u, w);
                }
            }
            return true;
        }

        /**
           \brief main routine for adding a new binary clause dynamically.
         */
        void try_add_binary(literal u, literal v) {
            SASSERT(u.var() != v.var());
            set_bstamps(~u);
            if (is_stamped(~v)) {                
                assign(u);        // u \/ ~v, u \/ v => u is a unit literal
            }
            else if (!is_stamped(v) && add_tc1(u, v)) {
                // u \/ v is not in index
                set_bstamps(~v);
                if (is_stamped(~u)) {
                    assign(v);    // v \/ ~u, u \/ v => v is a unit literal
                }
                else if (add_tc1(v, u)) {
                    add_binary(u, v);
                }
            }
        }

        // -------------------------------------
        // pre-selection
        // see also 91 - 102 sat11.w

        struct candidate {
            bool_var m_var;
            float    m_rating;
            candidate(bool_var v, float r): m_var(v), m_rating(r) {}
        };
        svector<candidate> m_candidates;

        float get_rating(bool_var v) const { return m_rating[v]; }
        float get_rating(literal l) const { return get_rating(l.var()); }

        bool_var select(unsigned level) {
            init_pre_selection(level);
            unsigned max_num_cand = level == 0 ? m_freevars.size() : m_config.m_level_cand / level;
            max_num_cand = std::max(m_config.m_min_cutoff, max_num_cand);

            float sum = 0;
            for (bool newbies = false; ; newbies = true) {
                sum = init_candidates(level, newbies);
                if (!m_candidates.empty()) break;
                if (is_sat()) {
                    return null_bool_var;
                }            
            }
            SASSERT(!m_candidates.empty());
            // cut number of candidates down to max_num_cand.
            // step 1. cut it to at most 2*max_num_cand.
            // step 2. use a heap to sift through the rest.
            bool progress = true;
            while (progress && m_candidates.size() >= max_num_cand * 2) {
                progress = false;
                float mean = sum / (float)(m_candidates.size() + 0.0001);
                sum = 0;
                for (unsigned i = 0; i < m_candidates.size(); ++i) {
                    if (m_candidates[i].m_rating >= mean) {
                        sum += m_candidates[i].m_rating;
                    }
                    else {
                        m_candidates[i] = m_candidates.back();
                        m_candidates.pop_back();
                        --i;
                        progress = true;
                    }
                }                
            }
            SASSERT(!m_candidates.empty());
            if (m_candidates.size() > max_num_cand) {
                unsigned j = m_candidates.size()/2;
                while (j > 0) {
                    --j;
                    sift_up(j);
                }
                while (true) {
                    m_candidates[0] = m_candidates.back();
                    m_candidates.pop_back();
                    if (m_candidates.size() == max_num_cand) break;
                    sift_up(0);
                }
            }
            SASSERT(!m_candidates.empty() && m_candidates.size() <= max_num_cand);
        }

        void sift_up(unsigned j) {
            unsigned i = j;
            candidate c = m_candidates[j];
            for (unsigned k = 2*j + 1; k < m_candidates.size(); i = k, k = 2*k + 1) {
                // pick largest parent
                if (k + 1 < m_candidates.size() && m_candidates[k].m_rating < m_candidates[k+1].m_rating) {
                    ++k;
                }
                if (c.m_rating <= m_candidates[k].m_rating) break;
                m_candidates[i] = m_candidates[k];
            }
            if (i > j) m_candidates[i] = c;
        }

        float init_candidates(unsigned level, bool newbies) {
            m_candidates.reset();
            float sum = 0;
            for (bool_var const* it = m_freevars.begin(), * end = m_freevars.end(); it != end; ++it) {
                bool_var x = *it;
                if (!newbies) {
                    // TBD filter out candidates based on prefix strings or similar method
                }
                m_candidates.push_back(candidate(x, m_rating[x]));
                sum += m_rating[x];
            }            
            return sum;
        }

        bool is_sat() const {
            for (bool_var const* it = m_freevars.begin(), * end = m_freevars.end(); it != end; ++it) {
                literal l(*it, false);
                literal_vector const& lits1 = m_binary[l.index()];
                for (unsigned i = 0; i < lits1.size(); ++i) {
                    if (!is_true(lits1[i])) return false;
                }
                literal_vector const& lits2 = m_binary[(~l).index()];
                for (unsigned i = 0; i < lits2.size(); ++i) {
                    if (!is_true(lits2[i])) return false;
                }
            }
            for (unsigned i = 0; i < m_clauses.size(); ++i) {
                clause& c = *m_clauses[i];
                if (!is_true(c[0]) && !is_true(c[1])) return false;
            }
            return true;
        }

        void init_pre_selection(unsigned level) {
            unsigned max_level = m_config.m_max_hlevel;
            if (level <= 1) {
                ensure_H(2);
                h_scores(m_H[0], m_H[1]);
                for (unsigned j = 0; j < 2; ++j) {
                    for (unsigned i = 0; i < 2; ++i) {
                        h_scores(m_H[i + 1], m_H[(i + 2) % 3]);
                    }
                }
                // heur = m_H[1];
            }
            else if (level < max_level) {
                ensure_H(level);
                h_scores(m_H[level-1], m_H[level]);
                // heur = m_H[level];
            }
            else {
                ensure_H(max_level);
                h_scores(m_H[max_level-1], m_H[max_level]);
                // heur = m_H[max_level];
            }
        }

        void ensure_H(unsigned level) {
            while (m_H.size() <= level) {
                m_H.push_back(svector<float>());
                m_H.back().resize(s.num_vars() * 2, 0);
            }
        }

        void h_scores(svector<float>& h, svector<float>& hp) {
            float sum = 0;
            for (bool_var const* it = m_freevars.begin(), * end = m_freevars.end(); it != end; ++it) {
                literal l(*it, false);
                sum += h[l.index()] + h[(~l).index()];
            }
            float factor = 2 * m_freevars.size() / sum;
            float sqfactor = factor * factor;
            float afactor = factor * m_config.m_alpha;
            for (bool_var const* it = m_freevars.begin(), * end = m_freevars.end(); it != end; ++it) {
                literal l(*it, false);
                float pos = l_score(l,  h, factor, sqfactor, afactor);
                float neg = l_score(~l, h, factor, sqfactor, afactor);
                hp[l.index()] = pos;
                hp[(~l).index()] = neg;
                m_rating[l.var()] = pos * neg;
            }
        }

        float l_score(literal l, svector<float> const& h, float factor, float sqfactor, float afactor) {
            float sum = 0, tsum = 0;
            literal_vector::iterator it = m_binary[l.index()].begin(), end = m_binary[l.index()].end();
            for (; it != end; ++it) {
                if (is_free(*it)) sum += h[it->index()]; 
            }
            // TBD walk ternary clauses.
            sum = (float)(0.1 + afactor*sum + sqfactor*tsum);
            return std::min(m_config.m_max_score, sum);
        }

        bool is_free(literal l) const {
            return !is_unit(l);
        }
        bool is_unit(literal l) const {
            return false; // TBD track variables that are units
        }

        // ------------------------------------       
        // Implication graph
        // Compute implication ordering and strongly connected components.
        // sat11.w 103 - 114.
        
        struct arcs : public literal_vector {}; 
        // Knuth uses a shared pool of fixed size for arcs.
        // Should it be useful we could use this approach tooo 
        // by changing the arcs abstraction and associated functions.

        struct dfs_info {
            unsigned m_rank;
            unsigned m_height;
            literal  m_parent;
            arcs     m_next;
            unsigned m_nextp;
            literal  m_link;
            literal  m_min;
            literal  m_vcomp;
            dfs_info() { reset(); }
            void reset() {
                m_rank = 0;
                m_height = 0;
                m_parent = null_literal;
                m_next.reset();
                m_link = null_literal;
                m_min = null_literal;
                m_vcomp = null_literal;
                m_nextp = 0;
            }
        };
        
        literal           m_active; 
        unsigned          m_rank; 
        literal           m_settled;
        vector<dfs_info>  m_dfs;
        
        void get_scc() {
            init_scc();
            for (unsigned i = 0; i < m_candidates.size(); ++i) {
                literal lit(m_candidates[i].m_var, false);
                if (get_rank(lit) == 0) get_scc(lit);
                if (get_rank(~lit) == 0) get_scc(~lit);
            }
        }
        void init_scc() {
            inc_bstamp();
            for (unsigned i = 0; i < m_candidates.size(); ++i) {
                literal lit(m_candidates[i].m_var, false);
                init_dfs_info(lit);
                init_dfs_info(~lit);
            }
            for (unsigned i = 0; i < m_candidates.size(); ++i) {
                literal lit(m_candidates[i].m_var, false);
                init_arcs(lit);
                init_arcs(~lit);
            }
            // set nextp = 0?
            m_rank = 0; 
            m_active = null_literal;
        }
        void init_dfs_info(literal l) {
            unsigned idx = l.index();
            m_dfs[idx].reset();
            set_bstamp(l);
        }
        // arcs are added in the opposite direction of implications.
        // So for implications l => u we add arcs u -> l
        void init_arcs(literal l) {
            literal_vector const& succ = m_binary[l.index()];
            for (unsigned i = 0; i < succ.size(); ++i) {
                literal u = succ[i];
                SASSERT(u != l);
                if (u.index() > l.index() && is_stamped(u)) {
                    add_arc(~l, ~u);
                    add_arc( u,  l);
                }
            }
        }
        void add_arc(literal u, literal v) { m_dfs[u.index()].m_next.push_back(v); }
        bool has_arc(literal v) const { 
            return m_dfs[v.index()].m_next.size() > m_dfs[v.index()].m_nextp; 
        } 
        literal pop_arc(literal u) { return m_dfs[u.index()].m_next[m_dfs[u.index()].m_nextp++]; }
        unsigned num_next(literal u) const { return m_dfs[u.index()].m_next.size(); }
        literal get_next(literal u, unsigned i) const { return m_dfs[u.index()].m_next[i]; }
        literal get_min(literal v) const { return m_dfs[v.index()].m_min; }
        unsigned get_rank(literal v) const { return m_dfs[v.index()].m_rank; }
        unsigned get_height(literal v) const { return m_dfs[v.index()].m_height; }
        literal get_parent(literal u) const { return m_dfs[u.index()].m_parent; }
        literal get_link(literal u) const { return m_dfs[u.index()].m_link; }
        literal get_vcomp(literal u) const { return m_dfs[u.index()].m_vcomp; }
        void set_link(literal v, literal u) { m_dfs[v.index()].m_link = u; }
        void set_min(literal v, literal u) { m_dfs[v.index()].m_min = u; }
        void set_rank(literal v, unsigned r) { m_dfs[v.index()].m_rank = r; }
        void set_height(literal v, unsigned h) { m_dfs[v.index()].m_height = h; }
        void set_parent(literal v, literal p) { m_dfs[v.index()].m_parent = p; }
        void set_vcomp(literal v, literal u) { m_dfs[v.index()].m_vcomp = u; }
        void get_scc(literal v) {
            set_parent(v, null_literal);
            activate_scc(v);
            literal u;
            do {
                literal ll = get_min(v);
                if (!has_arc(v)) {
                    u = get_parent(v);
                    if (v == ll) {
                        found_scc(v);
                    }
                    else if (get_rank(ll) < get_rank(get_min(u))) {
                        set_min(u, ll);
                    }
                    v = u;
                }
                else {
                    literal u = pop_arc(v);
                    unsigned r = get_rank(u);
                    if (r > 0) {
                        if (r < get_rank(ll)) set_min(v, u);
                    }
                    else {
                        set_parent(u, v);
                        v = u;
                        activate_scc(v);
                    }
                }                
            }
            while (v != null_literal);
        }
        void activate_scc(literal l) {
            SASSERT(get_rank(l) == 0);
            set_rank(l, ++m_rank);
            set_link(l, m_active);
            set_min(l, l);
            m_active = l;
        }
        // make v root of the scc equivalence class
        // set vcomp to be the highest rated literal
        void found_scc(literal v) {
            literal t = m_active; 
            m_active = get_link(v);
            literal best = v;
            float best_rating = get_rating(v);
            set_rank(v, UINT_MAX);
            while (t != v) {
                SASSERT(t != ~v);
                set_rank(t, UINT_MAX);
                set_parent(t, v);
                float t_rating = get_rating(t);
                if (t_rating > best_rating) {
                    best = t;
                    best_rating = t_rating;
                }
                t = get_link(t);
            }
            set_parent(v, v);
            set_vcomp(v, best);
            if (get_rank(~v) == UINT_MAX) {
                set_vcomp(v, ~get_vcomp(get_parent(~v))); // TBD check semantics
            }
        } 

        // ------------------------------------
        // lookahead forest
        // sat11.w 115-121

        literal m_root_child;

        literal get_child(literal u) const { 
            if (u == null_literal) return m_root_child; 
            return m_dfs[u.index()].m_min; 
        }
        void set_child(literal v, literal u) { 
            if (v == null_literal) m_root_child = u; 
            else m_dfs[v.index()].m_min = u; 
        }

        void construct_forest() {
            find_heights();
            construct_lookahead_table();
        }
        void find_heights() {
            literal pp = null_literal;
            set_child(pp, null_literal);
            unsigned h = 0;
            literal w;
            for (literal u = m_settled; u != null_literal; u = get_link(u)) {
                literal p = get_parent(u);
                if (p != pp) { 
                    h = 0; 
                    w = null_literal; 
                    pp = p; 
                }
                for (unsigned j = 0; j < num_next(~u); ++j) {
                    literal v = ~get_next(~u, j);
                    literal pv = get_parent(v);
                    if (pv == p) continue;
                    unsigned hh = get_height(pv);
                    if (hh >= h) {
                        h = hh + 1; 
                        w = pv;
                    }
                }
                if (p == u) { // u is an equivalence class representative
                    literal v = get_child(w);
                    set_height(u, h);
                    set_child(u, null_literal);
                    set_link(u, v);
                    set_child(w, u);
                }
            }
        }
        struct literal_offset {
            literal  m_lit;
            unsigned m_offset;
            literal_offset(literal l): m_lit(l), m_offset(0) {}
        };
        svector<literal_offset> m_lookahead;
        void set_offset(unsigned idx, unsigned offset) {
            m_lookahead[idx].m_offset = offset;
        }
        void set_lookahead(literal l) {
            m_lookahead.push_back(literal_offset(l));
        }
        void construct_lookahead_table() {
            literal u = get_child(null_literal), v = null_literal;
            unsigned offset = 0;
            m_lookahead.reset();
            while (u != null_literal) {                
                set_rank(u, m_lookahead.size());
                set_lookahead(get_vcomp(u));
                if (null_literal != get_child(u)) {
                    set_parent(u, v);
                    v = u;
                    u = get_child(u);
                }
                else {
                    while (true) {
                        set_offset(get_rank(u), offset);
                        offset += 2;
                        set_parent(u, v == null_literal ? v : get_vcomp(v));
                        u = get_link(u);
                        if (u == null_literal && v != null_literal) {
                            u = v;
                            v = get_parent(u);
                        }
                        else {
                            break;
                        }
                    }
                }
            }
            SASSERT(2*m_lookahead.size() == offset);
            TRACE("sat", for (unsigned i = 0; i < m_lookahead.size(); ++i) 
                    tout << m_lookahead[i].m_lit << " : " << m_lookahead[i].m_offset << "\n";);
        }
        

        // ------------------------------------
        // initialization
        
        void init_var(bool_var v) {
            m_assignment.push_back(l_undef);
            m_assignment.push_back(l_undef);
            m_binary.push_back(literal_vector());
            m_binary.push_back(literal_vector());
            m_watches.push_back(watch_list());
            m_watches.push_back(watch_list());
            m_bstamp.push_back(0);
            m_bstamp.push_back(0);
            m_rating.push_back(0);
            m_dfs.push_back(dfs_info());
            m_dfs.push_back(dfs_info());
        }

        void init() {
            m_delta_trigger = s.num_vars()/10;
            m_config.m_dl_success = 0.8;
            m_inconsistent = false;
            m_qhead = 0;
            m_bstamp_id = 0;

            for (unsigned i = 0; i < s.num_vars(); ++i) {
                init_var(i);
            }

            // copy binary clauses
            unsigned sz = s.m_watches.size();
            for (unsigned l_idx = 0; l_idx < sz; ++l_idx) {
                literal l = ~to_literal(l_idx);
                watch_list const & wlist = s.m_watches[l_idx];
                watch_list::const_iterator it  = wlist.begin();
                watch_list::const_iterator end = wlist.end();
                for (; it != end; ++it) {
                    if (!it->is_binary_non_learned_clause())
                        continue;
                    literal l2 = it->get_literal();
                    if (l.index() < l2.index()) 
                        add_binary(l, l2);
                }
            }

            // copy clauses
            clause_vector::const_iterator it =  s.m_clauses.begin();
            clause_vector::const_iterator end = s.m_clauses.end();
            for (; it != end; ++it) {
                clause& c = *(*it);
                m_clauses.push_back(m_cls_allocator.mk_clause(c.size(), c.begin(), false));
                // TBD: add watch
            }

            // copy units
            unsigned trail_sz = s.init_trail_size();
            for (unsigned i = 0; i < trail_sz; ++i) {
                literal l = s.m_trail[i];
                m_units.push_back(l);
                assign(l);
            }
        }
        
        void push(literal lit) { 
            m_binary_trail_lim.push_back(m_binary_trail.size());
            m_units_lim.push_back(m_units.size());
            m_trail_lim.push_back(m_trail.size());
            m_qhead_lim.push_back(m_qhead);
            m_trail.push_back(lit);
            assign(lit);
            propagate();
        }

        void pop() { 
            // remove local binary clauses
            unsigned old_sz = m_binary_trail_lim.back();
            m_binary_trail_lim.pop_back();
            for (unsigned i = old_sz; i < m_binary_trail.size(); ++i) {
                del_binary(m_binary_trail[i]);
            }

            // add implied binary clauses
            unsigned new_unit_sz = m_units_lim.back();
            for (unsigned i = new_unit_sz; i < m_units.size(); ++i) {
                add_binary(~m_trail.back(), m_units[i]);
            }
            m_units.shrink(new_unit_sz);
            m_units_lim.pop_back();
            m_trail.shrink(m_trail_lim.size()); // reset assignment.
            m_trail_lim.pop_back();
            m_qhead_lim.pop_back();
            m_qhead = m_qhead_lim.back();

            m_inconsistent = false;
        }
        
        unsigned diff() const { return m_units.size() - m_units_lim.back(); }

        unsigned mix_diff(unsigned l, unsigned r) const { return l + r + (1 << 10) * l * r; }

        clause const& get_clause(watch_list::iterator it) const {
            clause_offset cls_off = it->get_clause_offset();
            return *(s.m_cls_allocator.get_clause(cls_off));
        }

        bool is_nary_propagation(clause const& c, literal l) const {
            bool r = c.size() > 2 && ((c[0] == l && value(c[1]) == l_false) || (c[1] == l && value(c[0]) == l_false));
            DEBUG_CODE(if (r) for (unsigned j = 2; j < c.size(); ++j) SASSERT(value(c[j]) == l_false););
            return r;
        }

        void propagate_clauses(literal l) {
            SASSERT(value(l) == l_true);
            SASSERT(value(~l) == l_false);
            if (inconsistent()) return;
            watch_list& wlist = m_watches[l.index()];
            watch_list::iterator it = wlist.begin(), it2 = it, end = wlist.end();
            for (; it != end && !inconsistent(); ++it) {
                switch (it->get_kind()) {
                case watched::BINARY:
                    UNREACHABLE();
                    break;
                case watched::TERNARY: {
                    literal l1 = it->get_literal1();
                    literal l2 = it->get_literal2();
                    lbool val1 = value(l1);
                    lbool val2 = value(l2);
                    if (val1 == l_false && val2 == l_undef) {
                        m_stats.m_propagations++;
                        assign(l2);
                    }
                    else if (val1 == l_undef && val2 == l_false) {
                        m_stats.m_propagations++;
                        assign(l1);
                    }
                    else if (val1 == l_false && val2 == l_false) {
                        set_conflict();
                    }
                    else if (val1 == l_undef && val2 == l_undef) {
                        // TBD: the clause has become binary.
                    }
                    *it2 = *it;
                    it2++;
                    break;
                }
                case watched::CLAUSE: {
                    clause_offset cls_off = it->get_clause_offset();
                    clause & c = *(s.m_cls_allocator.get_clause(cls_off));
                    TRACE("propagate_clause_bug", tout << "processing... " << c << "\nwas_removed: " << c.was_removed() << "\n";);
                    if (c[0] == ~l)
                        std::swap(c[0], c[1]);
                    if (value(c[0]) == l_true) {
                        it2->set_clause(c[0], cls_off);
                        it2++;
                        break;
                    }
                    literal * l_it  = c.begin() + 2;
                    literal * l_end = c.end();
                    unsigned found = 0;
                    for (; l_it != l_end && found < 2; ++l_it) {
                        if (value(*l_it) != l_false) {
                            ++found;
                            if (found == 2) {
                                break;
                            }
                            else {
                                c[1]  = *l_it;
                                *l_it = ~l;
                                m_watches[(~c[1]).index()].push_back(watched(c[0], cls_off));
                            }
                        }
                    }
                    if (found == 1) {
                        // TBD: clause has become binary
                        break; 
                    }
                    if (found > 1) {
                        // not a binary clause
                        break;
                    }
                    else if (value(c[0]) == l_false) {
                        set_conflict();
                    }
                    else {
                        SASSERT(value(c[0]) == l_undef);
                        *it2 = *it;
                        it2++;
                        m_stats.m_propagations++;
                        assign(c[0]);
                    }
                    break;
                }
                case watched::EXT_CONSTRAINT:
                    UNREACHABLE();
                    break;
                default:
                    UNREACHABLE();
                    break;
                }
            }
            for (; it != end; ++it, ++it2) {
                *it2 = *it;                         
            }
            wlist.set_end(it2);
        
            
            //
            // TBD: count binary clauses created by propagation.
            // They used to be in the watch list of l.index(), 
            // both new literals in watch list should be unassigned.
            //
        }

        void propagate_binary(literal l) {
            literal_vector const& lits = m_binary[l.index()];
            unsigned sz = lits.size();
            for (unsigned i = 0; !inconsistent() && i < sz; ++i) {
                assign(lits[i]);
            }
        }

        void propagate() {
            for (; m_qhead < m_trail.size(); ++m_qhead) {
                if (inconsistent()) break;
                literal l = m_trail[m_qhead];
                propagate_binary(l);
                propagate_clauses(l);
            }
            TRACE("sat", s.display(tout << scope_lvl() << " " << (inconsistent()?"unsat":"sat") << "\n"););
        }

        literal choose() {
            literal l;
            while (!choose1(l)) {};
            return l;
        }

        bool choose1(literal& l) {
            literal_vector P;
            pre_select(P);
            l = null_literal;
            if (P.empty()) {
                return true;
            }
            unsigned h = 0, count = 1;
            for (unsigned i = 0; i < P.size(); ++i) {
                literal lit = P[i];

                push(lit);
                if (do_double()) double_look(P);
                if (inconsistent()) {
                    pop();
                    assign(~lit);
                    if (do_double()) double_look(P);
                    if (inconsistent()) return true;
                    continue;
                }
                unsigned diff1 = diff();
                pop();
                
                push(~lit);
                if (do_double()) double_look(P);
                bool unsat2 = inconsistent();
                unsigned diff2 = diff();
                pop();

                if (unsat2) {
                    assign(lit);
                    continue;
                }

                unsigned mixd = mix_diff(diff1, diff2);


                if (mixd > h || (mixd == h && s.m_rand(count) == 0)) {
                    CTRACE("sat", l != null_literal, tout << lit << " diff1: " << diff1 << " diff2: " << diff2 << "\n";);
                    if (mixd > h) count = 1; else ++count;
                    h = mixd;
                    l = diff1 < diff2 ? lit : ~lit;
                }
            }
            return l != null_literal;
        }

        void double_look(literal_vector const& P) {
            bool unsat;
            for (unsigned i = 0; !inconsistent() && i < P.size(); ++i) {
                literal lit = P[i];
                if (value(lit) != l_undef) continue;

                push(lit);
                unsat = inconsistent();
                pop();
                if (unsat) {
                    TRACE("sat", tout << "unit: " << ~lit << "\n";);
                    assign(~lit);
                    continue;
                }

                push(~lit);
                unsat = inconsistent();
                pop();
                if (unsat) {
                    TRACE("sat", tout << "unit: " << lit << "\n";);
                    assign(lit);
                }
            }
            update_delta_trigger();
        }

        bool is_fixed(literal l) const { return value(l) != l_undef; }
        bool is_contrary(literal l) const { return value(l) == l_false; }
        bool is_true(literal l) const { return value(l) == l_true; }
        void set_conflict() { m_inconsistent = true; }
        lbool value(literal l) const { return static_cast<lbool>(m_assignment[l.index()]); }
        unsigned scope_lvl() const { return m_trail_lim.size(); }

        void assign(literal l) {
            switch (value(l)) {
            case l_true: 
                break;
            case l_false: 
                set_conflict(); 
                break;
            default:
                m_assignment[l.index()] = l.sign() ? l_false : l_true;
                m_assignment[(~l).index()] = l.sign() ? l_false : l_true;
                m_trail.push_back(l);
                break;
            }
        }

        void set_inconsistent() { m_inconsistent = true; }
        bool inconsistent() { return m_inconsistent; }

        void pre_select(literal_vector& P) {
            select_variables(P);
            order_by_implication_trees(P);            
        }

        void order_by_implication_trees(literal_vector& P) {
            literal_set roots;
            literal_vector nodes, parent;

            //
            // Extract binary clauses in watch list. 
            // Produce implication graph between literals in P.
            //
            
            for (unsigned i = 0; i < P.size(); ++i) {
                literal lit1 = P[i], lit2;

                //
                // lit2 => lit1, where lit2 is a root.
                // make lit1 a root instead of lit2
                //

                literal_vector const& lits1 = m_binary[(~lit1).index()];
                unsigned sz = lits1.size();
                for (unsigned i = 0; i < sz; ++i) {
                    literal lit2 = lits1[i];
                    if (roots.contains(~lit2)) {
                        // ~lit2 => lit1
                        // if lit2 is a root, put it under lit2
                        parent.setx((~lit2).index(), lit1, null_literal);
                        roots.remove(~lit2);
                        roots.insert(lit1);
                        goto found;
                    }
                }

                //
                // lit1 => lit2.n
                // if lit2 is a node, put lit1 above lit2
                //

                literal_vector const& lits2 = m_binary[(~lit2).index()];
                sz = lits2.size();
                for (unsigned i = 0; i < sz; ++i) {
                    literal lit2 = lits2[i];
                    if (nodes.contains(lit2)) {
                        // lit1 => lit2
                        parent.setx(lit1.index(), lit2, null_literal);
                        nodes.insert(lit1);
                        goto found;
                    }
                }
                nodes.push_back(lit1);
                roots.insert(lit1);                
            found:
                ;
            }    
            TRACE("sat", 
                  tout << "implication trees\n";
                  for (unsigned i = 0; i < parent.size(); ++i) {
                      literal p = parent[i];
                      if (p != null_literal) {
                          tout << to_literal(i) << " |-> " << p << "\n";
                      }
                  });

            // TBD: extract ordering.
            
        }

        void select_variables(literal_vector& P) {
            for (unsigned i = 0; i < s.num_vars(); ++i) {
                if (value(literal(i,false)) == l_undef) {
                    P.push_back(literal(i, false));
                }
            }
        }

        bool do_double() {
            return !inconsistent() && diff() > m_delta_trigger;
        }

        void update_delta_trigger() {
            if (inconsistent()) {
                m_delta_trigger -= (1 - m_config.m_dl_success) / m_config.m_dl_success; 
            }
            else {
                m_delta_trigger += 1;
            }
            if (m_delta_trigger >= s.num_vars()) {
                // reset it.
            }
        }

        bool backtrack(literal_vector& trail) {
            if (trail.empty()) return false;      
            pop();                                  
            assign(~trail.back());                  
            trail.pop_back();               
            return true;
        }

        lbool search() {
            literal_vector trail;

            while (true) {
                s.checkpoint();
                literal l = choose();
                if (inconsistent()) {
                    if (!backtrack(trail)) return l_false;
                    continue;
                }
                if (l == null_literal) {
                    return l_true;
                }
                TRACE("sat", tout << "choose: " << l << " " << trail << "\n";);
                push(l);
                trail.push_back(l);
            }
        }

    public:
        lookahead(solver& s) : s(s) {
            init();
        }
        
        lbool check() {
            return search();
        }
              
    };
}

#endif
