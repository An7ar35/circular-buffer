#include "CircularBuffer.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/**
 * Gets a string representation of the error enum val for pthread returns
 * @param i Error enum integer val
 * @return String
 */
static const char * CircularBuffer_getPThreadErrStr( int i ) {
    switch( i ) {
        case EINVAL:
            return "EINVAL";
        case EBUSY:
            return "EBUSY";
        case EAGAIN:
            return "EAGAIN";
        case EDEADLK:
            return "EDEADLK";
        case EPERM:
            return "EPERM";
        default:
            return "UNKNOWN";
    }
}

/**
 * [PRIVATE] Gets a file descriptor for an anonymous file residing in memory (replica of https://man7.org/linux/man-pages/man2/memfd_create.2.html)
 * @param name  Name of file
 * @param flags Flags
 * @return File descriptor (-1 or error)
 */
static int CircularBuffer_memfd_create( const char * name, unsigned int flags ) {
    long fd   = syscall( __NR_memfd_create, name, flags );
    int  cast = 0;

    if( fd >= 0 ) {
        if( fd <= INT_MAX ) {
            cast = (int) fd;

        } else {
            fprintf( stderr,
                     "[CircularBuffer_memfd_create( \"%s\", %d )] Failed cast long->int (%ld)\n",
                     name, flags, fd
            );

            cast = -1;
        }
    }

    return cast;
}

/**
 * [PRIVATE] Advance the read position
 * @param cbuff Pointer to CircularBuffer_t object
 * @param n     Number of bytes to advance position by
 */
static void CircularBuffer_advanceReadPos( CircularBuffer_t * cbuff, size_t n ) {
    cbuff->position.read = ( ( cbuff->position.read + n ) % cbuff->size );

    if( cbuff->position.read == cbuff->position.write )
        cbuff->empty = true;
}

/**
 * [PRIVATE] Advance the write position
 * @param cbuff Pointer to CircularBuffer_t object
 * @param n     Number of bytes to advance position by
 */
static void CircularBuffer_advanceWritePos( CircularBuffer_t * cbuff, size_t n ) {
    cbuff->position.write = ( ( cbuff->position.write + n ) % cbuff->size );

    if( n )
        cbuff->empty = false;
}

/**
 * Initialises a circular buffer
 * @return Circular buffer object
 */
static CircularBuffer_t CircularBuffer_create( void ) {
    return (CircularBuffer_t) {
        .fd          = 0,
        .buffer      = NULL,
        .size        = 0,
        .mutex       = PTHREAD_MUTEX_INITIALIZER,
        .ready       = PTHREAD_COND_INITIALIZER,
        .empty       = true,
        .position    = { 0, 0 },
    };
}

/**
 * [THREAD-SAFE] Initialises the circular buffer
 * @param cbuff Pointer to CircularBuffer_t object
 * @param size  Required size for buffer
 * @return Success
 */
