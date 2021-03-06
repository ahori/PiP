/*
 * $RIKEN_copyright: 2018 Riken Center for Computational Sceience, 
 * 	  System Software Devlopment Team. All rights researved$
 * $PIP_VERSION: Version 1.0$
 * $PIP_license: <Simplified BSD License>
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of the PiP project.$
 */
/*
 * Written by Atsushi HORI <ahori@riken.jp>, 2016, 2017
 */

#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/wait.h>
#include <sched.h>
#include <malloc.h>
#include <signal.h>
#include <stdarg.h>

//#define PIP_CLONE_AND_DLMOPEN
#define PIP_DLMOPEN_AND_CLONE

#if      defined(PIP_CLONE_AND_DLMOPEN) &&  defined(PIP_DLMOPEN_AND_CLONE)
#error  "defined(PIP_CLONE_AND_DLMOPEN) &&  defined(PIP_DLMOPEN_AND_CLONE)"
#elif   !defined(PIP_CLONE_AND_DLMOPEN) && !defined(PIP_DLMOPEN_AND_CLONE)
#error "!defined(PIP_CLONE_AND_DLMOPEN) && !defined(PIP_DLMOPEN_AND_CLONE)"
#endif

//#define PIP_NO_MALLOPT

//#define DEBUG
//#define PRINT_MAPS
//#define PRINT_FDS

/* the EVAL env. is to measure the time for calling dlmopen() */
//#define EVAL

#define PIP_INTERNAL_FUNCS
#include <pip.h>
#include <pip_util.h>
#include <pip_gdbif.h>

#ifdef EVAL

#define ES(V,F)		\
  do { double __st=pip_gettime(); (F); (V) += pip_gettime()-__st; } while(0)
double time_dlmopen   = 0.0;
double time_load_dso  = 0.0;
double time_load_prog = 0.0;
#define REPORT(V)	 printf( "%s: %g\n", #V, V );

#else

#define ES(V,F)		(F)
#define REPORT(V)

#endif

extern char 		**environ;

/*** note that the following static variables are   ***/
/*** located at each PIP task and the root process. ***/
static pip_root_t	*pip_root = NULL;
static pip_task_t	*pip_task = NULL;
static pip_ulp_t	*pip_ulp  = NULL;

static pip_clone_t*	pip_cloneinfo = NULL;

static int (*pip_clone_mostly_pthread_ptr) (
	pthread_t *newthread,
	int clone_flags,
	int core_no,
	size_t stack_size,
	void *(*start_routine) (void *),
	void *arg,
	pid_t *pidp) = NULL;

struct pip_gdbif_root	*pip_gdbif_root;

int pip_root_p_( void ) {
  return pip_root != NULL && pip_task == NULL;
}

int pip_idstr( char *buf, size_t sz ) {
  int n = 0;

  if( pip_root_p_() ) {
    n = snprintf( buf, sz, "<PIP_ROOT(%d)>", getpid() );
  } else if( pip_task != NULL ) {
    if( pip_task->type == PIP_TYPE_TASK ) {
      n = snprintf( buf, sz, "<PIPID%d(%d)>", pip_task->pipid, getpid() );
    } else if( pip_task->type == PIP_TYPE_ULP ) {
      n = snprintf( buf, sz, "<ULPID%d(%d)>", pip_task->pipid, getpid() );
    }
  }
  if( n == 0 ) n = snprintf( buf, sz, "<(%d)>", getpid() );
  return n;
}

static void pip_message( char *tag, char *format, va_list ap ) {
#define MESGLEN		(512)
  char mesg[MESGLEN];
  char idstr[PIPIDLEN];
  int len;

  len = pip_idstr( idstr, PIPIDLEN );
  len = snprintf( &mesg[0], MESGLEN-len, tag, idstr );
  mesg[len++] = ' ';
  vsnprintf( &mesg[len], MESGLEN-len, format, ap );
  fprintf( stderr, "%s\n", mesg );
}

static void pip_info_mesg( char *format, ... ) __attribute__ ((unused));
static void pip_info_mesg( char *format, ... ) {
  va_list ap;
  va_start( ap, format );
  pip_message( "PIP-INFO%s:", format, ap );
}

static void pip_warn_mesg( char *format, ... ) __attribute__ ((unused));
static void pip_warn_mesg( char *format, ... ) {
  va_list ap;
  va_start( ap, format );
  pip_message( "PIP-WARN%s:", format, ap );
}

static void pip_err_mesg( char *format, ... ) __attribute__ ((unused));
static void pip_err_mesg( char *format, ... ) {
  va_list ap;
  va_start( ap, format );
  pip_message( "PIP-ERROR%s:", format, ap );
}

static int pip_count_vec( char **vecsrc ) {
  int n;

  for( n=0; vecsrc[n]!= NULL; n++ );
	
  return( n );
}

static void pip_set_magic( pip_root_t *root ) {
  memcpy( root->magic, PIP_MAGIC_WORD, PIP_MAGIC_LEN );
}

static int pip_is_magic_ok( pip_root_t *root ) {
  return root != NULL &&
    strncmp( root->magic, PIP_MAGIC_WORD, PIP_MAGIC_LEN ) == 0;
}

static int pip_is_version_ok( pip_root_t *root ) {
  if( root            != NULL        &&
      root->version   == PIP_VERSION &&
      root->root_size == sizeof( pip_root_t ) ) return 1;
  return 0;
}

static int pip_is_root_ok( char *env ) __attribute__ ((unused));
static int pip_is_root_ok( char *env ) {
  DBG;
  if( env == NULL || *env == '\0' ) {
    pip_err_mesg( "No PiP root found" );
    goto error;
  }
  DBG;
  pip_root = (pip_root_t*) strtoll( env, NULL, 16 );
  if( pip_root == NULL ) {
    pip_err_mesg( "Invalid PiP root" );
    goto error;
  }
  DBG;
  if( !pip_is_magic_ok(   pip_root ) ) {
    pip_err_mesg( "%s environment not found", PIP_ROOT_ENV );
    goto error;
  }
  DBG;
  if( !pip_is_version_ok( pip_root ) ) {
    pip_err_mesg( "Version miss-match between root and child" );
    goto error;
  }
  DBG;
  return 0;

 error:
  pip_root = NULL;
  RETURN( EPERM );
}

static int pip_page_alloc( size_t sz, void **allocp ) {
  size_t pgsz;
  if( pip_root == NULL ) {	/* in pip_init(), no pip_root yet */
    pgsz = sysconf( _SC_PAGESIZE );
  } else {
    pgsz = pip_root->page_size;
  }
  sz = ( ( sz + pgsz - 1 ) / pgsz ) * pgsz;
  RETURN( posix_memalign( allocp, pgsz, sz ) );
}

static void pip_init_task_struct( pip_task_t *taskp ) {
  memset( (void*) taskp, 0, offsetof(pip_task_t,boundary) );
  taskp->pipid = PIP_PIPID_NONE;
  taskp->type  = PIP_TYPE_NONE;
}

static int pipid_to_gdbif( int pipid ) {
  switch( pipid ) {
  case PIP_PIPID_ROOT:
    return( PIP_GDBIF_PIPID_ROOT );
  case PIP_PIPID_ANY:
    return( PIP_GDBIF_PIPID_ANY );
  default:
    return( pipid );
  }
}

static void pip_init_gdbif_task_struct(	struct pip_gdbif_task *gdbif_task,
					pip_task_t *task) {
  /* members from task->args are unavailable if PIP_GDBIF_STATUS_TERMINATED */
  gdbif_task->pathname = task->args.prog;
  gdbif_task->realpathname = NULL; /* filled by pip_load_gdbif() later */
  if ( task->args.argv == NULL ) {
    gdbif_task->argc = 0;
  } else {
    gdbif_task->argc = pip_count_vec( task->args.argv );
  }
  gdbif_task->argv = task->args.argv;
  gdbif_task->envv = task->args.envv;

  gdbif_task->handle = task->loaded; /* filled by pip_load_gdbif() later */
  gdbif_task->load_address = NULL; /* filled by pip_load_gdbif() later */
  gdbif_task->exit_code = -1;
  gdbif_task->pid = task->pid;
  gdbif_task->pipid = pipid_to_gdbif( task->pipid );
  gdbif_task->exec_mode =
      (pip_root->opts & PIP_MODE_PROCESS) ? PIP_GDBIF_EXMODE_PROCESS :
      (pip_root->opts & PIP_MODE_PTHREAD) ? PIP_GDBIF_EXMODE_THREAD :
      PIP_GDBIF_EXMODE_NULL;
  gdbif_task->status = PIP_GDBIF_STATUS_NULL;
  gdbif_task->gdb_status = PIP_GDBIF_GDB_DETACHED;
}

static void pip_init_gdbif_root_task_link(struct pip_gdbif_task *gdbif_task) {
  PIP_HCIRCLEQ_INIT(*gdbif_task, task_list);
}

static void pip_link_gdbif_task_struct(	struct pip_gdbif_task *gdbif_task) {
  gdbif_task->root = &pip_gdbif_root->task_root;
  pip_spin_lock( &pip_gdbif_root->lock_root );
  PIP_HCIRCLEQ_INSERT_TAIL(pip_gdbif_root->task_root, gdbif_task, task_list);
  pip_spin_unlock( &pip_gdbif_root->lock_root );
}

/*
 * NOTE: pip_load_gdbif() won't be called for PiP root tasks.
 * thus, load_address and realpathname are both NULL for them.
 * handle is available, though.
 *
 * because this function is only for PIP-gdb,
 * this does not return any error, but warn.
 */
