/*
** Requires:
**
**  gcc v?.?? [ 6.8 ?: elvis operator ]
*/

#define _GNU_SOURCE     // for linux namespaces

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>     // va_list
#include <stdbool.h>
#include <stdlib.h>     // exit()
#include <string.h>

#include <fcntl.h>      // open()
#include <limits.h>     // PATH_MAX
#include <sched.h>      // unshare()
#include <sys/mount.h>  // mount()
#include <sys/stat.h>   // mkdir()
#include <unistd.h>


/*
 *=======
 * Types
 *=======
 */

/// fstab style Entry for bind mount
typedef struct MountEntry
{
	char *hostname;
	char *source;
	char *dest;

	bool isReadOnly;

} MountEntry;

/// Array of MountEntry
typedef struct Config
{
	// Parsed
	MountEntry *entry; 
	size_t      length;
	size_t      reserved;

	// Raw utf-8 file data
	char  *data;
	size_t dataLen;

} Config;

/*
**=======
** Utils
**=======
*/

void usage( char *argv0 )
{
	printf( "%s Hostname\n", argv0 );
	exit(1);
}

void enforce( bool condition, const char const *msg, ... )
{
	if ( condition )
		return;

	va_list va;
	va_start( va, msg );

	// Idea: include argv0 ?
	fputs( "Error: ", stderr );
	vfprintf( stderr, msg, va );
	fputs( "\n\n",  stderr );

	va_end( va );
	// Death
	exit(1);
}
	

/*
 *===================
 * Parse Config file
 *===================
*/

/**
 * Tokenize NUL terminated string
 *
 * Slice and mutate 'str0' into array 'token'
 *
 * Params:
 *   token       = output: array
 *   tokenLength = allocated length of 'token'
 *   str0        = input: NUL terminated string
 *   strLength   = length of 'str0'
 *   delim       = ASCII character
 *
 * Returns: number of tokens found ( can exceed 'tokenLength' )
 *
 * TODO: what if str[0] == delim ?
 */
size_t parse_tokens0( /*out*/ char *token[], const size_t tokenLength, char *str0, const size_t strLength, const char delim )
{
	char *        cp = str0;
	char * const end = str0 + strLength;

	// Assumptions
	enforce( token != NULL, "arg 'token' is NULL" );
	enforce( str0  != NULL, "arg 'str0' is NULL" );
	enforce( delim != '\0', "arg 'delim' is NUL" );
	enforce( *end == '\0' , "string 'str0' is not NUL terminated" );

	int i=0;
	for ( ; cp < end; i++ )
	{
		// Don't overflow
		if ( i < tokenLength )
			token[i] = cp;
		
		cp = memchr( cp, delim, end - cp );
		if ( !cp )
			return i+1;

		cp[0] = '\0';
		/*
		 * Note: we can advance by 1 because:
		 * 1) memchr hit a 'delim'
		 * 2) 'delim' is not NUL
		 * 3) The string is enforced to be NUL terminated
		 */
		cp++;

		// skip repeat delim ( Same Note as above )
		while ( *cp == delim ) ++cp;
	}

//	if ( i >= tokenLength )
//		warn some thrown away

	return i;
}


/*
 * str -> lines
 */
