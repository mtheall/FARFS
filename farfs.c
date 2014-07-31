#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fuse.h>
#include <fuse_opt.h>

/*! \def le32_to_cpu(x)
 *
 *  Convert a 32-bit value from little-endian to native
 */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define le32_to_cpu(x) (x)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define le32_to_cpu(x) __builtin_bswap32(x)
#else
#error "You are neither big nor little endian"
#endif

/*! Magic macro */
#define MAGIC(a,b,c,d) ((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))

/*! FAR magic */
#define FAR_MAGIC      MAGIC('F', 'A', 'R', '\0')

/*! FARFS entry type */
typedef enum
{
  FAR_FILE_TYPE = 0, /*!< File entry */
  FAR_DIR_TYPE  = 1, /*!< Directory entry */
} far_type_t;

/*! FARFS directory mode (dr-xr-xr-x) */
#define FAR_DIR_MODE  (S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH|S_IFDIR)
/*! FARFS file mode (-r--r--r--) */
#define FAR_FILE_MODE (S_IRUSR|S_IRGRP|S_IROTH|S_IFREG)

/*! FAR file name */
static const char *far_file = NULL;

/*! FAR file last access time */
static time_t far_atime;
/*! FAR file last modification time */
static time_t far_mtime;
/*! FAR file last attribute change time */
static time_t far_ctime;

/*! FAR file mmap address */
static void   *far_mapping;

/*! FAR entry */
typedef struct FARentry_t
{
  uint32_t flags;   /*!< flags; currently lower byte is far_type_t, upper bytes unused */
  uint32_t nameoff; /*!< offset (from header) to name */
  uint32_t dataoff; /*!< offset (from header) to data */
  uint32_t size;    /*!< number of bytes (for file) or number of entries (for directory) */
} FARentry_t;

/*! FAR header */
typedef struct FARheader_t
{
  uint32_t   magic;       /*!< magic marker "FAR\0" */
  uint32_t   version;     /*!< archive version */
  uint32_t   nentries;    /*!< total number of entries */
  uint32_t   namesize;    /*!< ??? */
  uint32_t   rootentries; /*!< number of entries in root directory */
  FARentry_t rootdir[];   /*!< array of entries for root directory */
} FARheader_t;

/*! FAR open directory handle */
typedef struct far_dir_t
{
  const FARentry_t *parent; /*!< pointer to parent entry */
  const FARentry_t *entry;  /*!< pointer to entry */
} far_dir_t;

/*! FAR header */
static FARheader_t *header = NULL;

/*! A dummy root entry */
static FARentry_t dummy_root =
{
  .flags   = FAR_DIR_TYPE,
  .nameoff = 0,
  .dataoff = offsetof(FARheader_t, rootdir),
  .size    = 0,
};

/*! pointer to dummy_root */
static FARentry_t *root = &dummy_root;

/*! Get type from an entry
 *
 *  @param[in] entry Entry to get type of
 *
 *  @returns type of entry
 */
static inline far_type_t
far_type(const FARentry_t *entry)
{
  return le32_to_cpu(entry->flags) & 0xFF;
}

/*! Get name for an entry
 *
 *  @param[in] entry Entry to get name of
 *
 *  @returns name of entry
 */
static inline const char*
far_name(const FARentry_t *entry)
{
  return (const char*)far_mapping + le32_to_cpu(entry->nameoff);
}

/*! Get data for an entry
 *
 *  @param[in] entry Entry to get data for
 *
 *  @returns data for entry
 */
static inline const void*
far_data(const FARentry_t *entry)
{
  return (const char*)far_mapping + le32_to_cpu(entry->dataoff);
}

/*! Get data size for an entry
 *
 *  @param[in] entry Entry to get data size for
 *
 *  @returns data size for entry
 */
static inline uint32_t
far_datasize(const FARentry_t *entry)
{
  return le32_to_cpu(entry->size);
}

/*! Get children for a directory entry
 *
 *  @param[in] entry Entry to get children of
 *
 *  @returns children of entry
 */
static inline const FARentry_t*
far_children(const FARentry_t *entry)
{
  if(far_type(entry) != FAR_DIR_TYPE)
    abort();

  return (const FARentry_t*)far_data(entry);
}

/*! Create a new open directory handle
 *
 *  @param[in] parent Parent of entry
 *  @param[in] entry  Opened entry
 *
 *  @returns open directory handle
 */
static inline far_dir_t*
far_dir_new(const FARentry_t *parent,
            const FARentry_t *entry)
{
  far_dir_t *d = (far_dir_t*)malloc(sizeof(far_dir_t));
  if(d != NULL)
  {
    d->parent = parent;
    d->entry  = entry;
  }

  return d;
}
            
/*! Fill a stat struct from an entry
 *
 *  @param[in]  entry Entry to use
 *  @param[out] st    Buffer to fill
 */
static void
far_fill_stat(const FARentry_t *entry,
              struct stat      *st)
{
  st->st_dev = 0;

  if(entry == root)
    st->st_ino = 1;
  else
    st->st_ino = (entry - header->rootdir) + 2;

