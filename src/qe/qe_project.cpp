/*++
Copyright (c) 2010 Microsoft Corporation

Module Name:

    qe_arith.cpp

Abstract:

    Simple projection function for real arithmetic based on Loos-W.

Author:

    Nikolaj Bjorner (nbjorner) 2013-09-12

Revision History:

    Modified by Anvesh Komuravelli


--*/

#include "qe_project.h"
#include "qe_util.h"
#include "qe.h"
#include "arith_decl_plugin.h"
#include "ast_pp.h"
#include "th_rewriter.h"
#include "expr_functors.h"
#include "expr_substitution.h"
#include "expr_replacer.h"
#include "model_pp.h"
#include "qe_array.h"
#include "expr_safe_replace.h"
#include "model_evaluator.h"
#include "qe_lite.h"

namespace qe {

    class is_relevant_default : public i_expr_pred {
    public:
        bool operator()(expr* e) {
            return true;
        }
    };

    class mk_atom_default : public i_nnf_atom {
    public:
        virtual void operator()(expr* e, bool pol, expr_ref& result) {
            if (pol) result = e; 
            else result = result.get_manager().mk_not(e);
        }
    };

    class arith_project_util {
        ast_manager& m;
        arith_util   a;
        th_rewriter  m_rw;
        expr_ref_vector  m_lits;
        expr_ref_vector  m_terms;
        vector<rational> m_coeffs;
        vector<rational> m_divs;
        svector<bool>    m_strict;
        svector<bool>    m_eq;
        scoped_ptr<contains_app> m_var;

        struct cant_project {};

        void is_linear(rational const& mul, expr* t, rational& c, expr_ref_vector& ts) {
            expr* t1, *t2;
            rational mul1;
            if (t == m_var->x()) {
                c += mul;
            }
            else if (a.is_mul(t, t1, t2) && a.is_numeral(t1, mul1)) {
                is_linear(mul* mul1, t2, c, ts);
            }
            else if (a.is_mul(t, t1, t2) && a.is_numeral(t2, mul1)) {
                is_linear(mul* mul1, t1, c, ts);
            }
            else if (a.is_add(t)) {
                app* ap = to_app(t);
                for (unsigned i = 0; i < ap->get_num_args(); ++i) {
                    is_linear(mul, ap->get_arg(i), c, ts);
                }
            }
            else if (a.is_sub(t, t1, t2)) {
                is_linear(mul,  t1, c, ts);
                is_linear(-mul, t2, c, ts);
            }
            else if (a.is_uminus(t, t1)) {
                is_linear(-mul, t1, c, ts);
            }
            else if (a.is_numeral(t, mul1)) {
                ts.push_back(a.mk_numeral(mul*mul1, m.get_sort(t)));
            }
            else if ((*m_var)(t)) {
                IF_VERBOSE(1, verbose_stream() << "can't project:" << mk_pp(t, m) << "\n";);
                throw cant_project();
            }
            else if (mul.is_one()) {
                ts.push_back(t);
            }
            else {
                ts.push_back(a.mk_mul(a.mk_numeral(mul, m.get_sort(t)), t));
            }
        }

        // either an equality (cx + t = 0) or an inequality (cx + t <= 0) or a divisibility literal (d | cx + t)
        bool is_linear(expr* lit, rational& c, expr_ref& t, rational& d, bool& is_strict, bool& is_eq, bool& is_diseq) {
            if (!(*m_var)(lit)) {
                return false;
            }
            expr* e1, *e2;
            c.reset();
            sort* s;
            expr_ref_vector ts(m);            
            bool is_not = m.is_not(lit, lit);
            rational mul(1);
            if (is_not) {
                mul.neg();
            }
            SASSERT(!m.is_not(lit));
            if (a.is_le(lit, e1, e2) || a.is_ge(lit, e2, e1)) {
                is_linear( mul, e1, c, ts);
                is_linear(-mul, e2, c, ts);
                s = m.get_sort(e1);
                is_strict = is_not;
            }
            else if (a.is_lt(lit, e1, e2) || a.is_gt(lit, e2, e1)) {
                is_linear( mul, e1, c, ts);
                is_linear(-mul, e2, c, ts);
                s = m.get_sort(e1);
                is_strict = !is_not;
            }
            else if (m.is_eq(lit, e1, e2)) {
                expr *t, *num;
                rational num_val, d_val, z;
                bool is_int;
                if (a.is_mod (e1, t, num) && a.is_numeral (num, num_val, is_int) && is_int &&
                        a.is_numeral (e2, z) && z.is_zero ()) {
                    // divsibility constraint: t % num == 0 <=> num | t
                    if (num_val.is_zero ()) {
                        IF_VERBOSE(1, verbose_stream() << "div by zero" << mk_pp(lit, m) << "\n";);
                        throw cant_project();
                    }
                    d = num_val;
                    is_linear (mul, t, c, ts);
                } else if (a.is_mod (e2, t, num) && a.is_numeral (num, num_val, is_int) && is_int &&
                        a.is_numeral (e1, z) && z.is_zero ()) {
                    // divsibility constraint: 0 == t % num <=> num | t
                    if (num_val.is_zero ()) {
                        IF_VERBOSE(1, verbose_stream() << "div by zero" << mk_pp(lit, m) << "\n";);
                        throw cant_project();
                    }
                    d = num_val;
                    is_linear (mul, t, c, ts);
                } else {
                    // equality or disequality
                    is_linear( mul, e1, c, ts);
                    is_linear(-mul, e2, c, ts);
                    if (is_not) is_diseq = true;
                    else is_eq = true;
                }
                s = m.get_sort(e1);
            }
            else {
                IF_VERBOSE(1, verbose_stream() << "can't project:" << mk_pp(lit, m) << "\n";);
                throw cant_project();
            }

            if (ts.empty()) {
                t = a.mk_numeral(rational(0), s);
            }
            else {
                t = a.mk_add(ts.size(), ts.c_ptr());
            }

            return true;
        }

        void project(model& mdl, expr_ref_vector& lits) {
            unsigned num_pos = 0;
            unsigned num_neg = 0;
            bool use_eq = false;
            expr_ref_vector new_lits(m);
            expr_ref eq_term (m);

            m_lits.reset ();
            m_terms.reset();
            m_coeffs.reset();
            m_strict.reset();
            m_eq.reset ();

            for (unsigned i = 0; i < lits.size(); ++i) {
                rational c(0), d(0);
                expr_ref t(m);
                bool is_strict = false;
                bool is_eq = false;
                bool is_diseq = false;
                if (is_linear(lits.get (i), c, t, d, is_strict, is_eq, is_diseq)) {
                    if (c.is_zero()) {
                        m_rw(lits.get (i), t);
                        new_lits.push_back(t);
                    } else if (is_eq) {
                        if (!use_eq) {
                            // c*x + t = 0  <=>  x = -t/c
                            eq_term = mk_mul (-(rational::one ()/c), t);
                            use_eq = true;
                        }
                        m_lits.push_back (lits.get (i));
                        m_coeffs.push_back(c);
                        m_terms.push_back(t);
                        m_strict.push_back(false);
                        m_eq.push_back (true);
                    } else {
                        if (is_diseq) {
                            // c*x + t != 0
                            // find out whether c*x + t < 0, or c*x + t > 0
                            expr_ref cx (m), cxt (m), val (m);
                            rational r;
                            cx = mk_mul (c, m_var->x());
                            cxt = mk_add (cx, t);
                            VERIFY(mdl.eval(cxt, val, true));
                            VERIFY(a.is_numeral(val, r));
                            SASSERT (r > rational::zero () || r < rational::zero ());
                            if (r > rational::zero ()) {
                                c = -c;
                                t = mk_mul (-(rational::one()), t);
                            }
                            is_strict = true;
                        }
                        m_lits.push_back (lits.get (i));
                        m_coeffs.push_back(c);
                        m_terms.push_back(t);
                        m_strict.push_back(is_strict);
                        m_eq.push_back (false);
                        if (c.is_pos()) {
                            ++num_pos;
                        }
                        else {
                            ++num_neg;
                        }                    
                    }
                }
                else {
                    new_lits.push_back(lits.get (i));
                }
            }
            if (use_eq) {
                TRACE ("qe",
                        tout << "Using equality term: " << mk_pp (eq_term, m) << "\n";
                      );
                // substitute eq_term for x everywhere
                for (unsigned i = 0; i < m_lits.size(); ++i) {
                    expr_ref cx (m), cxt (m), z (m), result (m);
                    cx = mk_mul (m_coeffs[i], eq_term);
                    cxt = mk_add (cx, m_terms.get(i));
                    z = a.mk_numeral(rational(0), m.get_sort(eq_term));
                    if (m_eq[i]) {
                        // c*x + t = 0
                        result = a.mk_eq (cxt, z);
                    } else if (m_strict[i]) {
                        // c*x + t < 0
                        result = a.mk_lt (cxt, z);
                    } else {
                        // c*x + t <= 0
                        result = a.mk_le (cxt, z);
                    }
                    m_rw (result);
                    new_lits.push_back (result);
                }
            }
            lits.reset();
            lits.append(new_lits);
            if (use_eq || num_pos == 0 || num_neg == 0) {
                return;
            }
            bool use_pos = num_pos < num_neg;
            unsigned max_t = find_max(mdl, use_pos);

            expr_ref new_lit (m);
            for (unsigned i = 0; i < m_lits.size(); ++i) {
                if (i != max_t) {
                    if (m_coeffs[i].is_pos() == use_pos) {
                        new_lit = mk_le(i, max_t);
                    }
                    else {
                        new_lit = mk_lt(i, max_t);
                    }
                    lits.push_back(new_lit);
                    TRACE ("qe",
                            tout << "Old literal: " << mk_pp (m_lits.get (i), m) << "\n";
                            tout << "New literal: " << mk_pp (new_lit, m) << "\n";
                          );
                }
            }
        }

