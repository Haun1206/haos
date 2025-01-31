#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/fat.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk {
	//disk_sector_t start;                /* First data sector. */
	cluster_t start;
	cluster_t last;
	bool is_dir;
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	bool bunused[3];
	uint32_t unused[122];               /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_sectors (off_t size) {
	return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	disk_sector_t sector;               /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct inode_disk data;             /* Inode content. */
};

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) {
	ASSERT (inode != NULL);
	if (pos < inode->data.length){
		cluster_t clst = inode->data.start;
		for(int i=0; i< pos / (DISK_SECTOR_SIZE * SECTORS_PER_CLUSTER) ; i++){
			clst = fat_get(clst);
		}
		return cluster_to_sector(clst);
	}
	else
		return -1;
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length, bool is_dir) {
	struct inode_disk *disk_inode = NULL;
	bool success = true;
	

	ASSERT (length >= 0);
//printf("HI\n");
	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);
//	printf("HI\n");
	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t sectors = bytes_to_sectors (length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		disk_inode->is_dir = is_dir;
		//printf("HI\n");
		//printf("ISDIR: %d\n",is_dir);

		//메모1 : inode disk랑 data disk랑 연결이 안되있게 되있었고 그래서 그렇게 했는데 일단 메모
		//메모2 : sectors가 0일때는 start 가 할당이 되는건지 확실 ㄴ 기존 코드에서 bit map 막이용해서 어찌한건지 이해가 안됨.
		
		if(!is_dir){
			cluster_t clst = fat_create_chain (0);
			//printf("HI\n");
			if(clst){
				disk_inode->start = clst;
				for (int i=1; i < sectors; i++){
					clst = fat_create_chain(clst);
					//printf("%d\n",clst);
					//printf("HI\n");
					if(!clst){
						return false;
					}
					if(i == sectors-1)
						disk_inode->last = clst;
				}
				//printf("%d\n",disk_inode->last);
				static char zeros[DISK_SECTOR_SIZE];
				cluster_t clst2 = disk_inode->start;
				for(int i = 0; i<sectors; i++){ 
					disk_write (filesys_disk, cluster_to_sector(clst2), zeros);
					clst2 = fat_get(clst2);
				}
				//printf("HI\n");
			}
			else 
				return false;
		}
		//printf("%d\n",success);
		disk_write (filesys_disk, sector, disk_inode);
		//printf("HI\n");
		free(disk_inode);
		//printf("HI\n");
	}
	return success;
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) {
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->sector == sector) {
			inode_reopen (inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	disk_read (filesys_disk, inode->sector, &inode->data);
	if(sector==1)
		inode->data.is_dir = 1;
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	//printf("HI\n");
	if (inode != NULL)
		inode->open_cnt++;
	//printf("HI\n");
	return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode) {
	return inode->sector;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			cluster_t clst = (cluster_t)inode->sector;
			fat_put (clst, 0);
			clst = inode->data.start;
			for(int i=0 ; i<inode->data.length ; i++){
				fat_put (clst, 0);
				clst = fat_get(clst);
			}
		}

		free (inode); 
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void
inode_remove (struct inode *inode) {
	ASSERT (inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx;
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left;
		int sector_left;
		int min_left;
		if(inode->data.is_dir == 0){
			sector_idx = byte_to_sector (inode, offset);
			inode_left = inode_length (inode) - offset;
			sector_left = DISK_SECTOR_SIZE - sector_ofs;
			min_left = inode_left < sector_left ? inode_left : sector_left;
		}
		else{
			sector_idx = inode->sector;
			inode_left = DISK_SECTOR_SIZE - offset;
			sector_left = DISK_SECTOR_SIZE - sector_ofs;
			min_left = inode_left < sector_left ? inode_left : sector_left;
		}

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free (bounce);

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;
	//printf("HI\n");
	if (inode->deny_write_cnt)
		return 0;
	//printf("HI\n");
	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		//printf("HI\n");
		disk_sector_t sector_idx; 
		//printf("HI\n");
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left;
		int sector_left;
		int min_left;
		if(inode->data.is_dir == 0){
			sector_idx = byte_to_sector (inode, offset);
			inode_left = inode_length (inode) - offset;
			sector_left = DISK_SECTOR_SIZE - sector_ofs;
			min_left = inode_left < sector_left ? inode_left : sector_left;
		}
		else{
			sector_idx = inode->sector;
			inode_left = DISK_SECTOR_SIZE - offset;
			sector_left = DISK_SECTOR_SIZE - sector_ofs;
			min_left = inode_left < sector_left ? inode_left : sector_left;
		}

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		//printf("%d\n",chunk_size);
		if (chunk_size <= 0){
			//printf("HI\n");
			size_t old_sectors = bytes_to_sectors(inode_length (inode));
			//printf("HI\n");
			inode->data.length = offset + size;
			size_t addition = bytes_to_sectors(inode_length (inode)) - old_sectors;
			//printf("HI\n");
			if(addition > 0){
				cluster_t clst;
				for(int i=0 ; i<addition ; i++){
					//printf("HI\n");
					//printf("%d\n",inode->data.start);
					//printf("%d\n",inode->data.last);
					clst = fat_create_chain (inode->data.last);
					//printf("HI\n");
					disk_write (filesys_disk, cluster_to_sector(clst), 0);
					//printf("HI\n");
				}
				inode->data.last = clst;
				continue;
			}
			else continue;
		}

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else {
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);                                                                      
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left)
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
			printf("%llx\n",buffer+bytes_written);
			printf("%d\n", sector_idx);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free (bounce);

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode) {
	return inode->data.length;
}

bool
inode_is_dir(const struct inode *inode){
  	if (inode->removed){
		//printf("HI\n");
    	return false;
	}
	return inode->data.is_dir;
}