  if(far_type(entry) == FAR_DIR_TYPE)
  {
    uint32_t         i;
    uint32_t         num_children = far_datasize(entry);
    const FARentry_t *children    = far_children(entry);

    for(i = 0, st->st_nlink = 2; i < num_children; ++i)
    {
      if(far_type(children+i) == FAR_DIR_TYPE)
        st->st_nlink += 1;
    }
  }
  else
    st->st_nlink   = 1;

  st->st_uid     = getuid();
  st->st_gid     = getgid();
  st->st_rdev    = 0;
  if(far_type(entry) == FAR_DIR_TYPE)
    st->st_size = far_datasize(entry) * sizeof(FARentry_t);
  else
    st->st_size = far_datasize(entry);

  st->st_blksize = 4096;
  st->st_blocks  = (st->st_size + st->st_blksize-1) / 512;
  st->st_atime   = far_atime;
  st->st_mtime   = far_mtime;
  st->st_ctime   = far_ctime;

  if(far_type(entry) == FAR_DIR_TYPE)
    st->st_mode = FAR_DIR_MODE;
  else
    st->st_mode = FAR_FILE_MODE;
}

/*! Traverse path to get entry
 *
 *  @param[in] path Path to traverse
 *
 *  @returns entry that was found
 *  @returns NULL for no entry
 */
static const FARentry_t*
far_traverse_path(const char       *path,
                  const FARentry_t **parent)
{
  const char       *p, *name;
  const FARentry_t *dir = root, *entry;
  size_t           i, num_subdirs;

  *parent = dir;

  /* special case; this is the root directory */
  if(strcmp(path, "/") == 0)
    return dir;

  /* iterate through intermediate path components */
  p = strchr(++path, '/');
  while(p != NULL)
  {
    num_subdirs = far_datasize(dir);

    /* look at each child for a match */
    for(i = 0; i < num_subdirs; ++i)
    {
      /* check if the name matches */
      entry = far_children(dir) + i;
      name = far_name(entry);
      if(strlen(name) == p-path && memcmp(path, name, p-path) == 0)
      {
        /* we found a match; stop looking at these children */
        *parent = dir;
        dir = entry;
        break;
      }
    }

    /* move to the next component */
    path = ++p;
    p = strchr(path, '/');
  }

  num_subdirs = far_datasize(dir);

  /* we are at the final component; look at each child for a match */
  for(i = 0; i < num_subdirs; ++i)
  {
    /* check if the name matches */
    entry = far_children(dir) + i;
    if(strcmp(path, far_name(entry)) == 0)
      return entry;
  }

  /* didn't find this entry */
  return NULL;
}

/*! Get attributes
 *
 *  @param[in]  path Path to lookup
 *  @param[out] st   Buffer to fill
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
far_getattr(const char  *path,
            struct stat *st)
{
  const FARentry_t *parent, *entry = far_traverse_path(path, &parent);
  if(entry == NULL)
    return -ENOENT;

  far_fill_stat(entry, st);
  return 0;
}

/*! Read a directory
 *
 *  @param[in]  path   Directory to read
 *  @param[out] buffer Buffer to fill
 *  @param[in]  filler Callback which fills buffer
 *  @param[in]  offset Directory offset
 *  @param[in]  fi     Open directory information
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
far_readdir(const char            *path,
            void                  *buffer,
            fuse_fill_dir_t       filler,
            off_t                 offset,
            struct fuse_file_info *fi)
{
  struct stat     st;
  off_t           off;

  /* we set up this entry pointer in far_opendir */
  far_dir_t        *dir = (far_dir_t*)fi->fh;
  const FARentry_t *child;

  /* offset 0 means '.' */
  if(offset == 0)
  {
    far_fill_stat(dir->entry, &st);
    if(filler(buffer, ".", &st, ++offset))
      return 0;
  }

  /* offset 1 means '..' */
  if(offset == 1)
  {
    far_fill_stat(dir->parent, &st);
    if(filler(buffer, "..", &st, ++offset))
      return 0;
  }

  /* skip until we reach the desired offset */
  for(off = 2; off-2 < far_datasize(dir->entry); ++off)
  {
    if(off == offset)
    {
      /* we have reached the desired offset; start filling */
      child = far_children(dir->entry) + (off - 2);
      far_fill_stat(child, &st);
      if(filler(buffer, far_name(child), &st, ++offset))
        return 0;
    }
  }

  return 0;;
}

/*! Open a file
 *
 *  @param[in]  path File to open
 *  @param[out] fi   Open file information
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
far_open(const char            *path,
         struct fuse_file_info *fi)
{
  const FARentry_t *parent, *entry;

  /* lookup the path */
  entry = far_traverse_path(path, &parent);
  if(entry == NULL)
  {
    /* we didn't find it. if O_CREAT was specified, return EROFS */
    if(fi->flags & O_CREAT)
      return -EROFS;
    /* otherwise, return ENOENT */
    return -ENOENT;
  }

  /* don't allow write mode */
  if((fi->flags & O_ACCMODE) == O_RDWR)
    return -EACCES;
  if((fi->flags & O_ACCMODE) == O_WRONLY)
    return -EACCES;

  /* set the open file info to point to our found entry */
  fi->fh = (unsigned long)entry;

  /* tell kernel to cache data */
  fi->keep_cache = 0;

  return 0;
}