        void project(model& mdl, app_ref_vector const& lits, expr_map& map, app_ref& div_lit) {
            unsigned num_pos = 0; // number of positive literals true in the model
            unsigned num_neg = 0; // number of negative literals true in the model

            m_lits.reset ();
            m_terms.reset();
            m_coeffs.reset();
            m_divs.reset ();
            m_strict.reset();
            m_eq.reset ();

            expr_ref var_val (m);
            VERIFY (mdl.eval (m_var->x(), var_val, true));

            unsigned eq_idx = lits.size ();
            for (unsigned i = 0; i < lits.size(); ++i) {
                rational c(0), d(0);
                expr_ref t(m);
                bool is_strict = false;
                bool is_eq = false;
                bool is_diseq = false;
                if (is_linear(lits.get (i), c, t, d, is_strict, is_eq, is_diseq)) {
                    TRACE ("qe",
                            tout << "Literal: " << mk_pp (lits.get (i), m) << "\n";
                          );

                    if (c.is_zero()) {
                        TRACE ("qe",
                                tout << "independent of variable\n";
                              );
                        continue;
                    }

                    // evaluate c*x + t in the model
                    expr_ref cx (m), cxt (m), val (m);
                    rational r;
                    cx = mk_mul (c, m_var->x());
                    cxt = mk_add (cx, t);
                    VERIFY(mdl.eval(cxt, val, true));
                    VERIFY(a.is_numeral(val, r));

                    if (is_eq) {
                        TRACE ("qe",
                                tout << "equality term\n";
                              );
                        // check if the equality is true in the mdl
                        if (eq_idx == lits.size () && r == rational::zero ()) {
                            eq_idx = m_lits.size ();
                        }
                        m_lits.push_back (lits.get (i));
                        m_coeffs.push_back(c);
                        m_terms.push_back(t);
                        m_strict.push_back(false);
                        m_eq.push_back (true);
                        m_divs.push_back (d);
                    } else {
                        TRACE ("qe",
                                tout << "not an equality term\n";
                              );
                        if (is_diseq) {
                            // c*x + t != 0
                            // find out whether c*x + t < 0, or c*x + t > 0
                            if (r > rational::zero ()) {
                                c = -c;
                                t = mk_mul (-(rational::one()), t);
                                r = -r;
                            }
                            // note: if the disequality is false in the model,
                            // r==0 and we end up choosing c*x + t < 0
                            is_strict = true;
                        }
                        m_lits.push_back (lits.get (i));
                        m_coeffs.push_back(c);
                        m_terms.push_back(t);
                        m_strict.push_back(is_strict);
                        m_eq.push_back (false);
                        m_divs.push_back (d);
                        if (d.is_zero ()) { // not a div term
                            if ((is_strict && r < rational::zero ()) ||
                                    (!is_strict && r <= rational::zero ())) { // literal true in the model
                                if (c.is_pos()) {
                                    ++num_pos;
                                }
                                else {
                                    ++num_neg;
                                }
                            }
                        }
                    }
                    TRACE ("qe",
                            tout << "c: " << c << "\n";
                            tout << "t: " << mk_pp (t, m) << "\n";
                            tout << "d: " << d << "\n";
                          );
                }
            }

            rational lcm_coeffs (1), lcm_divs (1);
            if (a.is_int (m_var->x())) {
                // lcm of (absolute values of) coeffs
                for (unsigned i = 0; i < m_lits.size (); i++) {
                    lcm_coeffs = lcm (lcm_coeffs, abs (m_coeffs[i]));
                }
                // normalize coeffs of x to +/-lcm_coeffs and scale terms and divs appropriately;
                // find lcm of scaled-up divs
                for (unsigned i = 0; i < m_lits.size (); i++) {
                    rational factor (lcm_coeffs / abs(m_coeffs[i]));
                    m_terms[i] = a.mk_mul (a.mk_numeral (factor, a.mk_int ()),
                                           m_terms.get (i));
                    m_coeffs[i] = (m_coeffs[i].is_pos () ? lcm_coeffs : -lcm_coeffs);
                    if (!m_divs[i].is_zero ()) {
                        m_divs[i] *= factor;
                        lcm_divs = lcm (lcm_divs, m_divs[i]);
                    }
                    TRACE ("qe",
                            tout << "normalized coeff: " << m_coeffs[i] << "\n";
                            tout << "normalized term: " << mk_pp (m_terms.get (i), m) << "\n";
                            tout << "normalized div: " << m_divs[i] << "\n";
                          );
                }

                // consider new divisibility literal (lcm_coeffs | (lcm_coeffs * x))
                lcm_divs = lcm (lcm_divs, lcm_coeffs);

                TRACE ("qe",
                        tout << "lcm of coeffs: " << lcm_coeffs << "\n";
                        tout << "lcm of divs: " << lcm_divs << "\n";
                      );
            }

            expr_ref z (a.mk_numeral (rational::zero (), a.mk_int ()), m);
            expr_ref x_term_val (m);

            // use equality term
            if (eq_idx < lits.size ()) {
                if (a.is_real (m_var->x ())) {
                    // c*x + t = 0  <=>  x = -t/c
                    expr_ref eq_term (mk_mul (-(rational::one ()/m_coeffs[eq_idx]), m_terms.get (eq_idx)), m);
                    m_rw (eq_term);
                    map.insert (m_var->x (), eq_term, 0);
                    TRACE ("qe",
                            tout << "Using equality term: " << mk_pp (eq_term, m) << "\n";
                          );
                }
                else {
                    // find substitution term for (lcm_coeffs * x)
                    if (m_coeffs[eq_idx].is_pos ()) {
                        x_term_val = a.mk_uminus (m_terms.get (eq_idx));
                    } else {
                        x_term_val = m_terms.get (eq_idx);
                    }
                    m_rw (x_term_val);
                    TRACE ("qe",
                            tout << "Using equality literal: " << mk_pp (m_lits.get (eq_idx), m) << "\n";
                            tout << "substitution for (lcm_coeffs * x): " << mk_pp (x_term_val, m) << "\n";
                          );
                    // can't simply substitute for x; need to explicitly substitute the lits
                    mk_lit_substitutes (x_term_val, map, eq_idx);

                    if (!lcm_coeffs.is_one ()) {
                        // new div constraint: lcm_coeffs | x_term_val
                        div_lit = m.mk_eq (a.mk_mod (x_term_val,
                                                     a.mk_numeral (lcm_coeffs, a.mk_int ())),
                                           z);
                    }
                }

                return;
            }

            expr_ref new_lit (m);

            if (num_pos == 0 || num_neg == 0) {
                TRACE ("qe",
                        if (num_pos == 0) {
                            tout << "virtual substitution with +infinity\n";
                        } else {
                            tout << "virtual substitution with -infinity\n";
                        }
                      );

                /**
                 * make all equalities false;
                 * if num_pos = 0 (num_neg = 0), make all positive (negative) inequalities false;
                 * make the rest inequalities true;
                 * substitute value of x under given model for the rest (div terms)
                 */

                if (a.is_int (m_var->x())) {
                    // to substitute for (lcm_coeffs * x), it suffices to pick
                    // some element in the congruence class of (lcm_coeffs * x) mod lcm_divs;
                    // simply substituting var_val for x in the literals does this job;
                    // but to keep constants small, we use (lcm_coeffs * var_val) % lcm_divs instead
                    rational var_val_num;
                    VERIFY (a.is_numeral (var_val, var_val_num));
                    x_term_val = a.mk_numeral (mod (lcm_coeffs * var_val_num, lcm_divs),
                                               a.mk_int ());
                    TRACE ("qe",
                            tout << "Substitution for (lcm_coeffs * x):" << "\n";
                            tout << mk_pp (x_term_val, m) << "\n";
                          );
                }
                for (unsigned i = 0; i < m_lits.size (); i++) {
                    if (!m_divs[i].is_zero ()) {
                        // m_divs[i] | (x_term_val + m_terms[i])
                        new_lit = m.mk_eq (a.mk_mod (a.mk_add (m_terms.get (i), x_term_val),
                                                     a.mk_numeral (m_divs[i], a.mk_int ())),
                                           z);
                        m_rw (new_lit);
                    } else if (m_eq[i] ||
                               (num_pos == 0 && m_coeffs[i].is_pos ()) ||
                               (num_neg == 0 && m_coeffs[i].is_neg ())) {
                        new_lit = m.mk_false ();
                    } else {
                        new_lit = m.mk_true ();
                    }
                    map.insert (m_lits.get (i), new_lit, 0);
                    TRACE ("qe",
                            tout << "Old literal: " << mk_pp (m_lits.get (i), m) << "\n";
                            tout << "New literal: " << mk_pp (new_lit, m) << "\n";
                          );
                }
                return;
            }

            bool use_pos = num_pos < num_neg; // pick a side; both are sound

            unsigned max_t = find_max(mdl, use_pos);

            TRACE ("qe",
                    if (use_pos) {
                        tout << "virtual substitution with upper bound:\n";
                    } else {
                        tout << "virtual substitution with lower bound:\n";
                    }
                    tout << "test point: " << mk_pp (m_lits.get (max_t), m) << "\n";
                    tout << "coeff: " << m_coeffs[max_t] << "\n";
                    tout << "term: " << mk_pp (m_terms.get (max_t), m) << "\n";
                    tout << "is_strict: " << m_strict[max_t] << "\n";
                  );

            if (a.is_real (m_var->x ())) {
                for (unsigned i = 0; i < m_lits.size(); ++i) {
                    if (i != max_t) {
                        if (m_eq[i]) {
                            if (!m_strict[max_t]) {
                                new_lit = mk_eq (i, max_t);
                            } else {
                                new_lit = m.mk_false ();
                            }
                        } else if (m_coeffs[i].is_pos() == use_pos) {
                            new_lit = mk_le (i, max_t);
                        } else {
                            new_lit = mk_lt (i, max_t);
                        }
                    } else {
                        new_lit = m.mk_true ();
                    }
                    map.insert (m_lits.get (i), new_lit, 0);
                    TRACE ("qe",
                            tout << "Old literal: " << mk_pp (m_lits.get (i), m) << "\n";
                            tout << "New literal: " << mk_pp (new_lit, m) << "\n";
                          );
                }
            } else {
                SASSERT (a.is_int (m_var->x ()));

                // mk substitution term for (lcm_coeffs * x)

                // evaluate c*x + t for the literal at max_t
                expr_ref cx (m), cxt (m), val (m);
                rational r;
                cx = mk_mul (m_coeffs[max_t], m_var->x());
                cxt = mk_add (cx, m_terms.get (max_t));
                VERIFY(mdl.eval(cxt, val, true));
                VERIFY(a.is_numeral(val, r));

                // get the offset from the smallest/largest possible value for x
                // literal      smallest/largest val of x
                // -------      --------------------------
                // l < x            l+1
                // l <= x            l
                // x < u            u-1
                // x <= u            u
                rational offset;
                if (m_strict[max_t]) {
                    offset = abs(r) - rational::one ();
                } else {
                    offset = abs(r);
                }
                // obtain the offset modulo lcm_divs
                offset %= lcm_divs;

                // for strict negative literal (i.e. strict lower bound),
                // substitution term is (t+1+offset); for non-strict, it's (t+offset)
                //
                // for positive term, subtract from 0
                x_term_val = mk_add (m_terms.get (max_t), a.mk_numeral (offset, a.mk_int ()));
                if (m_strict[max_t]) {
                    x_term_val = a.mk_add (x_term_val, a.mk_numeral (rational::one(), a.mk_int ()));
                }
                if (m_coeffs[max_t].is_pos ()) {
                    x_term_val = a.mk_uminus (x_term_val);
                }
                m_rw (x_term_val);

                TRACE ("qe",
                        tout << "substitution for (lcm_coeffs * x): " << mk_pp (x_term_val, m) << "\n";
                      );

                // obtain substitutions for all literals in map
                mk_lit_substitutes (x_term_val, map, max_t);

                if (!lcm_coeffs.is_one ()) {
                    // new div constraint: lcm_coeffs | x_term_val
                    div_lit = m.mk_eq (a.mk_mod (x_term_val,
                                                 a.mk_numeral (lcm_coeffs, a.mk_int ())),
                                       z);
                }
            }
        }

        unsigned find_max(model& mdl, bool do_pos) {
            unsigned result;
            bool found = false;
            bool found_strict = false;
            rational found_val (0), r, r_plus_x, found_c;
            expr_ref val(m);

            // evaluate x in mdl
            rational r_x;
            VERIFY(mdl.eval(m_var->x (), val, true));
            VERIFY(a.is_numeral (val, r_x));

            for (unsigned i = 0; i < m_terms.size(); ++i) {
                rational const& ac = m_coeffs[i];
                if (!m_eq[i] && ac.is_pos() == do_pos) {
                    VERIFY(mdl.eval(m_terms.get (i), val, true));
                    VERIFY(a.is_numeral(val, r));
                    r /= abs(ac);
                    // skip the literal if false in the model
                    if (do_pos) { r_plus_x = r + r_x; }
                    else { r_plus_x = r - r_x; }
                    if (!((m_strict[i] && r_plus_x < rational::zero ()) ||
                            (!m_strict[i] && r_plus_x <= rational::zero ()))) {
                        continue;
                    }
                    IF_VERBOSE(1, verbose_stream() << "max: " << mk_pp(m_terms.get (i), m) << " " << r << " " <<
                                (!found || r > found_val || (r == found_val && !found_strict && m_strict[i])) << "\n";);
                    if (!found || r > found_val || (r == found_val && !found_strict && m_strict[i])) {
                        result = i;
                        found_val = r;
                        found_c = ac;
                        found = true;
                        found_strict = m_strict[i];
                    }
                }
            }
            SASSERT(found);
            return result;
        }