void parse_config( Config *c, const char const *hostname )
{
	// Assumptions
	{
		enforce( hostname, "Missing Hostname" );
		enforce( c->dataLen > 0, "No file data" );
		enforce( c->data[c->dataLen] == '\0', "Config data was not NUL terminated" );

		char *cp;
		if( cp = memchr( c->data, '\0', c->dataLen ) )
		{
			enforce( false, "Config file contains a NUL byte at offset %td", cp - c->data );
		}
	}

	// byLine()
	int lineNo = 0;
	char *cp = c->data;
	char * const dataEnd = c->data + c->dataLen;

	while ( cp < dataEnd )
	{
		char * const line = cp;
		char * const lineEnd = memchr( cp, '\n', dataEnd - cp ) ?: dataEnd; // GNU C
		
		lineNo++;

		// delim
		if ( lineEnd[0] == '\n' )
		/**/ lineEnd[0] = '\0';
		else fprintf( stderr, "Warning: Config file missing final newline\n" );

		// skip comment / empty lines
		if ( line[0] == '#' || line[0] == '\0' )
			goto Lnext;

		char *token[4] = { NULL };
		bool isReadOnly = false;

		size_t ret = parse_tokens0( token, 4, line, lineEnd-line, '\t' );

		/*
		 * Validation: Catch basic mistakes
		 */
		if ( ret != 4 )
		{
			fprintf( stderr, "Warning: line '%d' malformed\n", lineNo );
//				printf("FKT: tok[%ld], host='%s'\ns='%s'\nd='%s'\n",
//					ret, tokens[0], tokens[1], tokens[2] );

			goto Lnext;
		}

		/**/ if ( !strcmp( "ro", token[1] ) )
			isReadOnly = true;
		else if ( !strcmp( "rw", token[1] ) )
			isReadOnly = false;
		else
		{
			fprintf( stderr, "Warning: line '%d' malformed ( col 2 )\n", lineNo );
			goto Lnext;
		}

		// filter hostname
		if ( strcmp( hostname, token[0] ) )
		{
			goto Lnext;
		}
		
		if ( token[2][0] != '/'
		||   token[3][0] == '/' )
		{
			fprintf( stderr, "Error: line '%d' relative/absolute path\n", lineNo );
			goto Lnext;
		}

		// free space?
		if ( c->length == c->reserved )
		{
			c->reserved += 16 * sizeof( struct MountEntry );
			c->entry = realloc( c->entry, c->reserved );

			enforce( c->entry, "realloc failed" );
		}

		/* Success */
		MountEntry *e = c->entry + c->length;
		e->hostname   = token[0];
		e->isReadOnly = isReadOnly;
		e->source     = token[2];
		e->dest       = token[3];

		c->length++;
Lnext:
		cp = lineEnd+1;
	}
}


/*
 *================
 * Linux Syscalls
 *================
*/

/**
 * Implement mkdir -p
 */
void mkdir_p( const char const *path )
{
	enforce( path   , "BUG1" );
	enforce( path[0], "BUG2" );

	// malloc
	char *cp = strdup( path );
	enforce( cp, "strdup(): %m" );

	// swap '/' -> '\0'
	char *UNUSED[1] = { NULL };
	size_t dirCount = parse_tokens0( UNUSED, 0, cp, strlen(cp), '/' );
	enforce( dirCount, "line %d: Internal BUG", __LINE__ );

	// Absolute path ?
	if ( cp[0] == '\0' )
	{
		cp[0] = '/';
		dirCount--;
	}

	for ( ;; )
	{
		// if(verbose) printf("mkdir( %s, 0770)\n", cp );

		if ( mkdir( cp, 0770 ) )
		{
			// BUG: does not check file type, assumes dir
			enforce( errno == EEXIST, "mkdir(\"%s\"): %m", cp );
		}

		if ( --dirCount == 0 )
			break;

		// restore '\0' -> '/'
		size_t len = strlen( cp );
		cp[len] = '/';
	}
	free( cp );
}





void setup_mounts( Config *c, const char const *rootDir )
{
	// Lower euid for mkdir()
	enforce( ! seteuid(getuid()), "seteuid(%d): '%m'", getuid() );

	const char const *oldCWD = get_current_dir_name(); // GNU C
	puts( rootDir );
	mkdir_p( rootDir );
	chdir(   rootDir );

	for ( int i=0; i < c->length; i++ )
	{
		MountEntry *e = c->entry + i;
		mkdir_p( e->dest );
	}
	
	// Enter Mount Namespace
	enforce( ! seteuid(0), "setuid(0): '%m'" );
	enforce( ! unshare( CLONE_NEWNS ), "unshare(): '%m'" );

	// Remove Shared propagation ( Note: $ findmnt -o+PROPAGATION )
	mount( NULL, "/", NULL, MS_REC | MS_SLAVE, NULL );

	/*
	 * Note: im using mount() with a relative path
	 */
	for ( int i=0; i < c->length; i++ )
	{
		MountEntry *e = c->entry + i;

		enforce( !mount( e->source, e->dest, NULL, MS_BIND, NULL),
			   	"mount(\"%s\",\"%s\",,MS_BIND,): %m", e->source, e->dest );

		// Linux can't do this in a single mount() call
		if ( e->isReadOnly )
		{
			enforce( !mount( NULL, e->dest, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL),
				"mount(,\"%s\",,MS_REMOUNT | MS_BIND | MS_RDONLY,): %m", e->dest );
		}
	}

	// Return
	chdir( oldCWD );
	free( (void*)oldCWD ); // shutup -Wdiscard-qualifiers
}








