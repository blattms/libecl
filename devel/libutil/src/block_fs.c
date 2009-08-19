#define  _GNU_SOURCE   /* Must define this to get access to pthread_rwlock_t */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <hash.h>
#include <util.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <block_fs.h>
#include <vector.h>
#include <buffer.h>
#include <long_vector.h>
#include <time.h>

#define MOUNT_MAP_MAGIC_INT 8861290
#define BLOCK_FS_TYPE_ID    7100652


typedef struct file_node_struct file_node_type;
typedef struct sort_node_struct sort_node_type;



/**
   These should be bitwise "smart" - so it is possible
   to go on a wild chase through a binary stream and look for them.
*/

#define NODE_IN_USE_BYTE  85                /* Binary(85)  =  01010101 */
#define NODE_FREE_BYTE   170                /* Binary(170) =  10101010 */   
#define WRITE_START__    77162

static const int NODE_END_TAG            = 16711935;       /* Binary      =  00000000111111110000000011111111 */
static const int NODE_WRITE_ACTIVE_START = WRITE_START__;
static const int NODE_WRITE_ACTIVE_END   = 776512;


typedef enum {
  NODE_IN_USE       =  1431655765,    /* NODE_IN_USE_BYTE * ( 1 + 256 + 256**2 + 256**3) => Binary 01010101010101010101010101010101 */
  NODE_FREE         = -1431655766,    /* NODE_FREE_BYTE   * ( 1 + 256 + 256**2 + 256**3) => Binary 10101010101010101010101010101010 */
  NODE_WRITE_ACTIVE =  WRITE_START__, /* This */
  NODE_INVALID      = 13              /* This should __never__ be written to disk */
} node_status_type;




struct file_node_struct{
  long int           node_offset;   /* The offset into the data_file of this node. NEVER Changed. */
  long int           data_offset;   /* The offset into the data_file where the actual data starts. */ 
  int                node_size;     /* The size in bytes of this node - must be >= data_size. NEVER Changed. */
  int                data_size;     /* The size of the data stored in this node - in addition the node might need to store header information. */
  node_status_type   status;        /* This should be: NODE_IN_USE | NODE_FREE; in addition the disk can have NODE_WRITE_ACTIVE for incomplete writes. */
  char             * cache;
  int                cache_size;
  file_node_type   * next , * prev; /* Implementing doubly linked list behaviour WHEN the node is in the free_nodes list of the block_fs object. */
};


/**
   data_size   : manipulated in block_fs_fwrite__() and block_fs_insert_free_node().
   status      : manipulated in block_fs_fwrite__() and block_fs_unlink_file__();
   data_offset : manipulated in block_fs_fwrite__() and block_fs_insert_free_node().
*/



/* Help structure only used for pretty-printing the block file layout. */
struct sort_node_struct {
  long int          node_offset;
  int               node_size;
  int               data_size;
  char            * name; 
};



struct block_fs_struct {
  UTIL_TYPE_ID_DECLARATION;
  char           * mount_file;    /* The full path to a file with some mount information - input to the mount routine. */
  char           * path;          
  char           * base_name;
  char           * mount_point;   /* string equals path + base_name: unique for this FS */

  int              version;       /* A version number which is incremented each time the filesystem is defragmented - not implemented yet. */
  
  char           * data_file;
  char           * lock_file;
  
  FILE           * data_stream;

  long int         data_file_size;  /* The total number of bytes in the data_file. */
  long int         free_size;       /* Size of 'holes' in the data file. */ 
  int              block_size;      /* The size of blocks in bytes. */
  int              lock_fd;         /* The file descriptor for the lock_file. Set to -1 if we do not have write access. */
  
  pthread_mutex_t  io_lock;         /* Lock held during fread of the data file. */
  pthread_rwlock_t rw_lock;         /* Read-write lock during all access to the fs. */
  
  int              num_free_nodes;   
  hash_type      * index;           /* THE HASH table of all the nodes/files which have been stored. */
  file_node_type * free_nodes;
  vector_type    * file_nodes;      /* This vector owns all the file_node instances - the index and free_nodes structures
                                       only contain pointers to the objects stored in this vector. */
  int              write_count;     /* This just counts the number of writes since the file system was mounted. */
  int              max_cache_size;
  float            fragmentation_limit;
  bool             data_owner;
  time_t           index_time;   
};

static void block_fs_rotate__( block_fs_type * block_fs );


static void block_fs_fprintf_free_nodes( const block_fs_type * block_fs, FILE * stream) {
  file_node_type * current = block_fs->free_nodes;
  int counter = 0;
  while (current != NULL ) {
    fprintf(stream , "Offset:%ld   node_size:%d   data_size:%d \n",current->node_offset , current->node_size , current->data_size);
    if (current->next == current)
      util_abort("%s: linked list broken \n",__func__);
    current = current->next;
    counter++;
  }
  fprintf(stderr , "\n");
  if (counter != block_fs->num_free_nodes) 
    util_abort("%s: Counter:%d    num_free_nodes:%d \n",__func__ , counter , block_fs->num_free_nodes);
}


static inline void fseek__(FILE * stream , long int arg , int whence) {
  if (fseek(stream , arg , whence) != 0) {
    fprintf(stderr,"** Warning - seek:%ld failed %s(%d) \n",arg,strerror(errno) , errno);
    util_abort("%S - aborting\n",__func__);
  }
}

static inline void block_fs_fseek_data(block_fs_type * block_fs , long offset) {
  fseek__( block_fs->data_stream , offset , SEEK_SET );
}