/*! Read a file
 *
 *  @param[in]  path   Path of open file
 *  @param[out] buffer Buffer to fill
 *  @param[in]  size   Size to fill
 *  @param[in]  offset Offset to start at
 *
 *  @returns number of bytes read
 *  @returns negated errno otherwise
 */
static int
far_read(const char            *path,
         char                  *buffer,
         size_t                size,
         off_t                 offset,
         struct fuse_file_info *fi)
{
  FARentry_t *entry = (FARentry_t*)fi->fh;

  if(offset < 0)
    return -EINVAL;

  /* past end-of-file; return 0 bytes read */
  if(offset >= far_datasize(entry))
    return 0;

  /* if they want to read past end-of-file, truncate the amount to read */
  if(offset + size > far_datasize(entry))
    size = far_datasize(entry) - offset;

  /* copy the data */
  memcpy(buffer, far_data(entry) + offset, size);

  /* return number of bytes copied */
  return size;
}

/*! Open a directory
 *
 *  @param[in]  path Path to open
 *  @param[out] fi   Open directory information
 *
 *  @returns 0 for success
 *  @returns negated errno otherwise
 */
static int
far_opendir(const char            *path,
            struct fuse_file_info *fi)
{
  const FARentry_t *parent, *entry;
  far_dir_t        *dir;

  /* lookup the path */
  entry = far_traverse_path(path, &parent);
  if(entry == NULL)
    return -ENOENT;

  /* make sure this is a directory */
  if(far_type(entry) != FAR_DIR_TYPE)
    return -ENOTDIR;

  dir = far_dir_new(parent, entry);
  if(dir == NULL)
    return -ENOMEM;

  /* set the open directory info to point to our found entry */
  fi->fh = (unsigned long)dir;
  return 0;
}

static int
far_releasedir(const char            *path,
               struct fuse_file_info *fi)
{
  far_dir_t *dir = (far_dir_t*)fi->fh;
  free(dir);

  return 0;
}

/*! FARFS FUSE operations */
static const struct fuse_operations far_ops =
{
  .getattr          = far_getattr,
  .open             = far_open,
  .opendir          = far_opendir,
  .read             = far_read,
  .readdir          = far_readdir,
  .releasedir       = far_releasedir,
  .flag_nullpath_ok = 1,
  .flag_nopath      = 1,
};

/*! fuse_opt_parse callback
 *
 *  @param[in]  data    Unused
 *  @param[in]  arg     Current argument
 *  @param[in]  key     Argument key
 *  @param[out] outargs Unused
 *
 *  @returns 0 for success and discard
 *  @returns 1 for success and keep
 *  @returns -1 for failure
 */
static int
far_process_arg(void             *data,
                  const char       *arg,
                  int              key,
                  struct fuse_args *outargs)
{
  if(key == FUSE_OPT_KEY_NONOPT)
  {
    if(far_file == NULL)
    {
      /* discard first non-option, which is the far filename */
      far_file = arg;
      return 0;
    }
  }

  return 1;
}

int main(int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct stat      st;
  int              fd, rc;

  /* parse options */
  if(fuse_opt_parse(&args, NULL, NULL, far_process_arg) != 0)
    return EXIT_FAILURE;
  if(far_file == NULL)
    return EXIT_FAILURE;

  /* open the far file */
  fd = open(far_file, O_RDONLY);
  if(fd < 0)
  {
    perror("open");
    return EXIT_FAILURE;
  }

  /* get the file information */
  rc = fstat(fd, &st);
  if(rc != 0)
  {
    perror("fstat");
    close(fd);
    return EXIT_FAILURE;
  }

  /* set up global data about the far file */
  far_atime = st.st_atime;
  far_mtime = st.st_mtime;
  far_ctime = st.st_ctime;

  /* mmap the far file */
  far_mapping = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if(far_mapping == MAP_FAILED)
  {
    perror("mmap");
    close(fd);
    return EXIT_FAILURE;
  }
  close(fd);

  header = (FARheader_t*)far_mapping;
  if(le32_to_cpu(header->magic) != FAR_MAGIC)
  {
    fprintf(stderr, "Invalid magic %#x\n", le32_to_cpu(header->magic));
    munmap(far_mapping, st.st_size);
    return EXIT_FAILURE;
  }
  if(le32_to_cpu(header->version) != 0)
  {
    fprintf(stderr, "Invalid version %#x\n", le32_to_cpu(header->version));
    munmap(far_mapping, st.st_size);
    return EXIT_FAILURE;
  }
  root->size = header->rootentries;

  /* run the FUSE loop */
  rc = fuse_main(args.argc, args.argv, &far_ops, NULL);

  /* clean up */
  fuse_opt_free_args(&args);
  munmap(far_mapping, st.st_size);

  return rc;
}
