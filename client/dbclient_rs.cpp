// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "dbclient.h"
#include "../bson/util/builder.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include "../db/dbmessage.h"
#include "connpool.h"
#include "dbclient_rs.h"
#include "../util/background.h"

namespace mongo {

    // --------------------------------
    // ----- ReplicaSetMonitor ---------
    // --------------------------------

    // global background job responsible for checking every X amount of time
    class ReplicaSetMonitorWatcher : public BackgroundJob {
    public:
        ReplicaSetMonitorWatcher() : _safego("ReplicaSetMonitorWatcher::_safego") , _started(false) {}

        virtual string name() const { return "ReplicaSetMonitorWatcher"; }
        
        void safeGo() {
            // check outside of lock for speed
            if ( _started )
                return;
            
            scoped_lock lk( _safego );
            if ( _started )
                return;
            _started = true;

            go();
        }
    protected:
        void run() {
            log() << "starting" << endl;
            while ( ! inShutdown() ) {
                sleepsecs( 10 );
                try {
                    ReplicaSetMonitor::checkAll( true );
                }
                catch ( std::exception& e ) {
                    error() << "check failed: " << e.what() << endl;
                }
                catch ( ... ) {
                    error() << "unkown error" << endl;
                }
            }
        }

        mongo::mutex _safego;
        bool _started;

    } replicaSetMonitorWatcher;


    ReplicaSetMonitor::ReplicaSetMonitor( const string& name , const vector<HostAndPort>& servers )
        : _lock( "ReplicaSetMonitor instance" ) , _checkConnectionLock( "ReplicaSetMonitor check connection lock" ), _name( name ) , _master(-1), _nextSlave(0) {
        
        uassert( 13642 , "need at least 1 node for a replica set" , servers.size() > 0 );

        if ( _name.size() == 0 ) {
            warning() << "replica set name empty, first node: " << servers[0] << endl;
        }

        string errmsg;

        for ( unsigned i=0; i<servers.size(); i++ ) {

            bool haveAlready = false;
            for ( unsigned n = 0; n < _nodes.size() && ! haveAlready; n++ )
                haveAlready = ( _nodes[n].addr == servers[i] );
            if( haveAlready ) continue;

            auto_ptr<DBClientConnection> conn( new DBClientConnection( true , 0, 5.0 ) );
            if (!conn->connect( servers[i] , errmsg ) ) {
                log(1) << "error connecting to seed " << servers[i] << ": " << errmsg << endl;
                // skip seeds that don't work
                continue;
            }

            _nodes.push_back( Node( servers[i] , conn.release() ) );
            
            int myLoc = _nodes.size() - 1;
            string maybePrimary;
            _checkConnection( _nodes[myLoc].conn.get() , maybePrimary, false, myLoc );
        }
    }

    ReplicaSetMonitor::~ReplicaSetMonitor() {
        _nodes.clear();
        _master = -1;
    }

    ReplicaSetMonitorPtr ReplicaSetMonitor::get( const string& name , const vector<HostAndPort>& servers ) {
        scoped_lock lk( _setsLock );
        ReplicaSetMonitorPtr& m = _sets[name];
        if ( ! m )
            m.reset( new ReplicaSetMonitor( name , servers ) );

        replicaSetMonitorWatcher.safeGo();

        return m;
    }

    ReplicaSetMonitorPtr ReplicaSetMonitor::get( const string& name ) {
        scoped_lock lk( _setsLock );
        map<string,ReplicaSetMonitorPtr>::const_iterator i = _sets.find( name );
        if ( i == _sets.end() ) 
            return ReplicaSetMonitorPtr();
        return i->second;
    }


    void ReplicaSetMonitor::checkAll( bool checkAllSecondaries ) {
        set<string> seen;

        while ( true ) {
            ReplicaSetMonitorPtr m;
            {
                scoped_lock lk( _setsLock );
                for ( map<string,ReplicaSetMonitorPtr>::iterator i=_sets.begin(); i!=_sets.end(); ++i ) {
                    string name = i->first;
                    if ( seen.count( name ) )
                        continue;
                    LOG(1) << "checking replica set: " << name << endl;
                    seen.insert( name );
                    m = i->second;
                    break;
                }
            }

            if ( ! m )
                break;

            m->check( checkAllSecondaries );
        }


    }