/*****************************************************************/
/* file_node functions */



static void file_node_fprintf(const file_node_type * node, const char * name, FILE * stream) {
  if (name == NULL)
    fprintf(stream , "%-30s: [Status:%11d  node_offset:%12ld  node_size:%8d   data_offset:%12ld  data_size:%8d] \n","FREE", node->status , node->node_offset , node->node_size , node->data_offset , node->data_size);
  else
    fprintf(stream , "%-30s: [Status:%11d  node_offset:%12ld  node_size:%8d   data_offset:%12ld  data_size:%8d] \n",name , node->status , node->node_offset , node->node_size , node->data_offset , node->data_size);
}




/**
   Observe that the two input arguments to this function should NEVER change. They represent offset and size
   in the underlying data file, and that is for ever fixed.
*/


static file_node_type * file_node_alloc( node_status_type status , long int offset , int node_size) {
  file_node_type * file_node = util_malloc( sizeof * file_node , __func__);
  
  file_node->node_offset = offset;    /* These should NEVER change. */
  file_node->node_size   = node_size; /* -------------------------  */

  file_node->data_size   = 0;
  file_node->data_offset = 0;
  file_node->status      = status; 
  
  file_node->cache      = NULL;
  file_node->cache_size = 0;

  file_node->next = NULL;
  file_node->prev = NULL;
  
  return file_node;
}


static void file_node_clear_cache( file_node_type * file_node ) {
  if (file_node->cache != NULL) {
    file_node->cache_size = 0;
    free(file_node->cache);
    file_node->cache      = NULL;
  }
}


static void file_node_read_from_cache( const file_node_type * file_node , void * ptr , size_t read_bytes) {
  memcpy(ptr , file_node->cache , read_bytes);
  /*
    Could check: (ext_offset + file_node->cache_size <= read_bytes) - else
    we are reading beyond the end of the cache.
  */
}


static void file_node_buffer_read_from_cache( const file_node_type * file_node , buffer_type * buffer ) {
  buffer_fwrite( buffer , file_node->cache , 1 , file_node->cache_size );
}


static void file_node_update_cache( file_node_type * file_node , int data_size , const void * data) {
  if (data_size != file_node->cache_size) {
    file_node->cache = util_realloc_copy( file_node->cache , data , data_size , __func__);
    file_node->cache_size = data_size;
  } else
    memcpy( file_node->cache , data , data_size);
}





static void file_node_free( file_node_type * file_node ) {
  free( file_node );
}


static void file_node_free__( void * file_node ) {
  file_node_free( (file_node_type *) file_node );
}



static bool file_node_verify_end_tag( const file_node_type * file_node , FILE * stream ) {
  int end_tag;
  fseek__( stream , file_node->node_offset + file_node->node_size - sizeof NODE_END_TAG , SEEK_SET);      
  if (fread( &end_tag , sizeof end_tag , 1 , stream) == 1) {
    if (end_tag == NODE_END_TAG) 
      return true; /* All hunkadory. */
    else
      return false;
  } else 
    return false;
}



static file_node_type * file_node_fread_alloc( FILE * stream , char ** key) {
  file_node_type * file_node = NULL;
  node_status_type status;
  long int node_offset       = ftell( stream );
  if (fread( &status , sizeof status , 1 , stream) == 1) {
    if ((status == NODE_IN_USE) || (status == NODE_FREE)) {
      int node_size;
      if (status == NODE_IN_USE) 
        *key = util_fread_realloc_string( *key , stream );
      else
        *key = util_safe_free( *key );  /* Explicitly set to NULL for free nodes. */

      node_size = util_fread_int( stream );
      file_node = file_node_alloc( status , node_offset , node_size );
      if (status == NODE_IN_USE) {
        file_node->data_size   = util_fread_int( stream );
        file_node->data_offset = ftell( stream );
      }
    } else {
      /* 
         We did not recognize the status identifier; the node will
         eventually be marked as free. 
      */
      if (status != NODE_WRITE_ACTIVE)
        status = NODE_INVALID;
      file_node = file_node_alloc( status , node_offset , 0 );
    }
  }
  return file_node;
}


/**
   Internal index layout:

   |<InUse: Bool><Key: String><node_size: Int><data_size: Int>|
   |<InUse: Bool><node_size: Int><data_size: Int>|
             
  /|\                                                        /|\ 
   |                                                          | 
   |                                                          |
          
node_offset                                                data_offset

  The node_offset and data_offset values are not stored on disk, but rather
  implicitly read with ftell() calls.
*/

/**
   This function will write the node information to file, this
   includes the NODE_END_TAG identifier which shoule be written to the
   end of the node.
*/

static void file_node_fwrite( const file_node_type * file_node , const char * key , FILE * stream ) {
  if (file_node->node_size == 0)
    util_abort("%s: trying to write node with z<ero size \n",__func__);
  {
    fseek__( stream , file_node->node_offset , SEEK_SET);
    util_fwrite_int( file_node->status , stream );
    if (file_node->status == NODE_IN_USE)
      util_fwrite_string( key , stream );
    util_fwrite_int( file_node->node_size , stream );
    util_fwrite_int( file_node->data_size , stream );
    fseek__( stream , file_node->node_offset + file_node->node_size - sizeof NODE_END_TAG , SEEK_SET);
    util_fwrite_int( NODE_END_TAG , stream );
  }
}