static void pip_load_gdbif( pip_task_t *task ) {
  struct pip_gdbif_task *gdbif_task = task->gdbif_task;
  Dl_info dli;
  char buf[PATH_MAX];

  gdbif_task->handle = task->loaded;

  if( !dladdr( task->symbols.main, &dli ) ) {
    pip_warn_mesg( "dladdr(%s) failure"
		   " - PIP-gdb won't work with this PiP task %d",
		   task->args.prog, task->pipid );
    gdbif_task->load_address = NULL;
  } else {
    gdbif_task->load_address = dli.dli_fbase;
  }

  /* dli.dli_fname is same with task->args.prog and may be a relative path */
  if( realpath( task->args.prog, buf ) == NULL ) {
    gdbif_task->realpathname = NULL; /* give up */
    pip_warn_mesg( "realpath(%s): %s"
		   " - PIP-gdb won't work with this PiP task %d",
		   task->args.prog, strerror( errno ), task->pipid );
  } else {
    gdbif_task->realpathname = strdup( buf );
    if( gdbif_task->realpathname == NULL ) { /* give up */
      pip_warn_mesg( "strdup(%s) failure"
		     " - PIP-gdb won't work with this PiP task %d",
		     task->args.prog, task->pipid );
    }
  }
}

#include <elf.h>

static int pip_check_pie( char *path ) {
  Elf64_Ehdr elfh;
  int fd;
  int err = 0;

  if( ( fd = open( path, O_RDONLY ) ) < 0 ) {
    err = errno;
  } else {
    if( read( fd, &elfh, sizeof( elfh ) ) != sizeof( elfh ) ) {
      pip_warn_mesg( "Unable to read '%s'", path );
      err = EUNATCH;
    } else if( elfh.e_ident[EI_MAG0] != ELFMAG0 ||
	       elfh.e_ident[EI_MAG1] != ELFMAG1 ||
	       elfh.e_ident[EI_MAG2] != ELFMAG2 ||
	       elfh.e_ident[EI_MAG3] != ELFMAG3 ) {
      pip_warn_mesg( "'%s' is not an ELF file", path );
      err = EUNATCH;
    } else if( elfh.e_type != ET_DYN ) {
      pip_warn_mesg( "'%s' is not DYNAMIC (PIE)", path );
      err = ELIBEXEC;
    }
    (void) close( fd );
  }
  return err;
}

const char *pip_get_mode_str( void ) {
  char *mode;

  if( pip_root == NULL ) return NULL;
  switch( pip_root->opts & PIP_MODE_MASK ) {
  case PIP_MODE_PTHREAD:
    mode = PIP_ENV_MODE_PTHREAD;
    break;
  case PIP_MODE_PROCESS:
    mode = PIP_ENV_MODE_PROCESS;
    break;
  case PIP_MODE_PROCESS_PRELOAD:
    mode = PIP_ENV_MODE_PROCESS_PRELOAD;
    break;
  case PIP_MODE_PROCESS_PIPCLONE:
    mode = PIP_ENV_MODE_PROCESS_PIPCLONE;
    break;
  default:
    mode = "(unknown)";
  }
  return mode;
}

static void *pip_dlsym( void *handle, const char *name ) {
  void *addr;
  pip_spin_lock( &pip_root->lock_ldlinux );
  do {
    (void) dlerror();		/* reset error status */
    if( ( addr = dlsym( handle, name ) ) == NULL ) {
      DBGF( "dlsym(%p,%s): %s", handle, name, dlerror() );
    }
  } while( 0 );
  pip_spin_unlock( &pip_root->lock_ldlinux );
  return( addr );
}

static void pip_dlclose( void *handle ) {
#ifdef AH
  pip_spin_lock( &pip_root->lock_ldlinux );
  do {
    dlclose( handle );
  } while( 0 );
  pip_spin_unlock( &pip_root->lock_ldlinux );
#endif
}

static int pip_check_opt_and_env( int *optsp ) {
  int opts   = *optsp;
  int mode   = ( opts & PIP_MODE_MASK );
  int newmod = 0;
  char *env  = getenv( PIP_ENV_MODE );

  enum PIP_MODE_BITS {
    PIP_MODE_PTHREAD_BIT          = 1,
    PIP_MODE_PROCESS_PRELOAD_BIT  = 2,
    PIP_MODE_PROCESS_PIPCLONE_BIT = 4
  } desired = 0;

  if( ( opts & ~PIP_VALID_OPTS ) != 0 ) {
    /* unknown option(s) specified */
    RETURN( EINVAL );
  }

  if( opts & PIP_MODE_PTHREAD &&
      opts & PIP_MODE_PROCESS ) RETURN( EINVAL );
  if( opts & PIP_MODE_PROCESS ) {
    if( ( opts & PIP_MODE_PROCESS_PRELOAD  ) == PIP_MODE_PROCESS_PRELOAD &&
	( opts & PIP_MODE_PROCESS_PIPCLONE ) == PIP_MODE_PROCESS_PIPCLONE){
      RETURN (EINVAL );
    }
  }

  switch( mode ) {
  case 0:
    if( env == NULL ) {
      desired =
	PIP_MODE_PTHREAD_BIT |
	PIP_MODE_PROCESS_PRELOAD_BIT |
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_THREAD  ) == 0 ||
	       strcasecmp( env, PIP_ENV_MODE_PTHREAD ) == 0 ) {
      desired = PIP_MODE_PTHREAD_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS ) == 0 ) {
      desired =
	PIP_MODE_PROCESS_PRELOAD_BIT|
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PRELOAD  ) == 0 ) {
      desired = PIP_MODE_PROCESS_PRELOAD_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PIPCLONE ) == 0 ) {
      desired = PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else {
      pip_warn_mesg( "unknown environment setting PIP_MODE='%s'", env );
      RETURN( EPERM );
    }
    break;
  case PIP_MODE_PTHREAD:
    desired = PIP_MODE_PTHREAD_BIT;
    break;
  case PIP_MODE_PROCESS:
    if ( env == NULL ) {
      desired =
	PIP_MODE_PROCESS_PRELOAD_BIT|
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PRELOAD  ) == 0 ) {
      desired = PIP_MODE_PROCESS_PRELOAD_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_PROCESS_PIPCLONE ) == 0 ) {
      desired = PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else if( strcasecmp( env, PIP_ENV_MODE_THREAD  ) == 0 ||
	       strcasecmp( env, PIP_ENV_MODE_PTHREAD ) == 0 ||
	       strcasecmp( env, PIP_ENV_MODE_PROCESS ) == 0 ) {
      /* ignore PIP_MODE=thread in this case */
      desired =
	PIP_MODE_PROCESS_PRELOAD_BIT|
	PIP_MODE_PROCESS_PIPCLONE_BIT;
    } else {
      pip_warn_mesg( "unknown environment setting PIP_MODE='%s'", env );
      RETURN( EPERM );
    }
    break;
  case PIP_MODE_PROCESS_PRELOAD:
    desired = PIP_MODE_PROCESS_PRELOAD_BIT;
    break;
  case PIP_MODE_PROCESS_PIPCLONE:
    desired = PIP_MODE_PROCESS_PIPCLONE_BIT;
    break;
  default:
    pip_warn_mesg( "pip_init() invalid argument opts=0x%x", opts );
    RETURN( EINVAL );
  }

  if( desired & PIP_MODE_PROCESS_PRELOAD_BIT ) {
    /* check if the __clone() systemcall wrapper exists or not */
    if( pip_cloneinfo == NULL ) {
      pip_cloneinfo = (pip_clone_t*) dlsym( RTLD_DEFAULT, "pip_clone_info");
    }
    DBGF( "cloneinfo-%p", pip_cloneinfo );
    if( pip_cloneinfo != NULL ) {
      newmod = PIP_MODE_PROCESS_PRELOAD;
      goto done;
    } else if( !( desired & ( PIP_MODE_PTHREAD_BIT |
			      PIP_MODE_PROCESS_PIPCLONE_BIT ) ) ) {
      /* no wrapper found */
      if( ( env = getenv( "LD_PRELOAD" ) ) == NULL ) {
	pip_warn_mesg( "process:preload mode is requested but "
		       "LD_PRELOAD environment variable is empty." );
      } else {
	pip_warn_mesg( "process:preload mode is requested but "
		       "LD_PRELOAD='%s'",
		       env );
      }
      RETURN( EPERM );
    }
  }
  if( desired & PIP_MODE_PROCESS_PIPCLONE_BIT ) {
    if ( pip_clone_mostly_pthread_ptr == NULL )
      pip_clone_mostly_pthread_ptr =
	dlsym( RTLD_DEFAULT, "pip_clone_mostly_pthread" );
    if ( pip_clone_mostly_pthread_ptr != NULL ) {
      newmod = PIP_MODE_PROCESS_PIPCLONE;
      goto done;
    } else if( !( desired & PIP_MODE_PTHREAD_BIT) ) {
      if( desired & PIP_MODE_PROCESS_PRELOAD_BIT ) {
	pip_warn_mesg("process mode is requested but pip_clone_info symbol "
		      "is not found in $LD_PRELOAD and "
		      "pip_clone_mostly_pthread() symbol is not found in "
		      "glibc" );
      } else {
	pip_warn_mesg( "process:pipclone mode is requested but "
		       "pip_clone_mostly_pthread() is not found in glibc" );
      }
      RETURN( EPERM );
    }
  }
  if( desired & PIP_MODE_PTHREAD_BIT ) {
    newmod = PIP_MODE_PTHREAD;
    goto done;
  }
  if( newmod == 0 ) {
    pip_warn_mesg( "pip_init() implemenation error. desired = 0x%x", desired );
    RETURN( EPERM );
  }
 done:
  if( ( opts & ~PIP_MODE_MASK ) == 0 ) {
    if( ( env = getenv( PIP_ENV_OPTS ) ) != NULL ) {
      if( strcasecmp( env, PIP_ENV_OPTS_FORCEEXIT ) == 0 ) {
	opts |= PIP_OPT_FORCEEXIT;
      } else {
	pip_warn_mesg( "Unknown option %s=%s", PIP_ENV_OPTS, env );
	RETURN( EPERM );
      }
    }
  }
  *optsp = ( opts & ~PIP_MODE_MASK ) | newmod;
  RETURN( 0 );
}