    void ReplicaSetMonitor::setConfigChangeHook( ConfigChangeHook hook ) {
        massert( 13610 , "ConfigChangeHook already specified" , _hook == 0 );
        _hook = hook;
    }
    
    string ReplicaSetMonitor::getServerAddress() const {
        StringBuilder ss;
        if ( _name.size() )
            ss << _name << "/";

        {
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                if ( i > 0 )
                    ss << ",";
                ss << _nodes[i].addr.toString();
            }
        }
        return ss.str();
    }

    bool ReplicaSetMonitor::contains( const string& server ) const {
        scoped_lock lk( _lock );
        for ( unsigned i=0; i<_nodes.size(); i++ ) {
            if ( _nodes[i].addr == server )
                return true;
        }
        return false;
    }
    

    void ReplicaSetMonitor::notifyFailure( const HostAndPort& server ) {
        scoped_lock lk( _lock );
        if ( _master >= 0 && _master < (int)_nodes.size() ) {
            if ( server == _nodes[_master].addr ) {
                _nodes[_master].ok = false; 
                _master = -1;
            }
        }
    }



    HostAndPort ReplicaSetMonitor::getMaster() {
        {
            scoped_lock lk( _lock );
            if ( _master >= 0 && _nodes[_master].ok )
                return _nodes[_master].addr;
        }
        
        _check( false );

        scoped_lock lk( _lock );
        uassert( 10009 , str::stream() << "ReplicaSetMonitor no master found for set: " << _name , _master >= 0 );
        return _nodes[_master].addr;
    }
    
    HostAndPort ReplicaSetMonitor::getSlave( const HostAndPort& prev ) {
        // make sure its valid

        bool wasFound = false;

        // This is always true, since checked in port()
        assert( prev.port() >= 0 );
        if( prev.host().size() ){
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                if ( prev != _nodes[i].addr ) 
                    continue;

                wasFound = true;

                if ( _nodes[i].okForSecondaryQueries() )
                    return prev;

                break;
            }
        }
        
        if( prev.host().size() ){
            if( wasFound ){ LOG(1) << "slave '" << prev << "' is no longer ok to use" << endl; }
            else{ LOG(1) << "slave '" << prev << "' was not found in the replica set" << endl; }
        }
        else LOG(1) << "slave '" << prev << "' is not initialized or invalid" << endl;

        return getSlave();
    }

    HostAndPort ReplicaSetMonitor::getSlave() {

        LOG(2) << "selecting new slave from replica set " << getServerAddress() << endl;

        // Logic is to retry three times for any secondary node, if we can't find any secondary, we'll take
        // any "ok" node
        // TODO: Could this query hidden nodes?
        const int MAX = 3;
        for ( int xxx=0; xxx<MAX; xxx++ ) {

            {
                scoped_lock lk( _lock );
                
                unsigned i = 0;
                for ( ; i<_nodes.size(); i++ ) {
                    _nextSlave = ( _nextSlave + 1 ) % _nodes.size();
                    if ( _nextSlave == _master ){
                        LOG(2) << "not selecting " << _nodes[_nextSlave] << " as it is the current master" << endl;
                        continue;
                    }
                    if ( _nodes[ _nextSlave ].okForSecondaryQueries() || ( _nodes[ _nextSlave ].ok && ( xxx + 1 ) >= MAX ) )
                        return _nodes[ _nextSlave ].addr;
                    
                    LOG(2) << "not selecting " << _nodes[_nextSlave] << " as it is not ok to use" << endl;
                }
                
            }

            check(false);
        }
        
        LOG(2) << "no suitable slave nodes found, returning default node " << _nodes[ 0 ] << endl;

        return _nodes[0].addr;
    }

    /**
     * notify the monitor that server has faild
     */
    void ReplicaSetMonitor::notifySlaveFailure( const HostAndPort& server ) {
        int x = _find( server );
        if ( x >= 0 ) {
            scoped_lock lk( _lock );
            _nodes[x].ok = false;
        }
    }

    void ReplicaSetMonitor::_checkStatus(DBClientConnection *conn) {
        BSONObj status;

        if (!conn->runCommand("admin", BSON("replSetGetStatus" << 1), status) ||
                !status.hasField("members") ||
                status["members"].type() != Array) {
            return;
        }

        BSONObjIterator hi(status["members"].Obj());
        while (hi.more()) {
            BSONObj member = hi.next().Obj();
            string host = member["name"].String();

            int m = -1;
            if ((m = _find(host)) < 0) {
                continue;
            }

            double state = member["state"].Number();
            if (member["health"].Number() == 1 && (state == 1 || state == 2)) {
                scoped_lock lk( _lock );
                _nodes[m].ok = true;
            }
            else {
                scoped_lock lk( _lock );
                _nodes[m].ok = false;
            }
        }
    }

    void ReplicaSetMonitor::_checkHosts( const BSONObj& hostList, bool& changed ) {
        BSONObjIterator hi(hostList);
        while ( hi.more() ) {
            string toCheck = hi.next().String();

            if ( _find( toCheck ) >= 0 )
                continue;

            HostAndPort h( toCheck );
            DBClientConnection * newConn = new DBClientConnection( true, 0, 5.0 );
            string temp;
            newConn->connect( h , temp );
            {
                scoped_lock lk( _lock );
                if ( _find_inlock( toCheck ) >= 0 ) {
                    // we need this check inside the lock so there isn't thread contention on adding to vector
                    continue;
                }
                _nodes.push_back( Node( h , newConn ) );
            }
            log() << "updated set (" << _name << ") to: " << getServerAddress() << endl;
            changed = true;
        }
    }
    
    

    bool ReplicaSetMonitor::_checkConnection( DBClientConnection * c , string& maybePrimary , bool verbose , int nodesOffset ) {
        scoped_lock lk( _checkConnectionLock );
        bool isMaster = false;
        bool changed = false;
        try {
            Timer t;
            BSONObj o;
            c->isMaster(isMaster, &o);
            
            if ( o["setName"].type() != String || o["setName"].String() != _name ) {
                warning() << "node: " << c->getServerAddress() << " isn't a part of set: " << _name 
                          << " ismaster: " << o << endl;
                if ( nodesOffset >= 0 )
                    _nodes[nodesOffset].ok = false;
                return false;
            }

            if ( nodesOffset >= 0 ) {
                _nodes[nodesOffset].pingTimeMillis = t.millis();
                _nodes[nodesOffset].hidden = o["hidden"].trueValue();
                _nodes[nodesOffset].secondary = o["secondary"].trueValue();
                _nodes[nodesOffset].ismaster = o["ismaster"].trueValue();

                _nodes[nodesOffset].lastIsMaster = o.copy();
            }

            log( ! verbose ) << "ReplicaSetMonitor::_checkConnection: " << c->toString() << ' ' << o << endl;
            
            // add other nodes
            if ( o["hosts"].type() == Array ) {
                if ( o["primary"].type() == String )
                    maybePrimary = o["primary"].String();

                _checkHosts(o["hosts"].Obj(), changed);
            }
            if (o.hasField("passives") && o["passives"].type() == Array) {
                _checkHosts(o["passives"].Obj(), changed);
            }
            
            _checkStatus(c);

            
        }
        catch ( std::exception& e ) {
            log( ! verbose ) << "ReplicaSetMonitor::_checkConnection: caught exception " << c->toString() << ' ' << e.what() << endl;
            _nodes[nodesOffset].ok = false;
        }

        if ( changed && _hook )
            _hook( this );

        return isMaster;
    }

    void ReplicaSetMonitor::_check( bool checkAllSecondaries ) {

        bool triedQuickCheck = false;

        LOG(1) <<  "_check : " << getServerAddress() << endl;

        int newMaster = -1;
        
        for ( int retry = 0; retry < 2; retry++ ) {
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                shared_ptr<DBClientConnection> c;
                {
                    scoped_lock lk( _lock );
                    c = _nodes[i].conn;
                }

                string maybePrimary;
                if ( _checkConnection( c.get() , maybePrimary , retry , i ) ) {
                    _master = i;
                    newMaster = i;
                    if ( ! checkAllSecondaries )
                        return;
                }

                if ( ! triedQuickCheck && maybePrimary.size() ) {
                    int x = _find( maybePrimary );
                    if ( x >= 0 ) {
                        triedQuickCheck = true;
                        string dummy;
                        shared_ptr<DBClientConnection> testConn;
                        {
                            scoped_lock lk( _lock );
                            testConn = _nodes[x].conn;
                        }
                        if ( _checkConnection( testConn.get() , dummy , false , x ) ) {
                            _master = x;
                            newMaster = x;
                            if ( ! checkAllSecondaries )
                                return;
                        }
                    }
                }

            }
            
            if ( newMaster >= 0 )
                return;

            sleepsecs(1);
        }

    }

    void ReplicaSetMonitor::check( bool checkAllSecondaries ) {
        // first see if the current master is fine
        if ( _master >= 0 ) {
            string temp;
            if ( _checkConnection( _nodes[_master].conn.get() , temp , false , _master ) ) {
                if ( ! checkAllSecondaries ) {
                    // current master is fine, so we're done
                    return;
                }
            }
        }

        // we either have no master, or the current is dead
        _check( checkAllSecondaries );
    }

    int ReplicaSetMonitor::_find( const string& server ) const {
        scoped_lock lk( _lock );
        return _find_inlock( server );
    }

    int ReplicaSetMonitor::_find_inlock( const string& server ) const {
        for ( unsigned i=0; i<_nodes.size(); i++ )
            if ( _nodes[i].addr == server )
                return i;
        return -1;
    }


    int ReplicaSetMonitor::_find( const HostAndPort& server ) const {
        scoped_lock lk( _lock );
        for ( unsigned i=0; i<_nodes.size(); i++ )
            if ( _nodes[i].addr == server )
                return i;
        return -1;
    }
    
    void ReplicaSetMonitor::appendInfo( BSONObjBuilder& b ) const {
        scoped_lock lk( _lock );
        BSONArrayBuilder hosts( b.subarrayStart( "hosts" ) );
        for ( unsigned i=0; i<_nodes.size(); i++ ) {
            hosts.append( BSON( "addr" << _nodes[i].addr <<
                                // "lastIsMaster" << _nodes[i].lastIsMaster << // this is a potential race, so only used when debugging
                                "ok" << _nodes[i].ok <<
                                "ismaster" << _nodes[i].ismaster <<
                                "hidden" << _nodes[i].hidden <<
                                "secondary" << _nodes[i].secondary <<
                                "pingTimeMillis" << _nodes[i].pingTimeMillis  ) );
            
        }
        hosts.done();
        
        b.append( "master" , _master );
        b.append( "nextSlave" , _nextSlave );
    }
    

    mongo::mutex ReplicaSetMonitor::_setsLock( "ReplicaSetMonitor" );
    map<string,ReplicaSetMonitorPtr> ReplicaSetMonitor::_sets;
    ReplicaSetMonitor::ConfigChangeHook ReplicaSetMonitor::_hook;
    // --------------------------------
    // ----- DBClientReplicaSet ---------
    // --------------------------------

    DBClientReplicaSet::DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers, double so_timeout )
        : _monitor( ReplicaSetMonitor::get( name , servers ) ),
          _so_timeout( so_timeout ) {
    }

    DBClientReplicaSet::~DBClientReplicaSet() {
    }

    DBClientConnection * DBClientReplicaSet::checkMaster() {
        HostAndPort h = _monitor->getMaster();

        if ( h == _masterHost && _master ) {
            // a master is selected.  let's just make sure connection didn't die
            if ( ! _master->isFailed() )
                return _master.get();
            _monitor->notifyFailure( _masterHost );
        }

        _masterHost = _monitor->getMaster();
        _master.reset( new DBClientConnection( true , this , _so_timeout ) );
        string errmsg;
        if ( ! _master->connect( _masterHost , errmsg ) ) {
            _monitor->notifyFailure( _masterHost );
            uasserted( 13639 , str::stream() << "can't connect to new replica set master [" << _masterHost.toString() << "] err: " << errmsg );
        }
        _auth( _master.get() );
        return _master.get();
    }

    DBClientConnection * DBClientReplicaSet::checkSlave() {
        HostAndPort h = _monitor->getSlave( _slaveHost );

        if ( h == _slaveHost && _slave ) {
            if ( ! _slave->isFailed() )
                return _slave.get();
            _monitor->notifySlaveFailure( _slaveHost );
            _slaveHost = _monitor->getSlave();
        } 
        else {
            _slaveHost = h;
        }

        _slave.reset( new DBClientConnection( true , this , _so_timeout ) );
        _slave->connect( _slaveHost );
        _auth( _slave.get() );
        return _slave.get();
    }


    void DBClientReplicaSet::_auth( DBClientConnection * conn ) {
        for ( list<AuthInfo>::iterator i=_auths.begin(); i!=_auths.end(); ++i ) {
            const AuthInfo& a = *i;
            string errmsg;
            if ( ! conn->auth( a.dbname , a.username , a.pwd , errmsg, a.digestPassword ) )
                warning() << "cached auth failed for set: " << _monitor->getName() << " db: " << a.dbname << " user: " << a.username << endl;

        }

    }

    DBClientConnection& DBClientReplicaSet::masterConn() {
        return *checkMaster();
    }

    DBClientConnection& DBClientReplicaSet::slaveConn() {
        return *checkSlave();
    }

    bool DBClientReplicaSet::connect() {
        try {
            checkMaster();
        }
        catch (AssertionException&) {
            if (_master && _monitor) {
                _monitor->notifyFailure(_masterHost);
            }
            return false;
        }
        return true;
    }

    bool DBClientReplicaSet::auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword ) {
        DBClientConnection * m = checkMaster();

        // first make sure it actually works
        if( ! m->auth(dbname, username, pwd, errmsg, digestPassword ) )
            return false;

        // now that it does, we should save so that for a new node we can auth
        _auths.push_back( AuthInfo( dbname , username , pwd , digestPassword ) );
        return true;
    }

    // ------------- simple functions -----------------

    void DBClientReplicaSet::insert( const string &ns , BSONObj obj , int flags) {
        checkMaster()->insert(ns, obj, flags);
    }

    void DBClientReplicaSet::insert( const string &ns, const vector< BSONObj >& v , int flags) {
        checkMaster()->insert(ns, v, flags);
    }

    void DBClientReplicaSet::remove( const string &ns , Query obj , bool justOne ) {
        checkMaster()->remove(ns, obj, justOne);
    }

    void DBClientReplicaSet::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ) {
        return checkMaster()->update(ns, query, obj, upsert,multi);
    }

    auto_ptr<DBClientCursor> DBClientReplicaSet::query(const string &ns, Query query, int nToReturn, int nToSkip,
            const BSONObj *fieldsToReturn, int queryOptions, int batchSize) {

        if ( queryOptions & QueryOption_SlaveOk ) {
            // we're ok sending to a slave
            // we'll try 2 slaves before just using master
            // checkSlave will try a different slave automatically after a failure
            for ( int i=0; i<3; i++ ) {
                try {
                    return checkSlaveQueryResult( checkSlave()->query(ns,query,nToReturn,nToSkip,fieldsToReturn,queryOptions,batchSize) );
                }
                catch ( DBException &e ) {
                    LOG(1) << "can't query replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                }
            }
        }

        return checkMaster()->query(ns,query,nToReturn,nToSkip,fieldsToReturn,queryOptions,batchSize);
    }

    BSONObj DBClientReplicaSet::findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions) {
        if ( queryOptions & QueryOption_SlaveOk ) {
            // we're ok sending to a slave
            // we'll try 2 slaves before just using master
            // checkSlave will try a different slave automatically after a failure
            for ( int i=0; i<3; i++ ) {
                try {
                    return checkSlave()->findOne(ns,query,fieldsToReturn,queryOptions);
                }
                catch ( DBException &e ) {
                	LOG(1) << "can't findone replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                }
            }
        }

        return checkMaster()->findOne(ns,query,fieldsToReturn,queryOptions);
    }

    void DBClientReplicaSet::killCursor( long long cursorID ) {
        // we should neve call killCursor on a replica set conncetion
        // since we don't know which server it belongs to
        // can't assume master because of slave ok
        // and can have a cursor survive a master change
        assert(0);
    }

    void DBClientReplicaSet::isntMaster() { 
        log() << "got not master for: " << _masterHost << endl;
        _monitor->notifyFailure( _masterHost );
        _master.reset(); 
    }

    auto_ptr<DBClientCursor> DBClientReplicaSet::checkSlaveQueryResult( auto_ptr<DBClientCursor> result ){
        BSONObj error;
        bool isError = result->peekError( &error );
        if( ! isError ) return result;

        // We only check for "not master or secondary" errors here

        // If the error code here ever changes, we need to change this code also
        BSONElement code = error["code"];
        if( code.isNumber() && code.Int() == 13436 /* not master or secondary */ ){
            isntSecondary();
            throw DBException( str::stream() << "slave " << _slaveHost.toString() << " is no longer secondary", 14812 );
        }

        return result;
    }

    void DBClientReplicaSet::isntSecondary() {
        log() << "slave no longer has secondary status: " << _slaveHost << endl;
        // Failover to next slave
        _monitor->notifySlaveFailure( _slaveHost );
        _slave.reset();
    }

    void DBClientReplicaSet::say( Message& toSend, bool isRetry ) {

        if( ! isRetry )
            _lazyState = LazyState();

        int lastOp = -1;
        bool slaveOk = false;

        if ( ( lastOp = toSend.operation() ) == dbQuery ) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm( toSend );
            QueryMessage qm( dm );
            if ( ( slaveOk = ( qm.queryOptions & QueryOption_SlaveOk ) ) ) {

                for ( int i = _lazyState._retries; i < 3; i++ ) {
                    try {
                        DBClientConnection* slave = checkSlave();
                        slave->say( toSend );

                        _lazyState._lastOp = lastOp;
                        _lazyState._slaveOk = slaveOk;
                        _lazyState._retries = i;
                        _lazyState._lastClient = slave;
                        return;
                    }
                    catch ( DBException &e ) {
                       LOG(1) << "can't callLazy replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                    }
                }
            }
        }

        DBClientConnection* master = checkMaster();
        master->say( toSend );

        _lazyState._lastOp = lastOp;
        _lazyState._slaveOk = slaveOk;
        _lazyState._retries = 3;
        _lazyState._lastClient = master;
        return;
    }

    bool DBClientReplicaSet::recv( Message& m ) {

        assert( _lazyState._lastClient );

        // TODO: It would be nice if we could easily wrap a conn error as a result error
        try {
            return _lazyState._lastClient->recv( m );
        }
        catch( DBException& e ){
            log() << "could not receive data from " << _lazyState._lastClient << causedBy( e ) << endl;
            return false;
        }
    }

    void DBClientReplicaSet::checkResponse( const char* data, int nReturned, bool* retry, string* targetHost ){

        // For now, do exactly as we did before, so as not to break things.  In general though, we
        // should fix this so checkResponse has a more consistent contract.
        if( ! retry ){
            if( _lazyState._lastClient )
                return _lazyState._lastClient->checkResponse( data, nReturned );
            else
                return checkMaster()->checkResponse( data, nReturned );
        }

        *retry = false;
        if( targetHost && _lazyState._lastClient ) *targetHost = _lazyState._lastClient->getServerAddress();
        else if (targetHost) *targetHost = "";

        if( ! _lazyState._lastClient ) return;
        if( nReturned != 1 && nReturned != -1 ) return;

        BSONObj dataObj;
        if( nReturned == 1 ) dataObj = BSONObj( data );

        // Check if we should retry here
        if( _lazyState._lastOp == dbQuery && _lazyState._slaveOk ){

            // Check the error code for a slave not secondary error
            if( nReturned == -1 ||
                ( hasErrField( dataObj ) &&  ! dataObj["code"].eoo() && dataObj["code"].Int() == 13436 ) ){

                bool wasMaster = false;
                if( _lazyState._lastClient == _slave.get() ){
                    isntSecondary();
                }
                else if( _lazyState._lastClient == _master.get() ){
                    wasMaster = true;
                    isntMaster();
                }
                else
                    warning() << "passed " << dataObj << " but last rs client " << _lazyState._lastClient->toString() << " is not master or secondary" << endl;

                if( _lazyState._retries < 3 ){
                    _lazyState._retries++;
                    *retry = true;
                }
                else{
                    (void)wasMaster; // silence set-but-not-used warning
                    // assert( wasMaster );
                    // printStackTrace();
                    log() << "too many retries (" << _lazyState._retries << "), could not get data from replica set" << endl;
                }
            }
        }
    }


    bool DBClientReplicaSet::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        if ( toSend.operation() == dbQuery ) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm( toSend );
            QueryMessage qm( dm );
            if ( qm.queryOptions & QueryOption_SlaveOk ) {
                for ( int i=0; i<3; i++ ) {
                    try {
                        DBClientConnection* s = checkSlave();
                        if ( actualServer )
                            *actualServer = s->getServerAddress();
                        return s->call( toSend , response , assertOk );
                    }
                    catch ( DBException &e ) {
                    	LOG(1) << "can't call replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                        if ( actualServer )
                            *actualServer = "";
                    }
                }
            }
        }
        
        DBClientConnection* m = checkMaster();
        if ( actualServer )
            *actualServer = m->getServerAddress();
        return m->call( toSend , response , assertOk );
    }

}