        // ax + t <= 0
        // bx + s <= 0
        // a and b have different signs.
        // Infer: a|b|x + |b|t + |a|bx + |a|s <= 0
        // e.g.   |b|t + |a|s <= 0
        expr_ref mk_lt(unsigned i, unsigned j) {
            rational const& ac = m_coeffs[i];
            rational const& bc = m_coeffs[j];
            SASSERT(ac.is_pos() != bc.is_pos());
            SASSERT(ac.is_neg() != bc.is_neg());
            expr_ref bt (m), as (m), ts (m), z (m);
            expr* t = m_terms.get (i);
            expr* s = m_terms.get (j);
            bt = mk_mul(abs(bc), t);
            as = mk_mul(abs(ac), s);
            ts = mk_add(bt, as);
            z  = a.mk_numeral(rational(0), m.get_sort(t));
            expr_ref result1(m), result2(m);
            if (m_strict[i] || m_strict[j]) {
                result1 = a.mk_lt(ts, z);
            }
            else {
                result1 = a.mk_le(ts, z);
            }
            m_rw(result1, result2);
            return result2;
        }

        // ax + t <= 0
        // bx + s <= 0
        // a and b have same signs.
        // encode:// t/|a| <= s/|b|
        // e.g.   |b|t <= |a|s
        expr_ref mk_le(unsigned i, unsigned j) {
            rational const& ac = m_coeffs[i];
            rational const& bc = m_coeffs[j];
            SASSERT(ac.is_pos() == bc.is_pos());
            SASSERT(ac.is_neg() == bc.is_neg());
            expr_ref bt (m), as (m);
            expr* t = m_terms.get (i);
            expr* s = m_terms.get (j);
            bt = mk_mul(abs(bc), t);
            as = mk_mul(abs(ac), s);
            expr_ref result1(m), result2(m);
            if (!m_strict[j] && m_strict[i]) {
                result1 = a.mk_lt(bt, as);
            }
            else {
                result1 = a.mk_le(bt, as);
            }
            m_rw(result1, result2);
            return result2;
        }
        
        // ax + t = 0
        // bx + s <= 0
        // replace equality by (-t/a == -s/b), or, as = bt
        expr_ref mk_eq (unsigned i, unsigned j) {
            expr_ref as (m), bt (m);
            as = mk_mul (m_coeffs[i], m_terms.get (j));
            bt = mk_mul (m_coeffs[j], m_terms.get (i));
            expr_ref result (m);
            result = m.mk_eq (as, bt);
            m_rw (result);
            return result;
        }


        expr* mk_add(expr* t1, expr* t2) {
            return a.mk_add(t1, t2);
        }
        expr* mk_mul(rational const& r, expr* t2) {
            expr* t1 = a.mk_numeral(r, m.get_sort(t2));
            return a.mk_mul(t1, t2);
        }

        /**
         * walk the ast of fml and introduce a fresh variable for every mod term
         * (updating the mdl accordingly)
         */
        void factor_mod_terms (expr_ref& fml, app_ref_vector& vars, model& mdl) {
            expr_ref_vector todo (m), eqs (m);
            expr_map factored_terms (m);
            ast_mark done;

            todo.push_back (fml);
            while (!todo.empty ()) {
                expr* e = todo.back ();
                if (!is_app (e) || done.is_marked (e)) {
                    todo.pop_back ();
                    continue;
                }
                app* ap = to_app (e);
                unsigned num_args = ap->get_num_args ();
                bool all_done = true, changed = false;
                expr_ref_vector args (m);
                for (unsigned i = 0; i < num_args; i++) {
                    expr* old_arg = ap->get_arg (i);
                    if (!done.is_marked (old_arg)) {
                        todo.push_back (old_arg);
                        all_done = false;
                    }
                    if (!all_done) continue;
                    // all args so far have been processed
                    // get the correct arg to use
                    proof* pr = 0; expr* new_arg = 0;
                    factored_terms.get (old_arg, new_arg, pr);
                    if (new_arg) {
                        // changed
                        args.push_back (new_arg);
                        changed = true;
                    }
                    else {
                        // not changed
                        args.push_back (old_arg);
                    }
                }
                if (all_done) {
                    // all args processed; make new term
                    func_decl* d = ap->get_decl ();
                    expr_ref new_term (m);
                    new_term = m.mk_app (d, args.size (), args.c_ptr ());
                    // check for mod and introduce new var
                    if (a.is_mod (ap)) {
                        app_ref new_var (m);
                        new_var = m.mk_fresh_const ("mod_var", d->get_range ());
                        eqs.push_back (m.mk_eq (new_var, new_term));
                        // obtain value of new_term in mdl
                        expr_ref val (m);
                        mdl.eval (new_term, val, true);
                        // use the variable from now on
                        new_term = new_var;
                        changed = true;
                        // update vars and mdl
                        vars.push_back (new_var);
                        mdl.register_decl (new_var->get_decl (), val);
                    }
                    if (changed) {
                        factored_terms.insert (e, new_term, 0);
                    }
                    done.mark (e, true);
                    todo.pop_back ();
                }
            }

            // mk new fml
            proof* pr = 0; expr* new_fml = 0;
            factored_terms.get (fml, new_fml, pr);
            if (new_fml) {
                fml = new_fml;
                // add in eqs
                fml = m.mk_and (fml, m.mk_and (eqs.size (), eqs.c_ptr ()));
            }
            else {
                // unchanged
                SASSERT (eqs.empty ());
            }
        }

        /**
         * factor out mod terms by using divisibility terms;
         *
         * for now, only handle mod equalities of the form (t1 % num == t2),
         * replacing it by the equivalent (num | (t1-t2)) /\ (0 <= t2 < abs(num));
         * the divisibility atom is a special mod term ((t1-t2) % num == 0)
         */
        void mod2div (expr_ref& fml, expr_map& map) {
            expr* new_fml = 0;

            proof *pr = 0;
            map.get (fml, new_fml, pr);
            if (new_fml) {
                fml = new_fml;
                return;
            }

            expr_ref z (a.mk_numeral (rational::zero (), a.mk_int ()), m);
            bool is_mod_eq = false;

            expr *e1, *e2, *num;
            expr_ref t1 (m), t2 (m);
            rational num_val;
            bool is_int;
            // check if fml is a mod equality (t1 % num) == t2
            if (m.is_eq (fml, e1, e2)) {
                expr* t;
                if (a.is_mod (e1, t, num) && a.is_numeral (num, num_val, is_int) && is_int) {
                    t1 = t;
                    t2 = e2;
                    is_mod_eq = true;
                } else if (a.is_mod (e2, t, num) && a.is_numeral (num, num_val, is_int) && is_int) {
                    t1 = t;
                    t2 = e1;
                    is_mod_eq = true;
                }
            }

            if (is_mod_eq) {
                // recursively mod2div for t1 and t2
                mod2div (t1, map);
                mod2div (t2, map);

                rational t2_num;
                if (a.is_numeral (t2, t2_num) && t2_num.is_zero ()) {
                    // already in the desired form;
                    // new_fml is (num_val | t1)
                    new_fml = m.mk_eq (a.mk_mod (t1, a.mk_numeral (num_val, a.mk_int ())),
                                       z);
                }
                else {
                    expr_ref_vector lits (m);
                    // num_val | (t1 - t2)
                    lits.push_back (m.mk_eq (a.mk_mod (a.mk_sub (t1, t2),
                                                    a.mk_numeral (num_val, a.mk_int ())),
                                          z));
                    // 0 <= t2
                    lits.push_back (a.mk_le (z, t2));
                    // t2 < abs (num_val)
                    lits.push_back (a.mk_lt (t2, a.mk_numeral (abs (num_val), a.mk_int ())));

                    new_fml = m.mk_and (lits.size (), lits.c_ptr ());
                }
            }
            else if (!is_app (fml)) {
                new_fml = fml;
            }
            else {
                app* a = to_app (fml);
                expr_ref_vector children (m);
                expr_ref ch (m);
                for (unsigned i = 0; i < a->get_num_args (); i++) {
                    ch = a->get_arg (i);
                    mod2div (ch, map);
                    children.push_back (ch);
                }
                new_fml = m.mk_app (a->get_decl (), children.size (), children.c_ptr ());
            }

            map.insert (fml, new_fml, 0);
            fml = new_fml;
        }

        void collect_lits (expr* fml, app_ref_vector& lits) {
            expr_ref_vector todo (m);
            ast_mark visited;
            todo.push_back(fml);
            while (!todo.empty()) {
                expr* e = todo.back();
                todo.pop_back();
                if (visited.is_marked(e)) {
                    continue;
                }
                visited.mark(e, true);
                if (!is_app(e)) {
                    continue;
                }
                app* a = to_app(e);
                if (m.is_and(a) || m.is_or(a)) {
                    for (unsigned i = 0; i < a->get_num_args(); ++i) {
                        todo.push_back(a->get_arg(i));
                    }
                } else {
                    lits.push_back (a);
                }
            }
            SASSERT(todo.empty());
            visited.reset();
        }

        /**
         * assume that all coeffs of x are the same, say c
         * substitute x_term_val for (c*x) in all lits and update map
         * make the literal at idx true
         */
        void mk_lit_substitutes (expr_ref const& x_term_val, expr_map& map, unsigned idx) {
            expr_ref z (a.mk_numeral (rational::zero (), a.mk_int ()), m);
            expr_ref cxt (m), new_lit (m);
            for (unsigned i = 0; i < m_lits.size(); ++i) {
                if (i == idx) {
                    new_lit = m.mk_true ();
                } else {
                    // cxt
                    if (m_coeffs[i].is_neg ()) {
                        cxt = a.mk_sub (m_terms.get (i), x_term_val);
                    } else {
                        cxt = a.mk_add (m_terms.get (i), x_term_val);
                    }

                    if (m_divs[i].is_zero ()) {
                        if (m_eq[i]) {
                            new_lit = m.mk_eq (cxt, z);
                        } else if (m_strict[i]) {
                            new_lit = a.mk_lt (cxt, z);
                        } else {
                            new_lit = a.mk_le (cxt, z);
                        }
                    } else {
                        // div term
                        new_lit = m.mk_eq (a.mk_mod (cxt,
                                                     a.mk_numeral (m_divs[i], a.mk_int ())),
                                           z);
                    }
                }
                map.insert (m_lits.get (i), new_lit, 0);
                TRACE ("qe",
                        tout << "Old literal: " << mk_pp (m_lits.get (i), m) << "\n";
                        tout << "New literal: " << mk_pp (new_lit, m) << "\n";
                      );
            }
        }

        void substitute (expr_ref& fml, app_ref_vector& lits, expr_map& map) {
            expr_substitution sub (m);
            // literals
            for (unsigned i = 0; i < lits.size (); i++) {
                expr* new_lit = 0; proof* pr = 0;
                app* old_lit = lits.get (i);
                map.get (old_lit, new_lit, pr);
                if (new_lit) {
                    sub.insert (old_lit, new_lit);
                    TRACE ("qe",
                            tout << "old lit " << mk_pp (old_lit, m) << "\n";
                            tout << "new lit " << mk_pp (new_lit, m) << "\n";
                          );
                }
            }
            // substitute for x, if any
            expr* x_term = 0; proof* pr = 0;
            map.get (m_var->x (), x_term, pr);
            if (x_term) {
                sub.insert (m_var->x (), x_term);
                TRACE ("qe",
                        tout << "substituting " << mk_pp (m_var->x (), m) << " by " << mk_pp (x_term, m) << "\n";
                      );
            }
            scoped_ptr<expr_replacer> rep = mk_default_expr_replacer (m);
            rep->set_substitution (&sub);
            (*rep)(fml);
        }

