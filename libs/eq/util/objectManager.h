
/* Copyright (c) 2007-2012, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef EQUTIL_OBJECTMANAGER_H
#define EQUTIL_OBJECTMANAGER_H

#include <eq/util/types.h>
#include <eq/client/api.h>

#include <lunchbox/api.h>              // EQ_API definition
#include <lunchbox/debug.h>            // LBASSERT definition
#include <lunchbox/hash.h>             // member
#include <lunchbox/nonCopyable.h>      // base class
#include <lunchbox/referenced.h>       // base class

namespace eq
{
namespace util
{
    /**
     * A facility class to manage OpenGL objects across shared contexts.
     *
     * The object manager implements object sharing in the same way as
     * OpenGL. During creation, a shared object manager may be given, causing
     * the two (or more) object managers to allocate objects from the same
     * namespace. The last object manager will delete all data allocated on the
     * host. OpenGL objects have to be explictly deleted using deleteAll() to
     * ensure an OpenGL context is still current during destruction.
     *
     * For each type of OpenGL object supported the following methods are
     * available: 
     * - supportsObject: Check if the necessary OpenGL version or extension
     *   is present
     * - getObject: Lookup an existing object, may return 0
     * - newObject: Allocate new object, returns false if key exists
     * - obtainObject: Lookup or allocate an object for the given key
     * - deleteObject: Delete the object of the given key and all associated
     *   OpenGL data
     *
     * @sa http://www.equalizergraphics.com/documents/design/objectManager.html
     */
    template< class T > class ObjectManager : public lunchbox::NonCopyable
    {
    public:
        enum
        {
            INVALID = 0 //<! return value for failed operations.
        };

        /** Construct a new object manager. */
        EQ_API ObjectManager( const GLEWContext* const glewContext );

        /** Construct a new object manager sharing data with another manager. */
        EQ_API ObjectManager( ObjectManager* shared );

        EQ_API virtual ~ObjectManager();

        /** @return true if more than one OM is using the same data. */
        bool isShared() const { return _data->getRefCount() > 1; }

        EQ_API void deleteAll();

        EQ_API unsigned getList( const T& key ) const;
        EQ_API unsigned newList( const T& key, const int num = 1 );
        EQ_API unsigned obtainList( const T& key, const int num = 1 );
        EQ_API void   deleteList( const T& key );

        EQ_API unsigned getTexture( const T& key ) const;
        EQ_API unsigned newTexture( const T& key );
        EQ_API unsigned obtainTexture( const T& key );
        EQ_API void   deleteTexture( const T& key );

        EQ_API bool   supportsBuffers() const;
        EQ_API unsigned getBuffer( const T& key ) const;
        EQ_API unsigned newBuffer( const T& key );
        EQ_API unsigned obtainBuffer( const T& key );
        EQ_API void   deleteBuffer( const T& key );

        EQ_API bool   supportsPrograms() const;
        EQ_API unsigned getProgram( const T& key ) const;
        EQ_API unsigned newProgram( const T& key );
        EQ_API unsigned obtainProgram( const T& key );
        EQ_API void   deleteProgram( const T& key );

        EQ_API bool   supportsShaders() const;
        EQ_API unsigned getShader( const T& key ) const;
        EQ_API unsigned newShader( const T& key, const unsigned type );
        EQ_API unsigned obtainShader( const T& key, const unsigned type );
        EQ_API void   deleteShader( const T& key );

        EQ_API Accum* getEqAccum( const T& key ) const;
        EQ_API Accum* newEqAccum( const T& key );
        EQ_API Accum* obtainEqAccum( const T& key );
        EQ_API void   deleteEqAccum( const T& key );

#ifdef EQUALIZER_SHARED
        /** @internal */
        EQ_API GPUCompressor* getEqUploader( const T& key ) const;
        /** @internal */
        EQ_API GPUCompressor* newEqUploader( const T& key );
        /** @internal */
        EQ_API GPUCompressor* obtainEqUploader( const T& key );
        /** @internal */
        EQ_API void   deleteEqUploader( const T& key );
#endif

        EQ_API bool     supportsEqTexture() const;
        EQ_API Texture* getEqTexture( const T& key ) const;
        EQ_API Texture* newEqTexture( const T& key, const unsigned target );
        EQ_API Texture* obtainEqTexture( const T& key, const unsigned target );
        EQ_API void     deleteEqTexture( const T& key );

        EQ_API bool               supportsEqFrameBufferObject() const;
        EQ_API FrameBufferObject* getEqFrameBufferObject(const T& key) const;
        EQ_API FrameBufferObject* newEqFrameBufferObject( const T& key );
        EQ_API FrameBufferObject* obtainEqFrameBufferObject( const T& key );
        EQ_API void               deleteEqFrameBufferObject( const T& key );

        EQ_API bool               supportsEqPixelBufferObject() const;
        EQ_API PixelBufferObject* getEqPixelBufferObject(const T& key) const;
        EQ_API PixelBufferObject* newEqPixelBufferObject( const T& key,
                                                        const bool threadSafe );
        EQ_API PixelBufferObject* obtainEqPixelBufferObject( const T& key,
                                                        const bool threadSafe );
        EQ_API void               deleteEqPixelBufferObject( const T& key );

        EQ_API util::BitmapFont< T >* getEqBitmapFont( const T& key ) const;
        EQ_API util::BitmapFont< T >* newEqBitmapFont( const T& key );
        EQ_API util::BitmapFont< T >* obtainEqBitmapFont( const T& key );
        EQ_API void                   deleteEqBitmapFont( const T& key );

        const GLEWContext* glewGetContext() const { return _data->glewContext; }

    private:
        struct Object
        {
            unsigned   id;
            unsigned   num;
        };

        typedef stde::hash_map< T, Object >     ObjectHash;
        typedef stde::hash_map< T, Texture* >   TextureHash;
        typedef stde::hash_map< T, FrameBufferObject* > FBOHash;
        typedef stde::hash_map< T, PixelBufferObject* > PBOHash;
        typedef stde::hash_map< T, util::BitmapFont< T >* > FontHash;
        typedef stde::hash_map< T, Accum* > AccumHash;
        typedef stde::hash_map< T, GPUCompressor* > UploaderHash;

        struct SharedData : public lunchbox::Referenced
        {
            SharedData( const GLEWContext* glewContext );
            virtual ~SharedData();

            GLEWContext* const glewContext;
            ObjectHash lists;
            ObjectHash textures;
            ObjectHash buffers;
            ObjectHash programs;
            ObjectHash shaders;
            ObjectHash uploaderDatas;
            AccumHash  accums;
            TextureHash eqTextures;
            FBOHash eqFrameBufferObjects;
            PBOHash eqPixelBufferObjects;
            FontHash eqFonts;
            UploaderHash eqUploaders;

            union // placeholder for binary-compatible changes
            {
                char dummy[64];
            };
        };

        typedef lunchbox::RefPtr< SharedData > SharedDataPtr;
        SharedDataPtr _data;

        struct Private;
        Private* _private; // placeholder for binary-compatible changes
    };
}
}

#endif // EQUTIL_OBJECTMANAGER_H

