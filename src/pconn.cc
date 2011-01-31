/*
 * $Id$
 *
 * DEBUG: section 48    Persistent Connections
 * AUTHOR: Duane Wessels
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */

#include "squid.h"
#include "comm.h"
#include "comm/Connection.h"
#include "fde.h"
#include "mgr/Registration.h"
#include "pconn.h"
#include "Store.h"

#define PCONN_FDS_SZ	8	/* pconn set size, increase for better memcache hit rate */

//TODO: re-attach to MemPools. WAS: static MemAllocator *pconn_fds_pool = NULL;
PconnModule * PconnModule::instance = NULL;
CBDATA_CLASS_INIT(IdleConnList);

/* ========== IdleConnList ============================================ */

IdleConnList::IdleConnList(const char *key, PconnPool *thePool) :
        capacity_(PCONN_FDS_SZ),
        size_(0),
        parent_(thePool)
{
    hash.key = xstrdup(key);
    theList_ = new Comm::ConnectionPointer[capacity_];
// TODO: re-attach to MemPools. WAS: theList = (?? *)pconn_fds_pool->alloc();
}

IdleConnList::~IdleConnList()
{
    parent_->unlinkList(this);

/* TODO: re-attach to MemPools.
    if (capacity_ == PCONN_FDS_SZ)
        pconn_fds_pool->freeOne(theList_);
    else
*/
    delete[] theList_;

    xfree(hash.key);
}

/** Search the list. Matches by FD socket number.
 * Performed from the end of list where newest entries are.
 *
 * \retval <0   The connection is not listed
 * \retval >=0  The connection array index
 */
int
IdleConnList::findIndexOf(const Comm::ConnectionPointer &conn) const
{
    for (int index = size_ - 1; index >= 0; --index) {
        if (conn->fd == theList_[index]->fd) {
            debugs(48, 3, HERE << "found " << conn << " at index " << index);
            return index;
        }
    }

    debugs(48, 2, HERE << conn << " NOT FOUND!");
    return -1;
}

/** Remove the entry at specified index.
 * \retval false The index is not an in-use entry.
 */
bool
IdleConnList::removeAt(int index)
{
    if (index < 0 || index >= size_)
        return false;

    // shuffle the remaining entries to fill the new gap.
    for (; index < size_ - 1; index++)
        theList_[index] = theList_[index + 1];
    theList_[size_-1] = NULL;

    if (--size_ == 0) {
        debugs(48, 3, HERE << "deleting " << hashKeyStr(&hash));
        delete this;
    }
    return true;
}

void
IdleConnList::clearHandlers(const Comm::ConnectionPointer &conn)
{
    debugs(48, 3, HERE << "removing close handler for " << conn);
    comm_read_cancel(conn->fd, IdleConnList::Read, this);
    commUnsetConnTimeout(conn);
}

void
IdleConnList::push(const Comm::ConnectionPointer &conn)
{
    if (size_ == capacity_) {
        debugs(48, 3, HERE << "growing idle Connection array");
        capacity_ <<= 1;
        const Comm::ConnectionPointer *oldList = theList_;
        theList_ = new Comm::ConnectionPointer[capacity_];
        for (int index = 0; index < size_; index++)
            theList_[index] = oldList[index];

/* TODO: re-attach to MemPools.
        if (size_ == PCONN_FDS_SZ)
            pconn_fds_pool->freeOne(oldList);
        else
*/
        delete[] oldList;
    }

    theList_[size_++] = conn;
    AsyncCall::Pointer readCall = commCbCall(5,4, "IdleConnList::Read",
                                             CommIoCbPtrFun(IdleConnList::Read, this));
    comm_read(conn, fakeReadBuf_, sizeof(fakeReadBuf_), readCall);
    AsyncCall::Pointer timeoutCall = commCbCall(5,4, "IdleConnList::Read",
                                                CommTimeoutCbPtrFun(IdleConnList::Timeout, this));
    commSetConnTimeout(conn, Config.Timeout.pconn, timeoutCall);
}

/*
 * XXX this routine isn't terribly efficient - if there's a pending
 * read event (which signifies the fd will close in the next IO loop!)
 * we ignore the FD and move onto the next one. This means, as an example,
 * if we have a lot of FDs open to a very popular server and we get a bunch
 * of requests JUST as they timeout (say, it shuts down) we'll be wasting
 * quite a bit of CPU. Just keep it in mind.
 */