static bool CircularBuffer_init( CircularBuffer_t * cbuff, size_t size ) {
    /*
     * raw buffer (fd): [##########]
     *                   |        |
     *                   |<------>| (n * page size)
     *                   |        |
     *  virtual buffer: [##########|##########]
     *                   ^        ^ ^        ^
     *                   0        n 0        n
     *                   |          |
     *                   section 1  section 2
     */
    bool   error_state = false;
    size_t real_size   = size;

    if( cbuff == NULL ) {
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] CircularBuffer_t is NULL.\n",
                 cbuff, size
        );

        error_state = true;
        goto end;
    }

    pthread_mutex_lock( &cbuff->mutex );

    if( size < 1 || size > LONG_MAX ) {
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Bad size (0 > size =< %lu).\n",
                 cbuff, size, size, LONG_MAX
        );

        error_state = true;
        goto end;
    }

    { //calculate the actual min size based on the page size
        const size_t whole_pages = ( size / getpagesize() ) + ( size % getpagesize() > 0 ? 1 : 0 );

        real_size = whole_pages * getpagesize();

        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Calculated size: %lu bytes (detected page size: %lu bytes)\n",
                   cbuff,size, real_size, getpagesize()
        );
    }

    if( ( cbuff->fd = CircularBuffer_memfd_create( "circular_buffer", 0 ) ) < 0 ) {
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Failed to create raw buffer file descriptor: %s\n",
                 cbuff, size, strerror( errno )
        );

        error_state = true;
        goto end;
    }

    if( ftruncate( cbuff->fd, real_size ) < 0 ) { //truncate a file to a specified length
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Failed to adjust raw buffer size (%lu): %s\n",
                 cbuff, size, size, strerror( errno )
        );

        error_state = true;
        goto end;
    }

    if( ( cbuff->buffer = mmap( NULL, 2 * real_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 ) ) == MAP_FAILED ) {
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Failed to map raw buffer: %s\n",
                 cbuff, size, strerror( errno )
        );

        error_state = true;
        goto end;
    }

    if( mmap( cbuff->buffer, real_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, cbuff->fd, 0 ) == MAP_FAILED ) {
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Failed to map virtual buffer section 1: %s\n",
                 cbuff, size, strerror( errno )
        );

        error_state = true;
        goto end;
    }

    if( mmap( ( cbuff->buffer + real_size ), real_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, cbuff->fd, 0 ) == MAP_FAILED ) {
        fprintf( stderr,
                 "[CircularBuffer_init( %p, %lu )] Failed to map virtual buffer section 2: %s\n",
                 cbuff, size, strerror( errno )
        );

        error_state = true;
        goto end;
    }

    cbuff->size           = real_size;
    cbuff->position.write = 0;
    cbuff->position.read  = 0;

    end:
        pthread_mutex_unlock( &cbuff->mutex );
        return !( error_state );
}

/**
 * Writes a chunk to the buffer (no arg checks)
 * @param buffer Pointer to CircularBuffer_t object
 * @param src    Source byte buffer
 * @param length Source length in bytes to copy
 * @return Number or bytes written
 */
static size_t CircularBuffer_writeChunk( CircularBuffer_t * cbuff, const u_int8_t * src, size_t length ) {
    size_t bytes_writen = 0;

    pthread_mutex_lock( &cbuff->mutex );

    size_t free_bytes = ( ( cbuff->position.read + cbuff->size - cbuff->position.write ) % cbuff->size );

    if( cbuff->empty || length <= free_bytes ) {
#ifndef NDEBUG
        size_t old_pos = cbuff->position.write;
#endif

        memcpy( &cbuff->buffer[cbuff->position.write], src, length );
        CircularBuffer_advanceWritePos( cbuff, length );
        bytes_writen = length;

#ifndef NDEBUG
        printf( "[CircularBuffer_writeChunk( %p, %p, %lu )] [%ld:'%d'->'%d'] to [%ld/%ld:'%d'->'%d']\n",
                cbuff, src, length,
                old_pos, src[0], cbuff->buffer[old_pos],
                ( old_pos + length ), cbuff->position.write, src[length - 1], cbuff->buffer[old_pos + length - 1] );
#endif
        if( length > 0 ) {
            pthread_cond_signal( &cbuff->ready );
        }

    } else {
        fprintf( stderr,
                 "[CircularBuffer_writeChunk( %p, %p, %lu )] "
                 "Free space too small (%lu). Consider making the buffer larger (%lu).\n",
                 cbuff, src, length,
                 free_bytes, cbuff->size
        );
    }

    pthread_mutex_unlock( &cbuff->mutex );

    return bytes_writen;
}

/**
 * [THREAD-SAFE] Reads a chunk and copies to a buffer
 * @param cbuff  Pointer to CircularBuffer_t object
 * @param target Target buffer
 * @param length Length to read and transfer to buffer
 * @return Actual length read
 */
