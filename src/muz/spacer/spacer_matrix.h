#ifndef _SPACER_MATRIX_H_
#define _SPACER_MATRIX_H_

#include "ast.h"

namespace spacer {
    
    class spacer_matrix {
    public:
        spacer_matrix(unsigned m, unsigned n); // m rows, n colums
        
        unsigned num_rows();
        unsigned num_cols();
        
        rational get(unsigned i, unsigned j);
        void set(unsigned i, unsigned j, rational v);
        
        unsigned perform_gaussian_elimination();
        
        void print_matrix();
    private:
        unsigned m_num_rows;
        unsigned m_num_cols;
        vector<vector<rational>> m_matrix;
    };
}

#endif