    public:
        arith_project_util(ast_manager& m): 
            m(m), a(m), m_rw(m), m_lits (m), m_terms (m) {}

        expr_ref operator()(model& mdl, app_ref_vector& vars, expr_ref_vector const& lits) {
            app_ref_vector new_vars(m);
            expr_ref_vector result(lits);
            for (unsigned i = 0; i < vars.size(); ++i) {
                app* v = vars.get (i);
                m_var = alloc(contains_app, m, v);
                try {
                    if (a.is_int (v)) {
                        IF_VERBOSE(1, verbose_stream() << "can't project int vars:" << mk_pp(v, m) << "\n";);
                        throw cant_project ();
                    }
                    project(mdl, result);
                    TRACE("qe", tout << "projected: " << mk_pp(v, m) << "\n";
                          for (unsigned i = 0; i < result.size(); ++i) {
                              tout << mk_pp(result.get (i), m) << "\n";
                          });
                }
                catch (cant_project) {
                    IF_VERBOSE(1, verbose_stream() << "can't project:" << mk_pp(v, m) << "\n";);

                    new_vars.push_back(v);
                }
            }
            vars.reset();
            vars.append(new_vars);
            return qe::mk_and(result);
        }

        void operator()(model& mdl, app_ref_vector& vars, expr_ref& fml) {
          expr_map map (m);
        	operator()(mdl, vars, fml, map);
        }

