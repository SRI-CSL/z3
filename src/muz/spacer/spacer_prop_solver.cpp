/** 
Spacer
Copyright (c) 2015 Carnegie Mellon University.
All Rights Reserved.

THIS SOFTWARE IS PROVIDED "AS IS," WITH NO WARRANTIES
WHATSOEVER. CARNEGIE MELLON UNIVERSITY EXPRESSLY DISCLAIMS TO THE
FULLEST EXTENT PERMITTEDBY LAW ALL EXPRESS, IMPLIED, AND STATUTORY
WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
NON-INFRINGEMENT OF PROPRIETARY RIGHTS.

Released under a modified MIT license, please see SPACER_LICENSE.txt
for full terms.  DM-0002483

Spacer includes and/or makes use of the following Third-Party Software
subject to its own license:

Z3
Copyright (c) Microsoft Corporation
All rights reserved.

Released under the MIT License (LICENSE.txt)

Module Name:

    spacer_prop_solver.cpp

Abstract:

    SAT solver abstraction for SPACER.

Author:

    Arie Gurfinkel
    Anvesh Komuravelli

Revision History:

--*/

#include <sstream>
#include "model.h"
#include "spacer_util.h"
#include "spacer_prop_solver.h"
#include "ast_smt2_pp.h"
#include "dl_util.h"
#include "model_pp.h"
#include "smt_params.h"
#include "datatype_decl_plugin.h"
#include "bv_decl_plugin.h"
#include "spacer_farkas_learner.h"
#include "ast_smt2_pp.h"
#include "expr_replacer.h"
#include "fixedpoint_params.hpp"

namespace spacer {
    
    prop_solver::prop_solver(manager& pm, fixedpoint_params const& p, symbol const& name) :
        m(pm.get_manager()),
        m_pm(pm),
        m_name(name),
        m_ctx(NULL),
        m_pos_level_atoms(m),
        m_neg_level_atoms(m),
        m_core(0),
        m_subset_based_core(false),
        m_uses_level(infty_level ()),
        m_delta_level(false),
        m_in_level(false),
        m_use_push_bg (p.spacer_keep_proxy())
    {
      
        m_solvers[0] = pm.mk_fresh ();
        m_fparams[0] = &pm.fparams ();
        
        m_solvers[1] = pm.mk_fresh2 ();
        m_fparams[1] = &pm.fparams2 ();
      
        m_contexts[0] = alloc(spacer::itp_solver, *(m_solvers[0]), p.spacer_new_unsat_core(), p.spacer_minimize_unsat_core(), p.spacer_farkas_optimized(), p.spacer_farkas_a_const(), p.spacer_split_farkas_literals ());
        m_contexts[1] = alloc(spacer::itp_solver, *(m_solvers[1]), p.spacer_new_unsat_core(), p.spacer_minimize_unsat_core(), p.spacer_farkas_optimized(), p.spacer_farkas_a_const(), p.spacer_split_farkas_literals ());
      
        for (unsigned i = 0; i < 2; ++i)
            m_contexts[i]->assert_expr (m_pm.get_background ());
    }

    void prop_solver::add_level() {
        unsigned idx = level_cnt();
        std::stringstream name;
        name << m_name << "#level_" << idx;
        func_decl * lev_pred = m.mk_fresh_func_decl(name.str().c_str(), 0, 0,m.mk_bool_sort());
        m_level_preds.push_back(lev_pred);

        app_ref pos_la(m.mk_const(lev_pred), m);
        app_ref neg_la(m.mk_not(pos_la.get()), m);

        m_pos_level_atoms.push_back(pos_la);
        m_neg_level_atoms.push_back(neg_la);

        m_level_atoms_set.insert(pos_la.get());
        m_level_atoms_set.insert(neg_la.get());
    }

    void prop_solver::ensure_level(unsigned lvl) {
        while (lvl>=level_cnt()) {
            add_level();
        }
    }

    unsigned prop_solver::level_cnt() const {
        return m_level_preds.size();
    }

    void prop_solver::assert_level_atoms(unsigned level) {
        unsigned lev_cnt = level_cnt();
        for (unsigned i=0; i<lev_cnt; i++) {
            bool active = m_delta_level ? i == level : i>=level;
            app * lev_atom =
                active ? m_neg_level_atoms.get (i) : m_pos_level_atoms.get (i);
            m_ctx->push_bg (lev_atom);
        }
    }

