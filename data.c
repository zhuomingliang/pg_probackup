/*-------------------------------------------------------------------------
 *
 * data.c: data parsing pages
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2017, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "libpq/pqsignal.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/checksum_impl.h"

typedef struct BackupPageHeader
{
	BlockNumber	block;			/* block number */
} BackupPageHeader;

static bool
parse_page(const DataPage *page, XLogRecPtr *lsn)
{
	const PageHeaderData *page_data = &page->page_data;

	/* Get lsn from page header */
	*lsn = PageXLogRecPtrGet(page_data->pd_lsn);

	if (PageGetPageSize(page_data) == BLCKSZ &&
		PageGetPageLayoutVersion(page_data) == PG_PAGE_LAYOUT_VERSION &&
		(page_data->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
		page_data->pd_lower >= SizeOfPageHeaderData &&
		page_data->pd_lower <= page_data->pd_upper &&
		page_data->pd_upper <= page_data->pd_special &&
		page_data->pd_special <= BLCKSZ &&
		page_data->pd_special == MAXALIGN(page_data->pd_special))
		return true;

	return false;
}

static void
backup_data_page(pgFile *file, const XLogRecPtr *lsn,
				BlockNumber blknum, BlockNumber nblocks,
				FILE *in, FILE *out,
				pg_crc32 *crc)
{
	BackupPageHeader	header;
	off_t				offset;
	DataPage			page; /* used as read buffer */
	size_t				write_buffer_size = sizeof(header) + BLCKSZ;
	char				write_buffer[write_buffer_size];
	size_t				read_len = 0;
	XLogRecPtr	page_lsn;
	int 	ret;
	int		try_checksum = 100;
	bool	is_zero_page = false;
	struct stat 		st;

	header.block = blknum;
	offset = blknum * BLCKSZ;

	while(try_checksum--)
	{
		ret = fseek(in, offset, SEEK_SET);
		if (ret != 0)
			elog(ERROR, "Can't seek in file offset: %llu ret:%i\n",
			(long long unsigned int) offset, ret);

		read_len = fread(&page, 1, sizeof(page), in);

		if (read_len != sizeof(page))
		{
			stat(file->path, &st);

			if (st.st_size/BLCKSZ <= blknum)
			{
				if (verbose)
					elog(LOG, "File: %s, file was truncated after backup start."
							  "Expected nblocks %u. Real nblocks %ld. Cannot read block %u ",
							  file->path, nblocks, st.st_size/BLCKSZ, blknum);
				return;
			}
			else
				elog(ERROR, "File: %s, block size of block %u of nblocks %u is incorrect %lu",
						file->path, blknum, nblocks, read_len);
		}

		/*
		* If an invalid data page was found, fallback to simple copy to ensure
		* all pages in the file don't have BackupPageHeader.
		*/
		if (!parse_page(&page, &page_lsn))
		{
			int i;
			/* Check if the page is zeroed. */
			for(i = 0; i < BLCKSZ && page.data[i] == 0; i++);
			if (i == BLCKSZ)
			{
				is_zero_page = true;
				elog(LOG, "File: %s blknum %u, empty page", file->path, blknum);
			}

			/*
			 * If page is not completely empty and we couldn't parse it,
			 * try again several times. If it didn't help, throw error
			 */
			if (!is_zero_page)
			{
				/* Try to read and verify this page again several times. */
				if (try_checksum)
				{
					elog(WARNING, "File: %s blknum %u have wrong page header, try again",
									file->path, blknum);
					usleep(100);
					continue;
				}
				else
					elog(ERROR, "File: %s blknum %u have wrong page header.", file->path, blknum);
			}
		}

		/* Verify checksum */
		if(current.checksum_version && !is_zero_page)
		{
			/*
			 * If checksum is wrong, sleep a bit and then try again
			 * several times. If it didn't help, throw error
			 */
			if (pg_checksum_page(page.data, file->segno * RELSEG_SIZE + blknum) != ((PageHeader) page.data)->pd_checksum)
			{
				if (try_checksum)
				{
					elog(WARNING, "File: %s blknum %u have wrong checksum, try again",
									file->path, blknum);
					usleep(100);
				}
				else
					elog(ERROR, "File: %s blknum %u have wrong checksum.",
									file->path, blknum);
			}
		}
		else
			try_checksum = 0;
	}

	file->read_size += read_len;

	memcpy(write_buffer, &header, sizeof(header));
	memcpy(write_buffer + sizeof(header), page.data, BLCKSZ);
	/* write data page */
	if(fwrite(write_buffer, 1, write_buffer_size, out) != write_buffer_size)
	{
		int errno_tmp = errno;
		fclose(in);
		fclose(out);
		elog(ERROR, "File: %s, cannot write backup at block %u : %s",
				file->path, blknum, strerror(errno_tmp));
	}

	/* update CRC */
	COMP_CRC32C(*crc, &header, sizeof(header));
	COMP_CRC32C(*crc, page.data, BLCKSZ);

	file->write_size += write_buffer_size;
}

/*
 * Backup data file in the from_root directory to the to_root directory with
 * same relative path.
 * If lsn is not NULL, pages only which are modified after the lsn will be
 * copied.
 */
bool
backup_data_file(const char *from_root, const char *to_root,
				 pgFile *file, const XLogRecPtr *lsn)
{
	char				to_path[MAXPGPATH];
	FILE				*in;
	FILE				*out;
	BlockNumber			blknum = 0;
	BlockNumber			nblocks = 0;
	pg_crc32			crc;
	struct stat 		st;

	INIT_CRC32C(crc);

	/* reset size summary */
	file->read_size = 0;
	file->write_size = 0;

	/* open backup mode file for read */
	in = fopen(file->path, "r");
	if (in == NULL)
	{
		FIN_CRC32C(crc);
		file->crc = crc;

		/* maybe vanished, it's not error */
		if (errno == ENOENT)
			return false;

		elog(ERROR, "cannot open backup mode file \"%s\": %s",
			 file->path, strerror(errno));
	}
	stat(file->path, &st);

	if (st.st_size < file->size)
		elog(WARNING, "File: %s, file was truncated after backup start. Expected size %lu",
					 file->path, file->size);

	if (file->size % BLCKSZ != 0)
		elog(ERROR, "File: %s, file size %lu is incorrect",
					 file->path, file->size);

	nblocks = file->size/BLCKSZ;

	/* open backup file for write  */
	if (check)
		snprintf(to_path, lengthof(to_path), "%s/tmp", backup_path);
	else
		join_path_components(to_path, to_root, file->path + strlen(from_root) + 1);
	out = fopen(to_path, "w");
	if (out == NULL)
	{
		int errno_tmp = errno;
		fclose(in);
		elog(ERROR, "cannot open backup file \"%s\": %s",
			 to_path, strerror(errno_tmp));
	}

	/*
	 * Read each page, verify checksum and write it to backup.
	 * If page map is not empty we scan only these blocks, otherwise
	 * backup all pages of the relation.
	 */
	if (file->pagemap.bitmapsize == 0)
	{
		for (blknum = 0; blknum < nblocks; blknum++)
			backup_data_page(file, lsn, blknum, nblocks, in, out, &crc);
	}
	else
	{
		datapagemap_iterator_t *iter;
		iter = datapagemap_iterate(&file->pagemap);
		while (datapagemap_next(iter, &blknum))
			backup_data_page(file, lsn, blknum, nblocks, in, out, &crc);

		pg_free(iter);
		/*
		 * If we have pagemap then file can't be a zero size.
		 * Otherwise, we will clear the last file.
		 * Increase read_size to delete after.
		 */
		if (file->read_size == 0)
			file->read_size++;
	}

	/* update file permission */
	if (!check && chmod(to_path, FILE_PERMISSION) == -1)
	{
		int errno_tmp = errno;
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot change mode of \"%s\": %s", file->path,
			 strerror(errno_tmp));
	}

	fclose(in);
	fclose(out);

	/* finish CRC calculation and store into pgFile */
	FIN_CRC32C(crc);
	file->crc = crc;

	/* Treat empty file as not-datafile */
	if (file->read_size == 0)
		file->is_datafile = false;

	/* We do not backup if all pages skipped. */
	if (file->write_size == 0 && file->read_size > 0)
	{
		if (remove(to_path) == -1)
			elog(ERROR, "cannot remove file \"%s\": %s", to_path,
				 strerror(errno));
		return false;
	}

	/* remove $BACKUP_PATH/tmp created during check */
	if (check)
		remove(to_path);

	return true;
}