/**
   This marks the start and end of the node with the integer tags:
   NODE_WRITE_ACTIVE_START and NODE_WRITE_ACTIVE_END, signalling this
   section in the data file is 'work in progress', and should be
   discarded if the application aborts during the write.

   When the write is complete file_node_fwrite() should be called,
   which will replace the NODE_WRITE_ACTIVE_START and
   NODE_WRITE_ACTIVE_END tags with NODE_IN_USE and NODE_END_TAG
   identifiers.
*/

   
static void file_node_init_fwrite( const file_node_type * file_node , FILE * stream) {
  fseek__( stream , file_node->node_offset , SEEK_SET );
  util_fwrite_int( NODE_WRITE_ACTIVE_START , stream );
  fseek__( stream , file_node->node_offset + file_node->node_size - sizeof NODE_END_TAG , SEEK_SET);
  util_fwrite_int( NODE_WRITE_ACTIVE_END   , stream );
}


/**
   Observe that header in this context include the size of the tail
   marker NODE_END_TAG.
*/
   
static int file_node_header_size( const char * filename ) {
  file_node_type * file_node;
  return sizeof ( file_node->status    ) + 
         sizeof ( file_node->node_size ) + 
         sizeof ( file_node->data_size ) + 
         sizeof ( NODE_END_TAG )         + sizeof(int) /* embedded by the util_fwrite_string routine */ + strlen(filename) + 1 /* \0 */;
}


static void file_node_set_data_offset( file_node_type * file_node, const char * filename ) {
  file_node->data_offset = file_node->node_offset + file_node_header_size( filename ) - sizeof( NODE_END_TAG );
}


/* file_node functions - end. */
/*****************************************************************/

static inline void block_fs_aquire_wlock( block_fs_type * block_fs ) {
  if (block_fs->data_owner)
    pthread_rwlock_wrlock( &block_fs->rw_lock );
  else
    util_abort("%s: tried to write to read only filesystem mounted at: %s \n",__func__ , block_fs->mount_file );
}


static inline void block_fs_release_rwlock( block_fs_type * block_fs ) {
  pthread_rwlock_unlock( &block_fs->rw_lock );
}


static inline void block_fs_aquire_rlock( block_fs_type * block_fs ) {
  pthread_rwlock_rdlock( &block_fs->rw_lock );
}


static void block_fs_insert_index_node( block_fs_type * block_fs , const char * filename , const file_node_type * file_node) {
  hash_insert_ref( block_fs->index , filename , file_node);
}


/**
   Looks through the list of free nodes - looking for a node with
   offset 'node_offset'. If no such node can be found, NULL will be
   returned.
*/

static file_node_type * block_fs_lookup_free_node( const block_fs_type * block_fs , long int node_offset) {
  file_node_type * current = block_fs->free_nodes;
  while (current != NULL && (current->node_offset != node_offset)) 
    current = current->next;
  
  return current;
}









/**
   Inserts a file_node instance in the linked list of free nodes. The
   list is sorted in order of increasing node size.
*/

static void block_fs_insert_free_node( block_fs_type * block_fs , file_node_type * new ) {

  /* Special case: starting with a empty list. */
  if (block_fs->free_nodes == NULL) {
    new->next = NULL;
    new->prev = NULL;
    block_fs->free_nodes = new;
  } else {
    file_node_type * current = block_fs->free_nodes;
    file_node_type * prev    = NULL;
    
    while ( current != NULL && (current->node_size < new->node_size)) {
      prev = current;
      current = current->next;
    }
    
    if (current == NULL) {
      /* 
         The new node should be added at the end of the list - i.e. it
         will not have a next node.
      */
      new->next = NULL;
      new->prev = prev;
      prev->next = new;
    } else {
      /*
        The new node should be placed BEFORE the current node.
      */
      if (prev == NULL) {
        /* The new node should become the new list head. */
        block_fs->free_nodes = new;
        new->prev = NULL;
      } else {
        prev->next = new;
        new->prev  = prev;
      }
      current->prev = new;
      new->next  = current;
    }
    if (new != NULL)     if (new->next == new) util_abort("%s: broken LIST1 \n",__func__);
    if (prev != NULL)    if (prev->next == prev) util_abort("%s: broken LIST2 \n",__func__);
    if (current != NULL) if (current->next == current) util_abort("%s: Broken LIST 3\n",__func__);
  }
  block_fs->num_free_nodes++;
  block_fs->free_size += new->node_size;
  
  /* OKAY - this is going to take some time ... */
  if ((block_fs->free_size * 1.0 / block_fs->data_file_size) > block_fs->fragmentation_limit)
    block_fs_rotate__( block_fs );
}


/**
   Installing the new node AND updating file tail. 
*/

static void block_fs_install_node(block_fs_type * block_fs , file_node_type * node) {
  block_fs->data_file_size = util_size_t_max( block_fs->data_file_size , node->node_offset + node->node_size);  /* Updating the total size of the file - i.e the next available offset. */
  vector_append_owned_ref( block_fs->file_nodes , node , file_node_free__ );
}


static void block_fs_set_filenames( block_fs_type * block_fs ) {
  char * data_ext  = util_alloc_sprintf("data_%d" , block_fs->version );
  char * lock_ext  = util_alloc_sprintf("lock_%d" , block_fs->version );

  util_safe_free( block_fs->data_file );
  util_safe_free( block_fs->lock_file );
  block_fs->data_file  = util_alloc_filename( block_fs->path , block_fs->base_name , data_ext);
  block_fs->lock_file  = util_alloc_filename( block_fs->path , block_fs->base_name , lock_ext);

  free( data_ext );
  free( lock_ext );
}