    void prop_solver::assert_expr(expr * form) {
        SASSERT(!m_in_level);
        m_contexts[0]->assert_expr (form);
        m_contexts[1]->assert_expr (form);
        IF_VERBOSE(21, verbose_stream() << "$ asserted " << mk_pp(form, m) << "\n";);
        TRACE("spacer", tout << "add_formula: " << mk_pp(form, m) << "\n";);
    }

    void prop_solver::assert_expr(expr * form, unsigned level) {
        ensure_level(level);
        app * lev_atom = m_pos_level_atoms[level].get();
        app_ref lform(m.mk_or(form, lev_atom), m);
        assert_expr (lform);
    }


    /// Poor man's maxsat. No guarantees of maximum solution
    /// Runs maxsat loop on m_ctx Returns l_false if hard is unsat,
    /// otherwise reduces soft such that hard & soft is sat.
    lbool prop_solver::maxsmt (expr_ref_vector &hard, expr_ref_vector &soft)
    {
        // replace expressions by assumption literals
        itp_solver::scoped_mk_proxy _p_(*m_ctx, hard);
        unsigned hard_sz = hard.size ();
        // assume soft constraints are propositional literals (no need to proxy)
        hard.append (soft);
    
        lbool res = m_ctx->check_sat (hard.size (), hard.c_ptr ());
        // if hard constraints alone are unsat or there are no soft
        // constraints, we are done
        if (res != l_false || soft.empty ()) return res;
    
        // clear soft constraints, we will recompute them later
        soft.reset ();
    
        expr_ref saved (m);
        ptr_vector<expr> core;
        m_ctx->get_unsat_core (core);
    
        // while there are soft constraints
        while (hard.size () > hard_sz)
        {
            bool found = false;
            // look for a soft constraint that is in the unsat core
            for (unsigned i = hard_sz, sz = hard.size (); i < sz; ++i)
                if (core.contains (hard.get (i)))
                {
                    found = true;
                    // AG: not sure why we are saving it
                    saved = hard.get (i);
                    hard[i] = hard.back ();
                    hard.pop_back ();
                    break;
                }
            // if no soft constraints in the core, return this should
            // not happen because it implies that hard alone is unsat
            // and that is taken care of earlier
            if (!found)
            {
                hard.resize (hard_sz);
                return l_false;
            }
      
            // check that the NEW constraints became sat
            res = m_ctx->check_sat (hard.size (), hard.c_ptr ());
            if (res != l_false) break;
            // still unsat, update the core and repeat
            core.reset ();
            m_ctx->get_unsat_core (core);
        }

        // update soft with found soft constraints
        if (res == l_true)
        {
            for (unsigned i = hard_sz, sz = hard.size (); i < sz; ++i)
                soft.push_back (hard.get (i));
        }
        // revert hard back to the right size
        // proxies are undone on exit via scoped_mk_proxy
        hard.resize (hard_sz);
        return res;
    }
  