int pip_init( int *pipidp, int *ntasksp, void **rt_expp, int opts ) {
  size_t	sz;
  char		*envroot = NULL;
  char		*envtask = NULL;
  int		ntasks;
  int 		pipid;
  int 		i, err = 0;
  struct pip_gdbif_root *gdbif_root;

  if( pip_root != NULL ) RETURN( EBUSY ); /* already initialized */

  if( ( envroot = getenv( PIP_ROOT_ENV ) ) == NULL ) {
    /* root process ? */

    if( ntasksp == NULL ) {
      ntasks = PIP_NTASKS_MAX;
    } else if( *ntasksp <= 0 ) {
      RETURN( EINVAL );
    } else {
      ntasks = *ntasksp;
    }
    if( ntasks > PIP_NTASKS_MAX ) RETURN( EOVERFLOW );

    if( ( err = pip_check_opt_and_env( &opts ) ) != 0 ) RETURN( err );

    sz = sizeof( pip_root_t ) + sizeof( pip_task_t ) * ( ntasks + 1 );
    if( ( err = pip_page_alloc( sz, (void**) &pip_root ) ) != 0 ) {
      RETURN( err );
    }
    pip_task = NULL;
    (void) memset( pip_root, 0, sz );
    pip_root->root_size = sizeof( pip_root_t );
    pip_root->size      = sz;

    DBGF( "ROOTROOT (%p)", pip_root );

    pip_spin_init( &pip_root->lock_ldlinux     );
    pip_spin_init( &pip_root->lock_stack_flist );
    pip_spin_init( &pip_root->lock_tasks       );
    /* beyond this point, we can call the       */
    /* pip_dlsymc() and pip_dlclose() functions */

    pipid = PIP_PIPID_ROOT;
    pip_set_magic( pip_root );
    pip_root->version   = PIP_VERSION;
    pip_root->ntasks    = ntasks;
    pip_root->cloneinfo = pip_cloneinfo;
    pip_root->opts      = opts;
    pip_root->page_size = sysconf( _SC_PAGESIZE );
    pip_root->task_root = &pip_root->tasks[ntasks];
    for( i=0; i<ntasks+1; i++ ) {
      pip_init_task_struct( &pip_root->tasks[i] );
    }
    pip_root->task_root->pipid             = pipid;
    pip_root->task_root->type              = PIP_TYPE_ROOT;
    pip_root->task_root->symbols.add_stack = (add_stack_user_t)
      pip_dlsym( RTLD_DEFAULT, "pip_pthread_add_stack_user");
    pip_root->task_root->symbols.free      = (free_t) pip_dlsym( RTLD_DEFAULT, "free");
    pip_root->task_root->loaded            = dlopen( NULL, RTLD_NOW );
    pip_root->task_root->thread            = pthread_self();
    pip_root->task_root->pid               = getpid();
    if( rt_expp != NULL ) {
      pip_root->task_root->export          = *rt_expp;
    }
    pip_spin_init( &pip_root->task_root->lock_malloc );
    unsetenv( PIP_ROOT_ENV );

    sz = sizeof( *gdbif_root ) + sizeof( gdbif_root->tasks[0] ) * ntasks;
    if( ( err = pip_page_alloc( sz, (void**) &gdbif_root ) ) != 0 ) {
      RETURN( err );
    }
    gdbif_root->hook_before_main = NULL; /* XXX */
    gdbif_root->hook_after_main = NULL; /* XXX */
    pip_spin_init( &gdbif_root->lock_free );
    PIP_SLIST_INIT(&gdbif_root->task_free);
    pip_spin_init( &gdbif_root->lock_root );
    pip_init_gdbif_task_struct( &gdbif_root->task_root, pip_root->task_root );
    pip_init_gdbif_root_task_link( &gdbif_root->task_root );
    gdbif_root->task_root.status = PIP_GDBIF_STATUS_CREATED;
    pip_gdbif_root = gdbif_root; /* assign after initialization completed */

    DBGF( "PiP Execution Mode: %s", pip_get_mode_str() );

  } else if( ( envtask = getenv( PIP_TASK_ENV ) ) != NULL ) {
    /* child task */

    if( ( err = pip_is_root_ok( envroot ) ) != 0 ) RETURN( err );
    pipid = (int) strtoll( envtask, NULL, 10 );
    if( pipid >= pip_root->ntasks ) {
      pip_err_mesg( "Invalid PiP task (pipid=%d)" );
      RETURN( EPERM );
    }
    pip_task = &pip_root->tasks[pipid];
    ntasks = pip_root->ntasks;
    if( ntasksp != NULL ) *ntasksp = ntasks;
    if( rt_expp != NULL ) *rt_expp = (void*) pip_root->task_root->export;

    unsetenv( PIP_ROOT_ENV );
    unsetenv( PIP_TASK_ENV );

  } else {
    RETURN( EPERM );
  }
  /* root and child */
  if( pipidp != NULL ) *pipidp = pipid;
  DBGF( "pip_root=%p  pip_task=%p", pip_root, pip_task );

  RETURN( err );
}

static int pip_is_pthread_( void ) {
  return (pip_root->opts & PIP_MODE_PTHREAD) != 0 ? CLONE_THREAD : 0;
}

int pip_is_pthread( int *flagp ) {
  if( pip_root == NULL ) RETURN( EPERM  );
  if( flagp    == NULL ) RETURN( EINVAL );
  *flagp = pip_is_pthread_();
  RETURN( 0 );
}

static int pip_is_shared_fd_( void ) {
  if( pip_root->cloneinfo == NULL )
    return (pip_root->opts & PIP_MODE_PTHREAD) != 0 ? CLONE_FILES : 0;
  return pip_root->cloneinfo->flag_clone & CLONE_FILES;
}

int pip_is_shared_fd( int *flagp ) {
  if( pip_root == NULL ) RETURN( EPERM  );
  if( flagp    == NULL ) RETURN( EINVAL );
  *flagp = pip_is_shared_fd_();
  RETURN( 0 );
}

int pip_is_shared_sighand( int *flagp ) {
  if( pip_root == NULL ) RETURN( EPERM  );
  if( flagp    == NULL ) RETURN( EINVAL );
  if( pip_root->cloneinfo == NULL ) {
    *flagp = (pip_root->opts & PIP_MODE_PTHREAD) != 0 ? CLONE_SIGHAND : 0;
  } else {
    *flagp = pip_root->cloneinfo->flag_clone & CLONE_SIGHAND;
  }
  RETURN( 0 );
}

int pip_isa_piptask( void ) {
  /* this function might be called before calling pip_init() */
  return getenv( PIP_ROOT_ENV ) != NULL;
}

static int pip_task_p_( void ) {
  return pip_task != NULL;
}

static int pip_check_pipid( int *pipidp ) {
  int pipid = *pipidp;

  if( pipid >= pip_root->ntasks ) RETURN( EINVAL );
  if( pipid != PIP_PIPID_MYSELF &&
      pipid <  PIP_PIPID_ROOT   ) RETURN( EINVAL );
  if( pip_root == NULL          ) RETURN( EPERM  );
  switch( pipid ) {
  case PIP_PIPID_ROOT:
    break;
  case PIP_PIPID_ANY:
    RETURN( EINVAL );
  case PIP_PIPID_MYSELF:
    if( pip_root_p_() ) {
      *pipidp = PIP_PIPID_ROOT;
    } else {
      *pipidp = pip_task->pipid;
    }
    break;
  }
  RETURN( 0 );
}

static pip_task_t *pip_get_task_( int pipid ) {
  pip_task_t 	*task = NULL;

  switch( pipid ) {
  case PIP_PIPID_ROOT:
    task = pip_root->task_root;
    break;
  default:
    if( pipid >= 0 && pipid < pip_root->ntasks ) {
      task = &pip_root->tasks[pipid];
    }
    break;
  }
  return task;
}

int pip_get_dso( int pipid, void **loaded ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  task = pip_get_task_( pipid );
  if( loaded != NULL ) *loaded = task->loaded;
  RETURN( 0 );
}

int pip_isa_ulp( void ) {
  return pip_ulp != NULL;
}

int pip_get_pipid_( void ) {
  int pipid;
  if( pip_root == NULL ) {
    pipid = PIP_PIPID_ANY;
  } else if( pip_root_p_() ) {
    pipid = PIP_PIPID_ROOT;
  } else if( !pip_isa_ulp() ) {
    pipid = pip_task->pipid;
  } else {
    pipid = pip_ulp->pipid;
  }
  return pipid;
}

int pip_get_pipid( int *pipidp ) {
  if( pipidp == NULL ) RETURN( EINVAL );
  *pipidp = pip_get_pipid_();
  RETURN( 0 );
}

int pip_get_ntasks( int *ntasksp ) {
  if( ntasksp  == NULL ) RETURN( EINVAL );
  if( pip_root == NULL ) return( EPERM  ); /* intentionally using small return */
  *ntasksp = pip_root->ntasks_curr;
  RETURN( 0 );
}

static pip_task_t *pip_get_myself( void ) {
  pip_task_t *task;
  if( pip_root_p_() ) {
    task = pip_root->task_root;
  } else {
    task = pip_task;
  }
  return task;
}