Comm::ConnectionPointer
IdleConnList::findUseable(const Comm::ConnectionPointer &key)
{
    assert(size_);

    // small optimization: do the constant bool tests only once.
    const bool keyCheckAddr = !key->local.IsAnyAddr();
    const bool keyCheckPort = key->local.GetPort() > 0;

    for (int i=size_-1; i>=0; i--) {

        // Is the FD pending completion of the closure callback?
        // this flag is set while our early-read/close handler is
        // waiting for a remote response. It gets unset when the
        // handler is scheduled.
        if (!fd_table[theList_[i]->fd].flags.read_pending)
            continue;

        // connection already closed. useless.
        if (!Comm::IsConnOpen(theList_[i]))
            continue;

        // local end port is required, but dont match.
        if (keyCheckPort && key->local.GetPort() != theList_[i]->local.GetPort())
            continue;

        // local address is required, but does not match.
        if (keyCheckAddr && key->local.matchIPAddr(theList_[i]->local) != 0)
            continue;

        // finally, a match. pop and return it.
        Comm::ConnectionPointer result = theList_[i];
        /* may delete this */
        removeAt(i);
        return result;
    }

    return Comm::ConnectionPointer();
}

void
IdleConnList::Read(const Comm::ConnectionPointer &conn, char *buf, size_t len, comm_err_t flag, int xerrno, void *data)
{
    debugs(48, 3, HERE << len << " bytes from " << conn);

    if (flag == COMM_ERR_CLOSING) {
        /* Bail out early on COMM_ERR_CLOSING - close handlers will tidy up for us */
        return;
    }

    IdleConnList *list = (IdleConnList *) data;
    int index = list->findIndexOf(conn);
    if (index >= 0) {
        /* might delete list */
        list->removeAt(index);
        list->clearHandlers(conn);
    }
    // else we lost a race.
    // Somebody started using the pconn since the remote end disconnected.
    // pass the closure info on!
    conn->close();
}

void
IdleConnList::Timeout(const CommTimeoutCbParams &io)
{
    debugs(48, 3, HERE << io.conn);
    IdleConnList *list = static_cast<IdleConnList *>(io.data);
    int index = list->findIndexOf(io.conn);
    if (index >= 0) {
        /* might delete list */
        list->removeAt(index);
        io.conn->close();
    }
}

/* ========== PconnPool PRIVATE FUNCTIONS ============================================ */

const char *
PconnPool::key(const Comm::ConnectionPointer &destLink, const char *domain)
{
    LOCAL_ARRAY(char, buf, SQUIDHOSTNAMELEN * 3 + 10);

    destLink->remote.ToURL(buf, SQUIDHOSTNAMELEN * 3 + 10);
    if (domain) {
        const int used = strlen(buf);
        snprintf(buf+used, SQUIDHOSTNAMELEN * 3 + 10-used, "/%s", domain);
    }

    debugs(48,6,"PconnPool::key(" << destLink << ", " << (domain?domain:"[no domain]") << ") is {" << buf << "}" );
    return buf;
}

void
PconnPool::dumpHist(StoreEntry * e) const
{
    storeAppendPrintf(e,
                      "%s persistent connection counts:\n"
                      "\n"
                      "\treq/\n"
                      "\tconn      count\n"
                      "\t----  ---------\n",
                      descr);

    for (int i = 0; i < PCONN_HIST_SZ; i++) {
        if (hist[i] == 0)
            continue;

        storeAppendPrintf(e, "\t%4d  %9d\n", i, hist[i]);
    }
}

void
PconnPool::dumpHash(StoreEntry *e) const
{
    hash_table *hid = table;
    hash_first(hid);

    int i = 0;
    for (hash_link *walker = hid->next; walker; walker = hash_next(hid)) {
        storeAppendPrintf(e, "\t item %5d: %s\n", i++, (char *)(walker->key));
    }
}

/* ========== PconnPool PUBLIC FUNCTIONS ============================================ */

