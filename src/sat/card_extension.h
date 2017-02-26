/*++
Copyright (c) 2017 Microsoft Corporation

Module Name:

    card_extension.h

Abstract:

    Cardinality extensions.

Author:

    Nikolaj Bjorner (nbjorner) 2017-01-30

Revision History:

--*/
#ifndef CARD_EXTENSION_H_
#define CARD_EXTENSION_H_

#include"sat_extension.h"
#include"sat_solver.h"
#include"scoped_ptr_vector.h"

namespace sat {
    
    class card_extension : public extension {

        friend class local_search;

        struct stats {
            unsigned m_num_propagations;
            unsigned m_num_conflicts;
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
        };
        
        class card {
            unsigned       m_index;
            literal        m_lit;
            unsigned       m_k;
            unsigned       m_size;
            literal        m_lits[0];

        public:
            static size_t get_obj_size(unsigned num_lits) { return sizeof(card) + num_lits * sizeof(literal); }
            card(unsigned index, literal lit, literal_vector const& lits, unsigned k);
            unsigned index() const { return m_index; }
            literal lit() const { return m_lit; }
            literal operator[](unsigned i) const { return m_lits[i]; }
            unsigned k() const { return m_k; }
            unsigned size() const { return m_size; }
            void swap(unsigned i, unsigned j) { std::swap(m_lits[i], m_lits[j]); }
            void negate();                       
        };

        class xor {
            unsigned       m_index;
            literal        m_lit;
            unsigned       m_size;
            literal        m_lits[0];
        public:
            static size_t get_obj_size(unsigned num_lits) { return sizeof(xor) + num_lits * sizeof(literal); }
            xor(unsigned index, literal lit, literal_vector const& lits);
            unsigned index() const { return m_index; }
            literal lit() const { return m_lit; }
            literal operator[](unsigned i) const { return m_lits[i]; }
            unsigned size() const { return m_size; }
            void swap(unsigned i, unsigned j) { std::swap(m_lits[i], m_lits[j]); }
            void negate() { m_lits[0].neg(); }
        };

        struct ineq {
            literal_vector  m_lits;
            unsigned_vector m_coeffs;
            unsigned        m_k;
            void reset(unsigned k) { m_lits.reset(); m_coeffs.reset(); m_k = k; }
            void push(literal l, unsigned c) { m_lits.push_back(l); m_coeffs.push_back(c); }
        };

        typedef ptr_vector<card> card_watch;
        typedef ptr_vector<xor> xor_watch;
        struct var_info {
            card_watch* m_card_watch[2];
            xor_watch*  m_xor_watch;
            card*       m_card;
            xor*        m_xor;
            var_info(): m_xor_watch(0), m_card(0), m_xor(0) {
                m_card_watch[0] = 0;
                m_card_watch[1] = 0;
            }
            void reset() {
                dealloc(m_card);
                dealloc(m_xor);
                dealloc(card_extension::set_tag_non_empty(m_card_watch[0]));
                dealloc(card_extension::set_tag_non_empty(m_card_watch[1]));
                dealloc(card_extension::set_tag_non_empty(m_xor_watch));
            }
        };
        
        template<typename T>
        static ptr_vector<T>* set_tag_empty(ptr_vector<T>* c) {
            return TAG(ptr_vector<T>*, c, 1);
        }

        template<typename T>
        static bool is_tag_empty(ptr_vector<T> const* c) {
            return !c || GET_TAG(c) == 1;
        }

        template<typename T>
        static ptr_vector<T>* set_tag_non_empty(ptr_vector<T>* c) {
            return UNTAG(ptr_vector<T>*, c);
        }



        solver*             m_solver;
        stats               m_stats;        

        ptr_vector<card>    m_cards;
        ptr_vector<xor>     m_xors;

        scoped_ptr_vector<card> m_card_axioms;

        // watch literals
        svector<var_info>   m_var_infos;
        unsigned_vector     m_var_trail;
        unsigned_vector     m_var_lim;       

        // conflict resolution
        unsigned          m_num_marks;
        unsigned          m_conflict_lvl;
        svector<int>      m_coeffs;
        svector<bool_var> m_active_vars;
        int               m_bound;
        tracked_uint_set  m_active_var_set;
        literal_vector    m_lemma;
        unsigned          m_num_propagations_since_pop;
        bool              m_has_xor;
        unsigned_vector   m_parity_marks;
        literal_vector    m_parity_trail;
        void     ensure_parity_size(bool_var v);
        unsigned get_parity(bool_var v);
        void     inc_parity(bool_var v);
        void     reset_parity(bool_var v);