int pip_export( void *export ) {
  if( export == NULL ) RETURN( EINVAL );
  pip_get_myself()->export = export;
  RETURN( 0 );
}

int pip_import( int pipid, void **exportp  ) {
  pip_task_t *task;
  int err;

  if( exportp == NULL ) RETURN( EINVAL );
  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );

  task = pip_get_task_( pipid );
  *exportp = (void*) task->export;
  pip_memory_barrier();
  RETURN( 0 );
}

int pip_get_addr( int pipid, const char *name, void **addrp ) {
  void *handle;
  int err;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( name == NULL || addrp == NULL            ) RETURN( EINVAL );
  DBGF( "pipid=%d", pipid );
  if( pipid == PIP_PIPID_ROOT ) {;
    *addrp = pip_dlsym( pip_root->task_root->loaded, name );
  } else if( pipid == PIP_PIPID_MYSELF ) {
    *addrp = pip_dlsym( pip_task->loaded, name );
  } else if( ( handle = pip_root->tasks[pipid].loaded ) != NULL ) {
    *addrp = pip_dlsym( handle, name );
    /* FIXME: pip_dlsym() has a lock but this does not prevent for user */
    /*        programs to directly call dl*() functions without lock    */
    DBGF( "=%p", *addrp );
  } else {
    DBG;
    err = ESRCH;		/* tentative */
  }
  RETURN( err );
}

static char **pip_copy_vec3( char *addition0,
			     char *addition1,
			     char *addition2,
			     char **vecsrc ) {
  char 		**vecdst, *p;
  size_t	vecln, veccc, sz;
  int 		i, j;

  vecln = 0;
  veccc = 0;
  if( addition0 != NULL ) {
    vecln ++;
    veccc += strlen( addition0 ) + 1;
  }
  if( addition1 != NULL ) {
    vecln ++;
    veccc += strlen( addition1 ) + 1;
  }
  if( addition2 != NULL ) {
    vecln ++;
    veccc += strlen( addition2 ) + 1;
  }
  for( i=0; vecsrc[i]!=NULL; i++ ) {
    vecln ++;
    veccc += strlen( vecsrc[i] ) + 1;
  }
  vecln ++;		/* plus final NULL */

  sz = ( sizeof(char*) * vecln ) + veccc;
  if( ( vecdst = (char**) malloc( sz ) ) == NULL ) return NULL;
  p = ((char*)vecdst) + ( sizeof(char*) * vecln );
  i = j = 0;
  if( addition0 ) {
    vecdst[j++] = p;
    p = stpcpy( p, addition0 ) + 1;
  }
  ASSERT( ( (intptr_t)p < (intptr_t)(vecdst+sz) ) );
  if( addition1 ) {
    vecdst[j++] = p;
    p = stpcpy( p, addition1 ) + 1;
  }
  ASSERT( ( (intptr_t)p < (intptr_t)(vecdst+sz) ) );
  for( i=0; vecsrc[i]!=NULL; i++ ) {
    vecdst[j++] = p;
    p = stpcpy( p, vecsrc[i] ) + 1;
    ASSERT( ( (intptr_t)p < (intptr_t)(vecdst+sz) ) );
  }
  vecdst[j] = NULL;

  if( 0 ) {
    int ii;
    for( ii=0; vecsrc[ii]!=NULL; ii++ ) {
      fprintf( stderr, "<<SRC>> vec[%d] %s\n", ii, vecsrc[ii] );
    }
    for( ii=0; vecdst[ii]!=NULL; ii++ ) {
      fprintf( stderr, "<<DST>> vec[%d] %s\n", ii, vecdst[ii] );
    }
  }
  return( vecdst );
}

static char **pip_copy_vec( char **vecsrc ) {
  return pip_copy_vec3( NULL, NULL, NULL, vecsrc );
}

static char **pip_copy_env( char **envsrc, int pipid ) {
  char rootenv[128];
  char taskenv[128];
  char *preload_env = getenv( "LD_PRELOAD" );

  if( sprintf( rootenv, "%s=%p", PIP_ROOT_ENV, pip_root ) <= 0 ||
      sprintf( taskenv, "%s=%d", PIP_TASK_ENV, pipid    ) <= 0 ) {
    return NULL;
  }
  return pip_copy_vec3( rootenv, taskenv, preload_env, envsrc );
}

static size_t pip_stack_size( void ) {
  char 		*env, *endptr;
  size_t 	sz, scale;
  int 		i;

  if( ( sz = pip_root->stack_size ) == 0 ) {
    if( ( env = getenv( PIP_ENV_STACKSZ ) ) == NULL &&
	( env = getenv( "KMP_STACKSIZE" ) ) == NULL &&
	( env = getenv( "OMP_STACKSIZE" ) ) == NULL ) {
      sz = PIP_ULP_STACK_SIZE;	/* default */
    } else if( ( sz = (size_t) strtol( env, &endptr, 10 ) ) <= 0 ) {
      pip_warn_mesg( "stacksize: '%s' is illegal and "
		     "default size (%d KiB) is set",
		     env,
		     PIP_ULP_STACK_SIZE / 1024 );
      sz = PIP_ULP_STACK_SIZE;	/* default */
    } else {
      scale = 1;
      switch( *endptr ) {
      case 'T': case 't':
	scale *= 1024;
      case 'G': case 'g':
	scale *= 1024;
      case 'M': case 'm':
	scale *= 1024 * 1024;
	sz *= scale;
	break;
      default:
	pip_warn_mesg( "stacksize: '%s' is illegal and 'K' is assumed", env );
      case 'K': case 'k': case '\0':
	scale *= 1024;
      case 'B': case 'b':
	sz *= scale;
	for( i=PIP_ULP_MIN_STACK_SIZE; i<sz; i*=2 );
	sz = i;
	break;
      }
    }
    pip_root->stack_size = sz;
  }
  return sz;
}

int pip_is_coefd( int fd ) {
  int flags = fcntl( fd, F_GETFD );
  return( flags > 0 && FD_CLOEXEC );
}

static void pip_close_on_exec( void ) {
  DIR *dir;
  struct dirent *direntp;
  int fd;

#ifdef PRINT_FDS
  pip_print_fds();
#endif

#define PROCFD_PATH		"/proc/self/fd"
  if( ( dir = opendir( PROCFD_PATH ) ) != NULL ) {
    int fd_dir = dirfd( dir );
    while( ( direntp = readdir( dir ) ) != NULL ) {
      if( ( fd = atoi( direntp->d_name ) ) >= 0 &&
	  fd != fd_dir && pip_is_coefd( fd ) ) {
#ifdef DEBUG
	pip_print_fd( fd );
#endif
	(void) close( fd );
	DBGF( "<PID=%d> fd[%d] is closed (CLOEXEC)", getpid(), fd );
      }
    }
    (void) closedir( dir );
    (void) close( fd_dir );
  }
#ifdef PRINT_FDS
  pip_print_fds();
#endif
}

static int pip_load_dso( void **handlep, char *path ) {
  Lmid_t	lmid;
  int 		flags = RTLD_NOW | RTLD_LOCAL;
  /* RTLD_GLOBAL is NOT accepted and dlmopen() returns EINVAL */
  void 		*loaded;
  int		err;

  DBGF( "handle=%p", *handlep );
  if( *handlep == NULL ) {
    lmid = LM_ID_NEWLM;
  } else if( dlinfo( *handlep, RTLD_DI_LMID, (void*) &lmid ) != 0 ) {
    DBGF( "dlinfo(%p): %s", handlep, dlerror() );
    RETURN( ENXIO );
  }
  DBGF( "calling dlmopen(%s)", path );
  ES( time_dlmopen, ( loaded = dlmopen( lmid, path, flags ) ) );
  if( pip_root->task_root->symbols.add_stack != NULL ) {
    //pip_root->task_root->symbols.add_stack();
  }
  DBG;
  if( loaded == NULL ) {
    if( ( err = pip_check_pie( path ) ) != 0 ) RETURN( err );
    pip_warn_mesg( "dlmopen(%s): %s", path, dlerror() );
    RETURN( ENOEXEC );
  } else {
    DBGF( "dlmopen(%s): SUCCEEDED", path );
    *handlep = loaded;
  }
  RETURN( 0 );
}

static int pip_find_symbols( void *handle, pip_symbols_t *symp ) {
  int err = 0;

  //if( pip_root_p() ) pip_print_dsos();

  /* functions */
  symp->main          = dlsym( handle, "main"                         );
#ifdef PIP_PTHREAD_INIT
  symp->pthread_init  = dlsym( handle, "__pthread_initialize_minimal" );
#endif
  symp->ctype_init    = dlsym( handle, "__ctype_init"                 );
  symp->glibc_init    = dlsym( handle, "glibc_init"                   );
  symp->add_stack     = dlsym( handle, "pip_pthread_add_stack_user"   );
  symp->mallopt       = dlsym( handle, "mallopt"                      );
  symp->libc_fflush   = dlsym( handle, "fflush"                       );
  symp->free          = dlsym( handle, "free"                         );
  /* variables */
  symp->environ       = dlsym( handle, "environ"         );
  symp->libc_argvp    = dlsym( handle, "__libc_argv"     );
  symp->libc_argcp    = dlsym( handle, "__libc_argc"     );
  symp->progname      = dlsym( handle, "__progname"      );
  symp->progname_full = dlsym( handle, "__progname_full" );

  /* check mandatory symbols */
  if( symp->main == NULL || symp->environ == NULL ) {
    pip_warn_mesg( "Unable to find main (not linked with '-rdynamic' option?)" );
    err = ENOEXEC;
  } else if( symp->environ == NULL ) {
    err = ENOEXEC;
  } else {
#ifdef DEBUG
    //pip_check_addr( "MAIN", symp->main );
    //pip_check_addr( "ENVP", symp->environ );
#endif
  }
  RETURN( err );
}