PconnPool::PconnPool(const char *aDescr) : table(NULL), descr(aDescr)
{
    table = hash_create((HASHCMP *) strcmp, 229, hash_string);

    for (int i = 0; i < PCONN_HIST_SZ; i++)
        hist[i] = 0;

    PconnModule::GetInstance()->add(this);
}

PconnPool::~PconnPool()
{
    descr = NULL;
    hashFreeMemory(table);
}

void
PconnPool::push(const Comm::ConnectionPointer &conn, const char *domain)
{
    if (fdUsageHigh()) {
        debugs(48, 3, HERE << "Not many unused FDs");
        conn->close();
        return;
    } else if (shutting_down) {
        conn->close();
        debugs(48, 3, HERE << "Squid is shutting down. Refusing to do anything");
        return;
    }

    const char *aKey = key(conn, domain);
    IdleConnList *list = (IdleConnList *) hash_lookup(table, aKey);

    if (list == NULL) {
        list = new IdleConnList(aKey, this);
        debugs(48, 3, HERE << "new IdleConnList for {" << hashKeyStr(&list->hash) << "}" );
        hash_join(table, &list->hash);
    } else {
        debugs(48, 3, HERE << "found IdleConnList for {" << hashKeyStr(&list->hash) << "}" );
    }

    list->push(conn);
    assert(!comm_has_incomplete_write(conn->fd));

    LOCAL_ARRAY(char, desc, FD_DESC_SZ);
    snprintf(desc, FD_DESC_SZ, "Idle: %s", aKey);
    fd_note(conn->fd, desc);
    debugs(48, 3, HERE << "pushed " << conn << " for " << aKey);
}

Comm::ConnectionPointer
PconnPool::pop(const Comm::ConnectionPointer &destLink, const char *domain, bool isRetriable)
{
    const char * aKey = key(destLink, domain);

    IdleConnList *list = (IdleConnList *)hash_lookup(table, aKey);
    if (list == NULL) {
        debugs(48, 3, HERE << "lookup for key {" << aKey << "} failed.");
        return Comm::ConnectionPointer();
    } else {
        debugs(48, 3, HERE << "found " << hashKeyStr(&list->hash) << (isRetriable?"(to use)":"(to kill)") );
    }

    /* may delete list */
    Comm::ConnectionPointer temp = list->findUseable(destLink);
    if (!isRetriable && Comm::IsConnOpen(temp))
        temp->close();

    return temp;
}

void
PconnPool::unlinkList(IdleConnList *list) const
{
    hash_remove_link(table, &list->hash);
}

void
PconnPool::count(int uses)
{
    if (uses >= PCONN_HIST_SZ)
        uses = PCONN_HIST_SZ - 1;

    hist[uses]++;
}

/* ========== PconnModule ============================================ */

/*
 * This simple class exists only for the cache manager
 */

PconnModule::PconnModule() : pools(NULL), poolCount(0)
{
    pools = (PconnPool **) xcalloc(MAX_NUM_PCONN_POOLS, sizeof(*pools));
//TODO: re-link to MemPools. WAS:    pconn_fds_pool = memPoolCreate("pconn_fds", PCONN_FDS_SZ * sizeof(int));
    debugs(48, 0, "persistent connection module initialized");
    registerWithCacheManager();
}

PconnModule *
PconnModule::GetInstance()
{
    if (instance == NULL)
        instance = new PconnModule;

    return instance;
}

void
PconnModule::registerWithCacheManager(void)
{
    Mgr::RegisterAction("pconn",
                        "Persistent Connection Utilization Histograms",
                        DumpWrapper, 0, 1);
}

void
PconnModule::add(PconnPool *aPool)
{
    assert(poolCount < MAX_NUM_PCONN_POOLS);
    *(pools+poolCount) = aPool;
    poolCount++;
}

void
PconnModule::dump(StoreEntry *e)
{
    for (int i = 0; i < poolCount; i++) {
        storeAppendPrintf(e, "\n Pool %d Stats\n", i);
        (*(pools+i))->dumpHist(e);
        storeAppendPrintf(e, "\n Pool %d Hash Table\n",i);
        (*(pools+i))->dumpHash(e);
    }
}

void
PconnModule::DumpWrapper(StoreEntry *e)
{
    PconnModule::GetInstance()->dump(e);
}
