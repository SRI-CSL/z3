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

    spacer_legacy_mbp.cpp

Abstract:

   Legacy Model Based Projection. Used by Grigory Fedyukovich

Author:

    Arie Gurfinkel
    Anvesh Komuravelli
Notes:

--*/
#include <sstream>
#include "arith_simplifier_plugin.h"
#include "array_decl_plugin.h"
#include "ast_pp.h"
#include "basic_simplifier_plugin.h"
#include "bv_simplifier_plugin.h"
#include "bool_rewriter.h"
#include "dl_util.h"
#include "for_each_expr.h"
#include "smt_params.h"
#include "model.h"
#include "ref_vector.h"
#include "rewriter.h"
#include "rewriter_def.h"
#include "util.h"
#include "spacer_manager.h"
#include "spacer_util.h"
#include "arith_decl_plugin.h"
#include "expr_replacer.h"
#include "model_smt2_pp.h"
#include "scoped_proof.h"
#include "qe_lite.h"
#include "spacer_qe_project.h"
#include "model_pp.h"
#include "expr_safe_replace.h"

#include "datatype_decl_plugin.h"
#include "bv_decl_plugin.h"

#include "spacer_legacy_mev.h"

namespace spacer {
void qe_project(ast_manager& m, app_ref_vector& vars, expr_ref& fml, model_ref& M, expr_map& map)
{
    th_rewriter rw(m);
    // qe-lite; TODO: use qe_lite aggressively
    params_ref p;
    qe_lite qe(m, p, true);
    qe(vars, fml);
    rw(fml);

    TRACE("spacer",
          tout << "After qe_lite:\n";
          tout << mk_pp(fml, m) << "\n";
          tout << "Vars:\n";
    for (unsigned i = 0; i < vars.size(); ++i) {
    tout << mk_pp(vars.get(i), m) << "\n";
    }
         );

    // substitute model values for booleans and
    // use LW projection for arithmetic variables
    if (!vars.empty()) {
        app_ref_vector arith_vars(m);
        expr_substitution sub(m);
        proof_ref pr(m.mk_asserted(m.mk_true()), m);
        expr_ref bval(m);
        for (unsigned i = 0; i < vars.size(); i++) {
            if (m.is_bool(vars.get(i))) {
                // obtain the interpretation of the ith var using model completion
                VERIFY(M->eval(vars.get(i), bval, true));
                sub.insert(vars.get(i), bval, pr);
            } else {
                arith_vars.push_back(vars.get(i));
            }
        }
        if (!sub.empty()) {
            scoped_ptr<expr_replacer> rep = mk_expr_simp_replacer(m);
            rep->set_substitution(&sub);
            (*rep)(fml);
            rw(fml);
            TRACE("spacer",
                  tout << "Projected Boolean vars:\n" << mk_pp(fml, m) << "\n";
                 );
        }
        // model based projection
        if (!arith_vars.empty()) {
            TRACE("spacer",
                  tout << "Arith vars:\n";
            for (unsigned i = 0; i < arith_vars.size(); ++i) {
            tout << mk_pp(arith_vars.get(i), m) << "\n";
            }
                 );
            {
                scoped_no_proof _sp(m);
                qe::arith_project(*M, arith_vars, fml, map);
            }
            SASSERT(arith_vars.empty());
            TRACE("spacer",
                  tout << "Projected arith vars:\n" << mk_pp(fml, m) << "\n";
                 );
        }
        SASSERT(M->eval(fml, bval, true) && m.is_true(bval));    // M |= fml
        vars.reset();
        vars.append(arith_vars);
    }
}
}