static block_fs_type * block_fs_alloc_empty( const char * mount_file , int block_size , int max_cache_size, float fragmentation_limit) {
  block_fs_type * block_fs      = util_malloc( sizeof * block_fs , __func__);
  block_fs->mount_file          = util_alloc_string_copy( mount_file );
  block_fs->index               = hash_alloc();
  block_fs->file_nodes          = vector_alloc_new();
  block_fs->free_nodes          = NULL;
  block_fs->num_free_nodes      = 0;
  block_fs->write_count         = 0;
  block_fs->data_file_size      = 0;
  block_fs->free_size           = 0; 
  block_fs->block_size          = block_size;
  block_fs->max_cache_size      = max_cache_size;
  block_fs->fragmentation_limit = 1.0; //fragmentation_limit;   /* Never rotate currently */
  util_alloc_file_components( mount_file , &block_fs->path , &block_fs->base_name, NULL );
  block_fs->mount_point         = util_alloc_filename( block_fs->path , block_fs->base_name , NULL);
  pthread_mutex_init( &block_fs->io_lock  , NULL);
  pthread_rwlock_init( &block_fs->rw_lock , NULL);
  {
    FILE * stream            = util_fopen( mount_file , "r");
    int id                   = util_fread_int( stream );
    block_fs->version        = util_fread_int( stream );
    fclose( stream );
    
    if (id != MOUNT_MAP_MAGIC_INT) 
      util_abort("%s: The file:%s does not seem to a valid block_fs mount map \n",__func__ , mount_file);
  }
  block_fs->data_file   = NULL;
  block_fs->lock_file   = NULL;
  block_fs_set_filenames( block_fs );
  UTIL_TYPE_ID_INIT(block_fs , BLOCK_FS_TYPE_ID);
  block_fs->data_owner = util_try_lockf( block_fs->lock_file , S_IWUSR + S_IWGRP , &block_fs->lock_fd);
  block_fs->index_time = time( NULL );
  
  if (!block_fs->data_owner) 
    fprintf(stderr,"** Warning another program has already opened this filesystem read-write - this instance will be read-only.\n");
  
  return block_fs;
}


UTIL_IS_INSTANCE_FUNCTION(block_fs , BLOCK_FS_TYPE_ID);


static void block_fs_fwrite_mount_info__( const char * mount_file , int version) {
  FILE * stream = util_fopen( mount_file , "w");
  util_fwrite_int( MOUNT_MAP_MAGIC_INT , stream );
  util_fwrite_int( version , stream );
  fclose( stream );
}


/**
   Will seek the datafile to the end of the current file_node. So that the next read will be "guaranteed" to
   start at a new node.
*/
static void block_fs_fseek_node_end(block_fs_type * block_fs , const file_node_type * file_node) {
  block_fs_fseek_data( block_fs , file_node->node_offset + file_node->node_size);
}


/**
   This function will read through the datafile seeking for one of the
   identifiers: NODE_IN_USE | NODE_FREE. If one of the valid status
   identifiers is found the stream is repositioned at the beginning of
   the valid node, so the calling scope can continue with a

      file_node = file_node_date_fread_alloc()

   call. If no valid status ID is found whatsover the data_stream
   indicator is left at the end of the file; and the calling scope
   will finish from there.
*/
static bool block_fs_fseek_valid_node( block_fs_type * block_fs ) {
  unsigned char byte;
  int  status;
  while (true) {
    if (fread(&byte , sizeof byte ,1 , block_fs->data_stream) == 1) {
      if (byte == NODE_IN_USE_BYTE || byte == NODE_FREE_BYTE) {
        long int pos = ftell( block_fs->data_stream );
        /* 
           OK - we found one interesting byte; let us try to read the
           whole integer and see if we have hit any of the valid status identifiers.
        */
        fseek__( block_fs->data_stream , -1 , SEEK_CUR);
        if (fread(&status , sizeof status ,1 , block_fs->data_stream) == 1) {
          if (status == NODE_IN_USE || status == NODE_FREE_BYTE) {
            /* 
               OK - we have found a valid identifier. We reposition to
               the start of this valid status id and return true.
            */
            fseek__( block_fs->data_stream , -sizeof status , SEEK_CUR);
            return true;
          } else 
            /* 
               OK - this was not a valid id; we go back and continue 
               reading single bytes.
            */
            block_fs_fseek_data( block_fs , pos );
        } else
          break; /* EOF */
      }
    } else
      break; /* EOF */
  }
  fseek__( block_fs->data_stream , 0 , SEEK_END);
  return false;
}





/**
   The read-only open mode is only for the mount section, where the
   data file is read in to load/verify the index.

   If the read_only open fails - the data_stream is set to NULL. If
   the open succeeds the calling scope should close the stream before
   calling this function again, with read_only == false.
*/

static void block_fs_open_data( block_fs_type * block_fs , bool init_mode) {
  if (init_mode) {
    if (util_file_exists( block_fs->data_file ))
      block_fs->data_stream = util_fopen( block_fs->data_file , "r");
    else
      block_fs->data_stream = NULL;
  } else {
    if (block_fs->data_owner) {
      if (util_file_exists( block_fs->data_file ))
        block_fs->data_stream = util_fopen( block_fs->data_file , "r+");
      else
        block_fs->data_stream = util_fopen( block_fs->data_file , "w+");
    } else {
      /* This will fail hard if the datafile does not exist at all .. */
      block_fs->data_stream = util_fopen( block_fs->data_file , "r");
    }
  }
}


