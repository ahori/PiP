/*
 * $RIKEN_copyright:$
 * $PIP_VERSION:$
 * $PIP_license:$
 */
/*
 * Written by Atsushi HORI <ahori@riken.jp>, 2016, 2017
 */

#define PIP_INTERNAL_FUNCS

//#define DEBUG
#include <test.h>

int error = 0;
char tag[64];

#define CONST_VAL	(12345)

int var_static_global;
int init_static_global              = CONST_VAL;
const int const_static_global       = CONST_VAL;

void *var_pointers[10];
static int  nvars;

void set_addrs( void ) {
  int i = 0;

  var_pointers[i++] = (void*) &var_static_global;
  var_pointers[i++] = (void*) &init_static_global;
  var_pointers[i++] = (void*) &const_static_global;
  nvars = i;
}

void check_vars( int pipid ) {
  char *names[] = {
    "var_static_global",
    "init_static_global",
    "const_static_global",
  };
  void **imp, *varp;
  int i;

  TESTINT( pip_get_addr( pipid, "var_pointers", (void**) &imp ) );
  DBGF( "imp@%p", imp );
  for( i=0; i<nvars; i++ ) {
    TESTINT( pip_get_addr( pipid, names[i], &varp ) );
    DBGF( "[%d] %s@%p", pipid, names[i], varp );
    if( imp[i] != varp ) {
      printf( "%20s %32s: %p !!!!=== %p !!!!\n", tag, names[i], imp[i], varp );
      error = 1;
    }
  }
}

pthread_barrier_t	barrier;

int main( int argc, char **argv ) {
  pthread_barrier_t *barrp;
  int pipid = 999;
  int ntasks;
  int i, err;

  if( !pip_isa_piptask() ) {
    if( argc < 2 ) {
      ntasks = NTASKS;
    } else {
      ntasks = atoi( argv[1] );
    }
    TESTINT( pthread_barrier_init( &barrier, NULL, ntasks+1 ) );
    barrp = &barrier;
  }
  set_addrs();
  TESTINT( pip_init( &pipid, &ntasks, (void**) &barrp, 0 ) );
  pip_idstr( tag, 64 );

  if( pipid == PIP_PIPID_ROOT ) {
    for( i=0; i<NTASKS; i++ ) {
      pipid = i;
      err = pip_spawn( argv[0], argv, NULL, i%4, &pipid, NULL, NULL, NULL );
      if( err ) break;
      if( i != pipid ) {
	printf( "pip_spawn(%d!=%d)=%d !!!!!!\n", i, pipid, err );
	break;
      }
    }
    ntasks = i;

    if( argc > 1 ) {
      for( i=0; i<ntasks; i++ ) pthread_barrier_wait( barrp );
    } else {
      pthread_barrier_wait( barrp );
    }
    pthread_barrier_wait( barrp );
    for( i=0; i<ntasks; i++ ) {
      TESTINT( pip_wait( i, NULL ) );
    }

  } else {

    if( argc > 1 ) {
      for( i=0; i<pipid; i++ ) pthread_barrier_wait( barrp );
    } else {
      pthread_barrier_wait( barrp );
    }
    if( argc > 1 ) {
      for( i=pipid; i<ntasks; i++ ) pthread_barrier_wait( barrp );
    }
    check_vars( pipid );
    pthread_barrier_wait( barrp );
    if( !error ) {
      printf( "%s Hello, I am just fine !!\n", tag );
    } else {
      printf( "%s Hello, I am NOT fine !!\n", tag );
    }
    fflush( NULL );
  }
  TESTINT( pip_fin() );
  return 0;
}