/*
 * Restore compressed file that was backed up partly.
 * 
 */
static void
restore_file_partly(const char *from_root,const char *to_root, pgFile *file)
{
	FILE	   *in;
	FILE	   *out;
	size_t		read_len = 0;
	int			errno_tmp;
	struct stat	st;
	char		to_path[MAXPGPATH];
	char		buf[8192];
	size_t write_size = 0;

	join_path_components(to_path, to_root, file->path + strlen(from_root) + 1);
	/* open backup mode file for read */
	in = fopen(file->path, "r");
	if (in == NULL)
	{
		elog(ERROR, "cannot open backup file \"%s\": %s", file->path,
			strerror(errno));
	}
	out = fopen(to_path, "r+");

	/* stat source file to change mode of destination file */
	if (fstat(fileno(in), &st) == -1)
	{
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot stat \"%s\": %s", file->path,
			 strerror(errno));
	}

	if (fseek(out, 0, SEEK_END) < 0)
		elog(ERROR, "cannot seek END of \"%s\": %s",
				to_path, strerror(errno));

	/* copy everything from backup to the end of the file */
	for (;;)
	{
		if ((read_len = fread(buf, 1, sizeof(buf), in)) != sizeof(buf))
			break;

		if (fwrite(buf, 1, read_len, out) != read_len)
		{
			errno_tmp = errno;
			/* oops */
			fclose(in);
			fclose(out);
			elog(ERROR, "cannot write to \"%s\": %s", to_path,
				 strerror(errno_tmp));
		}
		write_size += read_len;
	}

	errno_tmp = errno;
	if (!feof(in))
	{
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot read backup mode file \"%s\": %s",
			 file->path, strerror(errno_tmp));
	}

	/* copy odd part. */
	if (read_len > 0)
	{
		if (fwrite(buf, 1, read_len, out) != read_len)
		{
			errno_tmp = errno;
			/* oops */
			fclose(in);
			fclose(out);
			elog(ERROR, "cannot write to \"%s\": %s", to_path,
				 strerror(errno_tmp));
		}

		write_size += read_len;
	}