int main( int argc, char **argv )
{
	// TODO: check euid == 0
	// TODO: future use capabilities

	///
	if ( argc != 2 )
		usage( argv[0] );

	Config config = {}; // GNU C + [C99 6.7.8.21]
	
#if 0 // Embedded version
{
	char TEMP[] =
		"#Host	ro?	src				target\n"
		"#================================\n"
		"vm1	rw	/pool/Code		Code\n"
		"vm1	ro	/pool/Music		Music\n"
		"\n"
		"vm2	ro	/pool/db		mysql\n"
		"\n"
		"vm3	ro	/pool/web		services/nginx\n"
		"vm3	rw	/pool/db		services/db\n"
	"";

	/* Note: Don't trust 'data' to be well formed;
	 * Append a NUL byte to simplify parsing */
	config.dataLen = strlen(TEMP);
	enforce( config.data = malloc( config.dataLen + 1 ), "malloc: '%m'" );
	memcpy(  config.data, TEMP, config.dataLen + 1 );
}
#else
	int fp = open( "fs.list", O_RDONLY ); // O_NOFOLLOW ?
	enforce( fp > 0, "open(\"fs.list\"): %m" );

	struct stat st;

	enforce( !fstat( fp, &st ), "stat(\"fs.list\"): %m" );
	enforce( st.st_uid == 0, "unsafe: 'fs.list' not owned by root" );
	enforce( st.st_gid == getuid(), "who are you?" );
	enforce( S_ISREG(st.st_mode), "not regular file" );
	enforce( (st.st_mode&07777) == 0640, "permission %03o, (required 640)", st.st_mode&07777 ); // Linux specific

	config.dataLen = st.st_size;

	/* Note: Don't trust 'data' to be well formed;
	 * Allocate and Append a NUL byte to simplify parsing */
	enforce( config.data = malloc( config.dataLen+1 ), "malloc" );
	config.data[ config.dataLen ] = '\0';

	// Note: I doubt this file will be larger than 4kb
	//       I already know this part requires a loop
	ssize_t sz = read( fp, config.data, config.dataLen );
	enforce( sz == config.dataLen, "Unlucky" );

#endif

	// Note: Typically should not need more than 128 bytes.
	char path_socket[ PATH_MAX ];
	char path_mount[ PATH_MAX ];
	
	// Sensible limit for Hostname
	enforce( strlen(argv[1]) < 1024, "Why is your hostname that long?" );

	int ret1, ret2;

	/*
	 * Note: rather than trust/verify $XDG_RUNTIME_DIR
	 *       Just build the socket url ourself
	 */
	ret1= snprintf(	path_socket,
					PATH_MAX,
					"--socket-path=/run/user/%d/autism/%s.virtfs.sock",
					getuid(),
					argv[1] );

	ret2= snprintf(	path_mount,
					PATH_MAX,
					"source=/run/user/%d/autism/%s.mount.d/",
					getuid(),
					argv[1] );

	enforce( ret1 < PATH_MAX, "socket path too long" );
	enforce( ret2 < PATH_MAX, "mount  path too long" );

	parse_config( &config, argv[1] );

	setup_mounts( &config, path_mount + 7 ); // 'source='
	
	char *args[6];
	args[0] = "virtiofsd";
	args[1] = "--socket-group=shahid";
	args[2] = path_socket;
	args[3] = "-o";
	args[4] = path_mount;
//	args[5] = "--daemonize";
	args[5] = NULL;

	execv( "/usr/lib/qemu/virtiofsd", args );

	/* unreachable code */
	printf( "Error: execv failed (%d): %s\n", errno, strerror(errno) );

	return 1;
}
