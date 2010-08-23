#ifndef SQUID_ADAPTATION__INITIATE_H
#define SQUID_ADAPTATION__INITIATE_H

#include "base/AsyncJob.h"
#include "base/CbcPointer.h"
#include "adaptation/forward.h"

class HttpMsg;

namespace Adaptation
{

/*
 * The  Initiate is a common base for  queries or transactions
 * initiated by an Initiator. This interface exists to allow an
 * initiator to signal its "initiatees" that it is aborting and no longer
 * expecting an answer. The class is also handy for implementing common
 * initiate actions such as maintaining and notifying the initiator.
 *
 * Initiate implementations must cbdata-protect themselves.
 *
 * This class could have been named Initiatee.
 */
class Initiate: virtual public AsyncJob
{

public:
    Initiate(const char *aTypeName);
    virtual ~Initiate();

    void initiator(const CbcPointer<Initiator> &i); ///< sets initiator

    // communication with the initiator
    virtual void noteInitiatorAborted() = 0;

protected:
    void sendAnswer(HttpMsg *msg); // send to the initiator
    void tellQueryAborted(bool final); // tell initiator
    void clearInitiator(); // used by noteInitiatorAborted; TODO: make private

    virtual void swanSong(); // internal cleanup

    virtual const char *status() const; // for debugging

    CbcPointer<Initiator> theInitiator;

private:
    Initiate(const Initiate &); // no definition
    Initiate &operator =(const Initiate &); // no definition
};

} // namespace Adaptation

#endif /* SQUID_ADAPTATION__INITIATE_H */