    lbool prop_solver::internal_check_assumptions(
                                                  expr_ref_vector& hard_atoms,
                                                  expr_ref_vector& soft_atoms)
    {
        // XXX Turn model generation if m_model != 0
        SASSERT (m_ctx);
        SASSERT (m_ctx_fparams);
        flet<bool> _model(m_ctx_fparams->m_model, m_model != 0);
        
        if (m_in_level) assert_level_atoms(m_current_level);
        lbool result = maxsmt (hard_atoms, soft_atoms);
        if (result != l_false && m_model) m_ctx->get_model (*m_model);

        SASSERT (result != l_false || soft_atoms.empty ());

        /// compute level used in the core
        // XXX this is a poor approximation because the core will get minimized further
        if (result == l_false) {
            ptr_vector<expr> core;
            m_ctx->get_full_unsat_core (core);
            unsigned core_size = core.size ();
            m_uses_level = infty_level ();
            
            for (unsigned i = 0; i < core_size; ++i) {
                if (m_level_atoms_set.contains (core[i]))
                {
                    unsigned sz = std::min (m_uses_level, m_neg_level_atoms.size ());
                    for (unsigned j = 0; j < sz; ++j)
                        if (m_neg_level_atoms [j].get () == core[i])
                        {
                            m_uses_level = j;
                            break;
                        }
                    SASSERT (!is_infty_level (m_uses_level));
                }
            }
        }

//        if (result == l_false && m_core && m.proofs_enabled() && !m_subset_based_core) {
        if (result == l_false && m_core && m.proofs_enabled() && false) {
            TRACE ("spacer", tout << "theory core\n";);
            m_core->reset ();
            m_ctx->get_itp_core (*m_core);
        }
        else if (result == l_false && m_core) {
            arith_util util(m);
            expr_ref t_1(util.mk_int(1), m);
            expr_ref t_2(util.mk_int(2), m);
            expr_ref t_3(util.mk_int(3), m);
            expr_ref t_4(util.mk_add(util.mk_add(t_1, t_1), t_1),m);
            expr_ref t_5(util.mk_add(util.mk_add(t_3, t_3), t_3),m);
            expr_ref t_55(util.mk_add(util.mk_add(t_2, t_2), t_2),m);
            
            anti_unifier au(t_4,m);
            au.add_term(t_5);
            au.add_term(t_55);
            expr_ref t_6(m);
            obj_map<expr, ptr_vector<expr> > substitutions;
            au.finalize();
            t_6 = au.get_generalization();
            verbose_stream() << "generalization of AU: " << mk_pp(t_6,m) << "\n";

            for (unsigned i=0; i < au.get_num_substitutions(); ++i)
            {
                expr_ref_vector pinned(m);
                auto substitution = au.get_substitution(i);
                verbose_stream() << "\nsubstitution: ";
                for (const auto& pair : substitution)
                {
                    verbose_stream() << "(" << mk_pp(&pair.get_key(),m) << "," << mk_pp(pair.get_value(),m) << "),";
                }
                verbose_stream() << "\n";
            }
            
            expr_ref t_7(m);
            naive_convex_closure::compute_closure(au, m, t_7);
            verbose_stream() << "closure: " << mk_pp(t_7,m) << "\n";

            verbose_stream() << "yo " << mk_pp(t_4,m) << "\n";
            verbose_stream() << "yo " << mk_pp(t_5,m) << "\n";
            verbose_stream() << "yo " << mk_pp(t_55,m) << "\n";

            
            
            m_core->reset ();
            m_ctx->get_unsat_core (*m_core);
            // manually undo proxies because maxsmt() call above manually adds proxies
            m_ctx->undo_proxies (*m_core);
        }
        return result;
    }



    lbool prop_solver::check_assumptions (const expr_ref_vector & _hard,
                                          expr_ref_vector& soft,
                                          unsigned num_bg, expr * const * bg,
                                          unsigned solver_id) 
    {
        // current clients expect that flattening of HARD  is
        // done implicitly during check_assumptions
        expr_ref_vector hard (m);
        hard.append (_hard.size (), _hard.c_ptr ());
        flatten_and (hard);
      
        m_ctx = m_contexts [solver_id == 0 ? 0 : 0 /* 1 */].get ();
        m_ctx_fparams = m_fparams [solver_id == 0 ? 0 : 0 /* 1 */];
        
        // can be disabled if use_push_bg == true
        // solver::scoped_push _s_(*m_ctx);
        if (!m_use_push_bg) m_ctx->push ();
        itp_solver::scoped_bg _b_(*m_ctx);
        
        for (unsigned i = 0; i < num_bg; ++i)
            if (m_use_push_bg) m_ctx->push_bg (bg [i]);
            else m_ctx->assert_expr (bg[i]);
        
        unsigned soft_sz = soft.size ();
        lbool res = internal_check_assumptions (hard, soft);
        if (!m_use_push_bg) m_ctx->pop (1);
        
        TRACE ("psolve_verbose",
               tout << "sat: " << mk_pp (mk_and (hard), m) << "\n"
               << mk_pp (mk_and (soft), m) << "\n";
               for (unsigned i = 0; i < num_bg; ++i)
                   tout << "bg" << i << ": " << mk_pp(bg[i], m) << "\n";
               tout << "res: " << res << "\n";);
        CTRACE("psolve", m_core,
               tout << "core is: " << mk_pp (mk_and (*m_core), m) << "\n";);
        
        SASSERT (soft_sz >= soft.size ());
        
        // -- reset all parameters
        m_core = 0;
        m_model = 0;
        m_subset_based_core = false;
        return res;
    }

    void prop_solver::collect_statistics(statistics& st) const {
        m_contexts[0]->collect_statistics (st);
        m_contexts[1]->collect_statistics (st);
    }

    void prop_solver::reset_statistics() {
    }

    


}