static int pip_load_prog( char *prog, pip_task_t *task ) {
  void		*loaded = NULL;
  int 		err;

  DBGF( "prog=%s", prog );

#ifdef PRINT_MAPS
  pip_print_maps();
#endif
  ES( time_load_dso, ( err = pip_load_dso( &loaded, prog ) ) );
#ifdef PRINT_MAPS
  pip_print_maps();
#endif
  DBG;
  if( err == 0 ) {
    err = pip_find_symbols( loaded, &task->symbols );
    DBG;
    if( err != 0 ) {
      (void) dlclose( loaded );
    } else {
      DBG;
      task->loaded = loaded;
      pip_load_gdbif( task );
    }
  }
  RETURN( err );
}

#ifdef PIP_DLMOPEN_AND_CLONE
static int pip_do_corebind( int coreno, cpu_set_t *oldsetp ) {
  int err = 0;

  if( coreno != PIP_CPUCORE_ASIS ) {
    cpu_set_t cpuset;

    CPU_ZERO( &cpuset );
    CPU_SET( coreno, &cpuset );

    if( pip_is_pthread_() ) {
      err = pthread_getaffinity_np( pthread_self(),
				    sizeof(cpu_set_t),
				    oldsetp );
      if( err == 0 ) {
	err = pthread_setaffinity_np( pthread_self(),
				      sizeof(cpu_set_t),
				      &cpuset );
      }
    } else {
      if( sched_getaffinity( 0, sizeof(cpuset), oldsetp ) != 0 ||
	  sched_setaffinity( 0, sizeof(cpuset), &cpuset ) != 0 ) {
	err = errno;
      }
    }
  }
  RETURN( err );
}

static int pip_undo_corebind( int coreno, cpu_set_t *oldsetp ) {
  int err = 0;

  if( coreno != PIP_CPUCORE_ASIS ) {
    if( pip_is_pthread_() ) {
      err = pthread_setaffinity_np( pthread_self(),
				    sizeof(cpu_set_t),
				    oldsetp );
    } else {
      if( sched_setaffinity( 0, sizeof(cpu_set_t), oldsetp ) != 0 ) {
	err = errno;
      }
    }
  }
  RETURN( err );
}
#endif

static int pip_corebind( int coreno ) {
  cpu_set_t cpuset;

  if( coreno != PIP_CPUCORE_ASIS &&
      coreno >= 0                &&
      coreno <  sizeof(cpuset) * 8 ) {
    DBG;
    CPU_ZERO( &cpuset );
    DBG;
    CPU_SET( coreno, &cpuset );
    DBG;
    if( sched_setaffinity( 0, sizeof(cpuset), &cpuset ) != 0 ) RETURN( errno );
    DBG;
  } else {
    DBGF( "coreno=%d", coreno );
  }
  DBG;
  RETURN( 0 );
}

static int pip_init_glibc( pip_symbols_t *symbols,
			   char **argv,
			   char **envv,
			   void *loaded,
			   int flag ) {
  int argc = pip_count_vec( argv );

  if( symbols->progname != NULL ) {
    char *p;
    if( ( p = strrchr( argv[0], '/' ) ) == NULL) {
      *symbols->progname = argv[0];
    } else {
      *symbols->progname = p + 1;
    }
  }
  if( symbols->progname_full != NULL ) {
    *symbols->progname_full = argv[0];
  }
  if( symbols->libc_argcp != NULL ) {
    DBGF( "&__libc_argc=%p", symbols->libc_argcp );
    *symbols->libc_argcp = argc;
  }
  if( symbols->libc_argvp != NULL ) {
    DBGF( "&__libc_argv=%p", symbols->libc_argvp );
    *symbols->libc_argvp = argv;
  }
  *symbols->environ = envv;	/* setting environment vars */

#ifdef PIP_PTHREAD_INIT
  if( symbols->pthread_init != NULL ) {
    symbols->pthread_init( argc, argv, envv );
  }
#endif

#ifndef PIP_NO_MALLOPT
  if( symbols->mallopt != NULL ) {
    DBGF( ">> mallopt()" );
    if( symbols->mallopt( M_MMAP_THRESHOLD, 1 ) == 1 ) {
      DBGF( "<< mallopt(M_MMAP_THRESHOLD): succeeded" );
    } else {
      DBGF( "<< mallopt(M_MMAP_THRESHOLD): failed !!!!!!" );
    }
    if( symbols->mallopt( M_TRIM_THRESHOLD, -1 ) == 1 ) {
      DBGF( "<< mallopt(M_TRIM_THRESHOLD): succeeded" );
    } else {
      DBGF( "<< mallopt(M_TRIM_THRESHOLD): failed !!!!!!" );
    }
  }
#endif

  if( flag ) {			/* if not ULP */
    DBG;
    if( symbols->glibc_init != NULL ) {
      DBGF( ">> glibc_init@%p()", symbols->glibc_init );
      symbols->glibc_init( argc, argv, envv );
      DBGF( "<< glibc_init@%p()", symbols->glibc_init );
    } else if( symbols->ctype_init != NULL ) {
      DBGF( ">> __ctype_init@%p()", symbols->ctype_init );
      symbols->ctype_init();
      DBGF( "<< __ctype_init@%p()", symbols->ctype_init );
    }
#ifdef DEBUG
    CHECK_CTYPE;
#endif
  }
  DBG;
  return( argc );
}

static void pip_glibc_fin( pip_symbols_t *symbols ) {
  /* call fflush() in the target context to flush out messages */
  if( symbols->libc_fflush != NULL ) {
    DBGF( ">> fflush@%p()", symbols->libc_fflush );
    symbols->libc_fflush( NULL );
    DBGF( "<< fflush@%p()", symbols->libc_fflush );
  }
}

static int pip_do_spawn( void *thargs )  {
  pip_spawn_args_t *args = (pip_spawn_args_t*) thargs;
  int 	pipid = args->pipid;
#ifdef PIP_CLONE_AND_DLMOPEN
  char *prog  = args->prog;
#endif
  char **argv = args->argv;
  char **envv = args->envv;
  int coreno  = args->coreno;
  pip_task_t *self = &pip_root->tasks[pipid];
  pip_spawnhook_t before = self->hook_before;
  void *hook_arg         = self->hook_arg;
  int 	err = 0;

  DBG;
  if( ( err = pip_corebind( coreno ) ) != 0 ) RETURN( err );
  DBG;

#ifdef DEBUG
  if( pip_is_pthread_() ) {
    pthread_attr_t attr;
    size_t sz;
    int _err;
    if( ( _err = pthread_getattr_np( self->thread, &attr      ) ) != 0 ) {
      DBGF( "pthread_getattr_np()=%d", _err );
    } else if( ( _err = pthread_attr_getstacksize( &attr, &sz ) ) != 0 ) {
      DBGF( "pthread_attr_getstacksize()=%d", _err );
    } else {
      DBGF( "stacksize = %ld [KiB]", sz/1024 );
    }
  }
#endif
  DBG;
#ifdef PIP_CLONE_AND_DLMOPEN
  pip_spin_lock( &pip_root->lock_ldlinux );
  /*** begin lock region ***/
  do {
    ES( time_load_prog, ( err = pip_load_prog( prog, self ) ) );
  } while( 0 );
  /*** end lock region ***/
  pip_spin_unlock( &pip_root->lock_ldlinux );
  if( err != 0 ) RETURN( err );
#else
  //fprintf( stderr, "self->symbols.add_stack=%p\n", self->symbols.add_stack );
  if( self->symbols.add_stack != NULL ) {
    //self->symbols.add_stack();
  }
#endif
  DBG;
  if( !pip_is_shared_fd_() ) pip_close_on_exec();
  DBG;

  /* calling hook, if any */
  if( before != NULL && ( err = before( hook_arg ) ) != 0 ) {
    pip_warn_mesg( "try to spawn(%s), but the before hook at %p returns %d",
		   argv[0], before, err );
    self->retval = err;
  } else {
    /* argv and/or envv might be changed in the hook function */
    ucontext_t 		ctx;
    volatile int	flag_exit;	/* must be volatile */

    DBG;
    flag_exit = 0;
    (void) getcontext( &ctx );
    if( !flag_exit ) {
      int argc;

      flag_exit = 1;
      self->ctx_exit = &ctx;
#ifdef PRINT_MAPS
      pip_print_maps();
#endif

      if( ( pip_root->opts & PIP_OPT_PGRP ) && !pip_is_pthread_() ) {
	(void) setpgid( 0, 0 );	/* Is this meaningful ? */
      }

      DBG;
      argc = pip_init_glibc( &self->symbols, argv, envv, self->loaded, 1 );
      DBGF( "[%d] >> main@%p(%d,%s,%s,...)",
	    pipid, self->symbols.main, argc, argv[0], argv[1] );
      self->retval = self->symbols.main( argc, argv, envv );
      DBGF( "[%d] << main@%p(%d,%s,%s,...)",
	    pipid, self->symbols.main, argc, argv[0], argv[1] );
    } else {
      DBGF( "!! main(%s,%s,...)", argv[0], argv[1] );
    }
    pip_glibc_fin( &self->symbols );

#ifndef PIP_CLONE_AND_DLMOPEN
    if( pip_root->task_root->symbols.add_stack != NULL ) {
      //pip_root->task_root->symbols.add_stack();
    }
#endif

    if( pip_root->opts & PIP_OPT_FORCEEXIT ) {
      if( pip_is_pthread_() ) {	/* thread mode */
	pthread_exit( NULL );
      } else {			/* process mode */
	exit( self->retval );
      }
    }
  }
  DBG;
  RETURN( 0 );
}

