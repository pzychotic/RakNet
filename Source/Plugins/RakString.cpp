/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "Plugins/RakString.h"
#include "RakAssert.h"
#include "RakMemoryOverride.h"
#include <string.h>
#include "LinuxStrings.h"
#include "StringCompressor.h"
#include <stdlib.h>

#include <charconv>
#include <mutex>

namespace RakNet {

RakString::SharedString RakString::emptyString = { 0, 0, 0, (char*)"", (char*)"" };
DataStructures::List<RakString::SharedString*> RakString::freeList;

class RakStringCleanup
{
public:
    ~RakStringCleanup()
    {
        RakString::FreeMemoryNoMutex();
    }
};

static RakStringCleanup cleanup;

std::mutex& GetPoolMutex( void )
{
    static std::mutex poolMutex;
    return poolMutex;
}

RakString::RakString()
{
    sharedString = &emptyString;
}
RakString::RakString( RakString::SharedString* _sharedString )
{
    sharedString = _sharedString;
}
RakString::RakString( char input )
{
    char str[2];
    str[0] = input;
    str[1] = 0;
    Assign( str );
}
RakString::RakString( unsigned char input )
{
    char str[2];
    str[0] = (char)input;
    str[1] = 0;
    Assign( str );
}
RakString::RakString( const unsigned char* format, ... )
{
    va_list ap;
    va_start( ap, format );
    Assign( (const char*)format, ap );
    va_end( ap );
}
RakString::RakString( const char* format, ... )
{
    va_list ap;
    va_start( ap, format );
    Assign( format, ap );
    va_end( ap );
}
RakString::RakString( const RakString& rhs )
{
    if( rhs.sharedString == &emptyString )
    {
        sharedString = &emptyString;
        return;
    }

    rhs.sharedString->refCountMutex->lock();
    if( rhs.sharedString->refCount == 0 )
    {
        sharedString = &emptyString;
    }
    else
    {
        rhs.sharedString->refCount++;
        sharedString = rhs.sharedString;
    }
    rhs.sharedString->refCountMutex->unlock();
}
RakString::~RakString()
{
    Free();
}
RakString& RakString::operator=( const RakString& rhs )
{
    Free();
    if( rhs.sharedString == &emptyString )
        return *this;

    rhs.sharedString->refCountMutex->lock();
    if( rhs.sharedString->refCount == 0 )
    {
        sharedString = &emptyString;
    }
    else
    {
        sharedString = rhs.sharedString;
        sharedString->refCount++;
    }
    rhs.sharedString->refCountMutex->unlock();
    return *this;
}
RakString& RakString::operator=( const char* str )
{
    Free();
    Assign( str );
    return *this;
}
RakString& RakString::operator=( char* str )
{
    return operator=( (const char*)str );
}
RakString& RakString::operator=( const unsigned char* str )
{
    return operator=( (const char*)str );
}
RakString& RakString::operator=( char unsigned* str )
{
    return operator=( (const char*)str );
}
RakString& RakString::operator=( const char c )
{
    char buff[2];
    buff[0] = c;
    buff[1] = 0;
    return operator=( (const char*)buff );
}
void RakString::Realloc( SharedString* sharedString, size_t bytes )
{
    if( bytes <= sharedString->bytesUsed )
        return;

    RakAssert( bytes > 0 );
    size_t oldBytes = sharedString->bytesUsed;
    size_t newBytes;
    const size_t smallStringSize = 128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2;
    newBytes = GetSizeToAllocate( bytes );
    if( oldBytes <= (size_t)smallStringSize && newBytes > (size_t)smallStringSize )
    {
        sharedString->bigString = (char*)rakMalloc_Ex( newBytes, _FILE_AND_LINE_ );
        strcpy( sharedString->bigString, sharedString->smallString );
        sharedString->c_str = sharedString->bigString;
    }
    else if( oldBytes > smallStringSize )
    {
        sharedString->bigString = (char*)rakRealloc_Ex( sharedString->bigString, newBytes, _FILE_AND_LINE_ );
        sharedString->c_str = sharedString->bigString;
    }
    sharedString->bytesUsed = newBytes;
}
RakString& RakString::operator+=( const RakString& rhs )
{
    if( rhs.IsEmpty() )
        return *this;

    if( IsEmpty() )
    {
        return operator=( rhs );
    }
    else
    {
        Clone();
        size_t strLen = rhs.GetLength() + GetLength() + 1;
        Realloc( sharedString, strLen + GetLength() );
        strcat( sharedString->c_str, rhs.C_String() );
    }
    return *this;
}
RakString& RakString::operator+=( const char* str )
{
    if( str == 0 || str[0] == 0 )
        return *this;

    if( IsEmpty() )
    {
        Assign( str );
    }
    else
    {
        Clone();
        size_t strLen = strlen( str ) + GetLength() + 1;
        Realloc( sharedString, strLen );
        strcat( sharedString->c_str, str );
    }
    return *this;
}
RakString& RakString::operator+=( char* str )
{
    return operator+=( (const char*)str );
}
RakString& RakString::operator+=( const unsigned char* str )
{
    return operator+=( (const char*)str );
}
RakString& RakString::operator+=( unsigned char* str )
{
    return operator+=( (const char*)str );
}
RakString& RakString::operator+=( const char c )
{
    char buff[2];
    buff[0] = c;
    buff[1] = 0;
    return operator+=( (const char*)buff );
}
unsigned char RakString::operator[]( const unsigned int position ) const
{
    RakAssert( position < GetLength() );
    return sharedString->c_str[position];
}
bool RakString::operator==( const RakString& rhs ) const
{
    return strcmp( sharedString->c_str, rhs.sharedString->c_str ) == 0;
}
bool RakString::operator==( const char* str ) const
{
    return strcmp( sharedString->c_str, str ) == 0;
}
bool RakString::operator==( char* str ) const
{
    return strcmp( sharedString->c_str, str ) == 0;
}
bool RakString::operator<( const RakString& right ) const
{
    return strcmp( sharedString->c_str, right.C_String() ) < 0;
}
bool RakString::operator<=( const RakString& right ) const
{
    return strcmp( sharedString->c_str, right.C_String() ) <= 0;
}
bool RakString::operator>( const RakString& right ) const
{
    return strcmp( sharedString->c_str, right.C_String() ) > 0;
}
bool RakString::operator>=( const RakString& right ) const
{
    return strcmp( sharedString->c_str, right.C_String() ) >= 0;
}
bool RakString::operator!=( const RakString& rhs ) const
{
    return strcmp( sharedString->c_str, rhs.sharedString->c_str ) != 0;
}
bool RakString::operator!=( const char* str ) const
{
    return strcmp( sharedString->c_str, str ) != 0;
}
bool RakString::operator!=( char* str ) const
{
    return strcmp( sharedString->c_str, str ) != 0;
}
const RakString operator+( const RakString& lhs, const RakString& rhs )
{
    if( lhs.IsEmpty() && rhs.IsEmpty() )
    {
        return RakString( &RakString::emptyString );
    }
    if( lhs.IsEmpty() )
    {
        rhs.sharedString->refCountMutex->lock();
        if( rhs.sharedString->refCount == 0 )
        {
            rhs.sharedString->refCountMutex->unlock();
            lhs.sharedString->refCountMutex->lock();
            lhs.sharedString->refCount++;
            lhs.sharedString->refCountMutex->unlock();
            return RakString( lhs.sharedString );
        }
        else
        {
            rhs.sharedString->refCount++;
            rhs.sharedString->refCountMutex->unlock();
            return RakString( rhs.sharedString );
        }
    }
    if( rhs.IsEmpty() )
    {
        lhs.sharedString->refCountMutex->lock();
        lhs.sharedString->refCount++;
        lhs.sharedString->refCountMutex->unlock();
        return RakString( lhs.sharedString );
    }

    size_t len1 = lhs.GetLength();
    size_t len2 = rhs.GetLength();
    size_t allocatedBytes = len1 + len2 + 1;
    allocatedBytes = RakString::GetSizeToAllocate( allocatedBytes );
    RakString::SharedString* sharedString;

    RakString::LockMutex();
    if( RakString::freeList.Size() == 0 )
    {
        unsigned i;
        for( i = 0; i < 128; i++ )
        {
            RakString::SharedString* ss;
            ss = (RakString::SharedString*)rakMalloc_Ex( sizeof( RakString::SharedString ), _FILE_AND_LINE_ );
            ss->refCountMutex = RakNet::OP_NEW<std::mutex>( _FILE_AND_LINE_ );
            RakString::freeList.Insert( ss, _FILE_AND_LINE_ );
        }
    }
    sharedString = RakString::freeList[RakString::freeList.Size() - 1];
    RakString::freeList.RemoveAtIndex( RakString::freeList.Size() - 1 );
    RakString::UnlockMutex();

    const int smallStringSize = 128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2;
    sharedString->bytesUsed = allocatedBytes;
    sharedString->refCount = 1;
    if( allocatedBytes <= (size_t)smallStringSize )
    {
        sharedString->c_str = sharedString->smallString;
    }
    else
    {
        sharedString->bigString = (char*)rakMalloc_Ex( sharedString->bytesUsed, _FILE_AND_LINE_ );
        sharedString->c_str = sharedString->bigString;
    }

    strcpy( sharedString->c_str, lhs.C_String() );
    strcat( sharedString->c_str, rhs.C_String() );

    return RakString( sharedString );
}
void RakString::Set( const char* format, ... )
{
    va_list ap;
    va_start( ap, format );
    Clear();
    Assign( format, ap );
    va_end( ap );
}
bool RakString::IsEmpty( void ) const
{
    return sharedString == &emptyString;
}
size_t RakString::GetLength( void ) const
{
    return strlen( sharedString->c_str );
}
void RakString::SetChar( unsigned index, unsigned char c )
{
    RakAssert( index < GetLength() );
    Clone();
    sharedString->c_str[index] = c;
}

void RakString::Truncate( unsigned int length )
{
    if( length < GetLength() )
    {
        SetChar( length, 0 );
    }
}

void RakString::Erase( unsigned int index, unsigned int count )
{
    size_t len = GetLength();
    RakAssert( index + count <= len );

    Clone();
    unsigned i;
    for( i = index; i < len - count; i++ )
    {
        sharedString->c_str[i] = sharedString->c_str[i + count];
    }
    sharedString->c_str[i] = 0;
}

RakString& RakString::URLEncode( void )
{
    RakString result;
    size_t strLen = strlen( sharedString->c_str );
    result.Allocate( strLen * 3 );
    char* output = result.sharedString->c_str;
    unsigned int outputIndex = 0;
    unsigned i;
    unsigned char c;
    for( i = 0; i < strLen; i++ )
    {
        c = sharedString->c_str[i];
        if(
            ( c <= 47 ) ||
            ( c >= 58 && c <= 64 ) ||
            ( c >= 91 && c <= 96 ) ||
            ( c >= 123 ) )
        {
            char buff[3];
            auto res = std::to_chars( buff, buff + 2, c, 16 );
            RakAssert( res.ec == std::errc() );
            *res.ptr = '\0';
            output[outputIndex++] = '%';
            output[outputIndex++] = buff[0];
            output[outputIndex++] = buff[1];
        }
        else
        {
            output[outputIndex++] = c;
        }
    }

    output[outputIndex] = 0;

    *this = result;
    return *this;
}
RakString& RakString::URLDecode( void )
{
    RakString result;
    size_t strLen = strlen( sharedString->c_str );
    result.Allocate( strLen );
    char* output = result.sharedString->c_str;
    unsigned int outputIndex = 0;
    char c;
    char hexDigits[2];
    char hexValues[2];
    unsigned int i;
    for( i = 0; i < strLen; i++ )
    {
        c = sharedString->c_str[i];
        if( c == '%' )
        {
            hexDigits[0] = sharedString->c_str[++i];
            hexDigits[1] = sharedString->c_str[++i];

            if( hexDigits[0] == ' ' )
                hexValues[0] = 0;

            if( hexDigits[0] >= 'A' && hexDigits[0] <= 'F' )
                hexValues[0] = hexDigits[0] - 'A' + 10;
            if( hexDigits[0] >= 'a' && hexDigits[0] <= 'f' )
                hexValues[0] = hexDigits[0] - 'a' + 10;
            else
                hexValues[0] = hexDigits[0] - '0';

            if( hexDigits[1] >= 'A' && hexDigits[1] <= 'F' )
                hexValues[1] = hexDigits[1] - 'A' + 10;
            if( hexDigits[1] >= 'a' && hexDigits[1] <= 'f' )
                hexValues[1] = hexDigits[1] - 'a' + 10;
            else
                hexValues[1] = hexDigits[1] - '0';

            output[outputIndex++] = hexValues[0] * 16 + hexValues[1];
        }
        else
        {
            output[outputIndex++] = c;
        }
    }

    output[outputIndex] = 0;

    *this = result;
    return *this;
}
void RakString::SplitURI( RakString& header, RakString& domain, RakString& path )
{
    header.Clear();
    domain.Clear();
    path.Clear();

    size_t strLen = strlen( sharedString->c_str );

    char c;
    unsigned int i = 0;
    if( strncmp( sharedString->c_str, "http://", 7 ) == 0 )
        i += (unsigned int)strlen( "http://" );
    else if( strncmp( sharedString->c_str, "https://", 8 ) == 0 )
        i += (unsigned int)strlen( "https://" );

    if( strncmp( sharedString->c_str, "www.", 4 ) == 0 )
        i += (unsigned int)strlen( "www." );

    if( i != 0 )
    {
        header.Allocate( i + 1 );
        strncpy( header.sharedString->c_str, sharedString->c_str, i );
        header.sharedString->c_str[i] = 0;
    }


    domain.Allocate( strLen - i + 1 );
    char* domainOutput = domain.sharedString->c_str;
    unsigned int outputIndex = 0;
    for( ; i < strLen; i++ )
    {
        c = sharedString->c_str[i];
        if( c == '/' )
        {
            break;
        }
        else
        {
            domainOutput[outputIndex++] = sharedString->c_str[i];
        }
    }

    domainOutput[outputIndex] = 0;

    path.Allocate( strLen - header.GetLength() - outputIndex + 1 );
    outputIndex = 0;
    char* pathOutput = path.sharedString->c_str;
    for( ; i < strLen; i++ )
    {
        pathOutput[outputIndex++] = sharedString->c_str[i];
    }
    pathOutput[outputIndex] = 0;
}

RakString RakString::FormatForPUTOrPost( const char* type, const char* uri, const char* contentType, const char* body, const char* extraHeaders )
{
    RakString out;
    RakString host;
    RakString remotePath;
    RakString header;
    RakString uriRs;
    uriRs = uri;
    uriRs.SplitURI( header, host, remotePath );

    if( host.IsEmpty() || remotePath.IsEmpty() )
        return out;

    if( extraHeaders != 0 && extraHeaders[0] )
    {
        out.Set( "%s %s HTTP/1.1\r\n"
                 "%s\r\n"
                 "Host: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %u\r\n"
                 "\r\n"
                 "%s",
                 type,
                 remotePath.C_String(),
                 extraHeaders,
                 host.C_String(),
                 contentType,
                 strlen( body ),
                 body );
    }
    else
    {
        out.Set( "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %u\r\n"
                 "\r\n"
                 "%s",
                 type,
                 remotePath.C_String(),
                 host.C_String(),
                 contentType,
                 strlen( body ),
                 body );
    }

    return out;
}
RakString RakString::FormatForPOST( const char* uri, const char* contentType, const char* body, const char* extraHeaders )
{
    return FormatForPUTOrPost( "POST", uri, contentType, body, extraHeaders );
}
RakString RakString::FormatForPUT( const char* uri, const char* contentType, const char* body, const char* extraHeaders )
{
    return FormatForPUTOrPost( "PUT", uri, contentType, body, extraHeaders );
}
RakString RakString::FormatForGET( const char* uri, const char* extraHeaders )
{
    RakString out;
    RakString host;
    RakString remotePath;
    RakString header;
    RakString uriRs;
    uriRs = uri;

    uriRs.SplitURI( header, host, remotePath );
    if( host.IsEmpty() || remotePath.IsEmpty() )
        return out;

    if( extraHeaders && extraHeaders[0] )
    {
        out.Set( "GET %s HTTP/1.1\r\n"
                 "%s\r\n"
                 "Host: %s\r\n"
                 "\r\n",
                 remotePath.C_String(),
                 extraHeaders,
                 host.C_String() );
    }
    else
    {
        out.Set( "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "\r\n",
                 remotePath.C_String(),
                 host.C_String() );
    }


    return out;
}
RakString RakString::FormatForDELETE( const char* uri, const char* extraHeaders )
{
    RakString out;
    RakString host;
    RakString remotePath;
    RakString header;
    RakString uriRs;
    uriRs = uri;

    uriRs.SplitURI( header, host, remotePath );
    if( host.IsEmpty() || remotePath.IsEmpty() )
        return out;

    if( extraHeaders && extraHeaders[0] )
    {
        out.Set( "DELETE %s HTTP/1.1\r\n"
                 "%s\r\n"
                 "Content-Length: 0\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 remotePath.C_String(),
                 extraHeaders,
                 host.C_String() );
    }
    else
    {
        out.Set( "DELETE %s HTTP/1.1\r\n"
                 "Content-Length: 0\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 remotePath.C_String(),
                 host.C_String() );
    }

    return out;
}

void RakString::FreeMemory( void )
{
    LockMutex();
    FreeMemoryNoMutex();
    UnlockMutex();
}
void RakString::FreeMemoryNoMutex( void )
{
    for( unsigned int i = 0; i < freeList.Size(); i++ )
    {
        RakNet::OP_DELETE( freeList[i]->refCountMutex, _FILE_AND_LINE_ );
        rakFree_Ex( freeList[i], _FILE_AND_LINE_ );
    }
    freeList.Clear( false, _FILE_AND_LINE_ );
}

void RakString::Clear( void )
{
    Free();
}
void RakString::Allocate( size_t len )
{
    RakString::LockMutex();
    if( RakString::freeList.Size() == 0 )
    {
        unsigned i;
        for( i = 0; i < 128; i++ )
        {
            RakString::SharedString* ss;
            ss = (RakString::SharedString*)rakMalloc_Ex( sizeof( RakString::SharedString ), _FILE_AND_LINE_ );
            ss->refCountMutex = RakNet::OP_NEW<std::mutex>( _FILE_AND_LINE_ );
            RakString::freeList.Insert( ss, _FILE_AND_LINE_ );
        }
    }
    sharedString = RakString::freeList[RakString::freeList.Size() - 1];
    RakString::freeList.RemoveAtIndex( RakString::freeList.Size() - 1 );
    RakString::UnlockMutex();

    const size_t smallStringSize = 128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2;
    sharedString->refCount = 1;
    if( len <= smallStringSize )
    {
        sharedString->bytesUsed = smallStringSize;
        sharedString->c_str = sharedString->smallString;
    }
    else
    {
        sharedString->bytesUsed = len << 1;
        sharedString->bigString = (char*)rakMalloc_Ex( sharedString->bytesUsed, _FILE_AND_LINE_ );
        sharedString->c_str = sharedString->bigString;
    }
}
void RakString::Assign( const char* str )
{
    if( str == 0 || str[0] == 0 )
    {
        sharedString = &emptyString;
        return;
    }

    size_t len = strlen( str ) + 1;
    Allocate( len );
    memcpy( sharedString->c_str, str, len );
}
void RakString::Assign( const char* str, va_list ap )
{
    if( str == 0 || str[0] == 0 )
    {
        sharedString = &emptyString;
        return;
    }

    char stackBuff[512];
    if( _vsnprintf( stackBuff, 512, str, ap ) != -1
#ifndef _WIN32
        // Here Windows will return -1 if the string is too long; Linux just truncates the string.
        && strlen( stackBuff ) < 511
#endif
    )
    {
        Assign( stackBuff );
        return;
    }
    char *buff = 0, *newBuff;
    size_t buffSize = 8096;
    while( 1 )
    {
        newBuff = (char*)rakRealloc_Ex( buff, buffSize, __FILE__, __LINE__ );
        if( newBuff == 0 )
        {
            notifyOutOfMemory( _FILE_AND_LINE_ );
            if( buff != 0 )
            {
                Assign( buff );
                rakFree_Ex( buff, __FILE__, __LINE__ );
            }
            else
            {
                Assign( stackBuff );
            }
            return;
        }
        buff = newBuff;
        if( _vsnprintf( buff, buffSize, str, ap ) != -1 )
        {
            Assign( buff );
            rakFree_Ex( buff, __FILE__, __LINE__ );
            return;
        }
        buffSize *= 2;
    }
}

RakString RakString::NonVariadic( const char* str )
{
    RakString rs;
    rs = str;
    return rs;
}

unsigned long RakString::ToInteger( const RakString& rs )
{
    unsigned long hash = 0;
    int c;

    const char* str = (const char*)rs.C_String();

    while( ( c = *str++ ) )
    {
        hash = c + ( hash << 6 ) + ( hash << 16 ) - hash;
    }

    return hash;
}
int RakString::ReadIntFromSubstring( const char* str, size_t pos, size_t n )
{
    char tmp[32];
    if( n >= 32 )
        return 0;
    for( size_t i = 0; i < n; i++ )
        tmp[i] = str[i + pos];
    return atoi( tmp );
}
void RakString::AppendBytes( const char* bytes, unsigned int count )
{
    if( IsEmpty() )
    {
        Allocate( count );
        memcpy( sharedString->c_str, bytes, count + 1 );
        sharedString->c_str[count] = 0;
    }
    else
    {
        Clone();
        unsigned int length = (unsigned int)GetLength();
        Realloc( sharedString, count + length + 1 );
        memcpy( sharedString->c_str + length, bytes, count );
        sharedString->c_str[length + count] = 0;
    }
}
void RakString::Clone( void )
{
    RakAssert( sharedString != &emptyString );
    if( sharedString == &emptyString )
    {
        return;
    }

    // Empty or solo then no point to cloning
    sharedString->refCountMutex->lock();
    if( sharedString->refCount == 1 )
    {
        sharedString->refCountMutex->unlock();
        return;
    }

    sharedString->refCount--;
    sharedString->refCountMutex->unlock();
    Assign( sharedString->c_str );
}
void RakString::Free( void )
{
    if( sharedString == &emptyString )
        return;
    sharedString->refCountMutex->lock();
    sharedString->refCount--;
    if( sharedString->refCount == 0 )
    {
        sharedString->refCountMutex->unlock();
        const size_t smallStringSize = 128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2;
        if( sharedString->bytesUsed > smallStringSize )
            rakFree_Ex( sharedString->bigString, _FILE_AND_LINE_ );

        RakString::LockMutex();
        RakString::freeList.Insert( sharedString, _FILE_AND_LINE_ );
        RakString::UnlockMutex();

        sharedString = &emptyString;
    }
    else
    {
        sharedString->refCountMutex->unlock();
    }
    sharedString = &emptyString;
}
void RakString::LockMutex( void )
{
    GetPoolMutex().lock();
}
void RakString::UnlockMutex( void )
{
    GetPoolMutex().unlock();
}

} // namespace RakNet

