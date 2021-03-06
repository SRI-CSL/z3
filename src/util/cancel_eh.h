/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    cancel_eh.h

Abstract:

    Template for implementing simple event handler that just invokes cancel method.

Author:

    Leonardo de Moura (leonardo) 2011-04-27.

Revision History:

--*/
#ifndef CANCEL_EH_H_
#define CANCEL_EH_H_

#include "util/event_handler.h"

/**
   \brief Generic event handler for invoking cancel method.
*/
template<typename T>
class cancel_eh : public event_handler {
    bool m_canceled;
    T & m_obj;
public:
    cancel_eh(T & o): m_canceled(false), m_obj(o) {}
    ~cancel_eh() { if (m_canceled) m_obj.dec_cancel(); }
    virtual void operator()() { 
        m_canceled = true;
        m_obj.inc_cancel(); 
    }
};

#endif