/**
   Observe that the arguments internal_index & external_indx can not
   be changed when remounting; when remounting you will get the
   original values.
*/



const char * block_fs_get_mount_point( const block_fs_type * block_fs ) {
  return block_fs->mount_point;
}

/**
   This functon will (attempt) to read the whole datafile in one large
   go, and then fill up the cache nodes. If it can not allocate enough
   memory to read in the whole datafile in one go, it will just fail
   silently, and no preload will be done.
*/


static void block_fs_preload( block_fs_type * block_fs ) {
  if (block_fs->max_cache_size > 0) {
    char * buffer = malloc( block_fs->data_file_size * sizeof * buffer );
    if (buffer != NULL) {
      FILE * stream = util_fopen( block_fs->data_file , "r");
      hash_iter_type * index_iter = hash_iter_alloc( block_fs->index );
      util_fread( buffer , 1 , block_fs->data_file_size , stream , __func__);
      
      while (!hash_iter_is_complete( index_iter )) {
        file_node_type * node = hash_iter_get_next_value( index_iter );
        if (node->data_size < block_fs->max_cache_size) 
          file_node_update_cache( node , node->data_size , &buffer[node->data_offset]);
        
      }
      fclose( stream );
      free( buffer );
    } 
  }
}


/**
   This function will 'fix' the nodes with offset in offset_list.  The
   fixing in this case means the following:

     1. The node is updated in place on the file to become a free node.
     2. The node is added to the block_fs instance as a free node, which can
        be recycled at a later stage.
   
   If the instance is not data owner (i.e. read-only) the function
   will return immediately.
*/


static void block_fs_fix_nodes( block_fs_type * block_fs , long_vector_type * offset_list ) {
  if (block_fs->data_owner) { 
    fsync( fileno(block_fs->data_stream) );
    {
      char * key = NULL;
      for (int inode = 0; inode < long_vector_size( offset_list ); inode++) {
        bool new_node = false;
        long int node_offset = long_vector_iget( offset_list , inode );
        file_node_type * file_node;
        block_fs_fseek_data(block_fs , node_offset);
        file_node = file_node_fread_alloc( block_fs->data_stream , &key );
        
        if ((file_node->status == NODE_INVALID) || (file_node->status == NODE_WRITE_ACTIVE)) {
          /* This node is really quite broken. */
          long int node_end;
          block_fs_fseek_valid_node( block_fs );
          node_end             = ftell( block_fs->data_stream );
          file_node->node_size = node_end - node_offset;
        } 
        
        file_node->status      = NODE_FREE;
        file_node->data_size   = 0;
        file_node->data_offset = 0;
        if (block_fs_lookup_free_node( block_fs , node_offset) == NULL) {
          /* The node is already on the free list - we just change some metadata. */
          new_node = true;
          block_fs_install_node( block_fs , file_node );
          block_fs_insert_free_node( block_fs , file_node );
        }
        
        block_fs_fseek_data(block_fs , node_offset);
        file_node_fwrite( file_node , NULL , block_fs->data_stream );
        if (!new_node)
          file_node_free( file_node );
      }
      util_safe_free( key );
    }
    fsync( fileno(block_fs->data_stream) );
  }
}



static void block_fs_build_index( block_fs_type * block_fs , long_vector_type * error_offset ) {
  char * filename = NULL;
  file_node_type * file_node;
  do {
    file_node = file_node_fread_alloc( block_fs->data_stream , &filename );
    if (file_node != NULL) {
      if ((file_node->status == NODE_INVALID) || (file_node->status == NODE_WRITE_ACTIVE)) {
        /* Oh fuck */
        
        if (file_node->status == NODE_INVALID)
          fprintf(stderr,"** Warning:: invalid node found at offset:%ld in datafile:%s - data will be lost. \n", file_node->node_offset , block_fs->data_file);
        else
          fprintf(stderr,"** Warning:: file system was prematurely shut down while writing node in %s/%ld - will be discarded.\n",block_fs->data_file , file_node->node_offset);
        
        long_vector_append( error_offset , file_node->node_offset );
        file_node_free( file_node );
        block_fs_fseek_valid_node( block_fs );
      } else {
        if (file_node_verify_end_tag(file_node, block_fs->data_stream)) {
          block_fs_fseek_node_end(block_fs , file_node);
          block_fs_install_node( block_fs , file_node );
          switch(file_node->status) {
          case(NODE_IN_USE):
            block_fs_insert_index_node(block_fs , filename , file_node);
            break;
          case(NODE_FREE):
            block_fs_insert_free_node( block_fs , file_node );
            break;
          default:
            util_abort("%s: node status flag:%d not recognized - error in data file \n",__func__ , file_node->status);
          }
        } else {
          /* 
             Could not find a valid END_TAG - indicating that
             the filesystem was shut down during the write of
             this node.  This node will NOT be added to the
             index.  The node will be updated to become a free node.
          */
          fprintf(stderr,"** Warning found node:%s at offset:%ld which was incomplete - discarded.\n",filename, file_node->node_offset);
          long_vector_append( error_offset , file_node->node_offset );
          file_node_free( file_node );
          block_fs_fseek_valid_node( block_fs );
        }
      }
    }
  } while (file_node != NULL);
  util_safe_free( filename );
  block_fs->index_time = time( NULL );
}