// 	elog(LOG, "restore_file_partly(). %s write_size %lu, file->write_size %lu",
// 			   file->path, write_size, file->write_size);

	/* update file permission */
	if (chmod(to_path, file->mode) == -1)
	{
		int errno_tmp = errno;

		fclose(in);
		fclose(out);
		elog(ERROR, "cannot change mode of \"%s\": %s", to_path,
			strerror(errno_tmp));
	}

	fclose(in);
	fclose(out);
}

static void
restore_compressed_file(const char *from_root,
						const char *to_root,
						pgFile *file)
{
	if (file->is_partial_copy == 0)
		copy_file(from_root, to_root, file);
	else if (file->is_partial_copy == 1)
		restore_file_partly(from_root, to_root, file);
	else
		elog(ERROR, "restore_compressed_file(). Unknown is_partial_copy value %d",
					file->is_partial_copy);
}

/*
 * Restore files in the from_root directory to the to_root directory with
 * same relative path.
 */
void
restore_data_file(const char *from_root,
				  const char *to_root,
				  pgFile *file,
				  pgBackup *backup)
{
	char				to_path[MAXPGPATH];
	FILE			   *in;
	FILE			   *out;
	BackupPageHeader	header;
	BlockNumber			blknum;

	if (!file->is_datafile)
	{
		/*
		 * If the file is not a datafile and not compressed file,
		 * just copy it.
		 */
		if (file->generation == -1)
			copy_file(from_root, to_root, file);
		else
			restore_compressed_file(from_root, to_root, file);

		return;
	}

	/* open backup mode file for read */
	in = fopen(file->path, "r");
	if (in == NULL)
	{
		elog(ERROR, "cannot open backup file \"%s\": %s", file->path,
			 strerror(errno));
	}

	/*
	 * Open backup file for write. 	We use "r+" at first to overwrite only
	 * modified pages for differential restore. If the file is not exists,
	 * re-open it with "w" to create an empty file.
	 */
	join_path_components(to_path, to_root, file->path + strlen(from_root) + 1);
	out = fopen(to_path, "r+");
	if (out == NULL && errno == ENOENT)
		out = fopen(to_path, "w");
	if (out == NULL)
	{
		int errno_tmp = errno;
		fclose(in);
		elog(ERROR, "cannot open restore target file \"%s\": %s",
			 to_path, strerror(errno_tmp));
	}

	for (blknum = 0; ; blknum++)
	{
		size_t		read_len;
		DataPage	page;		/* used as read buffer */

		/* read BackupPageHeader */
		read_len = fread(&header, 1, sizeof(header), in);
		if (read_len != sizeof(header))
		{
			int errno_tmp = errno;
			if (read_len == 0 && feof(in))
				break;		/* EOF found */
			else if (read_len != 0 && feof(in))
				elog(ERROR,
					 "odd size page found at block %u of \"%s\"",
					 blknum, file->path);
			else
				elog(ERROR, "cannot read block %u of \"%s\": %s",
					 blknum, file->path, strerror(errno_tmp));
		}

		if (header.block < blknum)
			elog(ERROR, "backup is broken at block %u",
				 blknum);


		if (fread(page.data, 1, BLCKSZ, in) != BLCKSZ)
			elog(ERROR, "cannot read block %u of \"%s\": %s",
				 blknum, file->path, strerror(errno));

		/* update checksum because we are not save whole */
		if(backup->checksum_version)
		{
			bool is_zero_page = false;

			if(page.page_data.pd_upper == 0)
			{
				int i;
				for(i = 0; i < BLCKSZ && page.data[i] == 0; i++);
				if (i == BLCKSZ)
					is_zero_page = true;
			}

			/* skip calc checksum if zero page */
			if (!is_zero_page)
				((PageHeader) page.data)->pd_checksum = pg_checksum_page(page.data, file->segno * RELSEG_SIZE + header.block);
		}

		/*
		 * Seek and write the restored page. Backup might have holes in
		 * differential backups.
		 */
		blknum = header.block;
		if (fseek(out, blknum * BLCKSZ, SEEK_SET) < 0)
			elog(ERROR, "cannot seek block %u of \"%s\": %s",
				 blknum, to_path, strerror(errno));
		if (fwrite(page.data, 1, sizeof(page), out) != sizeof(page))
			elog(ERROR, "cannot write block %u of \"%s\": %s",
				 blknum, file->path, strerror(errno));
	}

	/* update file permission */
	if (chmod(to_path, file->mode) == -1)
	{
		int errno_tmp = errno;

		fclose(in);
		fclose(out);
		elog(ERROR, "cannot change mode of \"%s\": %s", to_path,
			 strerror(errno_tmp));
	}

	fclose(in);
	fclose(out);
}