        void operator()(model& mdl, app_ref_vector& vars, expr_ref& fml, expr_map& map) {
            app_ref_vector new_vars(m);

            // factor out mod terms by introducing new variables
            TRACE ("qe",
                    tout << "before factoring out mod terms:" << "\n";
                    tout << mk_pp (fml, m) << "\n";
                    tout << "mdl:\n";
                    model_pp (tout, mdl);
                    tout << "\n";
                  );
            
            factor_mod_terms (fml, vars, mdl);

            TRACE ("qe",
                    tout << "after factoring out mod terms:" << "\n";
                    tout << mk_pp (fml, m) << "\n";
                    tout << "updated mdl:\n";
                    model_pp (tout, mdl);
                    tout << "\n";
                  );

            app_ref_vector lits (m);
//          expr_map map (m);
            for (unsigned i = 0; i < vars.size(); ++i) {
                app* v = vars.get (i);
                TRACE ("qe",
                        tout << "projecting variable: " << mk_pp (v, m) << "\n";
                      );
                m_var = alloc(contains_app, m, v);
                try {
                    map.reset ();
                    lits.reset ();
                    if (a.is_int (v)) {
                        // factor out mod terms using div terms
                        expr_map mod_map (m);
                        mod2div (fml, mod_map);
                        TRACE ("qe",
                                tout << "after mod2div:" << "\n";
                                tout << mk_pp (fml, m) << "\n";
                              );
                    }
                    collect_lits (fml, lits);
                    app_ref div_lit (m);
                    project (mdl, lits, map, div_lit);
                    substitute (fml, lits, map);
                    if (div_lit) {
                        fml = m.mk_and (fml, div_lit);
                    }
                    TRACE("qe",
                            tout << "projected: " << mk_pp(v, m) << " "
                                 << mk_pp(fml, m) << "\n";
                         );
                    /**
                     * DEBUG_CODE(
                     *     expr_ref bval (m);
                     *     // model evaluation doesn't always work for array
                     *     // variables
                     *     SASSERT (mdl.eval (fml, bval, true) && m.is_true (bval));
                     * );
                     */
                }
                catch (cant_project) {
                    IF_VERBOSE(1, verbose_stream() << "can't project:" << mk_pp(v, m) << "\n";);
                    new_vars.push_back(v);
                }
            }
            vars.reset();
            vars.append(new_vars);
            m_rw (fml);
        }
    };


//    class array_project_util {
//        ast_manager&                m;
//        array_util                  m_arr_u;
//        arith_util                  m_ari_u;
//        expr_map                    m_elim_stores_cache;
//        ast_mark                    m_elim_stores_done;
//        ast_mark                    m_has_stores;
//        expr_ref_vector             _tmp;   // to ensure a reference
//        app_ref_vector              m_aux_vars;
//        expr_ref_vector             m_aux_lits;
//        model_ref                   M;
//        model_evaluator_array_util  m_mev;
//
//        struct cant_project {};
//
//        void reset () {
//            m_elim_stores_cache.reset ();
//            m_elim_stores_done.reset ();
//            m_has_stores.reset ();
//            _tmp.reset ();
//            m_aux_vars.reset ();
//            m_aux_lits.reset ();
//        }
//
//        /**
//         * convert partial equality expression p_exp to an equality by
//         * recursively adding stores on diff indices
//         *
//         * add stores on lhs or rhs depending on whether stores_on_rhs is false/true
//         */
//        void convert_peq_to_eq (expr* p_exp, app_ref& eq, bool stores_on_rhs = true) {
//            peq p (to_app (p_exp), m);
//            app_ref_vector diff_val_consts (m);
//            p.mk_eq (diff_val_consts, eq, stores_on_rhs);
//            // extend M to include diff_val_consts
//            expr_ref arr (m);
//            p.lhs (arr);
//            expr_ref_vector I (m);
//            p.get_diff_indices (I);
//            expr_ref val (m);
//            unsigned num_diff = diff_val_consts.size ();
//            SASSERT (num_diff == I.size ());
//            for (unsigned i = 0; i < num_diff; i++) {
//                // mk val term
//                ptr_vector<expr> sel_args;
//                sel_args.push_back (arr);
//                sel_args.push_back (I.get (i));
//                expr_ref val_term (m_arr_u.mk_select (sel_args.size (), sel_args.c_ptr ()), m);
//                // evaluate and assign to ith diff_val_const
//                m_mev.eval (*M, val_term, val);
//                M->register_decl (diff_val_consts.get (i)->get_decl (), val);
//            }
//        }
//
//        /**
//         * mk (e0 ==indices e1)
//         *
//         * result has stores if either e0 or e1 or an index term has stores
//         */
//        void mk_peq_and_mark_stores (expr* e0, expr* e1, unsigned num_indices, expr* const* indices, app_ref& result) {
//            peq p (e0, e1, num_indices, indices, m);
//            p.mk_peq (result);
//            // mark stores
//            bool has_stores = m_has_stores.is_marked (e0) || m_has_stores.is_marked (e1);
//            for (unsigned i = 0; !has_stores && i < num_indices; i++) {
//                if (m_has_stores.is_marked (indices [i])) {
//                    has_stores = true;
//                }
//            }
//            if (has_stores) {
//                m_has_stores.mark (result.get (), true);
//            }
//        }
//
//        /**
//         * walk down the ast of fml and mark (in m_has_stores) all terms containing stores on or of v
//         *
//         * also populate all equalities
//         */
//        void mark_stores (expr_ref const& fml, app* v, expr_ref_vector& eqs) {
//            ast_mark done;
//            expr_ref_vector todo (m);
//            todo.push_back (fml);
//            while (!todo.empty ()) {
//                expr_ref e (todo.back (), m);
//                if (done.is_marked (e) || !is_app (e)) {
//                    todo.pop_back ();
//                    continue;
//                }
//
//                app* a = to_app (e);
//
//                bool all_done = true;
//                unsigned num_args = a->get_num_args ();
//                for (unsigned i = 0; i < num_args; i++) {
//                    expr* arg = a->get_arg (i);
//                    if (!done.is_marked (arg)) {
//                        all_done = false;
//                        todo.push_back (arg);
//                    }
//                }
//                if (!all_done) continue;
//                todo.pop_back ();
//
//                // check and mark if a has stores on/of v
//                bool args_have_stores = false;
//                expr_ref_vector eq_args (m);
//                for (unsigned i = 0; i < num_args; i++) {
//                    expr* arg = a->get_arg (i);
//                    if (!args_have_stores && m_has_stores.is_marked (arg)) {
//                        args_have_stores = true;
//                    }
//                }
//                if (args_have_stores ||
//                        (m_arr_u.is_store (a) && (a->get_arg (0) == v || a->get_arg (2) == v))) {
//                    m_has_stores.mark (a, true);
//
//                    TRACE ("qe",
//                            tout << "has stores:\n";
//                            tout << mk_pp (a, m) << "\n";
//                          );
//                }
//
//                // check if a is a relevant array equality
//                if (m.is_eq (a)) {
//                    expr* a0 = to_app (a)->get_arg (0);
//                    expr* a1 = to_app (a)->get_arg (1);
//                    if (a0 == v || a1 == v ||
//                            (m_arr_u.is_array (a0) && m_has_stores.is_marked (a))) {
//                        eqs.push_back (a);
//                    }
//                }
//                // else, we can check for disequalities and handle them using extensionality,
//                // but it's not necessary
//
//                done.mark (a, true);
//            }
//        }
//
//        /**
//         * reduce select after stores based on the model
//         *
//         * eq_to_exclude are array equalities which shouldn't be touched,
//         *  as they are processed separately
//         */
//        void project_sel_after_stores (expr_ref& fml, app* v, unsigned num_eq_to_exclude = 0, expr* const* eq_to_exclude = 0) {
//            if (!is_app (fml) || !m_has_stores.is_marked (fml)) return;
//
//            if (m_elim_stores_done.is_marked (fml)) {
//                expr* e_new = 0; proof* pr;
//                m_elim_stores_cache.get (fml, e_new, pr);
//                if (e_new) {
//                    fml = e_new;
//                }
//                return;
//            }
//
//            // process arguments
//            app* a = to_app (fml);
//
//            if (m.is_eq (a)) {
//                for (unsigned i = 0; i < num_eq_to_exclude; i++) {
//                    if (a == eq_to_exclude [i]) {
//                        return;
//                    }
//                }
//            }
//
//            expr_ref_vector args (m);
//            bool args_have_stores = false;
//            bool args_changed = false;
//            for (unsigned i = 0; i < a->get_num_args (); i++) {
//                expr_ref arg (a->get_arg (i), m);
//                project_sel_after_stores (arg, v, num_eq_to_exclude, eq_to_exclude);
//                args.push_back (arg);
//                if (!args_changed && arg != a->get_arg (i)) {
//                    args_changed = true;
//                }
//                // does arg have stores?
//                if (!args_have_stores && m_has_stores.is_marked (arg)) {
//                    args_have_stores = true;
//                }
//            }
//
//            expr_ref a_new (fml, m); // initialize to fml
//            if (m_arr_u.is_select (a->get_decl ()) && m_arr_u.is_store (args.get (0))
//                    && m_has_stores.is_marked (args.get (0))) {
//                // sel-after-store
//                // (store arr i val)[j]
//                app* store = to_app (args.get (0));
//                expr* arr = store->get_arg (0);
//                expr* i = store->get_arg (1);
//                expr* val = store->get_arg (2);
//                expr* j = args.get (1);
//
//                expr_ref val_i (m), val_j (m);
//                m_mev.eval (*M, i, val_i);
//                m_mev.eval (*M, j, val_j);
//                if (val_i == val_j) {
//                    // (store arr i val)[i] --> val
//                    a_new = val;
//                    m_aux_lits.push_back (m.mk_eq (i, j));
//                }
//                else {
//                    // (store arr i val)[j] /\ i!=j --> arr[j]
//                    expr_ref_vector sel_args (m);
//                    sel_args.push_back (arr);
//                    sel_args.push_back (j);
//                    a_new = m_arr_u.mk_select (sel_args.size (), sel_args.c_ptr ());
//                    // update m_has_stores
//                    if (m_has_stores.is_marked (arr)) {
//                        m_has_stores.mark (a_new, true);
//                    }
//                    m_aux_lits.push_back (m.mk_not (m.mk_eq (i, j)));
//                    // we need to store it as a_new might get deleted otherwise,
//                    // in which case, the keys in the expr_map's and ast_mark's
//                    // (m_has_stores, m_elim_stores_done, m_elim_stores_cache)
//                    // become stale (by assigning same ids to newly created
//                    // asts, or creating asts at the same address as the deleted
//                    // ones) and cause conflicts
//                    _tmp.push_back (a_new);
//                    // recursively eliminate sel-after-stores
//                    project_sel_after_stores (a_new, v);
//                }
//            }
//            else if (args_changed) {
//                // update with new args
//                a_new = m.mk_app (a->get_decl (), args.size (), args.c_ptr ());
//
//                // mark if there are stores on a variable being eliminated
//                if (args_have_stores ||
//                        (m_arr_u.is_store (a_new) && (args.get (0) == v || args.get (2) == v))) {
//                    m_has_stores.mark (a_new, true);
//                }
//            }
//            m_elim_stores_done.mark (fml, true);
//            if (fml != a_new) {
//                m_elim_stores_cache.insert (fml, a_new, 0);
//                _tmp.push_back (a_new);
//                fml = a_new;
//            }
//        }
//
//        /**
//         * factor out select terms on v using fresh consts
//         *
//         * this is only used when a substitution term is found for v, so we
//         * don't bother about marking stores for newly created terms
//         */
//        void factor_selects (expr_ref& fml, app* v) {
//            expr_map sel_cache (m);
//            ast_mark done;
//            expr_ref_vector todo (m);
//            expr_ref_vector _tmp (m); // to ensure a reference
//            todo.push_back (fml);
//            while (!todo.empty ()) {
//                expr_ref e (todo.back (), m);
//                if (done.is_marked (e) || !is_app (e)) {
//                    todo.pop_back ();
//                    continue;
//                }
//
//                app* a = to_app (e);
//                expr_ref_vector args (m);
//                bool all_done = true;
//                for (unsigned i = 0; i < a->get_num_args (); i++) {
//                    expr* arg = a->get_arg (i);
//                    if (!done.is_marked (arg)) {
//                        all_done = false;
//                        todo.push_back (arg);
//                    }
//                    else if (all_done) { // all done so far..
//                        expr* arg_new = 0; proof* pr;
//                        sel_cache.get (arg, arg_new, pr);
//                        if (!arg_new) {
//                            arg_new = arg;
//                        }
//                        args.push_back (arg_new);
//                    }
//                }
//                if (!all_done) continue;
//                todo.pop_back ();
//
//                expr_ref a_new (m.mk_app (a->get_decl (), args.size (), args.c_ptr ()), m);
//
//                // if a_new is select on v, introduce new constant
//                if (m_arr_u.is_select (a->get_decl ()) && args.get (0) == v) {
//                    sort* val_sort = get_array_range (m.get_sort (v));
//                    app_ref val_const (m.mk_fresh_const ("sel", val_sort), m);
//                    m_aux_vars.push_back (val_const);
//                    // extend M to include val_const
//                    expr_ref val (m);
//                    m_mev.eval (*M, a_new, val);
//                    M->register_decl (val_const->get_decl (), val);
//                    // add equality
//                    m_aux_lits.push_back (m.mk_eq (val_const, a_new));
//                    // replace select by const
//                    a_new = val_const;
//                }
//
//                if (a != a_new) {
//                    sel_cache.insert (e, a_new, 0);
//                    _tmp.push_back (a_new);
//                }
//                done.mark (e, true);
//            }
//            expr* res = 0; proof* pr;
//            sel_cache.get (fml, res, pr);
//            if (res) {
//                fml = res;
//            }
//        }
//
//        void find_subst_term (expr_ref_vector& peqs, app* v, expr_ref& subst_term) {
//            expr_ref p_exp (m);
//            bool subst_eq_found = false;
//            while (!peqs.empty ()) {
//                p_exp = peqs.back ();
//                peqs.pop_back ();
//                // we store this for the same reason as in
//                // project_sel_after_stores: the keys in the maps and marks
//                // might become stale otherwise
//                _tmp.push_back (p_exp);
//
//                TRACE ("qe",
//                        tout << "processing peq:\n";
//                        tout << mk_pp (p_exp, m) << "\n";
//                      );
//
//                // project sel-after-stores from p_exp (lhs, rhs, and diff indices)
//                project_sel_after_stores (p_exp, v);
//
//                TRACE ("qe",
//                        tout << "after projecting stores:\n";
//                        tout << mk_pp (p_exp, m) << "\n";
//                      );
//
//                // expand p_exp if needed
//                peq p (to_app (p_exp), m);
//                expr_ref lhs (m), rhs (m);
//                p.lhs (lhs); p.rhs (rhs);
//                if (!m_has_stores.is_marked (lhs)) {
//                    std::swap (lhs, rhs);
//                }
//                if (m_has_stores.is_marked (lhs)) {
//                    /** project using the equivalence:
//                     *
//                     *  (Store(arr0,i,x) ==I arr1) <->
//                     *
//                     *  (i \in I => (arr0 ==I arr1)) /\
//                     *  (i \not\in I => (arr0 ==I+i arr1) /\ (arr1[i] == x)))
//                     */
//                    expr_ref_vector I (m);
//                    p.get_diff_indices (I);
//                    app* a_lhs = to_app (lhs);
//                    expr* arr0 = a_lhs->get_arg (0);
//                    expr* i = a_lhs->get_arg (1);
//                    expr* x = a_lhs->get_arg (2);
//                    expr* arr1 = rhs;
//                    // check if (i \in I) in M
//                    bool i_in_I = false;
//                    if (!I.empty ()) {
//                        expr_ref i_val (m);
//                        m_mev.eval (*M, i, i_val);
//                        for (unsigned j = 0; j < I.size (); j++) {
//                            expr_ref val (m);
//                            m_mev.eval (*M, I.get (j), val);
//                            if (i_val == val) {
//                                i_in_I = true;
//
//                                TRACE ("qe",
//                                        tout << "store index in diff indices:\n";
//                                        tout << mk_pp (i, m) << "\n";
//                                      );
//
//                                break;
//                            }
//                        }
//                    }
//                    if (i_in_I) {
//                        // arr0 ==I arr1
//                        app_ref p1_exp (m);
//                        mk_peq_and_mark_stores (arr0, arr1, I.size (), I.c_ptr (), p1_exp);
//                        peqs.push_back (p1_exp.get ());
//
//                        TRACE ("qe",
//                                tout << "new peq:\n";
//                                tout << mk_pp (p1_exp, m) << "\n";
//                              );
//                    }
//                    else {
//                        // arr0 ==I+i arr1
//                        I.push_back (i);
//                        app_ref p1_exp (m);
//                        mk_peq_and_mark_stores (arr0, arr1, I.size (), I.c_ptr (), p1_exp);
//                        peqs.push_back (p1_exp.get ());
//
//                        TRACE ("qe",
//                                tout << "new peq:\n";
//                                tout << mk_pp (p1_exp, m) << "\n";
//                              );
//
//                        // arr1[i] == x
//                        ptr_vector<expr> sel_args;
//                        sel_args.push_back (arr1);
//                        sel_args.push_back (i);
//                        expr_ref arr1_i (m_arr_u.mk_select (sel_args.size (), sel_args.c_ptr ()), m);
//                        // update m_has_stores
//                        if (m_has_stores.is_marked (arr1)) {
//                            m_has_stores.mark (arr1_i, true);
//                        }
//
//                        if (m_arr_u.is_array (x)) {
//                            // array equality
//                            app_ref p2_exp (m);
//                            mk_peq_and_mark_stores (arr1_i, x, 0, 0, p2_exp);
//                            peqs.push_back (p2_exp.get ());
//
//                            TRACE ("qe",
//                                    tout << "new peq:\n";
//                                    tout << mk_pp (p2_exp, m) << "\n";
//                                  );
//                        }
//                        else {
//                            // not an array equality
//                            expr_ref eq (m.mk_eq (arr1_i, x), m);
//                            m_aux_lits.push_back (eq);
//                            if (m_has_stores.is_marked (arr1_i) || m_has_stores.is_marked (x)) {
//                                m_has_stores.mark (eq, true);
//                            }
//
//                            TRACE ("qe",
//                                    tout << "new eq:\n";
//                                    tout << mk_pp (eq, m) << "\n";
//                                  );
//                        }
//                    }
//                }
//                else if (lhs == rhs) { // trivial peq (a ==I a)
//                    TRACE ("qe",
//                            tout << "trivial peq -- dropping it\n";
//                          );
//                    continue;
//                }
//                else if (lhs == v || rhs == v) {
//                    subst_eq_found = true;
//                    TRACE ("qe",
//                            tout << "subst eq found!\n";
//                          );
//                    break;
//                }
//                else {
//                    app_ref eq (m);
//                    convert_peq_to_eq (p_exp, eq);
//                    m_aux_lits.push_back (eq);
//                    // no stores on eq -- no need to mark
//
//                    TRACE ("qe",
//                            tout << "no stores/eqs on v -- converting to equality:\n";
//                            tout << mk_pp (eq, m) << "\n";
//                          );
//                }
//            }
//            // factor out select terms on v from p_exp
//            if (subst_eq_found) {
//                m_aux_lits.reset ();
//                factor_selects (p_exp, v);
//
//                TRACE ("qe",
//                        tout << "after factoring selects:\n";
//                        tout << mk_pp (p_exp, m) << "\n";
//                        for (unsigned i = 0; i < m_aux_lits.size (); i++) {
//                            tout << mk_pp (m_aux_lits.get (i), m) << "\n";
//                        }
//                      );
//
//                // find subst_term
//                bool stores_on_rhs = true;
//                app* a = to_app (p_exp);
//                if (a->get_arg (1) == v) {
//                    stores_on_rhs = false;
//                }
//                app_ref eq (m);
//                convert_peq_to_eq (p_exp, eq, stores_on_rhs);
//                subst_term = eq->get_arg (1);
//                // no stores on eq -- no need to mark (no need anyway at this point)
//
//                TRACE ("qe",
//                        tout << "subst term found:\n";
//                        tout << mk_pp (subst_term, m) << "\n";
//                      );
//            }
//        }
//
//        /**
//         * replace all select terms on v by fresh constants, one per value of the index
//         *      terms in M
//         * for index terms that evlauate to the same value in M, add equalities
//         * for index terms that evaluate differently in M, add disequalities
//         * (the transitivity of equalities helps reduce the number of generated
//         *  literals -- without equalities, we need to add disequalities for
//         *  every pair of index terms in distinct equivalence classes)
//         *
//         * no need to mark stores for newly created terms
//         */
//        void project_selects (expr_ref& fml, app* v) {
//            expr_safe_replace sub (m); // for all select terms
//
//            // representative indices for distinct values
//            expr_ref_vector idx_reprs (m);
//            expr_ref_vector idx_vals (m);
//            app_ref_vector sel_consts (m);
//
//            // eqs and diseqs over idx terms
//            expr_ref_vector aux_lits (m);
//
//            expr_ref_vector todo (m);
//            todo.push_back (fml);
//            ast_mark done;
//            while (!todo.empty ()) {
//                expr_ref e (todo.back (), m);
//                if (done.is_marked (e) || !is_app (e)) {
//                    todo.pop_back ();
//                    continue;
//                }
//
//                app* a = to_app (e);
//
//                bool all_done = true;
//                unsigned num_args = a->get_num_args ();
//                for (unsigned i = 0; i < num_args; i++) {
//                    expr* arg = a->get_arg (i);
//                    if (!done.is_marked (arg)) {
//                        all_done = false;
//                        todo.push_back (arg);
//                    }
//                }
//                if (!all_done) continue;
//                todo.pop_back ();
//
//                if (m_arr_u.is_select (a) && a->get_arg (0) == v) {
//                    expr* idx = a->get_arg (1);
//                    expr_ref val (m);
//                    m_mev.eval (*M, idx, val);
//                    bool is_new = true;
//                    for (unsigned i = 0; i < idx_vals.size (); i++) {
//                        if (idx_vals.get (i) == val) {
//                            // val already found; use existing sel const and add
//                            // equality (idx == repr)
//                            expr* repr = idx_reprs.get (i);
//                            aux_lits.push_back (m.mk_eq (idx, repr));
//                            expr* c = sel_consts.get (i);
//                            sub.insert (a, c);
//                            is_new = false;
//                            break;
//                        }
//                    }
//                    if (is_new) {
//                        // new repr, val, and sel const
//                        idx_reprs.push_back (idx);
//                        idx_vals.push_back (val);
//                        sort* val_sort = get_array_range (m.get_sort (v));
//                        app_ref c (m.mk_fresh_const ("sel", val_sort), m);
//                        sel_consts.push_back (c);
//                        sub.insert (a, c);
//                        // extend M to include c
//                        m_mev.eval (*M, a, val);
//                        M->register_decl (c->get_decl (), val);
//                    }
//                }
//
//                done.mark (a, true);
//            }
//
//            TRACE ("qe",
//                    tout << "idx reprs:\n";
//                    for (unsigned i = 0; i < idx_reprs.size (); i++) {
//                        tout << mk_pp (idx_reprs.get (i), m) << "\n";
//                    }
//                  );
//
//            // add disequalities for all pairs of reprs
//            for (unsigned i = 0; i < idx_reprs.size (); i++) {
//                for (unsigned j = i+1; j < idx_reprs.size (); j++) {
//                    expr* idx_1 = idx_reprs.get (i);
//                    expr* idx_2 = idx_reprs.get (j);
//                    // if both indices are numerals, the disequality is redundant
//                    if (m_ari_u.is_numeral (idx_1) && m_ari_u.is_numeral (idx_2)) continue;
//                    expr_ref eq (m.mk_eq (idx_1, idx_2), m);
//                    aux_lits.push_back (m.mk_not (eq));
//                }
//            }
//
//            sub (fml);
//            if (!aux_lits.empty ()) {
//                aux_lits.push_back (fml);
//                fml = m.mk_and (aux_lits.size (), aux_lits.c_ptr ());
//            }
//            m_aux_vars.append (sel_consts);
//        }
//
//        /**
//         * 1. find all array equalities true in M
//         * 2. try to find a substitution term using array equalities
//         * 3. if none found, simplify sel-after-store and Ackermannize, w.r.t. M
//         */
//        void project (app* v, expr_ref& fml) {
//            // mark stores on/of v
//            expr_ref_vector eqs (m); // array equalities
//            mark_stores (fml, v, eqs);
//
//            TRACE ("qe",
//                    tout << "array equalities:\n";
//                    for (unsigned i = 0; i < eqs.size (); i++) {
//                        tout << mk_pp (eqs.get (i), m) << "\n";
//                    }
//                  );
//
//            // substitution for equalities, in case no substitution term is found
//            expr_safe_replace eq_sub (m);
//            // convert all true eqs to peqs
//            expr_ref_vector peqs (m);
//            for (unsigned j = 0; j < eqs.size (); j++) {
//                TRACE ("qe",
//                        tout << "array equality:\n";
//                        tout << mk_pp (eqs.get (j), m) << "\n";
//                      );
//                app* a = to_app (eqs.get (j));
//                expr_ref val (m);
//                m_mev.eval_array_eq (*M, a, a->get_arg (0), a->get_arg (1), val);
//                if (!val) {
//                    // unable to evaluate. set to true?
//                    val = m.mk_true ();
//                }
//                SASSERT (m.is_true (val) || m.is_false (val));
//                eq_sub.insert (a, val);
//                TRACE ("qe",
//                        tout << "true in model:\n";
//                        tout << mk_pp (val, m) << "\n";
//                      );
//                if (m.is_false (val)) {
//                    continue;
//                }
//                app_ref p_exp (m);
//                mk_peq_and_mark_stores (a->get_arg (0), a->get_arg (1), 0, 0, p_exp);
//                peqs.push_back (p_exp.get ());
//            }
//
//            // find a substitution term for v using peqs
//            expr_ref subst_term (m);
//            find_subst_term (peqs, v, subst_term);
//            if (!m_aux_lits.empty ()) {
//                // conjoin aux lits with fml
//                m_aux_lits.push_back (fml);
//                fml = m.mk_and (m_aux_lits.size (), m_aux_lits.c_ptr ());
//
//                // mark fml for stores
//                bool fml_has_stores = false;
//                for (unsigned i = 0; !fml_has_stores && i < m_aux_lits.size (); i++) {
//                    if (m_has_stores.is_marked (m_aux_lits.get (i))) {
//                        fml_has_stores = true;
//                    }
//                }
//                if (fml_has_stores) {
//                    m_has_stores.mark (fml, true);
//                }
//                // done with aux lits for now
//                m_aux_lits.reset ();
//            }
//
//            if (subst_term) {
//                expr_safe_replace eq_sub (m);
//                eq_sub.insert (v, subst_term);
//                eq_sub (fml);
//            }
//            else {
//                // all true equalities (if any) are useless
//                // e.g. of a useless equality: (v=v), (v=store(v,i,x)), etc.
//                // elim stores and selects
//                // postpone the substitution for equalities, as it's going to mess with the ast marking
//                project_sel_after_stores (fml, v, eqs.size (), eqs.c_ptr ());
//                if (!m_aux_lits.empty ()) {
//                    // add them in
//                    m_aux_lits.push_back (fml);
//                    fml = m.mk_and (m_aux_lits.size (), m_aux_lits.c_ptr ());
//                    m_aux_lits.reset ();
//                }
//
//                TRACE ("qe",
//                        tout << "after projecting stores:\n";
//                        tout << mk_pp (fml, m) << "\n";
//                      );
//
//                eq_sub (fml);
//
//                TRACE ("qe",
//                        tout << "after substituting for equalities:\n";
//                        tout << mk_pp (fml, m) << "\n";
//                      );
//
//                // stores eliminated; ackermannize
//                project_selects (fml, v);
//
//                TRACE ("qe",
//                        tout << "after projecting selects:\n";
//                        tout << mk_pp (fml, m) << "\n";
//                      );
//            }
//        }
//
//    public:
//
//        array_project_util (ast_manager& m):
//            m (m),
//            m_arr_u (m),
//            m_ari_u (m),
//            m_elim_stores_cache (m),
//            _tmp (m),
//            m_aux_vars (m),
//            m_aux_lits (m),
//            m_mev (m)
//        {}
//
//        void operator () (model& mdl, app_ref_vector& vars, expr_ref& fml) {
//            app_ref_vector new_vars (m);
//            M = &mdl;
//            for (unsigned i = 0; i < vars.size (); i++) {
//                app* v = vars.get (i);
//                if (!m_arr_u.is_array (v)) {
//                    TRACE ("qe",
//                            tout << "not an array variable: " << mk_pp (v, m) << "\n";
//                          );
//                    new_vars.push_back (v);
//                    continue;
//                }
//                TRACE ("qe",
//                        tout << "projecting variable: " << mk_pp (v, m) << "\n";
//                      );
//                try {
//                    reset ();
//                    project (v, fml);
//                    new_vars.append (m_aux_vars);
//                }
//                catch (cant_project) {
//                    IF_VERBOSE(1, verbose_stream() << "can't project:" << mk_pp(v, m) << "\n";);
//                    new_vars.push_back(v);
//                }
//            }
//            vars.reset ();
//            vars.append (new_vars);
//        }
//    };