/**
   This mutex is to ensure that only one thread (from the same
   application) is trying to mount a file system at a time. If they
   are trying to mount different mount files, it could be done in
   parallell - but what the fuck.
*/

static pthread_mutex_t mount_lock = PTHREAD_MUTEX_INITIALIZER;
block_fs_type * block_fs_mount( const char * mount_file , int block_size , int max_cache_size , float fragmentation_limit , bool preload) {
  block_fs_type * block_fs;
  pthread_mutex_lock( &mount_lock );
  {
    if (!util_file_exists(mount_file)) 
      /* This is a brand new filesystem - create the mount map first. */
      block_fs_fwrite_mount_info__( mount_file , 0 );
    {
      long_vector_type * fix_nodes = long_vector_alloc(0 , 0);
      block_fs = block_fs_alloc_empty( mount_file , block_size , max_cache_size , fragmentation_limit );
      /* We build up the index & free_nodes_list based on the header/index information embedded in the datafile. */
      block_fs_open_data( block_fs , true );
      if (block_fs->data_stream != NULL) {
        block_fs_build_index( block_fs , fix_nodes );
        fclose(block_fs->data_stream);
      }
      
      block_fs_open_data( block_fs , false     );  /* reopen the data_stream for reading AND writing (IFF we are data_owner - otherwise it is still read only) */
      block_fs_fix_nodes( block_fs , fix_nodes );
      long_vector_free( fix_nodes );
    }
  }
  if (preload) block_fs_preload( block_fs );
  pthread_mutex_unlock( &mount_lock );
  return block_fs;
}



static void block_fs_unlink_free_node( block_fs_type * block_fs , file_node_type * node) {
  file_node_type * prev = node->prev;
  file_node_type * next = node->next;
  
  if (prev == NULL)
      /* Special case: popping off the head of the list. */
    block_fs->free_nodes = next;
  else
    prev->next = next;
  
  if (next != NULL)
    next->prev = prev;

  block_fs->num_free_nodes--;
  block_fs->free_size -= node->node_size;
}


/**
   This function first checks the free nodes if any of them can be
   used, otherwise a new node is created.
*/

static file_node_type * block_fs_get_new_node( block_fs_type * block_fs , const char * filename , size_t min_size) {
  
  file_node_type * current = block_fs->free_nodes;
  file_node_type * prev    = NULL;
  
  while (current != NULL && (current->node_size < min_size)) {
    prev = current;
    current = current->next;
  }
  if (current != NULL) {
    /* 
       Current points to a file_node which can be used. Before we return current we must:
       
       1. Remove current from the free_nodes list.
       2. Add current to the index hash.
       
    */
    block_fs_unlink_free_node( block_fs , current );
    
    current->next = NULL;
    current->prev = NULL;
    return current;
  } else {
    /* No usable nodes in the free nodes list - must allocate a brand new one. */

    long int offset;
    int node_size;
    file_node_type * new_node;
    
    {
      div_t d   = div( min_size , block_fs->block_size );
      node_size = d.quot * block_fs->block_size;
      if (d.rem)
        node_size += block_fs->block_size;
    }
    /* Must lock the total size here ... */
    offset = block_fs->data_file_size;
    new_node = file_node_alloc(NODE_IN_USE , offset , node_size);  
    block_fs_install_node( block_fs , new_node );                   /* <- This will update the total file size. */
    
    return new_node;
  }
}



bool block_fs_has_file( const block_fs_type * block_fs , const char * filename) {
  return hash_has_key( block_fs->index , filename );
}




static void block_fs_unlink_file__( block_fs_type * block_fs , const char * filename ) {
  file_node_type * node = hash_pop( block_fs->index , filename );
  node->status      = NODE_FREE;
  node->data_offset = 0;
  node->data_size   = 0;
  if (block_fs->data_stream != NULL) {  
    fsync( fileno(block_fs->data_stream) );
    block_fs_fseek_data(block_fs , node->node_offset);
    file_node_fwrite( node , NULL , block_fs->data_stream );
    fsync( fileno(block_fs->data_stream) );
  }
  block_fs_insert_free_node( block_fs , node );
}


void block_fs_unlink_file( block_fs_type * block_fs , const char * filename) {
  block_fs_aquire_wlock( block_fs );
  block_fs_unlink_file__( block_fs , filename );
  block_fs_release_rwlock( block_fs );
}


/**
   The single lowest-level write function:
   
     2. fsync() the datafile.
     3. seek to correct position.
     4. Write the data with util_fwrite()
     5. fsync() again.

     7. increase the write_count
     8. set the data_size field of the node.

   Observe that when 'designing' this file-system the priority has
   been on read-spead, one consequence of this is that all write
   operations are sandwiched between two fsync() calls; that
   guarantees that the read access (which should be the fast path) can
   be without any calls to fsync().

   Not necessary to lock - since all writes are protected by the
   'global' rwlock anyway.
*/