/* If someone's want to use this function before correct
 * generation values is set, he can look up for corresponding
 * .cfm file in the file_list
 */
bool
is_compressed_data_file(pgFile *file)
{
	return (file->generation != -1);
}

bool
copy_file(const char *from_root, const char *to_root, pgFile *file)
{
	char		to_path[MAXPGPATH];
	FILE	   *in;
	FILE	   *out;
	size_t		read_len = 0;
	int			errno_tmp;
	char		buf[8192];
	struct stat	st;
	pg_crc32	crc;

	INIT_CRC32C(crc);

	/* reset size summary */
	file->read_size = 0;
	file->write_size = 0;

	/* open backup mode file for read */
	in = fopen(file->path, "r");
	if (in == NULL)
	{
		FIN_CRC32C(crc);
		file->crc = crc;

		/* maybe deleted, it's not error */
		if (errno == ENOENT)
			return false;

		elog(ERROR, "cannot open source file \"%s\": %s", file->path,
			 strerror(errno));
	}

	/* open backup file for write  */
	if (check)
		snprintf(to_path, lengthof(to_path), "%s/tmp", backup_path);
	else
		join_path_components(to_path, to_root, file->path + strlen(from_root) + 1);
	out = fopen(to_path, "w");
	if (out == NULL)
	{
		int errno_tmp = errno;
		fclose(in);
		elog(ERROR, "cannot open destination file \"%s\": %s",
			 to_path, strerror(errno_tmp));
	}

	/* stat source file to change mode of destination file */
	if (fstat(fileno(in), &st) == -1)
	{
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot stat \"%s\": %s", file->path,
			 strerror(errno));
	}

	/* copy content and calc CRC */
	for (;;)
	{
		if ((read_len = fread(buf, 1, sizeof(buf), in)) != sizeof(buf))
			break;

		if (fwrite(buf, 1, read_len, out) != read_len)
		{
			errno_tmp = errno;
			/* oops */
			fclose(in);
			fclose(out);
			elog(ERROR, "cannot write to \"%s\": %s", to_path,
				 strerror(errno_tmp));
		}
		/* update CRC */
		COMP_CRC32C(crc, buf, read_len);

		file->write_size += sizeof(buf);
		file->read_size += sizeof(buf);
	}

	errno_tmp = errno;
	if (!feof(in))
	{
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot read backup mode file \"%s\": %s",
			 file->path, strerror(errno_tmp));
	}

	/* copy odd part. */
	if (read_len > 0)
	{
		if (fwrite(buf, 1, read_len, out) != read_len)
		{
			errno_tmp = errno;
			/* oops */
			fclose(in);
			fclose(out);
			elog(ERROR, "cannot write to \"%s\": %s", to_path,
				 strerror(errno_tmp));
		}
		/* update CRC */
		COMP_CRC32C(crc, buf, read_len);

		file->write_size += read_len;
		file->read_size += read_len;
	}

	/* finish CRC calculation and store into pgFile */
	FIN_CRC32C(crc);
	file->crc = crc;

	/* update file permission */
	if (chmod(to_path, st.st_mode) == -1)
	{
		errno_tmp = errno;
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot change mode of \"%s\": %s", to_path,
			 strerror(errno_tmp));
	}

	fclose(in);
	fclose(out);

	if (check)
		remove(to_path);

	return true;
}