    class array_project_eqs_util {
        ast_manager&                m;
        array_util                  m_arr_u;
        ast_mark                    m_has_stores;
        app_ref_vector              m_aux_vars;
        expr_ref_vector             m_aux_lits;
        expr_ref_vector             m_idx_lits;
        model_ref                   M;
        model_evaluator_array_util  m_mev;
        app_ref                     m_v;    // array var to eliminate
        expr_safe_replace           m_true_sub;
        expr_safe_replace           m_false_sub;
        expr_ref                    m_subst_term;

        struct cant_project {};

        void reset () {
            m_has_stores.reset ();
            m_aux_vars.reset ();
            m_aux_lits.reset ();
            m_idx_lits.reset ();
        }

        /**
         * find all array equalities on m_v or containing stores on/of m_v
         *
         * also mark terms containing stores on/of m_v
         */
        void find_arr_eqs (expr_ref const& fml, expr_ref_vector& eqs) {
            if (!is_app (fml)) return;
            ast_mark done;
            ptr_vector<app> todo;
            todo.push_back (to_app (fml));
            while (!todo.empty ()) {
                app* a = todo.back ();
                if (done.is_marked (a)) {
                    todo.pop_back ();
                    continue;
                }
                bool all_done = true;
                bool args_have_stores = false;
                unsigned num_args = a->get_num_args ();
                for (unsigned i = 0; i < num_args; i++) {
                    expr* arg = a->get_arg (i);
                    if (!is_app (arg)) continue;
                    if (!done.is_marked (arg)) {
                        all_done = false;
                        todo.push_back (to_app (arg));
                    }
                    else if (!args_have_stores && m_has_stores.is_marked (arg)) {
                        args_have_stores = true;
                    }
                }
                if (!all_done) continue;
                todo.pop_back ();

                // mark if a has stores
                if ((!m_arr_u.is_select (a) && args_have_stores) ||
                        (m_arr_u.is_store (a) && (a->get_arg (0) == m_v))) {
                    m_has_stores.mark (a, true);

                    TRACE ("qe",
                            tout << "has stores:\n";
                            tout << mk_pp (a, m) << "\n";
                          );
                }

                // check if a is a relevant array equality
                if (m.is_eq (a)) {
                    expr* a0 = to_app (a)->get_arg (0);
                    expr* a1 = to_app (a)->get_arg (1);
                    if (a0 == m_v || a1 == m_v ||
                            (m_arr_u.is_array (a0) && m_has_stores.is_marked (a))) {
                        eqs.push_back (a);
                    }
                }
                // else, we can check for disequalities and handle them using extensionality,
                // but it's not necessary

                done.mark (a, true);
            }
        }