static int pip_find_a_free_task( int *pipidp ) {
  int pipid = *pipidp;
  int err = 0;

  if( pip_root->ntasks_accum >= PIP_NTASKS_MAX ) RETURN( EOVERFLOW );
  if( pipid < PIP_PIPID_ANY || pipid >= pip_root->ntasks ) {
    DBGF( "pipid=%d", pipid );
    RETURN( EINVAL );
  }

  pip_spin_lock( &pip_root->lock_tasks );
  /*** begin lock region ***/
  do {
    if( pipid != PIP_PIPID_ANY ) {
      if( pip_root->tasks[pipid].pipid != PIP_PIPID_NONE ) {
	err = EAGAIN;
	goto unlock;
      }
    } else {
      int i;

      for( i=pip_root->pipid_curr; i<pip_root->ntasks; i++ ) {
	if( pip_root->tasks[i].pipid == PIP_PIPID_NONE ) {
	  pipid = i;
	  goto found;
	}
      }
      for( i=0; i<pip_root->pipid_curr; i++ ) {
	if( pip_root->tasks[i].pipid == PIP_PIPID_NONE ) {
	  pipid = i;
	  goto found;
	}
      }
      err = EAGAIN;
      goto unlock;
    }
  found:
    pip_root->tasks[pipid].pipid = pipid;	/* mark it as occupied */
    pip_root->pipid_curr = pipid + 1;
    *pipidp = pipid;

  } while( 0 );
 unlock:
  /*** end lock region ***/
  pip_spin_unlock( &pip_root->lock_tasks );

  RETURN( err );
}

#include <sys/syscall.h>
static pid_t pip_gettid( void ) {
  return (pid_t) syscall( (long int) SYS_gettid );
}

int pip_spawn( char *prog,
	       char **argv,
	       char **envv,
	       int  coreno,
	       int  *pipidp,
	       pip_spawnhook_t before,
	       pip_spawnhook_t after,
	       void *hookarg ) {
  cpu_set_t 		cpuset;
  pip_spawn_args_t	*args = NULL;
  pip_task_t		*task = NULL;
  struct pip_gdbif_task *gdbif_task = NULL;
  size_t		stack_size = pip_stack_size();
  int 			pipid;
  pid_t			pid = 0;
  int 			err = 0;

  DBGF( ">> pip_spawn()" );

  if( pip_root == NULL ) RETURN( EPERM );
  if( pipidp   == NULL ) RETURN( EINVAL );
  if( argv     == NULL ) RETURN( EINVAL );
  if( prog     == NULL ) prog = argv[0];

  pipid = *pipidp;
  if( ( err = pip_find_a_free_task( &pipid ) ) != 0 ) goto error;
  task = &pip_root->tasks[pipid];
  pip_init_task_struct( task );
  task->pipid = pipid;	/* mark it as occupied */
  task->type  = PIP_TYPE_TASK;

  if( envv == NULL ) envv = environ;
  args = &task->args;
  args->pipid       = pipid;
  args->coreno      = coreno;
  if( ( args->prog = strdup( prog )              ) == NULL ||
      ( args->argv = pip_copy_vec( argv )        ) == NULL ||
      ( args->envv = pip_copy_env( envv, pipid ) ) == NULL ) {
    err = ENOMEM;
    goto error;
  }
  task->hook_before = before;
  task->hook_after  = after;
  task->hook_arg    = hookarg;
  pip_spin_init( &task->lock_malloc );

  gdbif_task = &pip_gdbif_root->tasks[pipid];
  task->pid = -1; /* pip_init_gdbif_task_struct() refers this */
  pip_init_gdbif_task_struct( gdbif_task, task );
  pip_link_gdbif_task_struct( gdbif_task );
  task->gdbif_task = gdbif_task;

#ifdef PIP_DLMOPEN_AND_CLONE
  pip_spin_lock( &pip_root->lock_ldlinux );
  /*** begin lock region ***/
  do {
    if( ( err = pip_do_corebind( coreno, &cpuset ) ) == 0 ) {
      /* corebinding should take place before loading solibs,       */
      /* hoping anon maps would be mapped onto the closer numa node */

      ES( time_load_prog, ( err = pip_load_prog( prog, task ) ) );

      /* and of course, the corebinding must be undone */
      (void) pip_undo_corebind( coreno, &cpuset );
    }
  } while( 0 );
  /*** end lock region ***/
  pip_spin_unlock( &pip_root->lock_ldlinux );

  if( err != 0 ) goto error;
#endif

  if( ( pip_root->opts & PIP_MODE_PROCESS_PIPCLONE ) ==
      PIP_MODE_PROCESS_PIPCLONE ) {
    int flags =
      CLONE_VM |
      /* CLONE_FS | CLONE_FILES | */
      /* CLONE_SIGHAND | CLONE_THREAD | */
      CLONE_SETTLS |
      CLONE_PARENT_SETTID |
      CLONE_CHILD_CLEARTID |
      CLONE_SYSVSEM |
      CLONE_PTRACE |
      SIGCHLD;

    err = pip_clone_mostly_pthread_ptr( &task->thread,
					flags,
					coreno,
					stack_size,
					(void*(*)(void*)) pip_do_spawn,
					args,
					&pid );
    DBGF( "pip_clone_mostly_pthread_ptr()=%d", err );
  } else {
    pthread_attr_t 	attr;
    pid_t tid = pip_gettid();

    if( ( err = pthread_attr_init( &attr ) ) == 0 ) {
#ifdef PIP_CLONE_AND_DLMOPEN
      if( coreno != PIP_CPUCORE_ASIS &&
	  coreno >= 0                &&
	  coreno <  sizeof(cpuset) * 8 ) {
	CPU_ZERO( &cpuset );
	CPU_SET( coreno, &cpuset );
	err = pthread_attr_setaffinity_np( &attr, sizeof(cpuset), &cpuset );
	DBGF( "pthread_attr_setaffinity_np( %d )= %d", coreno, err );
      }
#endif
#ifdef AH
      if( err == 0 ) {
	err = pthread_attr_setstacksize( &attr, stack_size );
	DBGF( "pthread_attr_setstacksize( %ld )= %d", stack_size, err );
      }
#endif
    }
    if( err == 0 ) {
      DBGF( "tid=%d  cloneinfo@%p", tid, pip_root->cloneinfo );
      if( pip_root->cloneinfo != NULL ) {
	/* lock is needed, because the preloaded clone()
	   might also be called from outside of PiP lib. */
	pip_spin_lock_wv( &pip_root->cloneinfo->lock, tid );
      }
      DBG;
      do {
	err = pthread_create( &task->thread,
			      &attr,
			      (void*(*)(void*)) pip_do_spawn,
			      (void*) args );
	DBGF( "pthread_create()=%d", errno );
      } while( 0 );
      /* unlock is done in the wrapper function */
      DBG;
      if( pip_root->cloneinfo != NULL ) {
	pid = pip_root->cloneinfo->pid_clone;
	pip_root->cloneinfo->pid_clone = 0;
      }
    }
  }
  DBG;
  if( err == 0 ) {
    task->pid = pid;
    pip_root->ntasks_accum ++;
    pip_root->ntasks_curr  ++;
    gdbif_task->pid = pid;
    gdbif_task->status = PIP_GDBIF_STATUS_CREATED;
    *pipidp = pipid;

  } else {
  error:			/* undo */
    DBG;
    if( args != NULL ) {
      if( args->prog != NULL ) free( args->prog );
      if( args->argv != NULL ) free( args->argv );
      if( args->envv != NULL ) free( args->envv );
    }
    if( task != NULL ) {
      if( task->loaded != NULL ) (void) pip_dlclose( task->loaded );
      pip_init_task_struct( task );
    }
  }
  DBGF( "<< pip_spawn(pipid=%d)", *pipidp );
  RETURN( err );
}

int pip_fin( void ) {
  int ntasks, i, err = 0;

  DBG;
  fflush( NULL );
  if( pip_root_p_() ) {
    ntasks = pip_root->ntasks;
    for( i=0; i<ntasks; i++ ) {
      if( pip_root->tasks[i].pipid != PIP_PIPID_NONE ) {
	DBGF( "%d/%d [%d] -- BUSY", i, ntasks, pip_root->tasks[i].pipid );
	err = EBUSY;
	break;
      }
    }
    if( err == 0 ) {
      memset( pip_root, 0, pip_root->size );
      DBG;
      free( pip_root );
      pip_root = NULL;
      pip_task = NULL;
      pip_ulp  = NULL;

      REPORT( time_load_dso   );
      REPORT( time_load_prog );
      REPORT( time_dlmopen   );
    }
  } else if( pip_ulp == NULL ) {
    pip_root = NULL;
    pip_task = NULL;
  }
  RETURN( err );
}

int pip_get_mode( int *mode ) {
  if( pip_root == NULL ) RETURN( EPERM  );
  if( mode     == NULL ) RETURN( EINVAL );
  *mode = ( pip_root->opts & PIP_MODE_MASK );
  RETURN( 0 );
}

int pip_get_id( int pipid, intptr_t *pidp ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( pipid == PIP_PIPID_ROOT ) RETURN( EINVAL );
  if( pidp  == NULL           ) RETURN( EINVAL );

  task = pip_get_task_( pipid );
  if( pip_is_pthread_() ) {
    /* Do not use gettid(). This is a very Linux-specific function */
    /* The reason of supporintg the thread PiP execution mode is   */
    /* some OSes other than Linux does not support clone()         */
    *pidp = (intptr_t) task->thread;
  } else if( task->type == PIP_TYPE_TASK ) {
    *pidp = (intptr_t) task->pid;
  } else {			/* ULP */
    *pidp = (intptr_t) task->task_parent->pid;
  }
  RETURN( 0 );
}

