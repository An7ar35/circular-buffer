#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

#include "CircularBuffer.h"

//======= VARIABLES =======
#define BYTES        100000
#define WRITE_CHUNKS   1000
#define READ_CHUNKS    1000
#define CBUFFER_SIZE   5000
//=========================

CircularBuffer_t cbuff;

struct {
    pthread_t thread;
    u_int8_t  buffer[BYTES];

} source = {
    .thread = PTHREAD_CREATE_DETACHED,
    .buffer = {}
};

struct {
    pthread_t thread;
    u_int8_t  buffer[BYTES];

} target = {
    .thread = PTHREAD_CREATE_DETACHED,
    .buffer = {}
};

/**
 * Fills a buffer with random data
 * @param buff   Pointer to buffer array
 * @param length Size of buffer
 */
void fillWithRandom( u_int8_t * buff, size_t length ) {
    for( size_t i = 0; i < length; ++i ) {
        buff[i] = rand() % CHAR_MAX;
    }
}

/**
 * Tests that 2 buffers hold the same data
 * @param buff_a Source buffer
 * @param buff_b Target buffer
 * @param length Size of the data
 * @return Equivalent state
 */
bool checkEqual( const u_int8_t * buff_a, const u_int8_t * buff_b, size_t length ) {
    for( size_t i = 0; i < length; ++i ) {
        if( buff_a[i] != buff_b[i] )
            return false;
    }

    return true;
}

/**
 * Prints a buffer to a file
 * @param out    Output file descriptor
 * @param buff   Pointer to buffer array
 * @param length Size of buffer
 */
void printBuffToFile( FILE * out, u_int8_t * buff, size_t length ) {
    for( size_t i = 0; i < length; ++i ) {
        if( i % READ_CHUNKS == 0 )
            fprintf( out, "\n" );

        fprintf( out, "%d ", buff[i] );
    }
}

/**
 * Compares 2 buffers and prints the second buffer's data in colour (red=mismatch, green=same)
 * @param buff_a Pointer to first buffer
 * @param buff_b Pointer to second buffer
 * @param length Size of buffers
 * @return Count of differing data
 */
size_t compareBuff( u_int8_t * buff_a, u_int8_t * buff_b, size_t length ) {
    size_t diff_count = 0;

    for( size_t i = 0; i < length; ++i ) {
        if( i % READ_CHUNKS == 0 )
            fprintf( stdout, "\n" );

        if( buff_a[i] == buff_b[i] ) {
            fprintf( stdout, "\x1b[32m%d\033[0m ", buff_b[i] );
        } else {
            fprintf( stdout, "\x1b[31m%d\033[0m ", buff_b[i] );
            ++diff_count;
        }
    }

    return diff_count;
}

/**
 * Producer method that sends source data to CircularBuffer
 * @return NULL
 */
static void * launchProducer() {
    size_t count = 0;
    while( count < BYTES ) {
        size_t to_write = ( BYTES - count < WRITE_CHUNKS ? ( BYTES - count ) : WRITE_CHUNKS );
#ifndef NDEBUG
        printf( "writing %ldB... %ld->%ld\n", to_write, count, count + to_write );
#endif
        count += CircularBuffer.writeChunk( &cbuff, &source.buffer[count], to_write );
        usleep( rand() % 1000 );
    }

    return NULL;
}

/**
 * Consumer method that reads data from CircularBuffer
 * @return NULL
 */
static void * launchConsumer() {
    size_t count = 0;
    while( count < BYTES ) {
        size_t to_read = ( BYTES - count < READ_CHUNKS ? ( BYTES - count ) : READ_CHUNKS );
#ifndef NDEBUG
        printf( "reading %ldB... %ld->%ld\n", to_read, count, count + to_read );
#endif
        count += CircularBuffer.readChunk( &cbuff, &target.buffer[count], to_read );
        usleep( rand() % 1000 );
    }

    return NULL;
}

/**
 * Run a test
 * @param round Test number
 * @return Success
 */
static bool run( int round ) {
    int ret    = 0;
    FILE * in  = fopen( "in.txt", "w" );
    FILE * out = fopen( "out.txt", "w" );

    fillWithRandom( source.buffer, BYTES );

//    printf( "\n=== IN ====\n" );
//    for( size_t i = 0; i < BYTES; ++i ) {
//        printf( "%d ", source.buffer[i] );
//    }

    cbuff = CircularBuffer.create();
    CircularBuffer.init( &cbuff, CBUFFER_SIZE );

    if( ( ret = pthread_create( &source.thread, NULL, launchProducer, NULL ) ) != 0 ) {
        fprintf( stderr, "Failed to create producer thread (%d)\n", ret );
    }

    if( ( ret = pthread_create( &target.thread, NULL, launchConsumer, NULL ) ) != 0 ) {
        fprintf( stderr, "Failed to create consumer thread (%d)\n", ret );
    }

    pthread_join( target.thread, NULL );
    pthread_join( source.thread, NULL );

    printBuffToFile( in, source.buffer, BYTES );
    printBuffToFile( out, target.buffer, BYTES );

//    printf( "\n=== OUT ====\n" );
//    size_t diff = compareBuff( source.buffer, target.buffer, BYTES );
//    printf( "\n===========\n" );

    bool same = checkEqual( source.buffer, target.buffer, BYTES );

    printf( "ROUND #%d - Buffer check: %s\n", round, ( same ? "SAME" : "NOT SAME" ) );

    fclose( in );
    fclose( out );
    return same;
}

int main() {
    const int test_count = 100;
    int       i          = 1;

    while( run( i++ ) && i <= test_count );

    return 0;
}