        /**
         * factor out select terms on m_v using fresh consts
         */
        void factor_selects (app_ref& fml) {
            expr_map sel_cache (m);
            ast_mark done;
            ptr_vector<app> todo;
            expr_ref_vector pinned (m); // to ensure a reference

            todo.push_back (fml);
            while (!todo.empty ()) {
                app* a = todo.back ();
                if (done.is_marked (a)) {
                    todo.pop_back ();
                    continue;
                }
                expr_ref_vector args (m);
                bool all_done = true;
                for (unsigned i = 0; i < a->get_num_args (); i++) {
                    expr* arg = a->get_arg (i);
                    if (!is_app (arg)) continue;
                    if (!done.is_marked (arg)) {
                        all_done = false;
                        todo.push_back (to_app (arg));
                    }
                    else if (all_done) { // all done so far..
                        expr* arg_new = 0; proof* pr;
                        sel_cache.get (arg, arg_new, pr);
                        if (!arg_new) {
                            arg_new = arg;
                        }
                        args.push_back (arg_new);
                    }
                }
                if (!all_done) continue;
                todo.pop_back ();

                expr_ref a_new (m.mk_app (a->get_decl (), args.size (), args.c_ptr ()), m);

                // if a_new is select on m_v, introduce new constant
                if (m_arr_u.is_select (a) &&
                        (args.get (0) == m_v || m_has_stores.is_marked (args.get (0)))) {
                    sort* val_sort = get_array_range (m.get_sort (m_v));
                    app_ref val_const (m.mk_fresh_const ("sel", val_sort), m);
                    m_aux_vars.push_back (val_const);
                    // extend M to include val_const
                    expr_ref val (m);
                    m_mev.eval (*M, a_new, val);
                    M->register_decl (val_const->get_decl (), val);
                    // add equality
                    m_aux_lits.push_back (m.mk_eq (val_const, a_new));
                    // replace select by const
                    a_new = val_const;
                }

                if (a != a_new) {
                    sel_cache.insert (a, a_new, 0);
                    pinned.push_back (a_new);
                }
                done.mark (a, true);
            }
            expr* res = 0; proof* pr;
            sel_cache.get (fml, res, pr);
            if (res) {
                fml = to_app (res);
            }
        }

        /**
         * convert partial equality expression p_exp to an equality by
         * recursively adding stores on diff indices
         *
         * add stores on lhs or rhs depending on whether stores_on_rhs is false/true
         */
        void convert_peq_to_eq (expr* p_exp, app_ref& eq, bool stores_on_rhs = true) {
            peq p (to_app (p_exp), m);
            app_ref_vector diff_val_consts (m);
            p.mk_eq (diff_val_consts, eq, stores_on_rhs);
            // extend M to include diff_val_consts
            expr_ref arr (m);
            expr_ref_vector I (m);
            p.lhs (arr);
            p.get_diff_indices (I);
            expr_ref val (m);
            unsigned num_diff = diff_val_consts.size ();
            SASSERT (num_diff == I.size ());
            for (unsigned i = 0; i < num_diff; i++) {
                // mk val term
                ptr_vector<expr> sel_args;
                sel_args.push_back (arr);
                sel_args.push_back (I.get (i));
                expr_ref val_term (m_arr_u.mk_select (sel_args.size (), sel_args.c_ptr ()), m);
                // evaluate and assign to ith diff_val_const
                m_mev.eval (*M, val_term, val);
                M->register_decl (diff_val_consts.get (i)->get_decl (), val);
            }
        }

        /**
         * mk (e0 ==indices e1)
         *
         * result has stores if either e0 or e1 or an index term has stores
         */
        void mk_peq (expr* e0, expr* e1, unsigned num_indices, expr* const* indices, app_ref& result) {
            peq p (e0, e1, num_indices, indices, m);
            p.mk_peq (result);
        }

        void find_subst_term (app* eq) {
            app_ref p_exp (m);
            mk_peq (eq->get_arg (0), eq->get_arg (1), 0, 0, p_exp);
            bool subst_eq_found = false;
            while (true) {
                TRACE ("qe",
                        tout << "processing peq:\n";
                        tout << mk_pp (p_exp, m) << "\n";
                      );

                peq p (p_exp, m);
                expr_ref lhs (m), rhs (m);
                p.lhs (lhs); p.rhs (rhs);
                if (!m_has_stores.is_marked (lhs)) {
                    std::swap (lhs, rhs);
                }
                if (m_has_stores.is_marked (lhs)) {
                    /** project using the equivalence:
                     *
                     *  (store(arr0,idx,x) ==I arr1) <->
                     *
                     *  (idx \in I => (arr0 ==I arr1)) /\
                     *  (idx \not\in I => (arr0 ==I+idx arr1) /\ (arr1[idx] == x)))
                     */
                    expr_ref_vector I (m);
                    p.get_diff_indices (I);
                    app* a_lhs = to_app (lhs);
                    expr* arr0 = a_lhs->get_arg (0);
                    expr* idx = a_lhs->get_arg (1);
                    expr* x = a_lhs->get_arg (2);
                    expr* arr1 = rhs;
                    // check if (idx \in I) in M
                    bool idx_in_I = false;
                    expr_ref_vector idx_diseq (m);
                    if (!I.empty ()) {
                        expr_ref val (m);
                        m_mev.eval (*M, idx, val);
                        for (unsigned i = 0; i < I.size () && !idx_in_I; i++) {
                            if (idx == I.get (i)) {
                                idx_in_I = true;
                            }
                            else {
                                expr_ref val1 (m);
                                expr* idx1 = I.get (i);
                                expr_ref idx_eq (m.mk_eq (idx, idx1), m);
                                m_mev.eval (*M, idx1, val1);
                                if (val == val1) {
                                    idx_in_I = true;
                                    m_idx_lits.push_back (idx_eq);
                                }
                                else {
                                    idx_diseq.push_back (m.mk_not (idx_eq));
                                }
                            }
                        }
                    }
                    if (idx_in_I) {
                        TRACE ("qe",
                                tout << "store index in diff indices:\n";
                                tout << mk_pp (m_idx_lits.back (), m) << "\n";
                              );

                        // arr0 ==I arr1
                        mk_peq (arr0, arr1, I.size (), I.c_ptr (), p_exp);

                        TRACE ("qe",
                                tout << "new peq:\n";
                                tout << mk_pp (p_exp, m) << "\n";
                              );
                    }
                    else {
                        m_idx_lits.append (idx_diseq);
                        // arr0 ==I+idx arr1
                        I.push_back (idx);
                        mk_peq (arr0, arr1, I.size (), I.c_ptr (), p_exp);

                        TRACE ("qe",
                                tout << "new peq:\n";
                                tout << mk_pp (p_exp, m) << "\n";
                              );

                        // arr1[idx] == x
                        ptr_vector<expr> sel_args;
                        sel_args.push_back (arr1);
                        sel_args.push_back (idx);
                        expr_ref arr1_idx (m_arr_u.mk_select (sel_args.size (), sel_args.c_ptr ()), m);
                        expr_ref eq (m.mk_eq (arr1_idx, x), m);
                        m_aux_lits.push_back (eq);

                        TRACE ("qe",
                                tout << "new eq:\n";
                                tout << mk_pp (eq, m) << "\n";
                              );
                    }
                }
                else if (lhs == rhs) { // trivial peq (a ==I a)
                    break;
                }
                else if (lhs == m_v || rhs == m_v) {
                    subst_eq_found = true;
                    TRACE ("qe",
                            tout << "subst eq found!\n";
                          );
                    break;
                }
                else {
                    UNREACHABLE ();
                }
            }

            // factor out select terms on m_v from p_exp using fresh constants
            if (subst_eq_found) {
                factor_selects (p_exp);

                TRACE ("qe",
                        tout << "after factoring selects:\n";
                        tout << mk_pp (p_exp, m) << "\n";
                        for (unsigned i = m_aux_lits.size () - m_aux_vars.size (); i < m_aux_lits.size (); i++) {
                            tout << mk_pp (m_aux_lits.get (i), m) << "\n";
                        }
                      );

                // find subst_term
                bool stores_on_rhs = true;
                app* a = to_app (p_exp);
                if (a->get_arg (1) == m_v) {
                    stores_on_rhs = false;
                }
                app_ref eq (m);
                convert_peq_to_eq (p_exp, eq, stores_on_rhs);
                m_subst_term = eq->get_arg (1);

                TRACE ("qe",
                        tout << "subst term found:\n";
                        tout << mk_pp (m_subst_term, m) << "\n";
                      );
            }
        }

        /**
         * try to substitute for m_v, using array equalities
         *
         * compute substitution term and aux lits
         */
        void project (expr_ref const& fml) {
            expr_ref_vector eqs (m);

            find_arr_eqs (fml, eqs);
            TRACE ("qe",
                    tout << "array equalities:\n";
                    for (unsigned i = 0; i < eqs.size (); i++) {
                        tout << mk_pp (eqs.get (i), m) << "\n";
                    }
                  );

            // find subst term
            // TODO: better ordering of eqs?
            for (unsigned i = 0; !m_subst_term && i < eqs.size (); i++) {
                TRACE ("qe",
                        tout << "array equality:\n";
                        tout << mk_pp (eqs.get (i), m) << "\n";
                      );

                expr* curr_eq = eqs.get (i);

                // evaluate curr_eq in M
                app* a = to_app (curr_eq);
                expr_ref val (m);
                m_mev.eval_array_eq (*M, a, a->get_arg (0), a->get_arg (1), val);
                if (!val) {
                    // unable to evaluate. set to true?
                    val = m.mk_true ();
                }
                SASSERT (m.is_true (val) || m.is_false (val));
                TRACE ("qe",
                        tout << "true in model:\n";
                        tout << mk_pp (val, m) << "\n";
                      );

                if (m.is_false (val)) {
                    m_false_sub.insert (curr_eq, m.mk_false ());
                }
                else {
                    m_true_sub.insert (curr_eq, m.mk_true ());
                    // try to find subst term
                    find_subst_term (to_app (curr_eq));
                }
            }
        }

        void mk_result (expr_ref& fml) {
            // add in aux_lits and idx_lits
            expr_ref_vector lits (m);
            // TODO: eliminate possible duplicates, especially in idx_lits
            //       theory rewriting is a possibility, but not sure if it
            //          introduces unwanted terms such as ite's
            lits.append (m_idx_lits);
            lits.append (m_aux_lits);
            lits.push_back (fml);
            fml = m.mk_and (lits.size (), lits.c_ptr ());

            if (m_subst_term) {
                m_true_sub.insert (m_v, m_subst_term);
                m_true_sub (fml);
            }
            else {
                m_true_sub (fml);
                m_false_sub (fml);
            }
        }

    public:

        array_project_eqs_util (ast_manager& m):
            m (m),
            m_arr_u (m),
            m_aux_vars (m),
            m_aux_lits (m),
            m_idx_lits (m),
            m_mev (m),
            m_v (m),
            m_true_sub (m),
            m_false_sub (m),
            m_subst_term (m)
        {}

        void operator () (model& mdl, app_ref_vector& vars, expr_ref& fml) {
            app_ref_vector new_vars (m);
            M = &mdl;
            for (unsigned i = 0; i < vars.size (); i++) {
                m_v = vars.get (i);
                if (!m_arr_u.is_array (m_v)) {
                    TRACE ("qe",
                            tout << "not an array variable: " << mk_pp (m_v, m) << "\n";
                          );
                    new_vars.push_back (m_v);
                    continue;
                }
                TRACE ("qe",
                        tout << "projecting variable: " << mk_pp (m_v, m) << "\n";
                      );
                try {
                    reset ();
                    project (fml);
                    mk_result (fml);
                    new_vars.append (m_aux_vars);
                }
                catch (cant_project) {
                    IF_VERBOSE(1, verbose_stream() << "can't project:" << mk_pp(m_v, m) << "\n";);
                    new_vars.push_back(m_v);
                }
            }
            vars.reset ();
            vars.append (new_vars);
        }
    };

    class array_project_selects_util {
        ast_manager&                m;
        array_util                  m_arr_u;
        arith_util                  m_ari_u;
        obj_map<expr, expr*>        m_elim_stores_cache;
        // representative indices for eliminating selects
        expr_ref_vector             m_idx_reprs;
        expr_ref_vector             m_idx_vals;
        app_ref_vector              m_sel_consts;
        expr_ref_vector             m_pinned;   // to ensure a reference
        expr_ref_vector             m_idx_lits;
        model_ref                   M;
        model_evaluator_array_util  m_mev;
        th_rewriter                 m_rw;
        expr_safe_replace           m_sub;

        struct cant_project {};