/*
#include "Plugins/RakString.h"
#include <string>
#include "GetTime.h"

using namespace RakNet;

int main(void)
{
    RakString s3("Hello world");
    RakString s5=s3;

    RakString s1;
    RakString s2('a');

    RakString s4("%i %f", 5, 6.0);

    RakString s6=s3;
    RakString s7=s6;
    RakString s8=s6;
    RakString s9;
    s9=s9;
    RakString s10(s3);
    RakString s11=s10 + s4 + s9 + s2;
    s11+=RakString("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    RakString s12("Test");
    s12+=s11;
    bool b1 = s12==s12;
    s11=s5;
    s12.ToUpper();
    RakString s13;
    bool b3 = s13.IsEmpty();
    s13.Set("blah %s", s12.C_String());
    bool b4 = s13.IsEmpty();
    size_t i1=s13.GetLength();
    s3.Clear(_FILE_AND_LINE_);
    s4.Clear(_FILE_AND_LINE_);
    s5.Clear(_FILE_AND_LINE_);
    s5.Clear(_FILE_AND_LINE_);
    RAKNET_DEBUG_PRINTF("\n");

    static const int repeatCount=750;
    DataStructures::List<RakString> rakStringList;
    DataStructures::List<std::string> stdStringList;
    DataStructures::List<char*> referenceStringList;
    char *c;
    unsigned i;
    RakNet::TimeMS beforeReferenceList, beforeRakString, beforeStdString, afterStdString;

    unsigned loop;
    for (loop=0; loop<2; loop++)
    {
        beforeReferenceList=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
        {
            c = RakNet::OP_NEW_ARRAY<char >(56,_FILE_AND_LINE_ );
            strcpy(c, "Aalsdkj alsdjf laksdjf ;lasdfj ;lasjfd");
            referenceStringList.Insert(c);
        }
        beforeRakString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            rakStringList.Insert("Aalsdkj alsdjf laksdjf ;lasdfj ;lasjfd");
        beforeStdString=RakNet::GetTimeMS();

        for (i=0; i < repeatCount; i++)
            stdStringList.Insert("Aalsdkj alsdjf laksdjf ;lasdfj ;lasjfd");
        afterStdString=RakNet::GetTimeMS();
        RAKNET_DEBUG_PRINTF("Insertion 1 Ref=%i Rak=%i, Std=%i\n", beforeRakString-beforeReferenceList, beforeStdString-beforeRakString, afterStdString-beforeStdString);

        beforeReferenceList=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
        {
            RakNet::OP_DELETE_ARRAY(referenceStringList[0], _FILE_AND_LINE_);
            referenceStringList.RemoveAtIndex(0);
        }
        beforeRakString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            rakStringList.RemoveAtIndex(0);
        beforeStdString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            stdStringList.RemoveAtIndex(0);
        afterStdString=RakNet::GetTimeMS();
        RAKNET_DEBUG_PRINTF("RemoveHead Ref=%i Rak=%i, Std=%i\n", beforeRakString-beforeReferenceList, beforeStdString-beforeRakString, afterStdString-beforeStdString);

        beforeReferenceList=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
        {
            c = RakNet::OP_NEW_ARRAY<char >(56, _FILE_AND_LINE_ );
            strcpy(c, "Aalsdkj alsdjf laksdjf ;lasdfj ;lasjfd");
            referenceStringList.Insert(0);
        }
        beforeRakString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            rakStringList.Insert("Aalsdkj alsdjf laksdjf ;lasdfj ;lasjfd");
        beforeStdString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            stdStringList.Insert("Aalsdkj alsdjf laksdjf ;lasdfj ;lasjfd");
        afterStdString=RakNet::GetTimeMS();
        RAKNET_DEBUG_PRINTF("Insertion 2 Ref=%i Rak=%i, Std=%i\n", beforeRakString-beforeReferenceList, beforeStdString-beforeRakString, afterStdString-beforeStdString);

        beforeReferenceList=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
        {
            RakNet::OP_DELETE_ARRAY(referenceStringList[referenceStringList.Size()-1], _FILE_AND_LINE_);
            referenceStringList.RemoveAtIndex(referenceStringList.Size()-1);
        }
        beforeRakString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            rakStringList.RemoveAtIndex(rakStringList.Size()-1);
        beforeStdString=RakNet::GetTimeMS();
        for (i=0; i < repeatCount; i++)
            stdStringList.RemoveAtIndex(stdStringList.Size()-1);
        afterStdString=RakNet::GetTimeMS();
        RAKNET_DEBUG_PRINTF("RemoveTail Ref=%i Rak=%i, Std=%i\n", beforeRakString-beforeReferenceList, beforeStdString-beforeRakString, afterStdString-beforeStdString);

    }

    printf("Done.");
    char str[128];
    Gets(str, sizeof(str));
    return 1;
}
*/