int pip_kill( int pipid, int signal ) {
  pip_task_t *task;
  int err  = 0;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( signal < 0 ) RETURN( EINVAL );

  task = pip_get_task_( pipid );
  if( pip_is_pthread_() ) {
    err = pthread_kill( task->thread, signal );
    DBGF( "pthread_kill(sig=%d)=%d", signal, err );
  } else if( task->type == PIP_TYPE_TASK ) {
    if( kill( task->pid, signal ) < 0 ) err = errno;
    DBGF( "kill(sig=%d)=%d", signal, err );
  } else {
    err = EPERM;
  }
  RETURN( err );
}

int pip_exit( int retval ) {
  fflush( NULL );
  DBG;
  if( !pip_root_p_() && !pip_task_p_() ) {
    DBG;
    /* since we must replace exit() with pip_exit(), pip_exit() */
    /* must be able to use even if it is NOT a PIP environment. */
    exit( retval );
  } else if( pip_is_pthread_() || /* thread mode */
	     pip_isa_ulp() ) {	  /* or ULP*/
    pip_task->retval = retval;
    pip_task->gdbif_task->status = PIP_GDBIF_STATUS_TERMINATED;
    pip_task->gdbif_task->exit_code = retval;
    DBGF( "[PIPID=%d] pip_exit(%d)!!!", pip_task->pipid, retval );
    (void) setcontext( pip_task->ctx_exit );
    DBGF( "[PIPID=%d] pip_exit() ????", pip_task->pipid );
  } else if( pip_task->type == PIP_TYPE_TASK ) { /* process mode */
    DBG;
    exit( retval );
  } else {
    DBGF( "?????" );
  }
  /* never reach here */
  DBG;
  return 0;
}

/*
 * The following functions must be called at root process
 */

static void pip_finalize_gdbif_tasks( void ) {
  struct pip_gdbif_task *gdbif_task, **prev, *next;

  if( pip_gdbif_root == NULL ) {
    DBGF( "pip_gdbif_root=NULL, pip_init() hasn't called?" );
    return;
  }
  pip_spin_lock( &pip_gdbif_root->lock_root );
  prev = &PIP_SLIST_FIRST(&pip_gdbif_root->task_free);
  PIP_SLIST_FOREACH_SAFE(gdbif_task, &pip_gdbif_root->task_free, free_list,
			 next) {
    if( gdbif_task->gdb_status != PIP_GDBIF_GDB_DETACHED ) {
      prev = &PIP_SLIST_NEXT(gdbif_task, free_list);
    } else {
      *prev = next;
      PIP_HCIRCLEQ_REMOVE(gdbif_task, task_list);
    }
  }
  pip_spin_unlock( &pip_gdbif_root->lock_root );
}

static void pip_finalize_task( pip_task_t *task, int *retvalp ) {

  struct pip_gdbif_task *gdbif_task = task->gdbif_task;

  DBGF( "pipid=%d", task->pipid );

  gdbif_task->status = PIP_GDBIF_STATUS_TERMINATED;
  gdbif_task->pathname = NULL;
  gdbif_task->argc = 0;
  gdbif_task->argv = NULL;
  gdbif_task->envv = NULL;
  if( gdbif_task->realpathname  != NULL ) {
    char *p = gdbif_task->realpathname;
    gdbif_task->realpathname = NULL; /* do this before free() for PIP-gdb */
    free( p );
  }
  pip_spin_lock( &pip_gdbif_root->lock_free );
  PIP_SLIST_INSERT_HEAD(&pip_gdbif_root->task_free, gdbif_task, free_list);
  pip_finalize_gdbif_tasks();
  pip_spin_unlock( &pip_gdbif_root->lock_free );

  if( retvalp != NULL ) *retvalp = ( task->retval & 0xFF );
  DBGF( "retval=%d", task->retval );

  /* dlclose() and free() must be called only from the root process since */
  /* corresponding dlmopen() and malloc() is called by the root process   */
  if( task->loaded     != NULL ) pip_dlclose( task->loaded );
  if( task->args.prog  != NULL ) free( task->args.prog );
  if( task->args.argv  != NULL ) free( task->args.argv );
  if( task->args.envv  != NULL ) free( task->args.envv );
  /* and the after hook may free the hook_arg if it is malloc()ed */
  if( task->hook_after != NULL ) (void) task->hook_after( task->hook_arg );

  pip_init_task_struct( task );
}

static int pip_do_wait( int pipid, int flag_try, int *retvalp ) {
  pip_task_t *task;
  int err;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  if( pipid      == PIP_PIPID_ROOT ) RETURN( EINVAL );
  task = &pip_root->tasks[pipid];
  if( task       == pip_task       ) RETURN( EPERM ); /* unable to wait itself */
  if( task->type == PIP_TYPE_ULP   ) RETURN( EPERM ); /* unable to wait ULP    */

  if( pip_is_pthread_() ) { /* thread mode */
    DBG;
    if( flag_try ) {
      err = pthread_tryjoin_np( task->thread, NULL );
      DBGF( "pthread_tryjoin_np()=%d", err );
    } else {
      err = pthread_join( task->thread, NULL );
      DBGF( "pthread_join()=%d", err );
    }
  } else {			/* process mode */
    int status = 0;
    pid_t pid;
    int options = __WALL;
    DBG;
    if( flag_try ) options |= WNOHANG;
    while( 1 ) {
      errno = 0;
      if( ( pid = waitpid( task->pid, &status, options ) ) >= 0 ) break;
      if( errno == EINTR ) continue;
      err = errno;
      break;
    }
    DBG;
    if( WIFEXITED( status ) ) {
      task->retval = WEXITSTATUS( status );
      task->gdbif_task->status = PIP_GDBIF_STATUS_TERMINATED;
      task->gdbif_task->exit_code = task->retval;
    } else if( WIFSIGNALED( status ) ) {
      pip_warn_mesg( "Signaled %s", strsignal( WTERMSIG( status ) ) );
    }
    DBGF( "wait(status=%x)=%d (errno=%d)", status, pid, err );
  }
  if( err == 0 ) pip_finalize_task( task, retvalp );
  RETURN( err );
}

int pip_wait( int pipid, int *retvalp ) {
  DBG;
  RETURN( pip_do_wait( pipid, 0, retvalp ) );
}

int pip_trywait( int pipid, int *retvalp ) {
  DBG;
  RETURN( pip_do_wait( pipid, 1, retvalp ) );
}

pip_clone_t *pip_get_cloneinfo_( void ) {
  return pip_root->cloneinfo;
}

int pip_get_pid_( int pipid, pid_t *pidp ) {
  int err = 0;

  if( pidp == NULL ) RETURN( EINVAL );
  if( pip_root->opts && PIP_MODE_PROCESS ) {
    /* only valid with the "process" execution mode */
    if( ( err = pip_check_pipid( &pipid ) ) == 0 ) {
      if( pipid == PIP_PIPID_ROOT ) {
	err = EPERM;
      } else {
	*pidp = pip_root->tasks[pipid].pid;
      }
    }
  } else {
    err = EPERM;
  }
  RETURN( err );
}

void pip_barrier_init( pip_barrier_t *barrp, int n ) {
  barrp->count       = n;
  barrp->count_init  = n;
  barrp->gsense      = 0;
}

void pip_barrier_wait( pip_barrier_t *barrp ) {
  if( barrp->count_init > 1 ) {
    int lsense = !barrp->gsense;
    if( __sync_sub_and_fetch( &barrp->count, 1 ) == 0 ) {
      barrp->count  = barrp->count_init;
      pip_memory_barrier();
      barrp->gsense = lsense;
    } else {
      while( barrp->gsense != lsense ) pip_pause();
    }
  }
}

/*** The following malloc/free functions are just for functional test ***/
/*** We should hvae the other functions allocating and freeing memory ***/

/* long long to align */
#define PIP_ALIGN_TYPE	long long

void *pip_malloc( size_t size ) {
  pip_task_t *task;

  if( pip_root_p_() ) {
    task = pip_root->task_root;
  } else {
    task = pip_task;
  }
  pip_spin_lock(   &task->lock_malloc );
  void *p = malloc( size + sizeof(PIP_ALIGN_TYPE) );
  pip_spin_unlock( &task->lock_malloc );

  *(int*) p = pip_get_pipid_();
  p += sizeof(PIP_ALIGN_TYPE);
  return p;
}

void pip_free( void *ptr ) {
  pip_task_t *task;
  free_t free_func;
  int pipid;

  ptr  -= sizeof(PIP_ALIGN_TYPE);
  pipid = *(int*) ptr;
  if( pipid >= 0 || pipid == PIP_PIPID_ROOT ) {
    if( pipid == PIP_PIPID_ROOT ) {
      task = pip_root->task_root;
    } else {
      task = &pip_root->tasks[pipid];
    }
    /* need of sanity check on pipid */
    if( ( free_func = task->symbols.free ) != NULL ) {

      pip_spin_lock(   &task->lock_malloc );
      free_func( ptr );
      pip_spin_unlock( &task->lock_malloc );

    } else {
      pip_warn_mesg( "No free function" );
    }
  } else {
    free( ptr );
  }
}

/*-----------------------------------------------------*/
/* ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP ULP */
/*-----------------------------------------------------*/

static void pip_ulp_recycle_stack( void *stack ) {
  /* the first page is protected as stack guard */
  pip_spin_lock( &pip_root->lock_stack_flist );
  {
    *((void**)stack) = pip_root->stack_flist;
    pip_root->stack_flist = stack;
  }
  pip_spin_unlock( &pip_root->lock_stack_flist );
}