        void reset () {
            m_elim_stores_cache.reset ();
            m_idx_reprs.reset ();
            m_idx_vals.reset ();
            m_sel_consts.reset ();
            m_pinned.reset ();
            m_idx_lits.reset ();
            m_sub.reset ();
        }

        bool is_equals (expr *e1, expr *e2) {
            if (e1 == e2) return true;
            expr_ref val1 (m), val2 (m);
            m_mev.eval (*M, e1, val1);
            m_mev.eval (*M, e2, val2);
            return (val1 == val2);
        }

        void add_idx_cond (expr_ref& cond) {
            m_rw (cond);
            if (!m.is_true (cond)) m_idx_lits.push_back (cond);
        }

        expr* sel_after_stores (expr *e) {
            if (!is_app (e)) return e;

            expr *r = 0;
            if (m_elim_stores_cache.find (e, r)) return r;

            ptr_vector<app> todo;
            todo.push_back (to_app (e));

            while (!todo.empty ()) {
                app *a = todo.back ();
                unsigned sz = todo.size ();
                expr_ref_vector args (m);
                bool dirty = false;

                for (unsigned i = 0; i < a->get_num_args (); ++i) {
                    expr *arg = a->get_arg (i);
                    expr *narg = 0;

                    if (!is_app (arg)) args.push_back (arg);
                    else if (m_elim_stores_cache.find (arg, narg)) { 
                        args.push_back (narg);
                        dirty |= (arg != narg);
                    }
                    else {
                        todo.push_back (to_app (arg));
                    }
                }

                if (todo.size () > sz) continue;
                todo.pop_back ();

                if (dirty) {
                    r = m.mk_app (a->get_decl (), args.size (), args.c_ptr ());
                    m_pinned.push_back (r);
                }
                else r = a;

                if (m_arr_u.is_select (r)) r = sel_after_stores_core (to_app(r));

                m_elim_stores_cache.insert (a, r);
            }

            SASSERT (r);
            return r;
        }

        expr* sel_after_stores_core (app *a) {
            if (!m_arr_u.is_store (a->get_arg (0))) return a;

            SASSERT (a->get_num_args () == 2 && "Multi-dimensional arrays are not supported");
            expr* array = a->get_arg (0);
            expr* j = a->get_arg (1);

            while (m_arr_u.is_store (array)) {
                a = to_app (array);
                expr* idx = a->get_arg (1);
                expr_ref cond (m);

                if (is_equals (idx, j)) {
                    cond = m.mk_eq (idx, j);
                    add_idx_cond (cond);
                    return a->get_arg (2);
                }
                else {
                    cond = m.mk_not (m.mk_eq (idx, j));
                    add_idx_cond (cond);
                    array = a->get_arg (0);
                }
            }

            expr* args[2] = {array, j};
            expr* r = m_arr_u.mk_select (2, args);
            m_pinned.push_back (r);
            return r;
        }

        /**
         * collect sel terms on array vars as given by arr_test
         */
        void collect_selects (expr* fml, ast_mark const& arr_test, obj_map<app, ptr_vector<app>*>& sel_terms) {
            if (!is_app (fml)) return;
            ast_mark done;
            ptr_vector<app> todo;
            todo.push_back (to_app (fml));
            while (!todo.empty ()) {
                app* a = todo.back ();
                if (done.is_marked (a)) {
                    todo.pop_back ();
                    continue;
                }
                unsigned num_args = a->get_num_args ();
                bool all_done = true;
                for (unsigned i = 0; i < num_args; i++) {
                    expr* arg = a->get_arg (i);
                    if (!done.is_marked (arg) && is_app (arg)) {
                        todo.push_back (to_app (arg));
                        all_done = false;
                    }
                }
                if (!all_done) continue;
                todo.pop_back ();
                if (m_arr_u.is_select (a)) {
                    expr* arr = a->get_arg (0);
                    if (arr_test.is_marked (arr)) {
                        ptr_vector<app>* lst = sel_terms.find (to_app (arr));;
                        lst->push_back (a);
                    }
                }
                done.mark (a, true);
            }
        }

        /**
         * model based ackermannization for sel terms of some array
         *
         * update sub with val consts for sel terms
         */
        void ackermann (ptr_vector<app> const& sel_terms) {
            if (sel_terms.empty ()) return;

            expr* v = sel_terms.get (0)->get_arg (0); // array variable
            sort* v_sort = m.get_sort (v);
            sort* val_sort = get_array_range (v_sort);
            sort* idx_sort = get_array_domain (v_sort, 0);

            unsigned start = m_idx_reprs.size (); // append at the end

            for (unsigned i = 0; i < sel_terms.size (); i++) {
                app* a = sel_terms.get (i);
                expr* idx = a->get_arg (1);
                expr_ref val (m);
                m_mev.eval (*M, idx, val);

                bool is_new = true;
                for (unsigned j = start; j < m_idx_vals.size (); j++) {
                    if (m_idx_vals.get (j) == val) {
                        // idx belongs to the jth equivalence class;
                        // substitute sel term with ith sel const
                        expr* c = m_sel_consts.get (j);
                        m_sub.insert (a, c);
                        // add equality (idx == repr)
                        expr* repr = m_idx_reprs.get (j);
                        m_idx_lits.push_back (m.mk_eq (idx, repr));

                        is_new = false;
                        break;
                    }
                }
                if (is_new) {
                    // new repr, val, and sel const
                    m_idx_reprs.push_back (idx);
                    m_idx_vals.push_back (val);
                    app_ref c (m.mk_fresh_const ("sel", val_sort), m);
                    m_sel_consts.push_back (c);
                    // substitute sel term with new const
                    m_sub.insert (a, c);
                    // extend M to include c
                    m_mev.eval (*M, a, val);
                    M->register_decl (c->get_decl (), val);
                }
            }

            // sort reprs by their value and add a chain of strict inequalities

            unsigned num_reprs = m_idx_reprs.size () - start;
            if (num_reprs == 0) return;

            SASSERT ((m_ari_u.is_real (idx_sort) || m_ari_u.is_int (idx_sort))
                        && "Unsupported index sort: neither real nor int");

            // using insertion sort
            unsigned end = start + num_reprs;
            for (unsigned i = start+1; i < end; i++) {
                expr* repr = m_idx_reprs.get (i);
                expr* val = m_idx_vals.get (i);
                unsigned j = i;
                for (; j > start && val < m_idx_vals.get (j-1); j--) {
                    m_idx_reprs[j] = m_idx_reprs.get (j-1);
                    m_idx_vals[j] = m_idx_vals.get (j-1);
                }
                m_idx_reprs[j] = repr;
                m_idx_vals[j] = val;
            }

            for (unsigned i = start; i < end-1; i++) {
                m_idx_lits.push_back (m_ari_u.mk_lt (m_idx_reprs.get (i),
                                                     m_idx_reprs.get (i+1)));
            }
        }

        /**
         * project selects
         * populates idx lits and obtains substitution for sel terms
         */
        void project (app_ref_vector& vars, expr_ref& fml) {
            typedef obj_map<app, ptr_vector<app>*> sel_map;

            if (vars.empty ()) return;

            // 1. proj sel after stores
            fml = sel_after_stores (fml);

            TRACE ("qe",
                    tout << "after projecting sel after stores:\n";
                    tout << mk_pp (fml, m) << "\n";
                    for (unsigned i = 0; i < m_idx_lits.size (); i++) {
                        tout << mk_pp (m_idx_lits.get (i), m) << "\n";
                    }
                  );

            // 2. proj selects over vars

            // indicator for arrays to eliminate
            ast_mark arr_test;
            // empty map from array var to sel terms over it
            sel_map sel_terms;
            for (unsigned i = 0; i < vars.size (); i++) {
                app* v = vars.get (i);
                arr_test.mark (v, true);
                ptr_vector<app>* lst = alloc (ptr_vector<app>);
                sel_terms.insert (v, lst);
            }

            // collect sel terms -- populate the map
            collect_selects (fml, arr_test, sel_terms);
            for (unsigned i = 0; i < m_idx_lits.size (); i++) {
                collect_selects (m_idx_lits.get (i), arr_test, sel_terms);
            }

            // model based ackermannization
            sel_map::iterator begin = sel_terms.begin (),
                              end = sel_terms.end ();
            for (sel_map::iterator it = begin; it != end; it++) {
                ackermann (*(it->m_value));
            }

            TRACE ("qe",
                    tout << "idx lits after ackermannization:\n";
                    for (unsigned i = 0; i < m_idx_lits.size (); i++) {
                        tout << mk_pp (m_idx_lits.get (i), m) << "\n";
                    }
                  );

            // dealloc
            for (sel_map::iterator it = begin; it != end; it++) {
                dealloc (it->m_value);
            }
        }

        void mk_result (expr_ref& fml) {
            // conjoin aux lits
            expr_ref_vector lits (m);
            lits.append (m_idx_lits);
            lits.push_back (fml);
            fml = m.mk_and (lits.size (), lits.c_ptr ());

            // substitute for sel terms
            m_sub (fml);

            TRACE ("qe",
                    tout << "after projection of selects:\n";
                    tout << mk_pp (fml, m) << "\n";
                  );
        }

    public:

        array_project_selects_util (ast_manager& m):
            m (m),
            m_arr_u (m),
            m_ari_u (m),
            m_idx_reprs (m),
            m_idx_vals (m),
            m_sel_consts (m),
            m_pinned (m),
            m_idx_lits (m),
            m_mev (m),
            m_rw (m),
            m_sub (m)
        {}

        void operator () (model& mdl, app_ref_vector& vars, expr_ref& fml) {
            app_ref_vector new_vars (m);
            M = &mdl;
            // assume all vars are of array sort
            try {
                reset ();
                project (vars, fml);
                mk_result (fml);
                new_vars.append (m_sel_consts);
            }
            catch (cant_project) {
                IF_VERBOSE(1, verbose_stream() << "can't project arrays:" << "\n";);
                new_vars.append(vars);
            }
            vars.reset ();
            vars.append (new_vars);
        }
    };


    expr_ref arith_project(model& mdl, app_ref_vector& vars, expr_ref_vector const& lits) {
        ast_manager& m = vars.get_manager();
        arith_project_util ap(m);
        return ap(mdl, vars, lits);
    }

    void arith_project(model& mdl, app_ref_vector& vars, expr_ref& fml) {
        ast_manager& m = vars.get_manager();
        arith_project_util ap(m);
        atom_set pos_lits, neg_lits;
        is_relevant_default is_relevant;
        mk_atom_default mk_atom;
        get_nnf (fml, is_relevant, mk_atom, pos_lits, neg_lits);
        ap(mdl, vars, fml);
    }

    void arith_project(model& mdl, app_ref_vector& vars, expr_ref& fml, expr_map& map) {
        ast_manager& m = vars.get_manager();
        arith_project_util ap(m);
        atom_set pos_lits, neg_lits;
        is_relevant_default is_relevant;
        mk_atom_default mk_atom;
        get_nnf (fml, is_relevant, mk_atom, pos_lits, neg_lits);
        ap(mdl, vars, fml, map);
    }

    /**
     * void array_project (model& mdl, app_ref_vector& vars, expr_ref& fml) {
     *     ast_manager& m = vars.get_manager ();
     *     array_project_util ap (m);
     *     ap (mdl, vars, fml);
     * }
     */

    void array_project_selects (model& mdl, app_ref_vector& vars, expr_ref& fml) {
        ast_manager& m = vars.get_manager ();
        array_project_selects_util ap (m);
        ap (mdl, vars, fml);
    }

    void array_project_eqs (model& mdl, app_ref_vector& vars, expr_ref& fml) {
        ast_manager& m = vars.get_manager ();
        array_project_eqs_util ap (m);
        ap (mdl, vars, fml);
    }
}
