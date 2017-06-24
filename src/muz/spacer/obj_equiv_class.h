/*++

Module Name:

    obj_equiv_class.h

Abstract:
  "Equivalence class structure" for objs. Uses a union_find structure internally.
  Operations are :
  -Declare a new equivalence class with a single element
  -Merge two equivalence classes
  -Retrieve whether two elements are in the same equivalence class
  -Iterate on all the elements of the equivalence class of a given element
  -Iterate on all equivalence classes (and then within them)

Author:

    Julien Braine

Revision History:

*/

#ifndef OBJ_EQUIV_CLASS_H_
#define OBJ_EQUIV_CLASS_H_

#include "../util/union_find.h"
#include "../ast/ast_util.h"

//All functions naturally add their parameters to the union_find class
template<typename OBJ, typename Manager>
class obj_equiv_class {
    basic_union_find uf;
    obj_map<OBJ, unsigned> to_int;
    ref_vector<OBJ, Manager> to_obj;

    unsigned add_elem_impl(OBJ*o)
    {
        unsigned id = to_obj.size();
        to_int.insert(o, id);
        to_obj.push_back(o);
        return id;
    }
    unsigned add_if_not_there(OBJ*o)
    {
        unsigned id;
        if(!to_int.find(o, id)) {
            id = add_elem_impl(o);
        }
        return id;
    }

public:
    class iterator;
    class equiv_iterator;
    friend class iterator;
    friend class equiv_iterator;

    obj_equiv_class(Manager& m) : to_obj(m) {}

    void add_elem(OBJ*o)
    {
        SASSERT(!to_int.find(o));
        add_elem_impl(o);
    }

    //Invalidates all iterators
    void merge(OBJ* a, OBJ* b)
    {
        unsigned v1 = add_if_not_there(a);
        unsigned v2 = add_if_not_there(b);
        unsigned tmp1 = uf.find(v1);
        unsigned tmp2 = uf.find(v2);
        uf.merge(tmp1, tmp2);
    }

    void reset()
    {
        uf.reset();
        to_int.reset();
        to_obj.reset();
    }

    bool are_equiv(OBJ*a, OBJ*b)
    {
        unsigned id1 = add_if_not_there(a);
        unsigned id2 = add_if_not_there(b);
        return uf.find(id1) == uf.find(id2);
    }

    class iterator {
        friend class obj_equiv_class;
    private :
        const obj_equiv_class& ouf;
        unsigned curr_id;
        bool first;
        iterator(const obj_equiv_class& uf, unsigned id, bool f) : ouf(uf), curr_id(id), first(f) {}
    public :
        OBJ*operator*()
        {
            return ouf.to_obj[curr_id];
        }
        iterator& operator++()
        {
            curr_id = ouf.uf.next(curr_id);
            first = false;
            return *this;
        }
        bool operator==(const iterator& o)
        {
            SASSERT(&ouf == &o.ouf);
            return  first == o.first && curr_id == o.curr_id;
        }
        bool operator!=(const iterator& o)
        {
            return !(*this == o);
        }
    };

    iterator begin(OBJ*o)
    {
        unsigned id = add_if_not_there(o);
        return iterator(*this, id, true);
    }
    iterator end(OBJ*o)
    {
        unsigned id = add_if_not_there(o);
        return iterator(*this, id, false);
    }

    class eq_class {
    private :
        iterator beg;
        iterator e;
    public :
        eq_class(const iterator& a, const iterator& b) : beg(a), e(b) {}
        iterator begin() {return beg;}
        iterator end() {return e;}
    };

    class equiv_iterator {
        friend class obj_equiv_class;
    private :
        const obj_equiv_class& ouf;
        unsigned rootnb;
        equiv_iterator(const obj_equiv_class& uf, unsigned nb) : ouf(uf), rootnb(nb)
        {
            while(rootnb != ouf.to_obj.size() && ouf.uf.is_root(rootnb) != true)
            { rootnb++; }
        }
    public :
        eq_class operator*() {return eq_class(iterator(ouf, rootnb, true), iterator(ouf, rootnb, false));}
        equiv_iterator& operator++()
        {
            do {
                rootnb++;
            } while(rootnb != ouf.to_obj.size() && ouf.uf.is_root(rootnb) != true);
            return *this;
        }
        bool operator==(const equiv_iterator& o)
        {
            SASSERT(&ouf == &o.ouf);
            return rootnb == o.rootnb;
        }
        bool operator!=(const equiv_iterator& o)
        {
            return !(*this == o);
        }
    };

    equiv_iterator begin()
    {
        return equiv_iterator(*this, 0);
    }
    equiv_iterator end()
    {
        return equiv_iterator(*this, to_obj.size());
    }
};

typedef obj_equiv_class<expr, ast_manager> expr_equiv_class;


#endif