static void *pip_ulp_reuse_stack( pip_task_t *task ) {
  void *stack;
  pip_spin_lock( &pip_root->lock_stack_flist );
  {
    stack = pip_root->stack_flist;
    if( pip_root->stack_flist != NULL ) {
      pip_root->stack_flist = *((void**)stack);
    }
  }
  pip_spin_unlock( &pip_root->lock_stack_flist );
  return stack;
}

static void *pip_ulp_alloc_stack( void ) {
  void 		*stack;

  if( ( stack = pip_ulp_reuse_stack( pip_task ) ) == NULL ) {
    /* guard pages, top and bottom, to be idependent from stack direction */
    size_t	pgsz  = pip_root->page_size;
    size_t	stksz = pip_stack_size();
    size_t	sz    = stksz + pgsz + pgsz;
    void       *region;

    if( pip_page_alloc( sz, &region ) != 0 ) return NULL;
    if( mprotect( region,            pgsz, PROT_NONE ) != 0 ||
	mprotect( region+pgsz+stksz, pgsz, PROT_NONE ) != 0 ) {
      pip_ulp_recycle_stack( region+pgsz );
      return NULL;
    }
    return region + pgsz;
  } else {
    return stack;
  }
}

#define MASK32		(0xFFFFFFFF)

int pip_ulp_create( char *prog,
		    char **argv,
		    char **envv,
		    int  *pipidp,
		    pip_ulp_termcb_t termcb,
		    void *aux,
		    pip_ulp_t *ulp ) {
  pip_spawn_args_t	*args = NULL;
  pip_task_t		*ulpt = NULL;
  int			pipid;
  int 			err = 0;

  if( pip_root == NULL && pip_task == NULL ) RETURN( EPERM );
  if( argv     == NULL ) RETURN( EINVAL );
  if( ulp      == NULL ) RETURN( EINVAL );
  if( prog == NULL ) prog = argv[0];
  if( envv == NULL ) envv = environ;

  DBGF( ">> pip_ulp_create()" );

  pipid = *pipidp;
  if( ( err = pip_find_a_free_task( &pipid ) ) != 0 ) goto error;

  ulpt = &pip_root->tasks[pipid];
  pip_init_task_struct( ulpt );
  ulpt->type = PIP_TYPE_ULP;

  args = &ulpt->args;
  args->pipid = pipid;
  if( ( args->prog = strdup( prog )              ) == NULL ||
      ( args->argv = pip_copy_vec( argv )        ) == NULL ||
      ( args->envv = pip_copy_env( envv, pipid ) ) == NULL ) {
    err = ENOMEM;
    goto error;
  }

  pip_spin_lock( &pip_root->lock_ldlinux );
  /*** begin lock region ***/
  do {
    ES( time_load_prog, ( err = pip_load_prog( prog, ulpt ) ) );
  } while( 0 );
  /*** end lock region ***/
  pip_spin_unlock( &pip_root->lock_ldlinux );

  if( err == 0 ) {
    void *stack;

    DBG;
    ulpt->task_parent =
      ( pip_task != NULL ) ? pip_task : pip_root->task_root;
    ulpt->ulp   = ulp;
    ulp->ctx    = NULL;	/* will be created when yield_to() is called */
    ulp->termcb = termcb;
    ulp->aux    = aux;
    ulp->pipid  = pipid;
    if( ( stack = pip_ulp_alloc_stack() ) != NULL ) {
      DBGF( "stack=%p", stack );
      ulpt->stack = stack;
      goto done;
    } else {
      err = ENOMEM;
    }
  }
 error:
  DBG;
  if( args != NULL ) {
    if( args->prog  != NULL ) free( args->prog );
    if( args->argv  != NULL ) free( args->argv );
    if( args->envv  != NULL ) free( args->envv );
  }
  if( ulpt != NULL ) {
    if( ulpt->loaded != NULL ) (void) pip_dlclose( ulpt->loaded );
    pip_init_task_struct( ulpt );
  }
 done:
  DBGF( "<< pip_ulp_create()=%d", err );
  RETURN( err );
}

int pip_make_ulp( int pipid,
		  pip_ulp_termcb_t termcb,
		  void *aux,
		  pip_ulp_t *ulp ) {
  pip_task_t 	*task;
  int		err;

  if( ulp == NULL ) RETURN( EINVAL );
  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  task = pip_get_task_( pipid );
  if( task->type == PIP_TYPE_ULP  ) RETURN( EPERM  );

  ulp->ctx    = NULL;
  ulp->termcb = termcb,
  ulp->aux    = aux;
  ulp->pipid  = task->pipid;

  return 0;
}

#define MASK32		(0xFFFFFFFF)

static void pip_ulp_main_( int pipid, int root_H, int root_L ) {
  ucontext_t		ctx;
  pip_task_t		*ulpt;
  pip_ulp_t		*ulp;
  pip_ulp_termcb_t 	termcb;
  void			*aux;
  volatile int 		flag;
  int 			argc;

  DBGF( "pip_root@%p", &pip_root );
  pip_root = (pip_root_t*)
    ( ( ((intptr_t)root_H) << 32 ) | ( ((intptr_t)root_L) & MASK32 ) );
  DBGF( "pip_root=%p (0x%x 0x%x)", pip_root, root_H, root_L );

  ulpt = pip_get_task_( pipid );
  pip_ulp  = ulp = ulpt->ulp;
  termcb   = ulp->termcb;
  aux      = ulp->aux;
  pip_task = ulpt->task_parent;

  argc = pip_init_glibc( &ulpt->symbols,
			 ulpt->args.argv,
			 ulpt->args.envv,
			 NULL,
			 0 );

#ifdef PRINT_MAPS
  pip_print_maps();
#endif

  flag = 0;
  (void) getcontext( &ctx );
  if( !flag ) {
    flag = 1;
    ulpt->ctx_exit = &ctx;

    DBGF( "[ULP] >> main@%p(%d,%s,%s,...)",
	  ulpt->symbols.main, argc, ulpt->args.argv[0], ulpt->args.argv[1] );
    ulpt->retval = ulpt->symbols.main( argc,
				       ulpt->args.argv,
				       ulpt->args.envv );
    DBGF( "[ULP] << main@%p(%d,%s,%s,...)",
	  ulpt->symbols.main, argc, ulpt->args.argv[0], ulpt->args.argv[1] );
  }

  pip_glibc_fin( &ulpt->symbols );

  pip_ulp_recycle_stack( ulpt->stack );
  ulpt->stack       = NULL;
  ulpt->task_parent = NULL;
  ulp->termcb	    = NULL;
  ulp->aux 	    = NULL;
  pip_ulp           = NULL;
  /* the current pip_task must be released !!!! */
  DBGF( "termcb=%p", termcb );
  if( termcb != NULL ) termcb( aux ); /* call back */
  DBG;
}

int pip_ulp_yield_to( pip_ulp_t *oldulp, pip_ulp_t *newulp ) {
  ucontext_t oldctx, newctx;
  int err = 0;

  if( newulp == NULL ) RETURN( EINVAL );

#ifdef DEBUG
  pip_ulp_describe( oldulp );
  pip_ulp_describe( newulp );
#endif

  if( newulp->ctx == NULL ) {
    stack_t 	*stk = &(newctx.uc_stack);
    int		root_H, root_L;

    DBG;
    getcontext( &newctx );	/* to reset newctx */
    DBG;
    newctx.uc_link = NULL;
    stk->ss_sp     = pip_root->tasks[newulp->pipid].stack;
    stk->ss_flags  = 0;
    stk->ss_size   = pip_root->stack_size;
    root_H = ( ((intptr_t) pip_root) >> 32 ) & MASK32;
    root_L = ((intptr_t) pip_root) & MASK32;
    DBGF( "pip_root=%p  (0x%x 0x%x)", pip_root, root_H, root_L );
    makecontext( &newctx,
		 (void(*)(void)) pip_ulp_main_,
		 3,
		 newulp->pipid,
		 root_H,
		 root_L );
    newulp->ctx = &newctx;
  }
  DBG;
  if( oldulp != NULL ) oldulp->ctx = &oldctx;
  if( swapcontext( &oldctx, newulp->ctx ) != 0 ) err = errno;
  DBG;
  if( err != 0 ) {
    DBGF( "swapcontext()=%d", err );
  } else if( oldulp != NULL ) {
    pip_ulp = oldulp;
  } else {
    pip_err_mesg( "Back to the terminated ULP!!" );
    exit( EPERM );
  }
  RETURN( err );
}

int pip_ulp_exit( int retval ) {
  pip_ulp = NULL;
  return pip_exit( retval );
}

int pip_ulp_do_finalize( int pipid, int *retvalp ) {
  pip_task_t	*ulp;
  int 		err = 0;

  if( ( err = pip_check_pipid( &pipid ) ) != 0 ) RETURN( err );
  ulp = pip_get_task_( pipid );
  if( ulp->type != PIP_TYPE_ULP ) RETURN( EPERM );
  pip_ulp_recycle_stack( ulp->stack );
  pip_finalize_task( ulp, retvalp ); /* FIXME: this violates the rule */
  RETURN( err );
}

void pip_ulp_describe( pip_ulp_t *ulp ) {
  if( ulp != NULL ) {
    pip_info_mesg( "ULP[%d](ctx=%p,termcb=%p,aux=%p)@%p",
		   ulp->pipid,
		   ulp->ctx,
		   ulp->termcb,
		   ulp->aux,
		   ulp );
  } else {
    pip_info_mesg( "ULP[](nil)" );
  }
}