static void block_fs_fwrite__(block_fs_type * block_fs , const char * filename , file_node_type * node , const void * ptr , int data_size) {
  if ((node->cache_size == data_size) && (memcmp( ptr , node->cache , data_size ) == 0)) 
    /* The current cache is identical to the data we are attempting to write - can leave immediately. */
    return;
  else {
    fsync( fileno(block_fs->data_stream) );
    block_fs_fseek_data(block_fs , node->node_offset);
    node->status      = NODE_IN_USE;
    node->data_size   = data_size; 
    file_node_set_data_offset( node , filename );
    
    /* This marks the node section in the datafile as write in progress with: NODE_WRITE_ACTIVE_START ... NODE_WRITE_ACTIVE_END */
    file_node_init_fwrite( node , block_fs->data_stream );                
    
    /* Writes the actual data content. */
    block_fs_fseek_data(block_fs , node->data_offset);
    util_fwrite( ptr , 1 , data_size , block_fs->data_stream , __func__);
    
    /* Writes the file node header data, including the NODE_END_TAG. */
    file_node_fwrite( node , filename , block_fs->data_stream );
    
    fsync( fileno(block_fs->data_stream) );
    block_fs_fseek_data(block_fs , block_fs->data_file_size);
    ftell( block_fs->data_stream );
    
    /* Update the cache */
    if (data_size <= block_fs->max_cache_size)
      file_node_update_cache( node , data_size , ptr );
    else
      file_node_clear_cache( node );
    block_fs->write_count++;
  }
}



void block_fs_fwrite_file(block_fs_type * block_fs , const char * filename , const void * ptr , size_t data_size) {
  block_fs_aquire_wlock( block_fs );
  {
    file_node_type * file_node;
    size_t min_size = data_size;
    bool   new_node = true;   
    min_size += file_node_header_size( filename );
    
    if (block_fs_has_file( block_fs , filename )) {
      file_node = hash_get( block_fs->index , filename );
      if (file_node->node_size < min_size) {
        /* The current node is too small for the new content:
           
           1. Remove the existing node, from the index and insert it into the free_nodes list.
           2. Get a new node.
      
        */
        block_fs_unlink_file__( block_fs , filename );
        file_node = block_fs_get_new_node( block_fs , filename , min_size );
      } else
        new_node = false;  /* We are reusing the existing node. */
    } else 
      file_node = block_fs_get_new_node( block_fs , filename , min_size );
    
    /* The actual writing ... */
    block_fs_fwrite__( block_fs , filename , file_node , ptr , data_size);
    if (new_node)
      block_fs_insert_index_node(block_fs , filename , file_node);
  }
  block_fs_release_rwlock( block_fs );
}



void block_fs_fwrite_buffer(block_fs_type * block_fs , const char * filename , const buffer_type * buffer) {
  block_fs_fwrite_file( block_fs , filename , buffer_get_data( buffer ) , buffer_get_size( buffer ));
}


/**
   Need extra locking here - because the global rwlock allows many concurrent readers.
*/
static void block_fs_fread__(block_fs_type * block_fs , const file_node_type * file_node , void * ptr , size_t read_bytes) {
  if (file_node->cache != NULL) 
    file_node_read_from_cache( file_node , ptr , read_bytes);
  else {
    pthread_mutex_lock( &block_fs->io_lock );
    block_fs_fseek_data( block_fs , file_node->data_offset );
    util_fread( ptr , 1 , read_bytes , block_fs->data_stream , __func__);
    //file_node_verify_end_tag( file_node , block_fs->data_stream );
    pthread_mutex_unlock( &block_fs->io_lock );
  }
}


/**
   Reads the full content of 'filename' into the buffer. 
*/

void block_fs_fread_realloc_buffer( block_fs_type * block_fs , const char * filename , buffer_type * buffer) {
  block_fs_aquire_rlock( block_fs );
  {
    file_node_type * node = hash_get( block_fs->index , filename);
    
    buffer_clear( buffer );   /* Setting: content_size = 0; pos = 0;  */
    {
      /* 
         Going low-level - essentially a second implementation of
         block_fs_fread__():
      */
      if (node->cache != NULL) 
        file_node_buffer_read_from_cache( node , buffer );
      else {
        pthread_mutex_lock( &block_fs->io_lock );
        block_fs_fseek_data(block_fs , node->data_offset);
        buffer_stream_fread( buffer , node->data_size , block_fs->data_stream );
        //file_node_verify_end_tag( node , block_fs->data_stream );
        pthread_mutex_unlock( &block_fs->io_lock );
      }
      
    }
    buffer_rewind( buffer );  /* Setting: pos = 0; */
  }
  block_fs_release_rwlock( block_fs );
}







/*
  This function will read all the data stored in 'filename' - it is
  the responsability of the calling scope that ptr is sufficiently
  large to hold it. You can use block_fs_get_filesize() first to
  check.
*/


void block_fs_fread_file( block_fs_type * block_fs , const char * filename , void * ptr) {
  block_fs_aquire_rlock( block_fs );
  {
    file_node_type * node = hash_get( block_fs->index , filename);
    block_fs_fread__( block_fs , node , ptr , node->data_size);
  }
  block_fs_release_rwlock( block_fs );
}



int block_fs_get_filesize( const block_fs_type * block_fs , const char * filename) {
  file_node_type * node = hash_get( block_fs->index , filename );
  return node->data_size;
}



/**
   If one of the files data_file or index_file already exists the
   function will fail hard.
*/


void block_fs_sync( block_fs_type * block_fs ) {
  
}



/**
   Close/synchronize the open file descriptors and free all memory
   related to the block_fs instance.

   If the boolean unlink_empty is set to true all the files will be
   unlinked if the filesystem is empty.
*/

