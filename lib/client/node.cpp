
/* Copyright (c) 2005-2009, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#include <pthread.h>

#include "node.h"

#include "client.h"
#include "commands.h"
#include "config.h"
#include "frameData.h"
#include "global.h"
#include "log.h"
#include "nodeFactory.h"
#include "nodeStatistics.h"
#include "packets.h"
#include "pipe.h"
#include "server.h"
#include "task.h"

#include <eq/base/scopedMutex.h>
#include <eq/net/command.h>
#include <eq/net/connection.h>

using namespace eq::base;
using namespace std;
using eq::net::CommandFunc;

namespace eq
{
typedef net::CommandFunc<Node> NodeFunc;

#define MAKE_ATTR_STRING( attr ) ( string("EQ_NODE_") + #attr )
std::string Node::_iAttributeStrings[IATTR_ALL] = {
    MAKE_ATTR_STRING( IATTR_THREAD_MODEL ),
    MAKE_ATTR_STRING( IATTR_HINT_STATISTICS )
};

Node::Node( Config* parent )
#ifdef EQ_ASYNC_TRANSMIT
        : transmitter( this )
        , _config( parent )
#else
        : _config( parent )
#endif
        , _tasks( TASK_NONE )
        , _unlockedFrame( 0 )
        , _finishedFrame( 0 )
{
    parent->_addNode( this );
    EQINFO << " New eq::Node @" << (void*)this << endl;
}

Node::~Node()
{
    _config->_removeNode( this );

    EQINFO << " Delete eq::Node @" << (void*)this << endl;
}

void Node::attachToSession( const uint32_t id, 
                            const uint32_t instanceID, 
                            net::Session* session )
{
    net::Object::attachToSession( id, instanceID, session );

    EQASSERT( _config );
    net::CommandQueue* queue = _config->getNodeThreadQueue();

    registerCommand( CMD_NODE_CREATE_PIPE, 
                     NodeFunc( this, &Node::_cmdCreatePipe ), queue );
    registerCommand( CMD_NODE_DESTROY_PIPE,
                     NodeFunc( this, &Node::_cmdDestroyPipe ), queue );
    registerCommand( CMD_NODE_CONFIG_INIT, 
                     NodeFunc( this, &Node::_cmdConfigInit ), queue );
    registerCommand( CMD_NODE_CONFIG_EXIT,
                     NodeFunc( this, &Node::_cmdConfigExit ), queue );
    registerCommand( CMD_NODE_FRAME_START,
                     NodeFunc( this, &Node::_cmdFrameStart ), queue );
    registerCommand( CMD_NODE_FRAME_FINISH,
                     NodeFunc( this, &Node::_cmdFrameFinish ), queue );
    registerCommand( CMD_NODE_FRAME_DRAW_FINISH, 
                     NodeFunc( this, &Node::_cmdFrameDrawFinish ), queue );
    registerCommand( CMD_NODE_FRAME_TASKS_FINISH, 
                     NodeFunc( this, &Node::_cmdFrameTasksFinish ), queue );
}

ClientPtr Node::getClient()
{
    EQASSERT( _config );
    return (_config ? _config->getClient() : 0);
}

ServerPtr Node::getServer()
{
    EQASSERT( _config );
    return (_config ? _config->getServer() : 0);
}

CommandQueue* Node::getNodeThreadQueue()
{
    return getClient()->getNodeThreadQueue();
}

VisitorResult Node::accept( NodeVisitor& visitor )
{ 
    VisitorResult result = visitor.visitPre( this );
    if( result != TRAVERSE_CONTINUE )
        return result;

    for( PipeVector::const_iterator i = _pipes.begin(); 
         i != _pipes.end(); ++i )
    {
        Pipe* pipe = *i;
        switch( pipe->accept( visitor ))
        {
            case TRAVERSE_TERMINATE:
                return TRAVERSE_TERMINATE;

            case TRAVERSE_PRUNE:
                result = TRAVERSE_PRUNE;
                break;
                
            case TRAVERSE_CONTINUE:
            default:
                break;
        }
    }

    switch( visitor.visitPost( this ))
    {
        case TRAVERSE_TERMINATE:
            return TRAVERSE_TERMINATE;

        case TRAVERSE_PRUNE:
            return TRAVERSE_PRUNE;
                
        case TRAVERSE_CONTINUE:
        default:
            break;
    }

    return result;
}

void Node::_addPipe( Pipe* pipe )
{
    EQASSERT( pipe->getNode() == this );
    _pipes.push_back( pipe );
}

void Node::_removePipe( Pipe* pipe )
{
    PipeVector::iterator iter = find( _pipes.begin(), _pipes.end(), pipe );
    EQASSERT( iter != _pipes.end( ))
    
    _pipes.erase( iter );
}

Pipe* Node::_findPipe( const uint32_t id )
{
    for( PipeVector::const_iterator i = _pipes.begin(); i != _pipes.end(); 
         ++i )
    {
        Pipe* pipe = *i;
        if( pipe->getID() == id )
            return pipe;
    }
    return 0;
}

net::Barrier* Node::getBarrier( const uint32_t id, const uint32_t version )
{
    _barriersMutex.set();
    net::Barrier* barrier = _barriers[ id ];

    if( !barrier )
    {
        net::Session* session = getSession();

        barrier = new net::Barrier;
        barrier->makeThreadSafe();
        EQCHECK( session->mapObject( barrier, id ));

        _barriers[ id ] = barrier;
    }
    _barriersMutex.unset();

    barrier->sync( version );
    return barrier;
}

FrameData* Node::getFrameData( const net::ObjectVersion& dataVersion )
{
    _frameDatasMutex.set();
    FrameData* frameData = _frameDatas[ dataVersion.id ];

    if( !frameData )
    {
        net::Session* session = getSession();
        
        frameData = new FrameData;
        frameData->makeThreadSafe();
        EQCHECK( session->mapObject( frameData, dataVersion.id ));

        _frameDatas[ dataVersion.id ] = frameData;
    }
    _frameDatasMutex.unset();

    if( frameData->getVersion() < dataVersion.version )
    {
        frameData->sync( dataVersion.version );
        frameData->update( dataVersion.version );
    }
    EQASSERT( frameData->getVersion() == dataVersion.version );

    return frameData;
}

void Node::_finishFrame( const uint32_t frameNumber ) const
{
    for( PipeVector::const_iterator i = _pipes.begin(); i != _pipes.end(); ++i )
    {
        const Pipe* pipe = *i;
        EQASSERT( pipe->isThreaded() || 
                  pipe->getFinishedFrame() >= frameNumber );

        pipe->waitFrameLocal( frameNumber );
        pipe->waitFrameFinished( frameNumber );
    }
}

void Node::_frameFinish( const uint32_t frameID, const uint32_t frameNumber )
{
    frameFinish( frameID, frameNumber );
    EQLOG( LOG_TASKS ) << "---- Finished Frame --- " << frameNumber << endl;

    if( _unlockedFrame < frameNumber )
    {
        EQWARN << "Finished frame was not locally unlocked, enforcing unlock" 
               << endl;
        releaseFrameLocal( frameNumber );
    }

    if( _finishedFrame < frameNumber )
    {
        EQWARN << "Finished frame was not released, enforcing unlock" << endl;
        releaseFrame( frameNumber );
    }
}

void Node::releaseFrame( const uint32_t frameNumber )
{
    EQASSERTINFO( _currentFrame >= frameNumber, 
                  "current " << _currentFrame << " release " << frameNumber );

    if( _finishedFrame >= frameNumber )
        return;
    _finishedFrame = frameNumber;

    NodeFrameFinishReplyPacket packet;
    packet.frameNumber = frameNumber;

    net::NodePtr node = 
        RefPtr_static_cast< Server, net::Node >( _config->getServer( ));
    send( node, packet );
}

void Node::releaseFrameLocal( const uint32_t frameNumber )
{
    EQASSERT( _unlockedFrame <= frameNumber );
    _unlockedFrame = frameNumber;
    
    Config* config = getConfig();
    EQASSERT( config->getNodes().size() == 1 );
    EQASSERT( config->getNodes()[0] == this );
    config->releaseFrameLocal( frameNumber );

    EQLOG( LOG_TASKS ) << "---- Unlocked Frame --- " << _unlockedFrame << endl;
}

void Node::frameStart( const uint32_t frameID, const uint32_t frameNumber )
{
    startFrame( frameNumber ); // unlock pipe threads
    
    switch( getIAttribute( IATTR_THREAD_MODEL ))
    {
        case ASYNC:
            // Don't wait for pipes to release frame locally, sync not needed
            releaseFrameLocal( frameNumber );
            break;

        case DRAW_SYNC:  // Sync and release in frameDrawFinish
        case LOCAL_SYNC: // Sync and release in frameTasksFinish
            break;

        default:
            EQUNIMPLEMENTED;
    }
}

void Node::frameDrawFinish( const uint32_t frameID, const uint32_t frameNumber )
{
    switch( getIAttribute( IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // No sync, release in frameStart
        case LOCAL_SYNC: // Sync and release in frameTasksFinish
            break;

        case DRAW_SYNC:
            for( PipeVector::const_iterator i = _pipes.begin();
                 i != _pipes.end(); ++i )
            {
                const Pipe* pipe = *i;
                pipe->waitFrameLocal( frameNumber );
            }
            
            releaseFrameLocal( frameNumber );
            break;

        default:
            EQUNIMPLEMENTED;
    }
}

void Node::frameTasksFinish( const uint32_t frameID, const uint32_t frameNumber)
{
    switch( getIAttribute( IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // No sync, release in frameStart
        case DRAW_SYNC:  // Sync and release in frameDrawFinish
            break;

        case LOCAL_SYNC:
            for( PipeVector::const_iterator i = _pipes.begin();
                 i != _pipes.end(); ++i )
            {
                const Pipe* pipe = *i;
                pipe->waitFrameLocal( frameNumber );
            }
            
            releaseFrameLocal( frameNumber );
            break;

        default:
            EQUNIMPLEMENTED;
    }
}

void Node::_flushObjects()
{
    net::Session* session = getSession();

    _barriersMutex.set();
    for( BarrierHash::const_iterator i =_barriers.begin();
         i != _barriers.end(); ++ i )
    {
        net::Barrier* barrier = i->second;
        session->unmapObject( barrier );
        delete barrier;
    }
    _barriers.clear();
    _barriersMutex.unset();

    _frameDatasMutex.set();
    for( FrameDataHash::const_iterator i = _frameDatas.begin(); 
         i != _frameDatas.end(); ++ i )
    {
        FrameData* frameData = i->second;
        session->unmapObject( frameData );
        delete frameData;
    }
    _frameDatas.clear();
    _frameDatasMutex.unset();
}

#ifdef EQ_ASYNC_TRANSMIT
void Node::TransmitThread::send( FrameData* data, net::NodePtr node, 
                                 const uint32_t frameNumber )
{
    _tasks.push( Task( data, node, frameNumber ));
}

void* Node::TransmitThread::run()
{
    while( true )
    {
        const Task task = _tasks.pop();
        if( _tasks.empty() && !task.node )
            return 0; // exit thread
        
        NodeStatistics event( Statistic::NODE_TRANSMIT, _node,
                              task.frameNumber );
        NodeStatistics compressEvent( Statistic::NODE_COMPRESS, _node, 
                                      task.frameNumber );

        EQLOG( LOG_ASSEMBLY ) << "node transmit " << task.data->getID()
                              << " to " << task.node->getNodeID() << endl;
        const int64_t compressTime = task.data->transmit( task.node );

        compressEvent.event.data.statistic.endTime =
            compressEvent.event.data.statistic.startTime + compressTime;
    }
    return 0;
}

#endif

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
net::CommandResult Node::_cmdCreatePipe( net::Command& command )
{
    const NodeCreatePipePacket* packet = 
        command.getPacket<NodeCreatePipePacket>();
    EQLOG( LOG_INIT ) << "Create pipe " << packet << endl;

    CHECK_THREAD( _nodeThread );
    EQASSERT( packet->pipeID != EQ_ID_INVALID );

    Pipe* pipe = Global::getNodeFactory()->createPipe( this );

    if( packet->threaded )
        pipe->startThread();

    _config->attachObject( pipe, packet->pipeID, EQ_ID_INVALID );

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdDestroyPipe( net::Command& command )
{
    const NodeDestroyPipePacket* packet = 
        command.getPacket<NodeDestroyPipePacket>();
    EQLOG( LOG_INIT ) << "Destroy pipe " << packet << endl;

    CHECK_THREAD( _nodeThread );
    Pipe* pipe = _findPipe( packet->pipeID );
    pipe->joinThread();

    _config->detachObject( pipe );
    Global::getNodeFactory()->releasePipe( pipe );

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdConfigInit( net::Command& command )
{
    const NodeConfigInitPacket* packet = 
        command.getPacket<NodeConfigInitPacket>();
    EQLOG( LOG_INIT ) << "TASK node config init " << packet << endl;

    CHECK_THREAD( _nodeThread );
    _name  = packet->name;
    _tasks = packet->tasks;
    memcpy( _iAttributes, packet->iAttributes, IATTR_ALL * sizeof( int32_t ));

    _currentFrame  = 0;
    _unlockedFrame = 0;
    _finishedFrame = 0;

#ifdef EQ_ASYNC_TRANSMIT
    transmitter.start();
#endif
    _error.clear();
    NodeConfigInitReplyPacket reply;
    reply.result = configInit( packet->initID );

    if( _iAttributes[ IATTR_THREAD_MODEL ] == eq::UNDEFINED )
        _iAttributes[ IATTR_THREAD_MODEL ] = eq::DRAW_SYNC;

    _initialized = true; // even if init failed we need to unlock the pipes

    EQLOG( LOG_INIT ) << "TASK node config init reply " << &reply << endl;
    send( command.getNode(), reply, _error );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdConfigExit( net::Command& command )
{
    const NodeConfigExitPacket* packet = 
        command.getPacket<NodeConfigExitPacket>();
    EQLOG( LOG_INIT ) << "TASK node config exit " << packet << endl;

    CHECK_THREAD( _nodeThread );
    for( PipeVector::const_iterator i = _pipes.begin(); i != _pipes.end(); 
         ++i )
    {
        Pipe* pipe = *i;
        pipe->waitExited();
    }
    
    NodeConfigExitReplyPacket reply;
    reply.result = configExit();

#ifdef EQ_ASYNC_TRANSMIT
    transmitter.send( 0, 0, 0 );
    transmitter.join();
#endif

    _initialized = false;
    _flushObjects();

    send( command.getNode(), reply );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameStart( net::Command& command )
{
    CHECK_THREAD( _nodeThread );
    const NodeFrameStartPacket* packet = 
        command.getPacket<NodeFrameStartPacket>();
    EQVERB << "handle node frame start " << packet << endl;

    const uint32_t frameNumber = packet->frameNumber;
    EQASSERT( _currentFrame == frameNumber-1 );

    EQLOG( LOG_TASKS ) << "----- Begin Frame ----- " << frameNumber << endl;

    frameStart( packet->frameID, frameNumber );
    EQASSERTINFO( _currentFrame >= frameNumber, 
                  "Node::frameStart() did not start frame " << frameNumber );

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameFinish( net::Command& command )
{
    CHECK_THREAD( _nodeThread );
    const NodeFrameFinishPacket* packet = 
        command.getPacket<NodeFrameFinishPacket>();
    EQVERB << "handle node frame finish " << packet << endl;

    const uint32_t frameNumber = packet->frameNumber;

    _finishFrame( frameNumber );
    _frameFinish( packet->frameID, frameNumber );

    if( packet->syncGlobalFinish )
    {
        // special sync for appNode with non-threaded pipes.
        const Config* config = getConfig();
        config->waitFrameFinished( frameNumber );
    }

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameDrawFinish( net::Command& command )
{
    NodeFrameDrawFinishPacket* packet = 
        command.getPacket< NodeFrameDrawFinishPacket >();
    EQLOG( LOG_TASKS ) << "TASK draw finish " << getName() <<  " " << packet
                       << endl;

    frameDrawFinish( packet->frameID, packet->frameNumber );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameTasksFinish( net::Command& command )
{
    NodeFrameTasksFinishPacket* packet = 
        command.getPacket< NodeFrameTasksFinishPacket >();
    EQLOG( LOG_TASKS ) << "TASK tasks finish " << getName() <<  " " << packet
                       << endl;

    frameTasksFinish( packet->frameID, packet->frameNumber );
    return net::COMMAND_HANDLED;
}
}