/*
 * Save part of the file into backup.
 * skip_size - size of the file in previous backup. We can skip it
 *			   and copy just remaining part of the file
 */
bool
copy_file_partly(const char *from_root, const char *to_root,
				 pgFile *file, size_t skip_size)
{
	char		to_path[MAXPGPATH];
	FILE	   *in;
	FILE	   *out;
	size_t		read_len = 0;
	int			errno_tmp;
	struct stat	st;
	char		buf[8192];

	/* reset size summary */
	file->read_size = 0;
	file->write_size = 0;

	/* open backup mode file for read */
	in = fopen(file->path, "r");
	if (in == NULL)
	{
		/* maybe deleted, it's not error */
		if (errno == ENOENT)
			return false;

		elog(ERROR, "cannot open source file \"%s\": %s", file->path,
			 strerror(errno));
	}

	/* open backup file for write  */
	if (check)
		snprintf(to_path, lengthof(to_path), "%s/tmp", backup_path);
	else
		join_path_components(to_path, to_root, file->path + strlen(from_root) + 1);

	out = fopen(to_path, "w");
	if (out == NULL)
	{
		int errno_tmp = errno;
		fclose(in);
		elog(ERROR, "cannot open destination file \"%s\": %s",
			 to_path, strerror(errno_tmp));
	}

	/* stat source file to change mode of destination file */
	if (fstat(fileno(in), &st) == -1)
	{
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot stat \"%s\": %s", file->path,
			 strerror(errno));
	}

	if (fseek(in, skip_size, SEEK_SET) < 0)
		elog(ERROR, "cannot seek %lu of \"%s\": %s",
				skip_size, file->path, strerror(errno));

	/*
	 * copy content
	 * NOTE: Now CRC is not computed for compressed files now.
	 */
	for (;;)
	{
		if ((read_len = fread(buf, 1, sizeof(buf), in)) != sizeof(buf))
			break;

		if (fwrite(buf, 1, read_len, out) != read_len)
		{
			errno_tmp = errno;
			/* oops */
			fclose(in);
			fclose(out);
			elog(ERROR, "cannot write to \"%s\": %s", to_path,
				 strerror(errno_tmp));
		}

		file->write_size += sizeof(buf);
		file->read_size += sizeof(buf);
	}

	errno_tmp = errno;
	if (!feof(in))
	{
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot read backup mode file \"%s\": %s",
			 file->path, strerror(errno_tmp));
	}

	/* copy odd part. */
	if (read_len > 0)
	{
		if (fwrite(buf, 1, read_len, out) != read_len)
		{
			errno_tmp = errno;
			/* oops */
			fclose(in);
			fclose(out);
			elog(ERROR, "cannot write to \"%s\": %s", to_path,
				 strerror(errno_tmp));
		}

		file->write_size += read_len;
		file->read_size += read_len;
	}

	/* update file permission */
	if (chmod(to_path, st.st_mode) == -1)
	{
		errno_tmp = errno;
		fclose(in);
		fclose(out);
		elog(ERROR, "cannot change mode of \"%s\": %s", to_path,
			 strerror(errno_tmp));
	}

	/* add meta information needed for recovery */
	file->is_partial_copy = 1;

