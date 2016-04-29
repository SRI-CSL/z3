/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_generalizers.h

Abstract:

    Generalizer plugins.

Author:

    Nikolaj Bjorner (nbjorner) 2011-11-22.

Revision History:

--*/

#ifndef _SPACER_GENERALIZERS_H_
#define _SPACER_GENERALIZERS_H_

#include "spacer_context.h"
#include "arith_decl_plugin.h"

namespace spacer {

    class core_bool_inductive_generalizer : public core_generalizer {

        struct stats
        {
          unsigned count;
          stopwatch watch;
          stats () {reset ();}
          void reset () {count=0; watch.reset ();}
        };
          
        unsigned m_failure_limit;
        stats m_st;
      
    public:
      core_bool_inductive_generalizer(context& ctx, unsigned failure_limit) : core_generalizer(ctx), m_failure_limit(failure_limit) {}
        virtual ~core_bool_inductive_generalizer() {}
        virtual void operator()(model_node& n, expr_ref_vector& core, unsigned& uses_level);
      
      virtual void collect_statistics(statistics& st) const;
      virtual void reset_statistics () {m_st.reset ();}
    };

    template <typename T>
    class r_map : public map<rational, T, rational::hash_proc, rational::eq_proc> {
    };

  
  class core_array_eq_generalizer : public core_generalizer 
  {
  public:
    core_array_eq_generalizer (context &ctx) : core_generalizer (ctx) {} 
    virtual ~core_array_eq_generalizer () {}
    virtual void operator() (model_node& n, expr_ref_vector& core, unsigned &uses_level);
    
  };
  
    
};
#endif