        solver& s() const { return *m_solver; }
        void init_watch(card& c, bool is_true);
        void init_watch(bool_var v);
        void assign(card& c, literal lit);
        lbool add_assign(card& c, literal lit);
        void watch_literal(card& c, literal lit);
        void set_conflict(card& c, literal lit);
        void clear_watch(card& c);
        void reset_coeffs();
        void reset_marked_literals();
        void unwatch_literal(literal w, card* c);

        // xor specific functionality
        void clear_watch(xor& x);
        void watch_literal(xor& x, literal lit);
        void unwatch_literal(literal w, xor* x);
        void init_watch(xor& x, bool is_true);
        void assign(xor& x, literal lit);
        void set_conflict(xor& x, literal lit);
        bool parity(xor const& x, unsigned offset) const;
        lbool add_assign(xor& x, literal alit);
        void asserted_xor(literal l, ptr_vector<xor>* xors, xor* x);

        bool is_card_index(unsigned idx) const { return 0 == (idx & 0x1); }
        card& index2card(unsigned idx) const { SASSERT(is_card_index(idx)); return *m_cards[idx >> 1]; }
        xor& index2xor(unsigned idx) const { SASSERT(!is_card_index(idx)); return *m_xors[idx >> 1]; }
        void get_xor_antecedents(unsigned index, literal_vector& r);


        template<typename T>
        bool remove(ptr_vector<T>& ts, T* t) {
            unsigned sz = ts.size();
            for (unsigned j = 0; j < sz; ++j) {
                if (ts[j] == t) {                        
                    std::swap(ts[j], ts[sz-1]);
                    ts.pop_back();
                    return sz == 1;
                }
            }
            return false;
        }
    


        inline lbool value(literal lit) const { return m_solver->value(lit); }
        inline unsigned lvl(literal lit) const { return m_solver->lvl(lit); }
        inline unsigned lvl(bool_var v) const { return m_solver->lvl(v); }


        void normalize_active_coeffs();
        void inc_coeff(literal l, int offset);
        int get_coeff(bool_var v) const;
        int get_abs_coeff(bool_var v) const;       

        literal get_asserting_literal(literal conseq);
        void process_antecedent(literal l, int offset);
        void process_card(card& c, int offset);
        void cut();

        // validation utilities
        bool validate_conflict(card& c);
        bool validate_conflict(xor& x);
        bool validate_assign(literal_vector const& lits, literal lit);
        bool validate_lemma();
        bool validate_unit_propagation(card const& c);
        bool validate_conflict(literal_vector const& lits, ineq& p);

        ineq m_A, m_B, m_C;
        void active2pb(ineq& p);
        void justification2pb(justification const& j, literal lit, unsigned offset, ineq& p);
        bool validate_resolvent();

        void display(std::ostream& out, ineq& p) const;
        void display(std::ostream& out, card& c, bool values) const;
        void display(std::ostream& out, xor& c, bool values) const;
        void display_watch(std::ostream& out, bool_var v) const;
        void display_watch(std::ostream& out, bool_var v, bool sign) const;

    public:
        card_extension();
        virtual ~card_extension();
        virtual void set_solver(solver* s) { m_solver = s; }
        void    add_at_least(bool_var v, literal_vector const& lits, unsigned k);
        void    add_xor(bool_var v, literal_vector const& lits);
        virtual void propagate(literal l, ext_constraint_idx idx, bool & keep);
        virtual bool resolve_conflict();
        virtual void get_antecedents(literal l, ext_justification_idx idx, literal_vector & r);
        virtual void asserted(literal l);
        virtual check_result check();
        virtual void push();
        virtual void pop(unsigned n);
        virtual void simplify();
        virtual void clauses_modifed();
        virtual lbool get_phase(bool_var v);
        virtual std::ostream& display(std::ostream& out) const;
        virtual std::ostream& display_justification(std::ostream& out, ext_justification_idx idx) const;
        virtual void collect_statistics(statistics& st) const;
        virtual extension* copy(solver* s);
        virtual void find_mutexes(literal_vector& lits, vector<literal_vector> & mutexes);
    };

};

#endif