//	elog(LOG, "copy_file_partly(). %s file->write_size %lu", to_path, file->write_size);

	fclose(in);
	fclose(out);

	if (check)
		remove(to_path);

	return true;
}

bool
calc_file(pgFile *file)
{
	FILE	   *in;
	size_t		read_len = 0;
	int			errno_tmp;
	char		buf[8192];
	struct stat	st;
	pg_crc32	crc;

	INIT_CRC32C(crc);

	/* reset size summary */
	file->read_size = 0;
	file->write_size = 0;

	/* open backup mode file for read */
	in = fopen(file->path, "r");
	if (in == NULL)
	{
		FIN_CRC32C(crc);
		file->crc = crc;

		/* maybe deleted, it's not error */
		if (errno == ENOENT)
			return false;

		elog(ERROR, "cannot open source file \"%s\": %s", file->path,
			 strerror(errno));
	}

	/* stat source file to change mode of destination file */
	if (fstat(fileno(in), &st) == -1)
	{
		fclose(in);
		elog(ERROR, "cannot stat \"%s\": %s", file->path,
			 strerror(errno));
	}

	for (;;)
	{
		read_len = fread(buf, 1, sizeof(buf), in);

		if(read_len == 0)
			break;

		/* update CRC */
		COMP_CRC32C(crc, buf, read_len);

		file->write_size += read_len;
		file->read_size += read_len;
	}

	errno_tmp = errno;
	if (!feof(in))
	{
		fclose(in);
		elog(ERROR, "cannot read backup mode file \"%s\": %s",
			 file->path, strerror(errno_tmp));
	}

	/* finish CRC calculation and store into pgFile */
	FIN_CRC32C(crc);
	file->crc = crc;

	fclose(in);

	return true;
}