static size_t CircularBuffer_readChunk( CircularBuffer_t * cbuff, u_int8_t * target, size_t length ) {
    if( cbuff == NULL || target == NULL ) {
        fprintf( stderr,
                 "[CircularBuffer_readChunk( %p, %p, %lu )] Pointer arg is NULL.\n",
                 cbuff, target, length
        );

        return 0; //EARLY RETURN
    }

    int    ret        = 0;
    size_t bytes_read = 0;

    if( ( ret = pthread_mutex_lock( &cbuff->mutex ) ) == 0 ) {
        while( cbuff->empty ) {
            pthread_cond_wait( &cbuff->ready, &cbuff->mutex );
        }

        size_t bytes_available = ( ( cbuff->position.write + cbuff->size ) - cbuff->position.read ) % cbuff->size;
#ifndef NDEBUG
        size_t old_pos = cbuff->position.read;
#endif
        bytes_read = ( bytes_available < length ? bytes_available : length );
        memcpy( target, &cbuff->buffer[cbuff->position.read], bytes_read );
        CircularBuffer_advanceReadPos( cbuff, bytes_read );

#ifndef NDEBUG
        printf( "[CircularBuffer_readChunk( %p, %p, %lu )] [%ld:'%d'] to [%ld/%ld:'%d']   (w: %ld, avail: %lu)\n",
                cbuff, target, length,
                old_pos, cbuff->buffer[old_pos],
                (old_pos + bytes_read), (cbuff->position.read), cbuff->buffer[(old_pos + bytes_read) - 1],
                cbuff->position.write, bytes_available);
#endif
        if( ( ret = pthread_mutex_unlock( &cbuff->mutex ) ) != 0 ) {
            fprintf( stderr,
                     "[CircularBuffer_readChunk( %p, %p, %lu )] Failed to unlock mutex: %s (%d).\n",
                     cbuff, target, length, CircularBuffer_getPThreadErrStr( ret ), ret
            );
        }

    } else {
        fprintf( stderr,
                 "[CircularBuffer_readChunk( %p, %p, %lu )] Failed to lock mutex: %s (%d).\n",
                 cbuff, target, length, CircularBuffer_getPThreadErrStr( ret ), ret
        );
    }

    return bytes_read;
}

/**
 * [THREAD-SAFE] Checks if the buffer is empty
 * @param cbuff Pointer to CircularBuffer_t object
 * @return Empty state
 */
static bool CircularBuffer_empty( CircularBuffer_t * cbuff ) {
    bool empty = true;

    if( cbuff != NULL ) {
        pthread_mutex_lock( &cbuff->mutex );
        empty = cbuff->empty;
        pthread_mutex_unlock( &cbuff->mutex );
    }

    return empty;
}

/**
 * Gets the current buffer size
 * @param cbuff Pointer to CircularBuffer_t object
 * @return Current buffer size in bytes
 */
static size_t CircularBuffer_size( CircularBuffer_t * cbuff ) {
    size_t size = 0;

    if( cbuff != NULL ) {
        size = cbuff->size;

    } else {
        fprintf( stderr,
                 "[CircularBuffer_clear( %p )] CircularBuffer_t is NULL.\n",
                 cbuff
        );
    }

    return size;
}

/**
 * Frees buffer content
 * @param cbuff Pointer to CircularBuffer_t object
 */
static void CircularBuffer_free( CircularBuffer_t * cbuff ) {
    if( cbuff != NULL ) {
        if( cbuff->buffer != NULL && munmap( cbuff->buffer + cbuff->size, cbuff->size ) != 0 ) {
            fprintf( stderr,
                     "[CircularBuffer_free( %p )] Failed unmap virtual buffer 2: %s\n",
                     cbuff, strerror( errno )
            );
        }

        if( cbuff->buffer != NULL && munmap( cbuff->buffer, cbuff->size ) != 0 ) {
            fprintf( stderr,
                     "[CircularBuffer_free( %p )] Failed unmap virtual buffer 1: %s\n",
                     cbuff, strerror( errno )
            );
        }

        if( cbuff->buffer != NULL && close( cbuff->fd ) != 0 ) {
            fprintf( stderr,
                     "[CircularBuffer_free( %p )] Failed close file descriptor: %s\n",
                     cbuff, strerror( errno )
            );
        }

        pthread_mutex_destroy( &cbuff->mutex );
        pthread_cond_destroy( &cbuff->ready );
        cbuff->fd             = 0;
        cbuff->buffer         = NULL;
        cbuff->position.read  = 0;
        cbuff->position.write = 0;
    }
}

/**
 * Namespace constructor
 */
const struct CircularBuffer_Namespace CircularBuffer = {
    .create     = &CircularBuffer_create,
    .init       = &CircularBuffer_init,
    .writeChunk = &CircularBuffer_writeChunk,
    .readChunk  = &CircularBuffer_readChunk,
    .size       = &CircularBuffer_size,
    .empty      = &CircularBuffer_empty,
    .free       = &CircularBuffer_free,
};