void block_fs_close( block_fs_type * block_fs , bool unlink_empty) {
  block_fs_sync( block_fs );

  printf("Shutting down filesystem: %s",block_fs->mount_file); fflush(stdout);
  fclose( block_fs->data_stream );
  if (block_fs->lock_fd > 0)
    close( block_fs->lock_fd );     /* Closing the lock_file file descriptor - and releasing the lock. */

  if ( unlink_empty && (hash_get_size( block_fs->index ) == 0)) {
    util_unlink_existing( block_fs->mount_file );
    util_unlink_existing( block_fs->data_file );
  }
  util_unlink_existing( block_fs->lock_file );

  free( block_fs->lock_file );
  free( block_fs->base_name );
  free( block_fs->data_file );
  free( block_fs->path );
  free( block_fs->mount_file );
  free( block_fs->mount_point );
  
  hash_free( block_fs->index );
  vector_free( block_fs->file_nodes );
  free( block_fs );
  printf("\n");

}


static void block_fs_rotate__( block_fs_type * block_fs ) {
  block_fs_type * old_fs = util_malloc( sizeof * old_fs , __func__);
  block_fs_type * new_fs;

  /* 
     Write a updated mount map where the version info has been bumped
     up with one; the new_fs will mount based on this mount_file.
  */
  block_fs_fwrite_mount_info__( block_fs->mount_file , block_fs->version + 1 ); 
  new_fs = block_fs_mount( block_fs->mount_file , block_fs->block_size , block_fs->max_cache_size , block_fs->fragmentation_limit , false);
  
  /* Play it over sam ... */
  {
    hash_iter_type * iter = hash_iter_alloc( block_fs->index );
    buffer_type * buffer = buffer_alloc(1024);

    while (!hash_iter_is_complete( iter )) {
      const char * key = hash_iter_get_next_key( iter );
      block_fs_fread_realloc_buffer(block_fs , key , buffer );   /* Load from the original file. */
      block_fs_fwrite_buffer( new_fs , key , buffer);            /* Write to the new file. */
    }
    
    buffer_free( buffer );
    hash_iter_free( iter );
  }
  
  /* Copy: 
        block_fs -> old_fs
        new_fs   -> block_fs
     close & free old_fs and continue with block_fs.
  */
  
  memcpy(old_fs , block_fs , sizeof * block_fs );
  memcpy(block_fs , new_fs , sizeof * block_fs );
  printf("%s: planning to unlink: %s \n",__func__ , old_fs->data_file );
  //unlink( old_fs->data_file );
  block_fs_close( old_fs , false );
}



/*****************************************************************/
/* Functions related to pretty-printing an image of the data file. */
/*****************************************************************/

static sort_node_type * sort_node_alloc(const char * name , long int offset , int node_size , int data_size) {
  sort_node_type * node = util_malloc( sizeof * node , __func__);
  node->name            = util_alloc_string_copy( name );
  node->node_offset          = offset;
  node->node_size       = node_size;
  node->data_size       = data_size;
  return node;
}

static void sort_node_free( sort_node_type * node ) {
  free( node->name );
  free( node );
}

static void sort_node_free__( void * node) {
  sort_node_free( (sort_node_type *) node );
}

static int sort_node_cmp( const void * arg1 , const void * arg2 ) {
  const sort_node_type * node1 = (sort_node_type *) arg1;
  const sort_node_type * node2 = (sort_node_type *) arg2;

  if (node1->node_offset > node2->node_offset)
    return 1;
  else
    return -1;
}

static void sort_node_fprintf(const sort_node_type * node, FILE * stream) {
  fprintf(stream , "%-20s  %10ld  %8d  %8d \n",node->name , node->node_offset , node->node_size , node->data_size);
}



void block_fs_fprintf( const block_fs_type * block_fs , FILE * stream) {
  vector_type    * sort_vector = vector_alloc_new();
  
  /* Inserting the nodes from the index. */
  {
    hash_iter_type * iter        = hash_iter_alloc( block_fs->index );
    while ( !hash_iter_is_complete( iter )) {
      const char * key            = hash_iter_get_next_key( iter );
      const file_node_type * node = hash_get( block_fs->index , key );
      sort_node_type * sort_node  = sort_node_alloc( key , node->node_offset , node->node_size , node->data_size);
      vector_append_owned_ref( sort_vector , sort_node , sort_node_free__ );
    }
    hash_iter_free( iter );
  }
  
  /* Inserting the free nodes - the holes. */
  {
    file_node_type * current = block_fs->free_nodes;
    while (current != NULL) {
      sort_node_type * sort_node  = sort_node_alloc( "--FREE--", current->node_offset , current->node_size , 0);
      vector_append_owned_ref( sort_vector , sort_node , sort_node_free__ );
      current = current->next;
    }
  }
  vector_sort(sort_vector , sort_node_cmp);
  fprintf(stream , "=======================================================\n");
  fprintf(stream , "%-20s  %10s   %8s  %8s\n","Filename" , "Offset", "Nodesize","Filesize");
  fprintf(stream , "-------------------------------------------------------\n");
  {
    int i;
    for (i=0; i < vector_get_size( sort_vector ); i++) 
      sort_node_fprintf( vector_iget_const( sort_vector , i) , stream);
  }
  fprintf(stream , "-------------------------------------------------------\n");
  
  vector_free( sort_vector );
}


void block_fs_fprintf_index( const block_fs_type * block_fs , FILE * stream) {
  hash_iter_type * iter        = hash_iter_alloc( block_fs->index );
  while ( !hash_iter_is_complete( iter )) {
    const char * key            = hash_iter_get_next_key( iter );
    const file_node_type * node = hash_get( block_fs->index , key );
    file_node_fprintf( node , key , stream);
  }
  fprintf(stream,"\n-----------------------------------------------------------------\nFree nodes: \n");
  block_fs_fprintf_free_nodes( block_fs , stream );
}


